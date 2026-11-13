/*
 * Save states: the save file writer (see ppu_ss_file.h).
 * Apple only; in the build only with CMake PORT_SAVE_STATES.
 *
 * Most of the file is written while every PPU thread is suspended. A suspended
 * thread may hold the malloc lock, a stdio lock or the dyld lock, so the writer
 * uses only system calls (write, pwrite, vm_region, mincore) and buffers that
 * ss_file_create allocated before the suspend.
 */
#if defined(__APPLE__)

#include "ppu_ss_file.h"
#include "vm.h"   /* vm_base */
#include "sys_ppu_thread.h"
#include "sys_mutex.h"
#include "sys_cond.h"
#include "sys_rwlock.h"
#include "sys_semaphore.h"
#include "sys_event.h"
#include "sys_timer.h"
#include "sys_memory.h"
#include "sys_vm.h"
#include "sys_fs.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern "C" uint32_t g_cond_id_addr[SYS_COND_MAX + 1];   /* sys_cond.c */

static const char SS_MAGIC[8] = { 'P', 'S', '3', 'S', 'A', 'V', 'E', '1' };
enum { SS_FILE_VERSION = 1 };

enum { SS_BUF = 8 << 20 };          /* write buffer */
enum { SS_BATCH_PAGES = 64 };       /* pages per compressed batch */
enum { SS_RANGES_MAX = 8192 };

struct ss_rng { uint32_t addr, len, prot; };

typedef size_t (*ss_enc_fn)(uint8_t*, size_t, const uint8_t*, size_t, void*, int);
typedef size_t (*ss_scr_fn)(int);

struct ss_out {
    int fd;
    uint8_t* buf;
    size_t fill;
    uint64_t off;           /* file offset of buf[0] */
    uint64_t sec_size_at;   /* file offset of the open section's size field */
    uint64_t sec_n;
    int sec_open;
    int err;
    uint32_t pg;
    ss_enc_fn enc;
    void* scratch;
    uint8_t* src;           /* SS_BATCH_PAGES pages */
    uint8_t* dst;           /* packed batch */
    size_t dst_cap;
    uint32_t* addrs;
    unsigned char* vec;     /* mincore, one byte per host page of 4 GB */
    ss_rng* rs;
};

/* LC_UUID of the main executable: the build id. Zero if missing. */
static void ss_build_uuid(uint8_t out[16])
{
    memset(out, 0, 16);
    const struct mach_header_64* h = (const struct mach_header_64*)_dyld_get_image_header(0);
    if (!h || h->magic != MH_MAGIC_64) return;
    const uint8_t* p = (const uint8_t*)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; i++) {
        const struct load_command* lc = (const struct load_command*)p;
        if (lc->cmd == LC_UUID) { memcpy(out, ((const struct uuid_command*)lc)->uuid, 16); return; }
        p += lc->cmdsize;
    }
}

static void ss_flush(ss_out* o)
{
    size_t done = 0;
    while (!o->err && done < o->fill) {
        const ssize_t w = write(o->fd, o->buf + done, o->fill - done);
        if (w <= 0) { o->err = 1; break; }
        done += (size_t)w;
    }
    o->off += o->fill;
    o->fill = 0;
}

static void ss_raw(ss_out* o, const void* p, size_t n)
{
    const uint8_t* s = (const uint8_t*)p;
    while (n && !o->err) {
        if (o->fill == SS_BUF) ss_flush(o);
        size_t k = SS_BUF - o->fill;
        if (k > n) k = n;
        memcpy(o->buf + o->fill, s, k);
        o->fill += k; s += k; n -= k;
    }
}

static void ss_free(ss_out* o)
{
    free(o->buf); free(o->scratch); free(o->src); free(o->dst); free(o->addrs); free(o->vec); free(o->rs);
    free(o);
}

extern "C" ss_out* ss_file_create(const char* path)
{
    ss_out* o = (ss_out*)calloc(1, sizeof(ss_out));
    if (!o) return NULL;
    o->pg = (uint32_t)getpagesize();
    void* lib = dlopen("/usr/lib/libcompression.dylib", RTLD_LAZY | RTLD_LOCAL);
    if (lib) {
        o->enc = (ss_enc_fn)dlsym(lib, "compression_encode_buffer");
        ss_scr_fn scr = (ss_scr_fn)dlsym(lib, "compression_encode_scratch_buffer_size");
        if (o->enc && scr) o->scratch = malloc(scr(0x100 /* LZ4 */) + 64);
        if (!o->scratch) o->enc = NULL;   /* no scratch: encode would malloc */
    }
    o->dst_cap = (size_t)SS_BATCH_PAGES * o->pg + 65536;
    o->buf = (uint8_t*)malloc(SS_BUF);
    o->src = (uint8_t*)malloc((size_t)SS_BATCH_PAGES * o->pg);
    o->dst = (uint8_t*)malloc(o->dst_cap);
    o->addrs = (uint32_t*)malloc(SS_BATCH_PAGES * sizeof(uint32_t));
    o->vec = (unsigned char*)malloc((size_t)(0x100000000ull / o->pg));
    o->rs = (ss_rng*)malloc(SS_RANGES_MAX * sizeof(ss_rng));
    o->fd = -1;
    if (!o->buf || !o->src || !o->dst || !o->addrs || !o->vec || !o->rs) { ss_free(o); return NULL; }
    o->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (o->fd < 0) { ss_free(o); return NULL; }
    uint8_t uuid[16];
    ss_build_uuid(uuid);
    const uint32_t ver = SS_FILE_VERSION;
    const uint64_t when = (uint64_t)time(NULL);
    ss_raw(o, SS_MAGIC, 8);
    ss_raw(o, &ver, 4);
    ss_raw(o, uuid, 16);
    ss_raw(o, &when, 8);
    return o;
}

extern "C" void ss_file_sec_begin(ss_out* o, const char* name)
{
    if (o->sec_open) ss_file_sec_end(o);
    const uint32_t nl = (uint32_t)strlen(name);
    ss_raw(o, &nl, 4);
    ss_raw(o, name, nl);
    o->sec_size_at = o->off + o->fill;
    const uint64_t z = 0;
    ss_raw(o, &z, 8);
    o->sec_n = 0;
    o->sec_open = 1;
}

extern "C" void ss_file_put(ss_out* o, const void* p, size_t n)
{
    ss_raw(o, p, n);
    o->sec_n += n;
}

extern "C" void ss_file_sec_end(ss_out* o)
{
    if (!o->sec_open) return;
    o->sec_open = 0;
    if (o->sec_size_at >= o->off) {   /* size field still in the buffer */
        memcpy(o->buf + (o->sec_size_at - o->off), &o->sec_n, 8);
        return;
    }
    if (pwrite(o->fd, &o->sec_n, 8, (off_t)o->sec_size_at) != 8) o->err = 1;
}

extern "C" int ss_file_close(ss_out* o)
{
    if (!o) return 0;
    ss_file_sec_end(o);
    ss_flush(o);
    int ok = !o->err;
    if (fsync(o->fd) != 0) ok = 0;
    if (close(o->fd) != 0) ok = 0;
    ss_free(o);
    return ok;
}

extern "C" uint64_t ss_file_size(const ss_out* o) { return o ? o->off + o->fill : 0; }

/* --- guest memory ---------------------------------------------------------------- */

static void ss_batch(ss_out* o, uint32_t nb, ss_mem_stats* st)
{
    if (!nb) return;
    const size_t raw = (size_t)nb * o->pg;
    size_t c = o->enc ? o->enc(o->dst, o->dst_cap, o->src, raw, o->scratch, 0x100 /* LZ4 */) : 0;
    const uint32_t cl = (uint32_t)c;
    ss_file_put(o, &nb, 4);
    ss_file_put(o, &o->pg, 4);
    ss_file_put(o, o->addrs, (size_t)nb * 4);
    ss_file_put(o, &cl, 4);
    if (c) ss_file_put(o, o->dst, c); else ss_file_put(o, o->src, raw);
    st->packed += c ? c : raw;
}

/* "mem": u32 range count, { u32 addr, u32 len, u32 prot } per readable range of
 * the 4 GB guest space, then batches { u32 page count, u32 page size,
 * u32 addr[count], u32 packed size (0 = raw), data } of non-zero host pages,
 * ended by a u32 0. Pages of a range missing from every batch are zero.
 * A page counts as touched when mincore says resident or paged out (compressed
 * or swapped); reading an untouched page would allocate it. */
extern "C" int ss_file_mem(ss_out* o, ss_mem_stats* st)
{
    memset(st, 0, sizeof *st);
    const uint32_t pg = o->pg;
    uint32_t nr = 0;
    const vm_address_t base = (vm_address_t)vm_base, end = base + 0x100000000ull;
    for (vm_address_t a = base; a < end;) {
        vm_size_t sz = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t ic = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        vm_address_t ra = a;
        if (vm_region_64(mach_task_self(), &ra, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &ic, &obj) != KERN_SUCCESS) break;
        if (ra >= end) break;
        const vm_address_t lo = ra < a ? a : ra, hi = ra + sz > end ? end : ra + sz;
        if (hi > lo && (info.protection & VM_PROT_READ)) {
            if (nr == SS_RANGES_MAX) { o->err = 1; break; }
            o->rs[nr++] = { (uint32_t)(lo - base), (uint32_t)(hi - lo), (uint32_t)info.protection };
            st->mapped += hi - lo;
        }
        a = hi;
    }
    st->ranges = nr;
    ss_file_sec_begin(o, "mem");
    ss_file_put(o, &nr, 4);
    ss_file_put(o, o->rs, (size_t)nr * sizeof(ss_rng));

    uint32_t nb = 0;
    for (uint32_t i = 0; i < nr && !o->err; i++) {
        uint8_t* lo = vm_base + o->rs[i].addr;
        const size_t np = o->rs[i].len / pg;
        if (mincore(lo, (size_t)o->rs[i].len, (char*)o->vec) != 0) { o->err = 1; break; }
        for (size_t k = 0; k < np; k++) {
            if (!(o->vec[k] & (MINCORE_INCORE | MINCORE_PAGED_OUT))) continue;
            const uint64_t* p = (const uint64_t*)(lo + k * pg);
            size_t w = 0;
            while (w < pg / 8 && !p[w]) w++;
            if (w == pg / 8) continue;
            memcpy(o->src + (size_t)nb * pg, p, pg);
            o->addrs[nb++] = o->rs[i].addr + (uint32_t)(k * pg);
            st->nonzero += pg;
            if (nb == SS_BATCH_PAGES) { ss_batch(o, nb, st); nb = 0; }
        }
    }
    ss_batch(o, nb, st);
    const uint32_t zero = 0;
    ss_file_put(o, &zero, 4);
    ss_file_sec_end(o);
    return !o->err;
}

/* --- kernel tables ------------------------------------------------------------------ */

struct ss_blob { const char* name; void* p; size_t n; };

#define B(v) { #v, (void*)&(v), sizeof(v) }
static const ss_blob s_blobs[] = {
    B(g_ppu_threads), B(g_vm_stack_alloc),
    B(g_sys_mutexes), B(g_sys_conds), B(g_cond_id_addr), B(g_sys_rwlocks), B(g_sys_semaphores),
    B(g_sys_event_queues), B(g_sys_event_ports), B(g_sys_event_flags), B(g_sys_timers),
    B(g_sys_mem_allocs), B(g_sys_mem_containers), B(g_sys_mmapper_shared), B(g_sys_mem_bump_ptr),
    B(g_sys_vm_maps), B(g_sys_fs_fds), B(g_sys_fs_dirs), B(g_sys_fs_root),
};
#undef B
enum { SS_NBLOBS = sizeof s_blobs / sizeof s_blobs[0] };

static void ss_tab_name(char* out, size_t n, const char* v)
{
    strlcpy(out, "tab:", n);
    strlcat(out, v, n);
}

extern "C" int ss_file_tables(ss_out* o, uint64_t* bytes)
{
    uint64_t total = 0;
    for (const ss_blob& b : s_blobs) {
        char name[96];
        ss_tab_name(name, sizeof name, b.name);
        ss_file_sec_begin(o, name);
        ss_file_put(o, b.p, b.n);
        ss_file_sec_end(o);
        total += b.n;
    }
    if (bytes) *bytes = total;
    return !o->err;
}

extern "C" int ss_file_sync(ss_out* o)
{
    ss_flush(o);
    return !o->err;
}

/* --- reader ----------------------------------------------------------------------- */

typedef size_t (*ss_dec_fn)(uint8_t*, size_t, const uint8_t*, size_t, void*, int);

enum { SS_SEC_MAX = 128 };
struct ss_sec { const char* name; uint32_t nl; const uint8_t* p; uint64_t n; };

struct ss_in {
    const uint8_t* map;
    size_t map_n;
    ss_sec sec[SS_SEC_MAX];
    int nsec;
    uint8_t uuid[16];
    uint32_t pg;
    ss_dec_fn dec;
    void* scratch;
    uint8_t* out;           /* one decoded batch */
    size_t out_cap;
    uint8_t* seen;          /* bitmap: host pages of 4 GB present in the file */
    unsigned char* vec;     /* mincore */
    ss_rng* rs;             /* range list (aligned copy) */
    const char* why;
    uint32_t dpg[8], ndpg;  /* check: first pages that differ (guest addresses) */
    const char* dtab;       /* check: first table that differs, and its first differing byte */
    uint32_t doff;
};

extern "C" ss_in* ss_file_in_new(void)
{
    ss_in* in = (ss_in*)calloc(1, sizeof(ss_in));
    if (!in) return NULL;
    in->pg = (uint32_t)getpagesize();
    ss_build_uuid(in->uuid);
    void* lib = dlopen("/usr/lib/libcompression.dylib", RTLD_LAZY | RTLD_LOCAL);
    if (lib) {
        in->dec = (ss_dec_fn)dlsym(lib, "compression_decode_buffer");
        ss_scr_fn scr = (ss_scr_fn)dlsym(lib, "compression_decode_scratch_buffer_size");
        if (in->dec && scr) in->scratch = malloc(scr(0x100 /* LZ4 */) + 64);
    }
    in->out_cap = (size_t)SS_BATCH_PAGES * in->pg;
    in->out = (uint8_t*)malloc(in->out_cap);
    in->seen = (uint8_t*)calloc(1, (size_t)(0x100000000ull / in->pg / 8));
    in->vec = (unsigned char*)malloc((size_t)(0x100000000ull / in->pg));
    in->rs = (ss_rng*)malloc(SS_RANGES_MAX * sizeof(ss_rng));
    if (!in->out || !in->seen || !in->vec || !in->rs || !in->scratch) { ss_file_in_free(in); return NULL; }
    return in;
}

extern "C" void ss_file_in_free(ss_in* in)
{
    if (!in) return;
    if (in->map) munmap((void*)in->map, in->map_n);
    free(in->scratch); free(in->out); free(in->seen); free(in->vec); free(in->rs);
    free(in);
}

extern "C" const char* ss_file_why(const ss_in* in) { return in && in->why ? in->why : "out of memory"; }

static int ss_fail(ss_in* in, const char* why) { in->why = why; return 0; }

extern "C" int ss_file_in_map(ss_in* in, const char* path)
{
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return ss_fail(in, "cannot open the file");
    struct stat sb;
    if (fstat(fd, &sb) != 0 || sb.st_size < 36) { close(fd); return ss_fail(in, "file too short"); }
    void* m = mmap(NULL, (size_t)sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return ss_fail(in, "cannot map the file");
    in->map = (const uint8_t*)m; in->map_n = (size_t)sb.st_size;
    const uint8_t* p = in->map;
    uint32_t ver;
    memcpy(&ver, p + 8, 4);
    if (memcmp(p, SS_MAGIC, 8) != 0) return ss_fail(in, "not a save file");
    if (ver != SS_FILE_VERSION) return ss_fail(in, "save file version differs");
    if (memcmp(p + 12, in->uuid, 16) != 0) return ss_fail(in, "save made by another build");
    size_t at = 36;
    in->nsec = 0;
    while (at + 4 <= in->map_n) {
        uint32_t nl; uint64_t n;
        memcpy(&nl, p + at, 4);
        if (nl == 0 || nl > 255 || at + 4 + nl + 8 > in->map_n) return ss_fail(in, "bad section header");
        memcpy(&n, p + at + 4 + nl, 8);
        const size_t d = at + 4 + nl + 8;
        if (n > in->map_n - d) return ss_fail(in, "section runs past the end");
        if (in->nsec == SS_SEC_MAX) return ss_fail(in, "too many sections");
        in->sec[in->nsec++] = { (const char*)(p + at + 4), nl, p + d, n };
        at = d + (size_t)n;
    }
    if (at != in->map_n) return ss_fail(in, "trailing bytes");
    return 1;
}

extern "C" const uint8_t* ss_file_sec(const ss_in* in, const char* name, uint64_t* size)
{
    const size_t nl = strlen(name);
    for (int i = 0; i < in->nsec; i++)
        if (in->sec[i].nl == nl && !memcmp(in->sec[i].name, name, nl)) {
            if (size) *size = in->sec[i].n;
            return in->sec[i].p;
        }
    return NULL;
}

static inline int ss_page_zero(const uint8_t* p, uint32_t pg)
{
    const uint64_t* w = (const uint64_t*)p;
    for (uint32_t i = 0; i < pg / 8; i++) if (w[i]) return 0;
    return 1;
}

extern "C" int ss_file_mem_load(ss_in* in, int apply, ss_mem_stats* st, uint64_t* differ)
{
    memset(st, 0, sizeof *st);
    *differ = 0;
    in->ndpg = 0;
    uint64_t n;
    const uint8_t* p = ss_file_sec(in, "mem", &n);
    if (!p || n < 4) return ss_fail(in, "no memory section");
    const uint8_t* const end = p + n;
    const uint32_t pg = in->pg;
    uint32_t nr;
    memcpy(&nr, p, 4); p += 4;
    if (nr > SS_RANGES_MAX || (uint64_t)nr * sizeof(ss_rng) > (uint64_t)(end - p)) return ss_fail(in, "bad range list");
    ss_rng* const rs = in->rs;
    memcpy(rs, p, (size_t)nr * sizeof(ss_rng));
    p += (size_t)nr * sizeof(ss_rng);
    st->ranges = nr;
    for (uint32_t i = 0; i < nr; i++) {
        if ((rs[i].addr | rs[i].len) % pg || (uint64_t)rs[i].addr + rs[i].len > 0x100000000ull)
            return ss_fail(in, "range not page aligned");
        st->mapped += rs[i].len;
        if (apply && mprotect(vm_base + rs[i].addr, rs[i].len, PROT_READ | PROT_WRITE) != 0)
            return ss_fail(in, "cannot make a range writable");
    }
    memset(in->seen, 0, (size_t)(0x100000000ull / pg / 8));
    for (;;) {
        if (end - p < 4) return ss_fail(in, "memory section cut short");
        uint32_t nb, bpg, cl;
        memcpy(&nb, p, 4); p += 4;
        if (!nb) break;
        if (nb > SS_BATCH_PAGES || end - p < 8 + 4 * (ptrdiff_t)nb) return ss_fail(in, "bad page batch");
        memcpy(&bpg, p, 4); p += 4;
        if (bpg != pg) return ss_fail(in, "saved with another page size");
        const uint8_t* addrs = p; p += 4 * (size_t)nb;
        memcpy(&cl, p, 4); p += 4;
        const size_t raw = (size_t)nb * pg, len = cl ? cl : raw;
        if ((size_t)(end - p) < len) return ss_fail(in, "page batch cut short");
        const uint8_t* src = p;
        if (cl) {
            if (!in->dec || in->dec(in->out, in->out_cap, p, cl, in->scratch, 0x100 /* LZ4 */) != raw)
                return ss_fail(in, "page batch does not unpack");
            src = in->out;
        }
        p += len;
        for (uint32_t k = 0; k < nb; k++) {
            uint32_t a;
            memcpy(&a, addrs + 4 * k, 4);
            if (a % pg) return ss_fail(in, "page not aligned");
            in->seen[(a / pg) >> 3] |= (uint8_t)(1u << ((a / pg) & 7));
            const uint8_t* s = src + (size_t)k * pg;
            if (apply) memcpy(vm_base + a, s, pg);
            else if (memcmp(vm_base + a, s, pg) != 0) {
                if (in->ndpg < 8) in->dpg[in->ndpg++] = a;
                (*differ)++;
            }
            st->nonzero += pg;
        }
        st->packed += len;
    }
    /* Touched pages the file does not have must be zero. */
    for (uint32_t i = 0; i < nr; i++) {
        uint8_t* lo = vm_base + rs[i].addr;
        if (mincore(lo, rs[i].len, (char*)in->vec) != 0) return ss_fail(in, "mincore failed");
        for (uint32_t k = 0; k < rs[i].len / pg; k++) {
            const uint32_t pi = rs[i].addr / pg + k;
            if (in->seen[pi >> 3] & (1u << (pi & 7))) continue;
            if (!(in->vec[k] & (MINCORE_INCORE | MINCORE_PAGED_OUT))) continue;
            if (ss_page_zero(lo + (size_t)k * pg, pg)) continue;
            if (apply) memset(lo + (size_t)k * pg, 0, pg);
            else {
                if (in->ndpg < 8) in->dpg[in->ndpg++] = pi * pg;
                (*differ)++;
            }
        }
        if (apply && mprotect(lo, rs[i].len, (int)(rs[i].prot & (PROT_READ | PROT_WRITE | PROT_EXEC))) != 0)
            return ss_fail(in, "cannot set a range's protection");
    }
    return 1;
}

extern "C" int ss_file_tables_load(ss_in* in, int apply, uint32_t* differ)
{
    *differ = 0;
    in->dtab = NULL;
    for (const ss_blob& b : s_blobs) {
        char name[96];
        ss_tab_name(name, sizeof name, b.name);
        uint64_t n;
        const uint8_t* p = ss_file_sec(in, name, &n);
        if (!p) return ss_fail(in, "a table is missing");
        if (n != b.n) return ss_fail(in, "a table has another size");
        if (apply) memcpy(b.p, p, b.n);
        else if (memcmp(b.p, p, b.n) != 0) {
            if (!in->dtab) {
                const uint8_t* live = (const uint8_t*)b.p;
                uint32_t k = 0;
                while (k < b.n && live[k] == p[k]) k++;
                in->dtab = b.name;
                in->doff = k;
            }
            (*differ)++;
        }
    }
    return 1;
}

extern "C" uint32_t ss_file_diff_pages(const ss_in* in, uint32_t* addr, uint32_t max)
{
    uint32_t n = in->ndpg < max ? in->ndpg : max;
    for (uint32_t i = 0; i < n; i++) addr[i] = in->dpg[i];
    return n;
}

extern "C" const char* ss_file_diff_table(const ss_in* in, uint32_t* off)
{
    if (off) *off = in->doff;
    return in->dtab;
}

#endif /* __APPLE__ */
