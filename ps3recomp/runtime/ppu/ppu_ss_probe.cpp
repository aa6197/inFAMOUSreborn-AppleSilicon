/*
 * ps3recomp - save-state phase 0 probe (Apple port, test only)
 *
 * SS_PROBE=<secs> turns it on. Off by default; the hooks then cost one
 * predictable branch per lv2 syscall and HLE call.
 *
 * What it answers (SAVE_STATES_PLAN.md, option A, phase 0):
 *   1. Can every PPU thread be stopped at a safe point (an lv2 syscall or an
 *      HLE import, at entry or exit, or blocked inside one), and how fast?
 *   2. Can each stopped thread's call chain be read from the GUEST stack alone
 *      (back chain + saved LR words)? The probe records both the host chain
 *      (native frame pointers, lifted functions named func_XXXXXXXX) and the
 *      guest chain, and logs them; tools/ss_probe_check.py compares them.
 *   3. How big would a save be (readable guest memory, resident, non-zero,
 *      LZ4-compressed)?
 *
 * Every SS_PROBE seconds on average (random 0.5x..1.5x), after SS_PROBE_START
 * (default 60 s from the first guest syscall), up to SS_PROBE_N times
 * (default 100): set the stop flag, wait up to SS_PROBE_WAIT_MS (default 1000)
 * for all threads to be safe, suspend them, copy both chains into static
 * buffers (no malloc, no locks while threads are suspended), resume, release,
 * then print. SS_PROBE_MEM_EVERY=<n> (default 10): memory scan every n-th probe.
 *
 * Log lines start with "[ss]". Nothing here changes guest state.
 */
#if defined(__APPLE__)

#include "ppu_recomp.h"   /* ppu_context, vm_base */
#if defined(PORT_SAVE_STATES)
#include "ppu_ss_resume.h"   /* phase 1: exact chains from the site tables */
#include "ppu_ss_file.h"     /* save file writer */
#include <setjmp.h>
#include <stddef.h>
#endif

#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <TargetConditionals.h>

#define SS_MAX 64          /* guest thread ids tracked (same bound as g_sc_inflight) */
#define SS_HF  64          /* host frames kept per thread */
#define SS_GF  64          /* guest frames kept per thread */

/* Guest areas that are always mapped read/write (vm.h): main memory and the
 * stack region. The guest stack walk never reads outside them. */
#define SS_MAIN_LO  0x00010000u
#define SS_MAIN_HI  0x10010000u
#define SS_STK_LO   0xD0000000u
#define SS_STK_HI   0xE0000000u

struct ss_slot {
    ppu_context* ctx;
    mach_port_t port;
    uintptr_t stk_lo, stk_hi;
    int alive, parked, where, dsc, dhle;   /* shared: __atomic builtins */
    uint32_t sc_num, hle_nid, park_num;
};

struct ss_rec {
    int t, susp, parked, where, dsc, dhle, run_state, gend;
    uint32_t sc, hle, park_num, sp, lr, cia, gtop;
    int nh, ng;
    uint32_t wlo, whi;   /* PORT_SAVE_STATES: guest stack copy [wlo, whi) at s_win + woff */
    size_t woff;
    uintptr_t h[SS_HF];
    uint32_t g[SS_GF + 1], gw[SS_GF + 1];
};

extern "C" { int g_ss_on = 0; }
static ss_slot s_ss[SS_MAX];
static volatile int s_ss_stop = 0;
static ss_rec s_rec[SS_MAX];
static double s_t0;
#if defined(PORT_SAVE_STATES)
static int s_reenter;   /* SS_REENTER=1 (any safe point) or 2 (entries only): re-entry test, see ss_re_point */
static void ss_re_point(ppu_context* ctx, ss_slot* s, unsigned t, int where);
#endif

static double ss_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int ss_env_int(const char* n, int d)
{
    const char* e = getenv(n);
    return (e && *e) ? atoi(e) : d;
}

/* Read once at load: the environment is complete before main(). SS_PROBE (probe),
 * SS_SAVE_AT (one save) or SS_LOAD (load a save; both PORT_SAVE_STATES builds) turns
 * the hooks on. */
static double ss_env_f(const char* n) { const char* e = getenv(n); return e ? atof(e) : 0.0; }
static int ss_env_set(const char* n) { const char* e = getenv(n); return e && *e; }

/* iOS / iPadOS, PORT_SAVE_STATES builds only (SS_MENU=0 turns it off): the hooks are on
 * from the start so the in-game menu can save (port_ss_save_async) and the app can save
 * on its way to the background. A load only works at process start, so "load" leaves a
 * marker file in Documents (port_ss_load_next) and the app closes; the next start reads
 * the marker once (it is deleted first: a load that crashes does not come back) and loads
 * that save as if SS_LOAD named it. Other builds: no change. */
#if defined(PORT_SAVE_STATES) && TARGET_OS_IOS
#define SS_MENU_BUILD 1
#else
#define SS_MENU_BUILD 0
#endif
#define SS_NEXT_MARK "ss_load_next"
static int s_ss_menu;               /* 1: menu saves on */
static char s_ss_marked[256];       /* save named by the marker at this start, for the log */
static int ss_menu_boot(void)
{
#if SS_MENU_BUILD
    const char* e = getenv("SS_MENU");
    if (e && *e == '0') return 0;
    s_ss_menu = 1;
    const char* home = getenv("HOME");
    if (home && *home && !ss_env_set("SS_LOAD")) {
        char mk[1024], name[256] = { 0 };
        snprintf(mk, sizeof mk, "%s/Documents/" SS_NEXT_MARK, home);
        if (FILE* f = fopen(mk, "r")) {
            if (fgets(name, sizeof name, f)) name[strcspn(name, "\r\n")] = 0;
            fclose(f);
            unlink(mk);
            if (name[0] && !strchr(name, '/')) {
                char p[1024];
                snprintf(p, sizeof p, "%s/Documents/%s", home, name);
                setenv("SS_LOAD", p, 1);
                snprintf(s_ss_marked, sizeof s_ss_marked, "%s", name);
            }
        }
    }
    return 1;
#else
    return 0;
#endif
}
__attribute__((used)) static int s_ss_init =
    (g_ss_on = (ss_menu_boot() || ss_env_f("SS_PROBE") > 0.0 || ss_env_f("SS_SAVE_AT") > 0.0 || ss_env_set("SS_LOAD")) ? 1 : 0, 0);

static inline int ss_gok(uint32_t a, uint32_t n)
{
    return (a >= SS_MAIN_LO && a + n <= SS_MAIN_HI && a + n > a) ||
           (a >= SS_STK_LO && a + n <= SS_STK_HI && a + n > a);
}
static inline uint32_t ss_g32(uint32_t a)
{
    uint32_t v; memcpy(&v, vm_base + a, 4); return __builtin_bswap32(v);
}
static inline uint64_t ss_g64(uint32_t a)
{
    uint64_t v; memcpy(&v, vm_base + a, 8); return __builtin_bswap64(v);
}
static inline uint32_t ss_code(uint32_t ra)
{
    return (ra >= 4 && ss_gok(ra - 4, 4)) ? ss_g32(ra - 4) : 0;
}

/* --- per-thread registration ---------------------------------------------- */

struct ss_tl { int t = -1; ~ss_tl() { if (t >= 0) __atomic_store_n(&s_ss[t].alive, 0, __ATOMIC_RELEASE); } };
static thread_local ss_tl tl_ss;

static void* ss_thread(void*);
static void ss_start(void)
{
    s_t0 = ss_now();
    pthread_t th;
    if (pthread_create(&th, NULL, ss_thread, NULL) == 0) pthread_detach(th);
}

/* where: 1 syscall entry, 2 syscall exit, 3 HLE entry, 4 HLE exit. */
extern "C" void ppu_ss_point(ppu_context* ctx, int where, uint32_t num)
{
    if (!ctx) return;
    const unsigned t = (unsigned)ctx->thread_id;
    if (t >= SS_MAX) return;
    ss_slot* s = &s_ss[t];
    if (tl_ss.t != (int)t) {
        static pthread_once_t once = PTHREAD_ONCE_INIT;
        pthread_t self = pthread_self();
        s->ctx = ctx;
        s->port = pthread_mach_thread_np(self);
        s->stk_hi = (uintptr_t)pthread_get_stackaddr_np(self);
        s->stk_lo = s->stk_hi - pthread_get_stacksize_np(self);
        __atomic_store_n(&s->dsc, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&s->dhle, 0, __ATOMIC_RELAXED);
        s->parked = 0;
        __atomic_store_n(&s->alive, 1, __ATOMIC_RELEASE);
        tl_ss.t = (int)t;
        pthread_once(&once, ss_start);
    }
    /* Exits leave the call before the stop check, entries join it after, so
     * a thread is always either parked, inside a call, or in guest code. */
    if (where == 2) __atomic_fetch_sub(&s->dsc, 1, __ATOMIC_RELEASE);
    else if (where == 4) __atomic_fetch_sub(&s->dhle, 1, __ATOMIC_RELEASE);
    if (__atomic_load_n(&s_ss_stop, __ATOMIC_ACQUIRE)) {
        s->where = where;
        s->park_num = num;
        __atomic_store_n(&s->parked, 1, __ATOMIC_RELEASE);
        while (__atomic_load_n(&s_ss_stop, __ATOMIC_ACQUIRE)) usleep(200);
        __atomic_store_n(&s->parked, 0, __ATOMIC_RELEASE);
    }
#if defined(PORT_SAVE_STATES)
    if (__builtin_expect(s_reenter, 0)) ss_re_point(ctx, s, t, where);
#endif
    if (where == 1) { s->sc_num = num; __atomic_fetch_add(&s->dsc, 1, __ATOMIC_RELEASE); }
    else if (where == 3) { s->hle_nid = num; __atomic_fetch_add(&s->dhle, 1, __ATOMIC_RELEASE); }
}

/* --- one probe -------------------------------------------------------------- */

static void ss_walk_host(ss_rec* r, const ss_slot* s)
{
    arm_thread_state64_t st;
    mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
    r->nh = 0;
    if (thread_get_state(s->port, ARM_THREAD_STATE64, (thread_state_t)&st, &cnt) != KERN_SUCCESS) return;
    const uintptr_t M = 0x00007FFFFFFFFFFFull;   /* drop pointer-auth bits from system frames */
    uintptr_t fp = (uintptr_t)arm_thread_state64_get_fp(st) & M;
    r->h[r->nh++] = (uintptr_t)arm_thread_state64_get_pc(st) & M;
    r->h[r->nh++] = (uintptr_t)arm_thread_state64_get_lr(st) & M;
    while (r->nh < SS_HF && fp >= s->stk_lo && fp + 16 <= s->stk_hi && !(fp & 7)) {
        const uintptr_t nfp = ((const uintptr_t*)fp)[0] & M;
        r->h[r->nh++] = ((const uintptr_t*)fp)[1] & M;
        if (nfp <= fp) break;
        fp = nfp;
    }
}

/* gend: 0 clean end (back chain 0), 1 sp outside known areas, 2 bad back
 * chain, 3 LR slot unreadable, 4 frame limit. */
static void ss_walk_guest(ss_rec* r, const ppu_context* c)
{
    r->sp = (uint32_t)c->gpr[1];
    r->lr = (uint32_t)c->lr;
    r->g[0] = r->lr; r->gw[0] = ss_code(r->lr);
    r->ng = 1;
    r->gend = 4;
    uint32_t cur = r->sp;
    while (r->ng <= SS_GF) {
        if ((cur & 7) || !ss_gok(cur, 8)) { r->gend = 1; break; }
        const uint32_t next = (uint32_t)ss_g64(cur);
        if (next == 0) { r->gend = 0; break; }
        if (next <= cur || next - cur > 0x100000u || (next & 7)) { r->gend = 2; break; }
        if (!ss_gok(next + 16, 8)) { r->gend = 3; break; }
        const uint32_t ra = (uint32_t)ss_g64(next + 16);
        r->g[r->ng] = ra; r->gw[r->ng] = ss_code(ra); r->ng++;
        cur = next;
    }
    r->gtop = cur;
}

static const char* ss_sym(uintptr_t a, char* buf, size_t n)
{
    Dl_info di;
    if (!a || !dladdr((void*)a, &di) || !di.dli_sname) { snprintf(buf, n, "-"); return buf; }
    const char* nm = di.dli_sname;
    const char* f = strstr(nm, "func_");
    if (f && strstr(nm, "spu_func_")) { snprintf(buf, n, "S"); return buf; }
    if (f && strlen(f) >= 13) { snprintf(buf, n, strstr(f, ".cold") ? "C:%.8s" : "F:%.8s", f + 5); return buf; }
    if (!strcmp(nm, "lv2_syscall")) { snprintf(buf, n, "SC"); return buf; }
    if (!strncmp(nm, "ps3_hle_call", 12)) { snprintf(buf, n, "HLE"); return buf; }
    if (!strncmp(nm, "ps3_indirect_call", 17)) { snprintf(buf, n, "IC"); return buf; }
    if (!strcmp(nm, "ppu_ss_point")) { snprintf(buf, n, "SS"); return buf; }
    if (!strncmp(nm, "OUTLINED_FUNCTION", 17)) { snprintf(buf, n, "O"); return buf; }
    static const void* s_main_base = NULL;
    if (!s_main_base) { Dl_info m; if (dladdr((void*)&ss_sym, &m)) s_main_base = m.dli_fbase; }
    if (di.dli_fbase != s_main_base) { snprintf(buf, n, "L"); return buf; }
    snprintf(buf, n, "?%.28s", nm);
    return buf;
}

static char ss_kind(uint32_t w, uint32_t ra, uint32_t* tgt)
{
    *tgt = 0;
    if (!w) return '?';
    if ((w & 0xFC000003u) == 0x48000001u) {           /* bl */
        int32_t d = (int32_t)(w & 0x03FFFFFCu);
        if (d & 0x02000000) d |= (int32_t)0xFC000000;
        *tgt = (ra - 4) + (uint32_t)d;
        return 'b';
    }
    if ((w & 0xFC0007FFu) == 0x4C000421u) return 'c';  /* bcctrl / bctrl */
    if ((w & 0xFC0007FFu) == 0x4C000021u) return 'l';  /* bclrl / blrl */
    if ((w & 0xFC000003u) == 0x40000001u) return 'k';  /* bcl */
    return 'x';
}

static int s_mem_every;
static void ss_mem(int probe);

#if defined(PORT_SAVE_STATES)
/* Phase 1 check: build each thread's exact chain from the site tables (ss_chain_build)
 * and log "[ss]  x ok ..." or "[ss]  x FAIL ...". The guest stack is copied while the
 * threads are suspended (no malloc then: one static buffer, pages touched as used). */
#define SS_WIN_MAX  (16u << 20)
#define SS_WIN_T    (1u << 20)
static uint8_t s_win[SS_WIN_MAX];

/* The guest stack window of a thread: [sp, end of its back chain + 0x100), at most SS_WIN_T. */
static void ss_win_range(const ss_rec* r, uint32_t* lo_, uint32_t* hi_)
{
    uint32_t lo = r->sp & ~7u, hi = r->gtop + 0x100u;
    if (hi < lo || hi - lo > SS_WIN_T) hi = lo + SS_WIN_T;
    while (hi > lo && !ss_gok(lo, hi - lo)) hi -= 8;
    *lo_ = lo; *hi_ = hi;
}

/* Copy guest [lo, hi) to dst. Guest stacks have guard pages (no access) between them: copy
 * page by page through the kernel, which fails instead of faulting, and stop at the first page
 * that cannot be read. Returns the end of what was copied. */
static uint32_t ss_copy_guest(uint32_t lo, uint32_t hi, uint8_t* dst)
{
    uint32_t a = lo;
    while (a < hi) {
        const uint32_t end = ((a & ~0x3FFFu) + 0x4000u) < hi ? ((a & ~0x3FFFu) + 0x4000u) : hi;
        vm_size_t got = 0;
        if (vm_read_overwrite(mach_task_self(), (vm_address_t)(uintptr_t)(vm_base + a), end - a,
                                   (vm_address_t)(uintptr_t)(dst + (a - lo)), &got) != KERN_SUCCESS ||
            got != end - a)
            break;
        a = end;
    }
    return a;
}

static void ss_copy_stack(ss_rec* r, size_t* fill)
{
    r->wlo = r->whi = 0; r->woff = *fill;
    uint32_t lo, hi;
    ss_win_range(r, &lo, &hi);
    if (hi <= lo || *fill + (hi - lo) > SS_WIN_MAX) return;
    hi = ss_copy_guest(lo, hi, s_win + *fill);
    if (hi <= lo) return;
    r->wlo = lo; r->whi = hi;
    *fill += hi - lo;
}

#define SS_H_COLD 7   /* a func_X.cold.N part; only between ss_classify and ss_fold_cold */
static void ss_classify(uintptr_t a, ss_hframe* o)
{
    o->cls = SS_H_OTHER; o->fn = 0;
    Dl_info di;
    if (!a || !dladdr((void*)a, &di) || !di.dli_sname) return;
    const char* nm = di.dli_sname;
    const char* f = strstr(nm, "func_");
    if (f && !strstr(nm, "spu_func_")) {
        char hex[9], *end;
        if (strlen(f) < 13) return;
        memcpy(hex, f + 5, 8); hex[8] = 0;
        const unsigned long v = strtoul(hex, &end, 16);
        if (end == hex + 8) { o->cls = strstr(f, ".cold") ? SS_H_COLD : SS_H_FUNC; o->fn = (uint32_t)v; }
        return;
    }
    if (!strcmp(nm, "lv2_syscall")) o->cls = SS_H_SC;
    else if (!strncmp(nm, "ps3_hle_call", 12)) o->cls = SS_H_HLE;
    else if (!strncmp(nm, "ps3_indirect_call", 17)) o->cls = SS_H_IC;
    else if (!strncmp(nm, "OUTLINED_FUNCTION", 17)) o->cls = SS_H_OUTLINED;
}

/* Hot/cold splitting moves unlikely code of a lifted function, such as its resume block, into
 * func_X.cold.N. A call made from there leaves two host frames for one guest frame: the cold
 * part, then func_X that called it. Fold them into one SS_H_FUNC frame. */
static void ss_fold_cold(ss_hframe* h, int nh)
{
    for (int i = 0; i < nh; i++) {
        if (h[i].cls != SS_H_COLD) continue;
        h[i].cls = SS_H_FUNC;
        if (i + 1 < nh && h[i + 1].cls == SS_H_FUNC && h[i + 1].fn == h[i].fn) h[i + 1].cls = SS_H_OUTLINED;
    }
}

/* The exact chain of one recorded thread (after the threads run again: dladdr may lock). */
static int ss_rec_chain(const ss_rec* r, ss_rframe* out, int max, ss_chain_info* info)
{
    ss_hframe hf[SS_HF];
    int nh = 0;
    for (int k = 0; k < r->nh; k++) {
        if (k == 1 && r->nh > 2 && r->h[1] == r->h[2]) continue;   /* LR register = first saved LR */
        ss_classify(r->h[k], &hf[nh++]);
    }
    ss_fold_cold(hf, nh);
    const ss_gwin g = { r->wlo, r->whi, s_win + r->woff };
    return ss_chain_build(hf, nh, r->sp, r->lr, r->cia, r->parked ? r->where : 0, &g, out, max, info);
}

static void ss_exact(int idx, int nrec)
{
    static ss_rframe out[128];
    int ok = 0, tried = 0;
    for (int i = 0; i < nrec; i++) {
        const ss_rec* r = &s_rec[i];
        if (!r->susp) continue;
        tried++;
        ss_chain_info info;
        const int n = ss_rec_chain(r, out, 128, &info);
        if (n > 0) {
            ok++;
            fprintf(stderr, "[ss]  x t=%d ok n=%d amb=%d top=%d\n", r->t, n, info.amb, info.top);
            if (info.amb)
                fprintf(stderr, "[ss]  a t=%d k=%d of %d fn=%08X took=%d ra=%08X other=%d ra=%08X in0=%08X\n", r->t,
                        info.amb_k, n, info.amb_fn, info.amb_took, info.amb_ra_took, info.amb_other, info.amb_ra_other,
                        out[n - 1].fn);
        } else {
            fprintf(stderr, "[ss]  x t=%d FAIL k=%d fn=%08X %s\n", r->t, info.fail_k, info.fail_fn, info.why);
        }
    }
    fprintf(stderr, "[ss] exact probe %d: %d of %d threads ok\n", idx, ok, tried);
}

/* --- in-process re-entry test (SS_REENTER=1, with SS_PROBE) ----------------------------
 * After each probe every thread is asked to re-enter at its next safe point (SS_REENTER=2: its
 * next syscall or HLE entry, so the register check below runs every time). There it builds
 * its own exact chain, unwinds its host stack back to ss_run (longjmp) and calls its outermost
 * function in resume mode; the resume blocks rebuild the host stack from the chain. Nothing is
 * saved or restored (guest memory and kernel objects stay as they are), so this tests only the
 * resume code in the lifted functions. At a syscall or HLE entry the thread then meets the same
 * point again at once, and its registers must be bit for bit the same ("same" / "diff"). */
typedef void (*ss_ppu_fn)(ppu_context*);
extern "C" void (*g_ss_run_hook)(ppu_context*, ss_ppu_fn);   /* ppu_loader.cpp */
extern "C" ss_ppu_fn ppu_ss_lookup(uint32_t a);              /* ppu_loader.cpp */
extern "C" void ppu_ss_icall_reset(void);                    /* ppu_loader.cpp */
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);  /* ppu_loader.cpp */
extern "C" PPU_THREAD_LOCAL ppu_context* g_active_ctx;       /* ppu_loader.cpp */
extern "C" void ss_resume_begin(const ss_rframe* f, int n);  /* ppu_ss_resume.cpp */
extern "C" const ss_rframe* ss_load_take(ppu_context* ctx, int* n);   /* ppu_ss_load.cpp */
extern "C" void ss_mods_init(void);                                  /* ppu_ss_load.cpp */
#if defined(PORT_SAVE_STATES)
extern "C" int  ss_gcm_hold(int on);                             /* libs/video/cellGcmSys.c */
extern "C" int  ss_spurs_hold(int on, long* threads, long* parked);   /* libs/spurs/cellSpurs.c */
extern "C" void spurs_pm_ss_save(void (*put)(void*, const void*, size_t), void* o);   /* runtime/spu/spurs_policy.c */
extern "C" int  ss_spu_tasks_hold(int on, int* tasks, int* held);   /* runtime/spu/spu_workload.c */
extern "C" void ss_spu_tasks_save(void (*put)(void*, const void*, size_t), void* o);
extern "C" int  ss_sputhr_hold(int on, int* threads);                /* syscalls/lv2_register.c */
extern "C" int  ss_audio_hold(int on);                               /* libs/audio/cellAudio.c */
extern "C" int  ss_timer_hold(int on);                               /* syscalls/sys_timer.c */
extern "C" void ss_clock_mark(void);
extern "C" int  ss_sysutil_presave(void);                           /* libs/system/cellSysutil.c */
static void ss_put_cb(void* o, const void* p, size_t n) { ss_file_put((ss_out*)o, p, n); }
#endif
static int s_load;   /* SS_LOAD=<file> */

#define SS_RE_SNAP offsetof(ppu_context, thread_id)   /* gpr .. cia */
struct ss_re {
    jmp_buf jb;
    ppu_context* ctx;   /* while ss_run runs on this thread */
    int n, check;       /* chain length; point (1 or 3) to compare at next, 0 none */
    ss_rframe chain[128];
    uint8_t snap[SS_RE_SNAP];
};
static thread_local ss_re tl_re;
static int s_re_req[SS_MAX];
static unsigned s_re_n[5], s_re_skip, s_re_same, s_re_diff, s_re_log;

/* g_ss_run_hook: the thread entry trampoline and ppu_run call this instead of fn(ctx). */
static void ss_run(ppu_context* ctx, ss_ppu_fn fn)
{
    ss_re* r = &tl_re;
    volatile ss_ppu_fn f = fn;
    const int outer = r->ctx == NULL;
    if (outer) {
        if (_setjmp(r->jb)) {
            /* back from ss_re_point: no host frames above this one any more */
            ppu_ss_icall_reset();
            f = ppu_ss_lookup(r->chain[0].fn);
            if (!f) { fprintf(stderr, "[ss] re: func_%08X not registered\n", r->chain[0].fn); abort(); }
            ss_resume_begin(r->chain, r->n);
            g_active_ctx = ctx;
        }
        r->ctx = ctx;
        if (s_load) {   /* a thread of a loaded save: its registers and chain */
            int n = 0;
            const ss_rframe* ch = ss_load_take(ctx, &n);
            if (ch) {
                ppu_ss_icall_reset();
                f = ppu_ss_lookup(ch[0].fn);
                if (!f) { fprintf(stderr, "[ss] load: func_%08X not registered\n", ch[0].fn); abort(); }
                ss_resume_begin(ch, n);
                g_active_ctx = ctx;
            }
        }
    }
    f(ctx);
    while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(ctx); }
    if (outer) r->ctx = NULL;
}

__attribute__((used)) static int s_re_init =
    (s_reenter = g_ss_on ? ss_env_int("SS_REENTER", 0) : 0, s_load = ss_env_set("SS_LOAD"),
     (s_reenter > 0 || s_load) ? (g_ss_run_hook = ss_run) : 0, 0);

static const char* ss_ctx_field(size_t off)
{
    static thread_local char b[24];
    if (off < offsetof(ppu_context, fpr)) snprintf(b, sizeof b, "r%zu", off / 8);
    else if (off < offsetof(ppu_context, vr)) snprintf(b, sizeof b, "f%zu", (off - offsetof(ppu_context, fpr)) / 8);
    else if (off < offsetof(ppu_context, cr)) snprintf(b, sizeof b, "v%zu", (off - offsetof(ppu_context, vr)) / 16);
    else if (off < offsetof(ppu_context, lr)) return "cr";
    else if (off < offsetof(ppu_context, ctr)) return "lr";
    else if (off < offsetof(ppu_context, xer)) return "ctr";
    else if (off < offsetof(ppu_context, fpscr)) return "xer";
    else if (off < offsetof(ppu_context, cia)) return "fpscr/vscr";
    else return "cia";
    return b;
}

static void ss_re_skip(unsigned t, int where, const char* why, uint32_t fn)
{
    __atomic_add_fetch(&s_re_skip, 1, __ATOMIC_RELAXED);
    if (__atomic_add_fetch(&s_re_log, 1, __ATOMIC_RELAXED) <= 60)
        fprintf(stderr, "[ss] re t=%u where=%d skipped: %s (func_%08X)\n", t, where, why, fn);
}

static void ss_re_point(ppu_context* ctx, ss_slot* s, unsigned t, int where)
{
    ss_re* r = &tl_re;
    if (r->check) {
        const int w = r->check;
        r->check = 0;
        if (w == where && !memcmp(r->snap, ctx, SS_RE_SNAP)) __atomic_add_fetch(&s_re_same, 1, __ATOMIC_RELAXED);
        else {
            __atomic_add_fetch(&s_re_diff, 1, __ATOMIC_RELAXED);
            size_t off = 0;
            while (off < SS_RE_SNAP && r->snap[off] == ((const uint8_t*)ctx)[off]) off++;
            fprintf(stderr, "[ss] re t=%u DIFF: expected point %d, got %d; first difference %s\n", t, w, where,
                    off < SS_RE_SNAP ? ss_ctx_field(off) : "none");
        }
    }
    if (!__atomic_load_n(&s_re_req[t], __ATOMIC_ACQUIRE)) return;
    if (s_reenter == 2 && (where == 2 || where == 4)) return;   /* SS_REENTER=2: at call entries only */
    __atomic_store_n(&s_re_req[t], 0, __ATOMIC_RELEASE);
    if (r->ctx != ctx) { ss_re_skip(t, where, r->ctx ? "other context (guest callback)" : "not started through ss_run", 0); return; }

    /* own host frames, innermost first (return addresses) */
    const uintptr_t M = 0x00007FFFFFFFFFFFull;
    ss_hframe hf[SS_HF];
    int nh = 0;
    uintptr_t fp = (uintptr_t)__builtin_frame_address(0);
    while (nh < SS_HF && fp >= s->stk_lo && fp + 16 <= s->stk_hi && !(fp & 7)) {
        const uintptr_t nfp = ((const uintptr_t*)fp)[0] & M;
        ss_classify(((const uintptr_t*)fp)[1] & M, &hf[nh++]);
        if (nfp <= fp) break;
        fp = nfp;
    }
    ss_fold_cold(hf, nh);
    /* the call this point is in must sit right under the innermost lifted frame
     * (ps3_hle_call and ps3_hle_call_impl can both have a frame there) */
    int i0 = -1;
    for (int i = 0; i < nh; i++) if (hf[i].cls == SS_H_FUNC) { i0 = i; break; }
    if (i0 < 1) { ss_re_skip(t, where, "no lifted frame", 0); return; }
    const uint32_t want = where <= 2 ? SS_H_SC : SS_H_HLE;
    int j = i0 - 1;
    while (j >= 0 && hf[j].cls == want) j--;
    for (int i = 0; i <= j; i++)
        if (hf[i].cls == SS_H_SC || hf[i].cls == SS_H_HLE) { ss_re_skip(t, where, "syscall inside an HLE call", hf[i0].fn); return; }
    if (j == i0 - 1) { ss_re_skip(t, where, "point not in a lifted call", hf[i0].fn); return; }

    /* own guest stack */
    ss_rec* gr = (ss_rec*)malloc(sizeof(ss_rec));
    if (!gr) return;
    ss_walk_guest(gr, ctx);
    uint32_t lo, hi;
    ss_win_range(gr, &lo, &hi);
    uint8_t* buf = hi > lo ? (uint8_t*)malloc(hi - lo) : NULL;
    if (buf) hi = ss_copy_guest(lo, hi, buf);
    const ss_gwin g = { lo, buf ? hi : lo, buf };
    ss_chain_info info;
    const int n = ss_chain_build(hf, nh, gr->sp, gr->lr, (uint32_t)ctx->cia, where, &g, r->chain, 128, &info);
    free(buf);
    free(gr);
    if (n <= 0) { ss_re_skip(t, where, info.why, info.fail_fn); return; }
    r->n = n;
    if (where == 1 || where == 3) { memcpy(r->snap, ctx, SS_RE_SNAP); r->check = where; }
    __atomic_add_fetch(&s_re_n[where], 1, __ATOMIC_RELAXED);
    _longjmp(r->jb, 1);
}

static void ss_re_mark(int idx)
{
    for (int t = 0; t < SS_MAX; t++)
        if (__atomic_load_n(&s_ss[t].alive, __ATOMIC_ACQUIRE)) __atomic_store_n(&s_re_req[t], 1, __ATOMIC_RELEASE);
    fprintf(stderr, "[ss] re (probe %d): re-entered %u (sc in %u, sc out %u, hle in %u, hle out %u), skipped %u; "
                    "registers same %u, diff %u\n", idx,
            s_re_n[1] + s_re_n[2] + s_re_n[3] + s_re_n[4], s_re_n[1], s_re_n[2], s_re_n[3], s_re_n[4], s_re_skip,
            s_re_same, s_re_diff);
}
#endif

/* Set the stop flag and wait up to wait_ms for every registered thread to be safe.
 * Returns the time it took in seconds, or -1 when some thread was not safe in time. */
static double ss_stop_all(double t0, int wait_ms, int* alive_, int* safe_)
{
    __atomic_store_n(&s_ss_stop, 1, __ATOMIC_RELEASE);
    for (;;) {
        int alive = 0, safe = 0;
        for (int t = 0; t < SS_MAX; t++) {
            ss_slot* s = &s_ss[t];
            if (!__atomic_load_n(&s->alive, __ATOMIC_ACQUIRE)) continue;
            alive++;
            if (__atomic_load_n(&s->parked, __ATOMIC_ACQUIRE) || __atomic_load_n(&s->dsc, __ATOMIC_ACQUIRE) > 0 ||
                __atomic_load_n(&s->dhle, __ATOMIC_ACQUIRE) > 0) safe++;
        }
        *alive_ = alive; *safe_ = safe;
        if (safe == alive) return ss_now() - t0;
        if ((ss_now() - t0) * 1000.0 >= wait_ms) return -1.0;
        usleep(500);
    }
}

/* Suspend every registered thread (one consistent snapshot), returns the record count. */
static int ss_suspend_all(void)
{
    int nrec = 0;
    for (int t = 0; t < SS_MAX && nrec < SS_MAX; t++) {
        ss_slot* s = &s_ss[t];
        if (!__atomic_load_n(&s->alive, __ATOMIC_ACQUIRE)) continue;
        ss_rec* r = &s_rec[nrec++];
        r->t = t;
        r->susp = thread_suspend(s->port) == KERN_SUCCESS;
    }
    return nrec;
}

/* Read every suspended thread: host and guest chains (and, for phase 1, its guest stack). */
static void ss_read_all(int nrec)
{
    size_t wfill = 0;
    (void)wfill;
    for (int i = 0; i < nrec; i++) {
        ss_rec* r = &s_rec[i];
        ss_slot* s = &s_ss[r->t];
        r->parked = s->parked; r->where = s->where; r->park_num = s->park_num;
        r->dsc = s->dsc; r->dhle = s->dhle; r->sc = s->sc_num; r->hle = s->hle_nid;
        r->nh = r->ng = 0; r->run_state = -1; r->gend = -1;
        if (!r->susp) continue;
        thread_basic_info_data_t bi;
        mach_msg_type_number_t bc = THREAD_BASIC_INFO_COUNT;
        if (thread_info(s->port, THREAD_BASIC_INFO, (thread_info_t)&bi, &bc) == KERN_SUCCESS)
            r->run_state = bi.run_state;
        ss_walk_host(r, s);
        ss_walk_guest(r, s->ctx);
        r->cia = (uint32_t)s->ctx->cia;
#if defined(PORT_SAVE_STATES)
        ss_copy_stack(r, &wfill);
#endif
    }
}

static void ss_resume_all(int nrec)
{
    for (int i = 0; i < nrec; i++)
        if (s_rec[i].susp) thread_resume(s_ss[s_rec[i].t].port);
    __atomic_store_n(&s_ss_stop, 0, __ATOMIC_RELEASE);
}

static void ss_probe(int idx, int wait_ms)
{
    const double t0 = ss_now();
    int alive = 0, safe = 0;
    const double t_safe = ss_stop_all(t0, wait_ms, &alive, &safe);

    /* Suspend everyone first, then read: one consistent snapshot. */
    const int nrec = ss_suspend_all();
    const double t_susp = ss_now() - t0;
    ss_read_all(nrec);
    const double t_read = ss_now() - t0;
    ss_resume_all(nrec);
    const double t_all = ss_now() - t0;

    fprintf(stderr, "[ss] probe %d at %.1f s: %d threads, %d safe, %s %.2f ms; suspended %.2f, read %.2f, released %.2f ms\n",
            idx, t0 - s_t0, alive, safe, t_safe >= 0 ? "all safe after" : "NOT all safe after",
            (t_safe >= 0 ? t_safe : (ss_now() - t0)) * 1000.0, t_susp * 1000.0, t_read * 1000.0, t_all * 1000.0);
    char line[8192], sym[48];
    for (int i = 0; i < nrec; i++) {
        const ss_rec* r = &s_rec[i];
        const char* st = r->parked ? (r->where == 1 ? "P-sc-in" : r->where == 2 ? "P-sc-out" : r->where == 3 ? "P-hle-in" : "P-hle-out")
                       : r->dsc > 0 ? "in-sc" : r->dhle > 0 ? "in-hle" : "guest";
        fprintf(stderr, "[ss] t=%d st=%s num=%X sc=%u hle=%08X dsc=%d dhle=%d rs=%d susp=%d sp=%08X lr=%08X gend=%d\n",
                r->t, st, r->parked ? r->park_num : 0, r->sc, r->hle, r->dsc, r->dhle, r->run_state, r->susp,
                r->sp, r->lr, r->gend);
        size_t o = 0;
        o += snprintf(line + o, sizeof line - o, "[ss]  h");
        for (int k = 0; k < r->nh && o < sizeof line - 64; k++)
            o += snprintf(line + o, sizeof line - o, " %s", ss_sym(r->h[k], sym, sizeof sym));
        fprintf(stderr, "%s\n", line);
        o = 0;
        o += snprintf(line + o, sizeof line - o, "[ss]  g");
        for (int k = 0; k < r->ng && o < sizeof line - 64; k++) {
            uint32_t tg;
            const char kd = ss_kind(r->gw[k], r->g[k], &tg);
            if (kd == 'b') o += snprintf(line + o, sizeof line - o, " %08X:b:%08X", r->g[k], tg);
            else           o += snprintf(line + o, sizeof line - o, " %08X:%c", r->g[k], kd);
        }
        fprintf(stderr, "%s\n", line);
    }
#if defined(PORT_SAVE_STATES)
    ss_exact(idx, nrec);
    if (s_reenter) ss_re_mark(idx);
#endif
    if (s_mem_every > 0 && (idx - 1) % s_mem_every == 0) ss_mem(idx);
    fflush(stderr);
}

#if defined(PORT_SAVE_STATES)
/* --- one save (SS_SAVE_AT=<secs> SS_SAVE=<file>) ----------------------------------------
 * Stop and suspend every PPU thread as a probe does, write guest memory and the kernel
 * tables while they are suspended (ppu_ss_file.cpp takes no locks), let them run, then
 * build each thread's exact chain from what was read and write the "ppu" section. Any
 * thread that is not safe in time or has no chain cancels the save (the file is removed).
 * While they are suspended the SPURS kernel threads park between passes and the
 * graphics hold still (frame clock, FIFO walker). SPURS taskset tasks and audio keep
 * running meanwhile (not saved yet). */
static ppu_context s_save_ctx[SS_MAX];

static int ss_save(const char* path, int wait_ms)
{
    ss_mods_init();   /* module blobs: before anything is suspended */
    const double t0 = ss_now();
    ss_out* o = ss_file_create(path);
    if (!o) { fprintf(stderr, "[ss] save: cannot create %s\n", path); return 0; }
    /* SS_SAVE_CHECK=1 (test): read the file back while the threads are still suspended and
     * compare it with live memory and tables. Pages SPU / RSX threads wrote meanwhile differ. */
    ss_in* chk = ss_env_int("SS_SAVE_CHECK", 0) ? ss_file_in_new() : NULL;
    int alive = 0, safe = 0;
    const double t_safe = ss_stop_all(t0, wait_ms, &alive, &safe);
    if (t_safe < 0) {
        __atomic_store_n(&s_ss_stop, 0, __ATOMIC_RELEASE);
        ss_file_close(o);
        ss_file_in_free(chk);
        unlink(path);
        fprintf(stderr, "[ss] save FAILED: %d of %d threads safe after %d ms\n", safe, alive, wait_ms);
        return 0;
    }
    const int nrec = ss_suspend_all();
    ss_read_all(nrec);
    int nsusp = 0;
    for (int i = 0; i < nrec; i++) {
        if (!s_rec[i].susp) continue;
        nsusp++;
        memcpy(&s_save_ctx[i], s_ss[s_rec[i].t].ctx, sizeof(ppu_context));
    }
    ss_mem_stats ms;
    uint64_t tab = 0;
    int ok = nsusp == nrec;
    /* SPURS first (a job may wait on the RSX), then graphics. No stdio until resumed. */
    const double t_hold = ss_now();
    long sk_n = 0, sk_p = 0;
    const int sk_ok = ss_spurs_hold(1, &sk_n, &sk_p);
    const double t_spurs = ss_now() - t_hold;
    ok = sk_ok && ok;
    int st_n = 0, st_w = 0;   /* SPURS taskset tasks (own threads): held in their wait */
    const int st_ok = ss_spu_tasks_hold(1, &st_n, &st_w);
    ok = st_ok && ok;
    int sp_n = 0;   /* SPU thread group workers (Bink decoder): held between requests */
    const int sp_ok = ss_sputhr_hold(1, &sp_n);
    ok = sp_ok && ok;
    const int au_ok = ss_audio_hold(1);   /* the mixer writes read indexes and sends events */
    ok = au_ok && ok;
    const int tm_ok = ss_timer_hold(1);   /* timer ticks push events */
    ok = tm_ok && ok;
    ss_clock_mark();                      /* the guest clock goes on from here after a load */
    const int gcm_ok = ss_gcm_hold(1);
    const double t_held = ss_now() - t_hold;
    ok = ss_file_mem(o, &ms) && ok;
    const int ncomp = ss_sysutil_presave();   /* -1: list busy or too long */
    ok = ncomp >= 0 && ok;
    ok = ss_file_tables(o, &tab) && ok;
    ss_file_sec_begin(o, "spurs.pm");
    spurs_pm_ss_save(ss_put_cb, o);
    ss_file_sec_end(o);
    ss_file_sec_begin(o, "spu.tasks");
    ss_spu_tasks_save(ss_put_cb, o);
    ss_file_sec_end(o);
    ss_mem_stats cms;
    uint64_t c_mem = 0;
    uint32_t c_tab = 0;
    int c_ok = 0;
    double t_chk = ss_now();
    if (chk) {
        ok = ss_file_sync(o) && ok;
        c_ok = ss_file_in_map(chk, path) && ss_file_mem_load(chk, 0, &cms, &c_mem) && ss_file_tables_load(chk, 0, &c_tab);
    }
    t_chk = ss_now() - t_chk;
    ss_gcm_hold(0);
    ss_timer_hold(0);
    ss_audio_hold(0);
    ss_sputhr_hold(0, NULL);
    ss_spu_tasks_hold(0, NULL, NULL);
    ss_spurs_hold(0, NULL, NULL);
    ss_resume_all(nrec);
    fprintf(stderr, "[ss] save: SPURS %ld of %ld kernel threads parked after %.1f ms; SPU tasks %d of %d held; "
            "SPU threads %d %s; audio %s; timers %s; graphics %s after %.1f ms; sysutil completions %d\n",
            sk_p, sk_n, t_spurs * 1000.0, st_w, st_n, sp_n, sp_ok ? "held" : "NOT held",
            au_ok ? "held" : "NOT held", tm_ok ? "held" : "NOT held",
            gcm_ok ? "held" : "NOT held", t_held * 1000.0, ncomp);
    const double t_frozen = ss_now() - t0;
    if (chk) {
        if (c_ok) {
            fprintf(stderr, "[ss] save check: %.0f MB of pages read back, %llu pages differ; %u tables differ; %.0f ms\n",
                    cms.nonzero / 1048576.0, (unsigned long long)c_mem, c_tab, t_chk * 1000.0);
            uint32_t dp[8];
            const uint32_t ndp = ss_file_diff_pages(chk, dp, 8);
            for (uint32_t i = 0; i < ndp; i++) fprintf(stderr, "[ss]  page differs: 0x%08x\n", dp[i]);
            uint32_t doff = 0;
            const char* dt = ss_file_diff_table(chk, &doff);
            if (dt) fprintf(stderr, "[ss]  table differs: %s at byte %u\n", dt, doff);
        } else
            fprintf(stderr, "[ss] save check FAILED: %s\n", ss_file_why(chk));
        ss_file_in_free(chk);
    }

    static ss_rframe out[128];
    const uint32_t cnt = (uint32_t)nrec;
    ss_file_sec_begin(o, "ppu");
    ss_file_put(o, &cnt, 4);
    int bad = 0;
    for (int i = 0; i < nrec; i++) {
        const ss_rec* r = &s_rec[i];
        ss_chain_info info;
        const int n = r->susp ? ss_rec_chain(r, out, 128, &info) : -1;
        if (n <= 0) {
            bad++;
            fprintf(stderr, "[ss] save: t=%d no chain (k=%d fn=%08X %s)\n", r->t, r->susp ? info.fail_k : -1,
                    r->susp ? info.fail_fn : 0, r->susp ? info.why : "not suspended");
            continue;
        }
        const ss_ppu_rec pr = { (uint32_t)r->t, (uint32_t)r->parked, (uint32_t)r->where, r->dsc, r->dhle,
                                r->sc, r->hle, r->park_num, (uint32_t)sizeof(ppu_context), (uint32_t)n };
        ss_file_put(o, &pr, sizeof pr);
        ss_file_put(o, &s_save_ctx[i], sizeof(ppu_context));
        ss_file_put(o, out, (size_t)n * sizeof(ss_rframe));
    }
    ss_file_sec_end(o);
    const uint64_t size = ss_file_size(o);
    ok = ss_file_close(o) && ok && !bad;
    const double MB = 1048576.0;
    if (!ok) unlink(path);
    fprintf(stderr, "[ss] save %s: %s, %d threads (%d without chain), all safe after %.1f ms, frozen %.0f ms, "
                    "total %.0f ms; memory %u ranges, %.0f MB mapped, %.0f MB non-zero, %.0f MB packed; tables %.1f MB; "
                    "file %.0f MB\n",
            ok ? "ok" : "FAILED", path, nrec, bad, t_safe * 1000.0, t_frozen * 1000.0, (ss_now() - t0) * 1000.0,
            ms.ranges, ms.mapped / MB, ms.nonzero / MB, ms.packed / MB, tab / MB, size / MB);
    return ok;
}

/* Menu / background save: on a thread of its own, into <path>.part, renamed over <path>
 * only when it worked (a failed save keeps the old one). */
typedef void (*port_ss_done_fn)(int ok, void* user);
struct ss_async { char path[1024]; port_ss_done_fn done; void* user; };
static volatile int s_ss_busy;
static void* ss_async_thread(void* p)
{
    pthread_setname_np("ss-save");
    ss_async* a = (ss_async*)p;
    char tmp[1040];
    snprintf(tmp, sizeof tmp, "%s.part", a->path);
    int ok = ss_save(tmp, ss_env_int("SS_PROBE_WAIT_MS", 5000));
    if (ok && rename(tmp, a->path) != 0) { ok = 0; unlink(tmp); }
    fprintf(stderr, "[ss] menu save %s: %s\n", ok ? "ok" : "FAILED", a->path);
    fflush(stderr);
    __atomic_store_n(&s_ss_busy, 0, __ATOMIC_RELEASE);
    if (a->done) a->done(ok, a->user);
    free(a);
    return NULL;
}
#endif

/* Shell interface (all Apple builds; 0 = not a save-state build, or SS_MENU=0). */
extern "C" int port_ss_available(void) { return s_ss_menu; }

/* Starts a save; done(ok, user) runs on the save thread when it ends. Returns 0 when it
 * did not start (no save states, a save already running). */
extern "C" int port_ss_save_async(const char* path, void (*done)(int ok, void* user), void* user)
{
#if defined(PORT_SAVE_STATES)
    if (!s_ss_menu || !path || !*path || strlen(path) >= 1000) return 0;
    if (__atomic_exchange_n(&s_ss_busy, 1, __ATOMIC_ACQ_REL)) return 0;
    ss_async* a = (ss_async*)calloc(1, sizeof(ss_async));
    pthread_t th;
    if (a) {
        snprintf(a->path, sizeof a->path, "%s", path);
        a->done = done;
        a->user = user;
        if (pthread_create(&th, NULL, ss_async_thread, a) == 0) { pthread_detach(th); return 1; }
        free(a);
    }
    __atomic_store_n(&s_ss_busy, 0, __ATOMIC_RELEASE);
#else
    (void)path; (void)done; (void)user;
#endif
    return 0;
}

extern "C" int port_ss_save_busy(void)
{
#if defined(PORT_SAVE_STATES)
    return __atomic_load_n(&s_ss_busy, __ATOMIC_ACQUIRE);
#else
    return 0;
#endif
}

/* Leaves the marker that makes the next start load Documents/<name>. Returns 1 if written. */
extern "C" int port_ss_load_next(const char* name)
{
    const char* home = getenv("HOME");
    if (!s_ss_menu || !home || !name || !*name || strchr(name, '/')) return 0;
    char mk[1024];
    snprintf(mk, sizeof mk, "%s/Documents/" SS_NEXT_MARK, home);
    FILE* f = fopen(mk, "w");
    if (!f) return 0;
    const int ok = fprintf(f, "%s\n", name) > 0;
    return (fclose(f) == 0) && ok;
}

/* --- save size estimate ---------------------------------------------------- */

typedef size_t (*ss_enc_fn)(uint8_t*, size_t, const uint8_t*, size_t, void*, int);

static void ss_mem(int probe)
{
    static ss_enc_fn enc = NULL;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        void* h = dlopen("/usr/lib/libcompression.dylib", RTLD_LAZY | RTLD_LOCAL);
        if (h) enc = (ss_enc_fn)dlsym(h, "compression_encode_buffer");
    }
    const double t0 = ss_now();
    const size_t pg = (size_t)getpagesize();
    const size_t BATCH = (size_t)1 << 20;
    static uint8_t* src = NULL;
    static uint8_t* dst = NULL;
    if (!src) { src = (uint8_t*)malloc(BATCH); dst = (uint8_t*)malloc(BATCH + BATCH / 4 + 4096); }
    if (!src || !dst) return;
    unsigned long long readable = 0, resident = 0, nonzero = 0, packed = 0;
    size_t fill = 0;
    vm_address_t a = (vm_address_t)vm_base;
    const vm_address_t end = (vm_address_t)vm_base + 0x100000000ull;
    unsigned char* vec = (unsigned char*)malloc((size_t)(0x100000000ull / pg));
    while (a < end && vec) {
        vm_size_t sz = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t ic = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        vm_address_t ra = a;
        if (vm_region_64(mach_task_self(), &ra, &sz, VM_REGION_BASIC_INFO_64,
                         (vm_region_info_t)&info, &ic, &obj) != KERN_SUCCESS) break;
        if (ra >= end) break;
        vm_address_t lo = ra < a ? a : ra;
        vm_address_t hi = ra + sz > end ? end : ra + sz;
        if ((info.protection & VM_PROT_READ) && hi > lo) {
            readable += hi - lo;
            const size_t np = (size_t)((hi - lo) / pg);
            if (mincore((void*)lo, hi - lo, (char*)vec) == 0) {
                for (size_t i = 0; i < np; i++) {
                    if (!(vec[i] & MINCORE_INCORE)) continue;
                    resident += pg;
                    const uint64_t* p = (const uint64_t*)(lo + i * pg);
                    size_t w = 0;
                    while (w < pg / 8 && !p[w]) w++;
                    if (w == pg / 8) continue;
                    nonzero += pg;
                    memcpy(src + fill, p, pg);
                    fill += pg;
                    if (fill == BATCH) {
                        packed += enc ? enc(dst, BATCH + BATCH / 4, src, fill, NULL, 0x100 /* LZ4 */) : fill;
                        fill = 0;
                    }
                }
            }
        }
        a = hi;
    }
    if (fill) packed += enc ? enc(dst, BATCH + BATCH / 4, src, fill, NULL, 0x100) : fill;
    free(vec);
    const double MB = 1048576.0;
    fprintf(stderr, "[ss] mem (probe %d): readable %.0f MB, resident %.0f MB, non-zero %.0f MB, lz4 %.0f MB (%s) in %.0f ms\n",
            probe, readable / MB, resident / MB, nonzero / MB, packed / MB, enc ? "compressed" : "no libcompression",
            (ss_now() - t0) * 1000.0);
}

/* --- driver ------------------------------------------------------------------ */

static void* ss_thread(void*)
{
    pthread_setname_np("ss-probe");
    if (s_ss_menu)
        fprintf(stderr, "[ss] menu saves on (SS_MENU)%s%s\n", s_ss_marked[0] ? "; this start loads " : "", s_ss_marked);
    const double mean = ss_env_f("SS_PROBE");
    if (mean <= 0.0) {   /* SS_SAVE_AT: one save */
        const double at = ss_env_f("SS_SAVE_AT");
        const char* path = getenv("SS_SAVE");
        if (at <= 0.0) return NULL;   /* SS_LOAD alone */
#if defined(PORT_SAVE_STATES)
        if (!path || !*path) { fprintf(stderr, "[ss] SS_SAVE_AT needs SS_SAVE=<file>\n"); return NULL; }
        fprintf(stderr, "[ss] save at %.1f s to %s\n", at, path);
        usleep((useconds_t)(at * 1e6));
        const int ok = ss_save(path, ss_env_int("SS_PROBE_WAIT_MS", 5000));
        fflush(stderr);
        if (ss_env_int("SS_SAVE_EXIT", 0)) _exit(ok ? 0 : 3);
#else
        (void)at; (void)path;
        fprintf(stderr, "[ss] SS_SAVE_AT needs a PORT_SAVE_STATES build\n");
#endif
        return NULL;
    }
    const int start = ss_env_int("SS_PROBE_START", 60);
    const int n = ss_env_int("SS_PROBE_N", 100);
    const int wait_ms = ss_env_int("SS_PROBE_WAIT_MS", 1000);
    s_mem_every = ss_env_int("SS_PROBE_MEM_EVERY", 10);
    fprintf(stderr, "[ss] save-state probe on: %d checks, every %.1f s on average from %d s, wait %d ms\n",
            n, mean, start, wait_ms);
    usleep((useconds_t)start * 1000000u);
    for (int i = 1; i <= n; i++) {
        const double w = mean * (0.5 + (double)arc4random_uniform(1001) / 1000.0);
        usleep((useconds_t)(w * 1e6));
        ss_probe(i, wait_ms);
    }
    fprintf(stderr, "[ss] probe done: %d checks\n", n);
    return NULL;
}

#endif /* __APPLE__ */
