/*
 * Save states: the load (SS_LOAD=<file>, SAVE_STATES_PLAN.md "Load").
 * Apple only, CMake PORT_SAVE_STATES (default OFF); compiled only in that build.
 *
 * The main thread runs the load from ss_run (ppu_ss_probe.cpp) before the guest
 * entry: guest memory, the kernel tables and the module blobs come back from the
 * file, every host lock and thread in them is made again, the open files are
 * reopened, and one host thread starts per saved PPU thread. Each thread (the
 * main one too) then takes its saved registers and chain (ss_load_take), takes
 * back the sys_mutexes it owned, waits until every thread is there, fixes up
 * a wait it was blocked in, and resumes its chain.
 *
 * Fix-ups for calls that were blocked inside (their host locals are gone):
 *   - sys_lwcond_wait (HLE) and sys_cond_wait (sc 107): relock the mutex and
 *     return CELL_OK, as a spurious wake-up (the guest checks its condition again).
 *   - every other blocking call runs again from its start (event queue receive,
 *     usleep, mutex / lwmutex lock, ...): a wait that had not finished is the
 *     same wait again.
 *
 * Not yet: GCM / RSX, audio, SPURS and SPU tasks (the next steps).
 */
#if defined(__APPLE__)

#include "ppu_context.h"     /* same layout as the lift's ppu_recomp.h (which the sys headers clash with) */
#include "ppu_ss_resume.h"
#include "ppu_ss_file.h"
#include "vm.h"
#include "sys_ppu_thread.h"
#include "sys_mutex.h"
#include "sys_cond.h"
#include "sys_rwlock.h"
#include "sys_semaphore.h"
#include "sys_event.h"
#include "sys_timer.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef void (*ss_add_fn)(const char*, void*, size_t);
extern "C" void ppu_sysprx_ss_blobs(ss_add_fn add);           /* ppu_sysprx.cpp */
extern "C" int  ppu_sysprx_ss_restore(void);
extern "C" void ppu_sysprx_ss_lwcond_relock(ppu_context* ctx);
extern "C" void ppu_loader_ss_blobs(ss_add_fn add);           /* ppu_loader.cpp */
extern "C" void ppu_fs_ss_blobs(ss_add_fn add);               /* ppu_fs.cpp */
extern "C" int  ppu_fs_ss_restore(int* lost);
extern "C" void sys_memory_ss_blobs(ss_add_fn add);           /* sys_memory.c */
extern "C" void sys_vm_ss_blobs(ss_add_fn add);               /* sys_vm.c */
extern "C" int  ss_site_has(uint32_t fn, uint64_t key);       /* ppu_ss_resume.cpp */

#define SS_NID_LWCOND_WAIT 0x2A6D9D51u
#define SS_SC_COND_WAIT    107u

/* --- module blobs: registered once, before the first save or the load ---------- */

static void ss_mods_do(void)
{
    ppu_sysprx_ss_blobs(ss_file_mod_add);
    ppu_loader_ss_blobs(ss_file_mod_add);
    ppu_fs_ss_blobs(ss_file_mod_add);
    sys_memory_ss_blobs(ss_file_mod_add);
    sys_vm_ss_blobs(ss_file_mod_add);
}
extern "C" void ss_mods_init(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, ss_mods_do);
}

/* --- state of one load ----------------------------------------------------------- */

struct ss_lthr {
    ss_ppu_rec r;
    ppu_context ctx;
    ss_rframe* chain;   /* heap, never freed: ss_resume_begin keeps a pointer */
    int taken;
};
static ss_lthr* s_th;
static int s_nth;
static int s_loading;                 /* 1 from the load until every saved thread has resumed */
static int s_ready, s_go, s_resumed;  /* __atomic */
static double s_t0;

static double ss_lnow(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void* ss_dummy_thread(void*) { return NULL; }

/* The kernel tables came back as raw bytes: their host locks hold the saving
 * process's bytes and their waiter lists point into its stacks. Make each lock
 * again (as its create call does) and clear what only blocked threads had. */
static void ss_rehost_kernel(void)
{
    int nm = 0, nc = 0, nr = 0, ns = 0, nq = 0, nf = 0, nt = 0, nfin = 0, nlost = 0;
    for (int i = 0; i < SYS_MUTEX_MAX; i++) {
        sys_mutex_info* m = &g_sys_mutexes[i];
        if (!m->active) continue;
        pthread_mutexattr_t a;
        pthread_mutexattr_init(&a);
        pthread_mutexattr_settype(&a, m->recursive ? PTHREAD_MUTEX_RECURSIVE : PTHREAD_MUTEX_ERRORCHECK);
        pthread_mutex_init(&m->mtx, &a);
        pthread_mutexattr_destroy(&a);
        nm++;
    }
    for (int i = 0; i < SYS_COND_MAX; i++) {
        sys_cond_info* c = &g_sys_conds[i];
        if (!c->initialized) continue;   /* a freed cond keeps its locks for reuse */
        pthread_mutex_init(&c->signal_lock, NULL);
        pthread_cond_init(&c->cv, NULL);
        c->waiters = NULL;
        nc += c->active != 0;
    }
    for (int i = 0; i < SYS_RWLOCK_MAX; i++) {
        sys_rwlock_info* r = &g_sys_rwlocks[i];
        if (!r->active) continue;
        pthread_rwlock_init(&r->rwl, NULL);
        nr++;
    }
    for (int i = 0; i < SYS_SEMAPHORE_MAX; i++) {
        sys_semaphore_info* s = &g_sys_semaphores[i];
        if (!s->active) continue;
        pthread_mutex_init(&s->mtx, NULL);
        pthread_cond_init(&s->cv, NULL);
        s->waiters = 0;
        ns++;
    }
    for (int i = 0; i < SYS_EVENT_QUEUE_MAX; i++) {
        sys_event_queue_info* q = &g_sys_event_queues[i];
        if (!q->active) continue;
        pthread_mutex_init(&q->lock, NULL);
        pthread_cond_init(&q->not_empty, NULL);
        nq++;
    }
    for (int i = 0; i < SYS_EVENT_FLAG_MAX; i++) {
        sys_event_flag_info* f = &g_sys_event_flags[i];
        if (!f->active) continue;
        pthread_mutex_init(&f->lock, NULL);
        pthread_cond_init(&f->cv, NULL);
        nf++;
    }
    for (int i = 0; i < SYS_TIMER_MAX; i++) nt += g_sys_timers[i].active != 0;
    sys_timer_ss_restart();
    /* PPU threads: the saved ones start later (ss_start_threads). Slot 0 is the main
     * thread (this one), nobody joins it. A finished thread nobody has joined yet gets
     * a host thread that has already ended, so a join finds it. */
    for (int i = 0; i < PPU_THREAD_MAX; i++) {
        ppu_thread_info* t = &g_ppu_threads[i];
        if (t->state == PPU_THREAD_STATE_FREE) continue;
        pthread_mutex_init(&t->finish_mutex, NULL);
        pthread_cond_init(&t->finish_cond, NULL);
        t->host_thread = 0;
        t->finished = 0;
        if (t->state == PPU_THREAD_STATE_FINISHED) {
            t->finished = 1;
            if (pthread_create(&t->host_thread, NULL, ss_dummy_thread, NULL) != 0) t->host_thread = 0;
            nfin++;
            continue;
        }
        if (i == 0) { t->host_thread = pthread_self(); continue; }
        int saved = 0;
        for (int k = 0; k < s_nth; k++) saved |= s_th[k].r.t == (uint32_t)(i + 1);
        if (!saved) {
            nlost++;
            fprintf(stderr, "[ss] load: thread %d '%s' (state %d) is in the table but not in the save; it does not run\n",
                    i + 1, t->name, t->state);
        }
    }
    fprintf(stderr, "[ss] load: kernel locks made again: %d mutexes, %d conds, %d rwlocks, %d semaphores, "
                    "%d event queues, %d event flags, %d timers; %d finished threads, %d threads not in the save\n",
            nm, nc, nr, ns, nq, nf, nt, nfin, nlost);
}

/* Read the "ppu" section into s_th. */
static int ss_parse_ppu(ss_in* in)
{
    uint64_t size = 0;
    const uint8_t* p = ss_file_sec(in, "ppu", &size);
    if (!p || size < 4) { fprintf(stderr, "[ss] load: no ppu section\n"); return 0; }
    const uint8_t* end = p + size;
    uint32_t cnt;
    memcpy(&cnt, p, 4); p += 4;
    s_th = (ss_lthr*)calloc(cnt ? cnt : 1, sizeof(ss_lthr));
    if (!s_th) return 0;
    for (uint32_t i = 0; i < cnt; i++) {
        ss_lthr* L = &s_th[s_nth];
        if (end - p < (ptrdiff_t)sizeof(ss_ppu_rec)) break;
        memcpy(&L->r, p, sizeof L->r); p += sizeof L->r;
        if (L->r.ctx_size != sizeof(ppu_context) || L->r.n == 0 || L->r.n > 128 ||
            (uint64_t)(end - p) < (uint64_t)L->r.ctx_size + (uint64_t)L->r.n * sizeof(ss_rframe)) {
            fprintf(stderr, "[ss] load: thread record %u is damaged\n", i);
            return 0;
        }
        memcpy(&L->ctx, p, sizeof(ppu_context)); p += sizeof(ppu_context);
        L->chain = (ss_rframe*)malloc((size_t)L->r.n * sizeof(ss_rframe));
        if (!L->chain) return 0;
        memcpy(L->chain, p, (size_t)L->r.n * sizeof(ss_rframe)); p += (size_t)L->r.n * sizeof(ss_rframe);
        s_nth++;
    }
    return s_nth == (int)cnt;
}

/* Start one host thread per saved PPU thread other than main. The trampoline reads
 * ctx->cia as the entry OPD, so put the entry back there; ss_run then swaps in the
 * saved registers and chain. */
static int ss_start_threads(void)
{
    int n = 0;
    for (int k = 0; k < s_nth; k++) {
        const uint32_t t = s_th[k].r.t;
        if (t == 1) continue;
        if (t == 0 || t > PPU_THREAD_MAX || g_ppu_threads[t - 1].state == PPU_THREAD_STATE_FREE) {
            fprintf(stderr, "[ss] load: saved thread %u has no table slot\n", t);
            return -1;
        }
        ppu_thread_info* info = &g_ppu_threads[t - 1];
        info->ctx.cia = info->entry_addr;
        if (!sys_ppu_thread_ss_start((int)(t - 1))) {
            fprintf(stderr, "[ss] load: thread %u '%s' did not start\n", t, info->name);
            return -1;
        }
        n++;
    }
    return n;
}

/* The whole load, on the main thread before the guest entry. Returns 0 if nothing
 * was changed yet (the game then boots normally), aborts after that. */
static int ss_load_all(const char* path)
{
    s_t0 = ss_lnow();
    ss_in* in = ss_file_in_new();
    if (!in || !ss_file_in_map(in, path)) {
        fprintf(stderr, "[ss] load %s: %s\n", path, in ? ss_file_why(in) : "out of memory");
        ss_file_in_free(in);
        return 0;
    }
    if (!ss_parse_ppu(in)) { ss_file_in_free(in); return 0; }
    ss_mods_init();
    s_loading = 1;

    ss_mem_stats ms;
    uint64_t md = 0;
    uint32_t td = 0;
    if (!ss_file_mem_load(in, 1, &ms, &md) || !ss_file_tables_load(in, 1, &td)) {
        fprintf(stderr, "[ss] load FAILED after it changed memory: %s\n", ss_file_why(in));
        abort();
    }
    const double t_mem = ss_lnow() - s_t0;
    ss_file_in_free(in);

    ss_rehost_kernel();
    const int nlwm = ppu_sysprx_ss_restore();
    int flost = 0;
    const int nf = ppu_fs_ss_restore(&flost);
    fprintf(stderr, "[ss] load: memory %u ranges, %.0f MB in %.0f ms; %d lwmutexes; %d files reopened, %d lost\n",
            ms.ranges, ms.nonzero / 1048576.0, t_mem * 1000.0, nlwm, nf, flost);

    const int ns = ss_start_threads();
    if (ns < 0) { fprintf(stderr, "[ss] load FAILED: threads\n"); abort(); }
    fprintf(stderr, "[ss] load: %d threads started (+ main), %.0f ms so far\n", ns, (ss_lnow() - s_t0) * 1000.0);
    return 1;
}

/* Take back the sys_mutexes and write locks this thread owned (before the others
 * can try for them). The host lock count matches the guest one (recursive mutexes). */
static void ss_relock_owned(uint32_t t)
{
    for (int i = 0; i < SYS_MUTEX_MAX; i++) {
        sys_mutex_info* m = &g_sys_mutexes[i];
        if (!m->active || m->owner_tid != t) continue;
        const int n = m->lock_count > 0 ? m->lock_count : 1;
        for (int k = 0; k < n; k++) pthread_mutex_lock(&m->mtx);
    }
    for (int i = 0; i < SYS_RWLOCK_MAX; i++) {
        sys_rwlock_info* r = &g_sys_rwlocks[i];
        if (r->active && r->writer_tid == t) pthread_rwlock_wrlock(&r->rwl);
    }
}

/* A wait this thread was blocked inside: relock and return as a spurious wake-up,
 * when the function has a resume point after the call. */
static void ss_fixup(ppu_context* ctx, ss_lthr* L)
{
    if (L->r.parked) return;   /* parked at a point: nothing ran yet, or it has all run */
    ss_rframe* f = &L->chain[L->r.n - 1];
    const uint32_t kind = (uint32_t)(f->key >> 32), addr = (uint32_t)f->key;
    if (kind == SS_K_HLE_IN && L->r.hle == SS_NID_LWCOND_WAIT) {
        ppu_sysprx_ss_lwcond_relock(ctx);   /* r3 = 0 */
        const uint64_t out = SS_KEY(SS_K_HLE_OUT, addr);
        if (ss_site_has(f->fn, out)) f->key = out;   /* else: wait again, holding the lock */
    } else if (kind == SS_K_SC_IN && L->r.sc == SS_SC_COND_WAIT) {
        const uint32_t id = (uint32_t)ctx->gpr[3];
        if (id == 0 || id > SYS_COND_MAX || !g_sys_conds[id - 1].active) return;   /* runs again: ESRCH */
        const uint32_t mid = g_sys_conds[id - 1].mutex_id;
        if (mid == 0 || mid > SYS_MUTEX_MAX || !g_sys_mutexes[mid - 1].active) return;
        sys_mutex_info* m = &g_sys_mutexes[mid - 1];
        pthread_mutex_lock(&m->mtx);
        __atomic_store_n(&m->owner_tid, ctx->thread_id, __ATOMIC_RELEASE);
        __atomic_store_n(&m->lock_count, 1, __ATOMIC_RELEASE);
        const uint64_t out = SS_KEY(SS_K_SC_OUT, addr);
        if (ss_site_has(f->fn, out)) { f->key = out; ctx->gpr[3] = 0; }
    }
}

/* ss_run (ppu_ss_probe.cpp) on every thread start while loading: the main thread
 * runs the load first. Returns this thread's chain (the registers are already in
 * ctx), or NULL to run fn as usual. */
extern "C" const ss_rframe* ss_load_take(ppu_context* ctx, int* n)
{
    static int s_tried;
    if (!s_tried && ctx->thread_id == 1) {
        s_tried = 1;
        const char* path = getenv("SS_LOAD");
        if (!path || !*path || !ss_load_all(path)) return NULL;
    }
    if (!__atomic_load_n(&s_loading, __ATOMIC_ACQUIRE)) return NULL;
    ss_lthr* L = NULL;
    for (int k = 0; k < s_nth; k++)
        if (s_th[k].r.t == (uint32_t)ctx->thread_id && !__atomic_exchange_n(&s_th[k].taken, 1, __ATOMIC_ACQ_REL)) { L = &s_th[k]; break; }
    if (!L) return NULL;   /* a thread the game makes after the load */

    memcpy(ctx, &L->ctx, sizeof(ppu_context));
    ctx->reserve_valid = 0;
    ss_relock_owned(L->r.t);

    /* everyone holds what they owned before anyone waits for it */
    if (__atomic_add_fetch(&s_ready, 1, __ATOMIC_ACQ_REL) == s_nth) {
        fprintf(stderr, "[ss] load: all %d threads ready after %.0f ms\n", s_nth, (ss_lnow() - s_t0) * 1000.0);
        __atomic_store_n(&s_go, 1, __ATOMIC_RELEASE);
    }
    double next = ss_lnow() + 5.0;
    while (!__atomic_load_n(&s_go, __ATOMIC_ACQUIRE)) {
        usleep(200);
        if (L->r.t == 1 && ss_lnow() > next) {
            next += 5.0;
            fprintf(stderr, "[ss] load: waiting, %d of %d threads ready:", __atomic_load_n(&s_ready, __ATOMIC_ACQUIRE), s_nth);
            for (int k = 0; k < s_nth; k++)
                if (!__atomic_load_n(&s_th[k].taken, __ATOMIC_ACQUIRE)) fprintf(stderr, " %u", s_th[k].r.t);
            fprintf(stderr, " missing\n");
        }
    }
    ss_fixup(ctx, L);
    if (__atomic_add_fetch(&s_resumed, 1, __ATOMIC_ACQ_REL) == s_nth) {
        __atomic_store_n(&s_loading, 0, __ATOMIC_RELEASE);
        fprintf(stderr, "[ss] load done: %d threads resumed, %.0f ms\n", s_nth, (ss_lnow() - s_t0) * 1000.0);
    }
    *n = (int)L->r.n;
    return L->chain;
}

#endif /* __APPLE__ */
