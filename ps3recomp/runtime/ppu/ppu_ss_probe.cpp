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

#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

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
    uint32_t sc, hle, park_num, sp, lr;
    int nh, ng;
    uintptr_t h[SS_HF];
    uint32_t g[SS_GF + 1], gw[SS_GF + 1];
};

extern "C" { int g_ss_on = 0; }
static ss_slot s_ss[SS_MAX];
static volatile int s_ss_stop = 0;
static ss_rec s_rec[SS_MAX];
static double s_t0;

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

/* Read once at load: the environment is complete before main(). */
__attribute__((used)) static int s_ss_init = (g_ss_on = (getenv("SS_PROBE") && atof(getenv("SS_PROBE")) > 0.0) ? 1 : 0, 0);

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
}

static const char* ss_sym(uintptr_t a, char* buf, size_t n)
{
    Dl_info di;
    if (!a || !dladdr((void*)a, &di) || !di.dli_sname) { snprintf(buf, n, "-"); return buf; }
    const char* nm = di.dli_sname;
    const char* f = strstr(nm, "func_");
    if (f && strstr(nm, "spu_func_")) { snprintf(buf, n, "S"); return buf; }
    if (f && strlen(f) >= 13) { snprintf(buf, n, "F:%.8s", f + 5); return buf; }
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

static void ss_probe(int idx, int wait_ms)
{
    const double t0 = ss_now();
    __atomic_store_n(&s_ss_stop, 1, __ATOMIC_RELEASE);
    int alive = 0, safe = 0;
    double t_safe = -1.0;
    for (;;) {
        alive = safe = 0;
        for (int t = 0; t < SS_MAX; t++) {
            ss_slot* s = &s_ss[t];
            if (!__atomic_load_n(&s->alive, __ATOMIC_ACQUIRE)) continue;
            alive++;
            if (__atomic_load_n(&s->parked, __ATOMIC_ACQUIRE) || __atomic_load_n(&s->dsc, __ATOMIC_ACQUIRE) > 0 ||
                __atomic_load_n(&s->dhle, __ATOMIC_ACQUIRE) > 0) safe++;
        }
        if (safe == alive) { t_safe = ss_now() - t0; break; }
        if ((ss_now() - t0) * 1000.0 >= wait_ms) break;
        usleep(500);
    }

    /* Suspend everyone first, then read: one consistent snapshot. */
    int nrec = 0;
    for (int t = 0; t < SS_MAX && nrec < SS_MAX; t++) {
        ss_slot* s = &s_ss[t];
        if (!__atomic_load_n(&s->alive, __ATOMIC_ACQUIRE)) continue;
        ss_rec* r = &s_rec[nrec++];
        r->t = t;
        r->susp = thread_suspend(s->port) == KERN_SUCCESS;
    }
    const double t_susp = ss_now() - t0;
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
    }
    const double t_read = ss_now() - t0;
    for (int i = 0; i < nrec; i++)
        if (s_rec[i].susp) thread_resume(s_ss[s_rec[i].t].port);
    __atomic_store_n(&s_ss_stop, 0, __ATOMIC_RELEASE);
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
    if (s_mem_every > 0 && (idx - 1) % s_mem_every == 0) ss_mem(idx);
    fflush(stderr);
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
    const double mean = atof(getenv("SS_PROBE"));
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
