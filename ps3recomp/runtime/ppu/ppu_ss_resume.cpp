/*
 * ps3recomp - save states phase 1 runtime (Apple port, CMake PORT_SAVE_STATES, default OFF)
 *
 * The lifted TUs that ps3recomp/tools/ss_resume_pass.py wrote register one site
 * table each. This file keeps them, builds the exact chain of a stopped PPU
 * thread from its host frames and guest stack (save side), and walks a saved
 * chain back into the lifted code (load side). See ppu_ss_resume.h.
 */
#if defined(__APPLE__)

#include "ppu_recomp.h"   /* ppu_context */
#include "ppu_ss_resume.h"

#include <mach/mach.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*ss_fn_t)(ppu_context*);
extern "C" ss_fn_t ppu_ss_lookup(uint32_t a);              /* ppu_loader.cpp */
extern "C" PPU_THREAD_LOCAL ppu_context* g_active_ctx;      /* ppu_loader.cpp */

extern "C" { int g_ss_resume = 0; }

/* --- site tables ------------------------------------------------------------ */

#define SS_MAX_TABLES 32
static const ss_site* s_tab[SS_MAX_TABLES];
static uint32_t s_tab_n[SS_MAX_TABLES];
static int s_ntab;

extern "C" void ss_register_sites(const ss_site* s, uint32_t n)
{
    /* static constructors, before main: single threaded */
    if (s_ntab < SS_MAX_TABLES) { s_tab[s_ntab] = s; s_tab_n[s_ntab] = n; s_ntab++; }
    else fprintf(stderr, "[ss] too many site tables, one dropped\n");
}

static ss_site* s_idx;      /* all rows, sorted by (fn, kind, addr) */
static uint32_t s_nidx;
static pthread_once_t s_idx_once = PTHREAD_ONCE_INIT;

static int ss_cmp(const ss_site* a, uint32_t fn, uint32_t kind, uint32_t addr)
{
    if (a->fn != fn) return a->fn < fn ? -1 : 1;
    if (a->kind != kind) return a->kind < kind ? -1 : 1;
    if (a->addr != addr) return a->addr < addr ? -1 : 1;
    return 0;
}

static void ss_index_build(void)
{
    uint32_t n = 0;
    for (int t = 0; t < s_ntab; t++) n += s_tab_n[t];
    s_idx = (ss_site*)malloc((size_t)(n ? n : 1) * sizeof(ss_site));
    if (!s_idx) return;
    for (int t = 0; t < s_ntab; t++) { memcpy(s_idx + s_nidx, s_tab[t], s_tab_n[t] * sizeof(ss_site)); s_nidx += s_tab_n[t]; }
    qsort(s_idx, s_nidx, sizeof(ss_site), [](const void* x, const void* y) -> int {
        const ss_site* b = (const ss_site*)y;
        return ss_cmp((const ss_site*)x, b->fn, b->kind, b->addr);
    });
    uint32_t none = 0;
    for (uint32_t i = 0; i < s_nidx; i++) none += s_idx[i].kind == SS_K_NONE;
    fprintf(stderr, "[ss] save-state sites: %u rows from %d tables, %u functions not resumable\n", s_nidx, s_ntab, none);
}

static const ss_site* ss_find(uint32_t fn, uint32_t kind, uint32_t addr)
{
    uint32_t lo = 0, hi = s_nidx;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        const int c = ss_cmp(&s_idx[mid], fn, kind, addr);
        if (c == 0) return &s_idx[mid];
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

/* 1 resumable, -1 in a table but not resumable, 0 not in any table */
static int ss_fn_state(uint32_t fn)
{
    if (ss_find(fn, SS_K_NONE, 0)) return -1;
    uint32_t lo = 0, hi = s_nidx;
    while (lo < hi) { const uint32_t mid = (lo + hi) / 2; if (s_idx[mid].fn < fn) lo = mid + 1; else hi = mid; }
    return (lo < s_nidx && s_idx[lo].fn == fn) ? 1 : 0;
}

/* --- save side ---------------------------------------------------------------- */

static int ss_rd64(const ss_gwin* g, uint32_t a, uint64_t* v)
{
    if (a < g->lo || a > g->hi || g->hi - a < 8) return 0;
    uint64_t x;
    memcpy(&x, g->p + (a - g->lo), 8);
    *v = __builtin_bswap64(x);
    return 1;
}

/* Target of the "bl" just before return address ra, or 0. Read with vm_read_overwrite so a
 * bad ra cannot fault. */
static uint32_t ss_bl_target(uint32_t ra)
{
    if (ra < 4 || (ra & 3)) return 0;
    uint32_t w = 0;
    vm_size_t got = 0;
    if (vm_read_overwrite(mach_task_self(), (vm_address_t)(uintptr_t)(vm_base + ra - 4), 4,
                               (vm_address_t)(uintptr_t)&w, &got) != KERN_SUCCESS || got != 4)
        return 0;
    w = __builtin_bswap32(w);
    if ((w >> 26) != 18 || (w & 3) != 1) return 0;          /* bl: opcode 18, AA=0, LK=1 */
    const int32_t li = (int32_t)(w << 6) >> 6 & ~3;          /* 26-bit signed offset */
    return (ra - 4) + (uint32_t)li;
}

#define SS_CHAIN_MAX 128

extern "C" int ss_chain_build(const ss_hframe* h, int nh, uint32_t r1, uint32_t lr, uint32_t cia, int parked_where,
                              const ss_gwin* g, ss_rframe* out, int max, ss_chain_info* info)
{
    pthread_once(&s_idx_once, ss_index_build);
    memset(info, 0, sizeof *info);
#define SS_FAIL(k_, fn_, w_) do { info->why = (w_); info->fail_k = (k_); info->fail_fn = (fn_); return -1; } while (0)
    if (!s_idx) SS_FAIL(0, 0, "no site tables");

    /* 1. lifted frames, innermost first; IC / outlined frames may sit between them */
    int i0 = -1;
    for (int i = 0; i < nh; i++) if (h[i].cls == SS_H_FUNC) { i0 = i; break; }
    if (i0 < 0) SS_FAIL(0, 0, "no lifted frame");
    if (i0 == 0) SS_FAIL(0, h[0].fn, "stopped inside lifted code");
    uint32_t kind0, addr0, stub = 0;
    if (h[i0 - 1].cls == SS_H_SC)       { kind0 = parked_where == 2 ? SS_K_SC_OUT : SS_K_SC_IN; addr0 = cia; }
    else if (h[i0 - 1].cls == SS_H_HLE) {
        kind0 = parked_where == 4 ? SS_K_HLE_OUT : SS_K_HLE_IN; addr0 = h[i0].fn;
        if (!ss_find(addr0, kind0, addr0)) {
            /* An import stub is "ps3_hle_call(nid, ctx); return;": the compiler makes that a
             * tail call, so the stub leaves no host frame. It is frameless in the guest too,
             * so LR is still the return address into its caller: the stub is that bl's target. */
            const uint32_t t = ss_bl_target(lr);
            if (t && ss_find(t, kind0, t)) { stub = t; addr0 = t; }
        }
    }
    else SS_FAIL(0, h[i0].fn, "innermost lifted frame is not in a syscall or HLE call");

    uint32_t L[SS_CHAIN_MAX];
    uint8_t viaic[SS_CHAIN_MAX];   /* an indirect-call frame between L[k] and L[k-1] */
    int n = 0, ic = 0, i = i0;
    if (stub) { L[n] = stub; viaic[n] = 0; n++; }
    for (; i < nh; i++) {
        const uint32_t c = h[i].cls;
        if (c == SS_H_FUNC) {
            if (n >= SS_CHAIN_MAX || n >= max) SS_FAIL(n, h[i].fn, "chain too deep");
            L[n] = h[i].fn; viaic[n] = (uint8_t)ic; ic = 0; n++;
        } else if (c == SS_H_IC) ic = 1;
        else if (c != SS_H_OUTLINED) break;
    }
    for (; i < nh; i++)
        if (h[i].cls == SS_H_FUNC) SS_FAIL(n, h[i].fn, "lifted code under a host frame (guest callback)");

    /* 2. walk the guest stack along it */
    ss_rframe tmp[SS_CHAIN_MAX];
    uint64_t v;
    const ss_site* e = ss_find(L[0], kind0, addr0);
    if (!e) SS_FAIL(0, L[0], ss_fn_state(L[0]) < 0 ? "function not resumable" :
                             ss_fn_state(L[0]) == 0 ? "function not in any site table" : "no such sc / HLE site");
    uint32_t S = r1, E;
    if (e->delta == SS_DELTA_DYN) { if (!ss_rd64(g, S, &v)) SS_FAIL(0, L[0], "guest stack unreadable"); E = (uint32_t)v; }
    else E = S - (uint32_t)e->delta;
    tmp[0] = { SS_KEY(kind0, addr0), L[0], E, 0, S };
    int lr_live = e->delta == 0;   /* frameless innermost: its return address is still in LR */
    for (int k = 1; k < n; k++) {
        const uint32_t fn = L[k];
        struct { uint32_t s, ra; int src; } c[3];
        int nc = 0;
        if (lr_live) c[nc++] = { E, lr, 0 };
        if (ss_rd64(g, E + 16, &v)) c[nc++] = { E, (uint32_t)v, 1 };
        uint64_t b;
        if (ss_rd64(g, E, &b) && (uint32_t)b > E && ss_rd64(g, (uint32_t)b + 16, &v)) c[nc++] = { (uint32_t)b, (uint32_t)v, 2 };
        int hit = -1;
        const ss_site* he = NULL;
        for (int j = 0; j < nc; j++) {
            const ss_site* x = ss_find(fn, SS_K_CALL, c[j].ra);
            if (!x) continue;
            if (hit < 0) { hit = j; he = x; }
            else if (c[j].ra != c[hit].ra) {
                if (!info->amb) {
                    info->amb_k = k; info->amb_fn = fn; info->amb_took = c[hit].src; info->amb_other = c[j].src;
                    info->amb_ra_took = c[hit].ra; info->amb_ra_other = c[j].ra;
                }
                info->amb++;
                break;
            }
        }
        if (hit >= 0) {
            S = c[hit].s;
            uint32_t Ek;
            if (he->delta == SS_DELTA_DYN) { if (!ss_rd64(g, S, &v)) SS_FAIL(k, fn, "guest stack unreadable"); Ek = (uint32_t)v; }
            else Ek = S - (uint32_t)he->delta;
            tmp[k] = { SS_KEY(SS_K_CALL, c[hit].ra), fn, Ek, L[k - 1], S };
            E = Ek;
            lr_live = 0;
            continue;
        }
        if (viaic[k] && ss_find(fn, SS_K_TAIL, fn)) {
            /* bctr tail jump made as a host call: no guest frame of its own */
            tmp[k] = { SS_KEY(SS_K_TAIL, fn), fn, E, L[k - 1], E };
            continue;
        }
        SS_FAIL(k, fn, ss_fn_state(fn) < 0 ? "function not resumable" :
                       ss_fn_state(fn) == 0 ? "function not in any site table" : "no call site fits the guest stack");
    }
    info->top = ss_rd64(g, E, &v) && v == 0;
    for (int k = 0; k < n; k++) out[k] = tmp[n - 1 - k];
    return n;
#undef SS_FAIL
}

/* --- load side ------------------------------------------------------------------ */

struct ss_cursor { const ss_rframe* f; int n, i; };
static thread_local ss_cursor tl_cur;
static int s_resuming;

/* Start rebuilding this thread's chain f[0..n-1] (outermost first). The caller then
 * calls f[0].fn with the saved registers in ctx. */
extern "C" void ss_resume_begin(const ss_rframe* f, int n)
{
    tl_cur.f = f; tl_cur.n = n; tl_cur.i = 0;
    __atomic_add_fetch(&s_resuming, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&g_ss_resume, 1, __ATOMIC_RELEASE);
}

static void ss_resume_done(void)
{
    tl_cur.f = NULL;
    if (__atomic_sub_fetch(&s_resuming, 1, __ATOMIC_ACQ_REL) == 0) __atomic_store_n(&g_ss_resume, 0, __ATOMIC_RELEASE);
}

extern "C" const ss_rframe* ss_resume_frame(ppu_context* ctx, uint32_t fn)
{
    ss_cursor* c = &tl_cur;
    if (!c->f) return NULL;            /* this thread is not resuming */
    const ss_rframe* f = &c->f[c->i];
    if (f->fn != fn) {
        fprintf(stderr, "[ss] load failed: thread %u entered func_%08X, saved chain frame %d is func_%08X\n",
                (unsigned)ctx->thread_id, fn, c->i, f->fn);
        abort();
    }
    if (++c->i >= c->n) ss_resume_done();   /* innermost frame: the thread runs normally from here */
    return f;
}

extern "C" void ss_resume_call(ppu_context* ctx, const ss_rframe* f)
{
    const ss_fn_t fn = ppu_ss_lookup(f->next_fn);
    if (!fn) {
        fprintf(stderr, "[ss] load failed: func_%08X is not registered\n", f->next_fn);
        abort();
    }
    g_active_ctx = ctx;
    fn(ctx);
}

extern "C" void ss_resume_fail(ppu_context* ctx, const ss_rframe* f)
{
    fprintf(stderr, "[ss] load failed: thread %u, func_%08X has no resume point for key %016llX\n",
            (unsigned)ctx->thread_id, f->fn, (unsigned long long)f->key);
    abort();
}

#endif /* __APPLE__ */
