/* Disc import (Apple port). See disc_import.h.
 *
 * ISO 9660 reading and PS3 disc decryption follow installer/src/iso.rs.
 * SELF decryption follows RPCS3 (rpcs3/Crypto/unself.cpp), output byte for
 * byte the same as `rpcs3 --decrypt`:
 *
 *   SCE header (0x20) -> metadata info at 0x20 + se_meta (0x40 bytes,
 *   AES-256-CBC with the APP key) gives the metadata key and IV ->
 *   metadata headers after it (AES-128-CTR) give the section list and the
 *   data keys -> each encrypted section is AES-128-CTR with its own key/IV,
 *   then zlib -> written at its program header's file offset. The ELF
 *   header, program headers and section headers are stored in the clear.
 *
 * The app only carries the metadata key and IV of this one EBOOT.BIN
 * (eboot_key.h), never the APP key. */
#if defined(__APPLE__)

#include "disc_import.h"

#include <CommonCrypto/CommonCrypto.h>
#include <copyfile.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <removefile.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/clonefile.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#if defined(__has_include)
#  if __has_include("eboot_key.h")
#    include "eboot_key.h"   /* k_eboot_meta_key[16], k_eboot_meta_iv[16] */
#    define DI_HAVE_KEY 1
#  endif
#endif
#ifndef DI_HAVE_KEY
#  define DI_HAVE_KEY 0
#endif

/* The EBOOT this build was generated from (installer/infamous.toml eboot_sha256)
 * and the retail SELF it comes from. */
static const char k_elf_sha256[]  = "095d851432f3b55179cd725b46838ffedb3e5eb9f73889e52b6f11f3d3f494fd";
static const char k_self_sha256[] = "e47bfc06b2d91ddb464dff821d6262d4375bd13f5e51f792e8b231fcb2bbde01";

#define SECTOR 2048ull

/* ------------------------------------------------------------------------- */
/* small helpers                                                             */
/* ------------------------------------------------------------------------- */

static di_status fail(char* err, size_t n, di_status s, const char* fmt, ...)
{
    if (err && n) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, n, fmt, ap);
        va_end(ap);
    }
    return s;
}

static uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t* p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void to_hex(const uint8_t* d, size_t n, char* out)
{
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = hx[d[i] >> 4]; out[2 * i + 1] = hx[d[i] & 15]; }
    out[2 * n] = 0;
}

static void sha256_hex(const void* d, size_t n, char out[65])
{
    uint8_t h[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_CTX c;
    CC_SHA256_Init(&c);
    while (n) {   /* CC_LONG is 32-bit */
        const CC_LONG k = n > (1u << 30) ? (1u << 30) : (CC_LONG)n;
        CC_SHA256_Update(&c, d, k);
        d = (const uint8_t*)d + k;
        n -= k;
    }
    CC_SHA256_Final(h, &c);
    to_hex(h, sizeof h, out);
}

int di_sha256_file(const char* path, char hex_out[65])
{
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    CC_SHA256_CTX c;
    CC_SHA256_Init(&c);
    enum { B = 1 << 20 };
    uint8_t* b = (uint8_t*)malloc(B);
    ssize_t r;
    while (b && (r = read(fd, b, B)) > 0) CC_SHA256_Update(&c, b, (CC_LONG)r);
    close(fd);
    if (!b) return -1;
    free(b);
    uint8_t h[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(h, &c);
    to_hex(h, sizeof h, hex_out);
    return 0;
}

static int aes_cbc_dec(const uint8_t* key, size_t keylen, const uint8_t iv[16], uint8_t* d, size_t n)
{
    size_t moved = 0;
    return CCCrypt(kCCDecrypt, kCCAlgorithmAES, 0, key, keylen, iv, d, n, d, n, &moved) == kCCSuccess && moved == n ? 0 : -1;
}

/* AES-128-CTR, 128-bit big-endian counter starting at iv (RPCS3 aes_crypt_ctr). */
static int aes_ctr(const uint8_t key[16], const uint8_t iv[16], uint8_t* d, size_t n)
{
    CCCryptorRef c = NULL;
    if (CCCryptorCreateWithMode(kCCEncrypt, kCCModeCTR, kCCAlgorithmAES, ccNoPadding, iv, key, 16,
                                NULL, 0, 0, kCCModeOptionCTR_BE, &c) != kCCSuccess)
        return -1;
    size_t moved = 0;
    const int ok = CCCryptorUpdate(c, d, n, d, n, &moved) == kCCSuccess && moved == n;
    CCCryptorRelease(c);
    return ok ? 0 : -1;
}

static int mkdirs(const char* path)
{
    char tmp[2048];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char* p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    return (mkdir(tmp, 0755) == 0 || errno == EEXIST) ? 0 : -1;
}

static int mkparent(const char* path)
{
    char tmp[2048];
    snprintf(tmp, sizeof tmp, "%s", path);
    char* s = strrchr(tmp, '/');
    if (!s || s == tmp) return 0;
    *s = 0;
    return mkdirs(tmp);
}

static int is_dir(const char* p)  { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static int is_file(const char* p) { struct stat st; return stat(p, &st) == 0 && S_ISREG(st.st_mode); }

static void remove_tree(const char* p)
{
    if (!is_dir(p) && !is_file(p)) return;
    removefile_state_t s = removefile_state_alloc();
    removefile(p, s, REMOVEFILE_RECURSIVE);
    removefile_state_free(s);
}

static uint64_t free_bytes(const char* dir)
{
    struct statfs st;
    if (statfs(dir, &st) != 0) return UINT64_MAX;   /* unknown: let the write fail instead */
    return (uint64_t)st.f_bavail * st.f_bsize;
}

static int read_whole(const char* path, uint8_t** out, size_t* n)
{
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > (1ll << 30)) { close(fd); return -1; }
    uint8_t* b = (uint8_t*)malloc((size_t)st.st_size);
    size_t got = 0;
    while (b && got < (size_t)st.st_size) {
        const ssize_t r = pread(fd, b + got, (size_t)st.st_size - got, (off_t)got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    if (!b || got != (size_t)st.st_size) { free(b); return -1; }
    *out = b;
    *n = got;
    return 0;
}

static int write_whole(const char* path, const uint8_t* d, size_t n)
{
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    size_t put = 0;
    while (put < n) {
        const ssize_t w = write(fd, d + put, n - put);
        if (w <= 0) { close(fd); return -1; }
        put += (size_t)w;
    }
    return close(fd);
}

/* ------------------------------------------------------------------------- */
/* PARAM.SFO                                                                 */
/* ------------------------------------------------------------------------- */

static void sfo_get(const uint8_t* d, size_t n, const char* key, char* out, size_t outn)
{
    out[0] = 0;
    if (n < 20 || memcmp(d, "\0PSF", 4) != 0) return;
    const uint32_t kt = le32(d + 8), dt = le32(d + 12), cnt = le32(d + 16);
    for (uint32_t i = 0; i < cnt && 20 + 16 * (size_t)(i + 1) <= n; i++) {
        const uint8_t* e = d + 20 + 16 * i;
        const size_t ko = kt + le16(e), dl = le32(e + 4), dofs = dt + le32(e + 12);
        if (ko >= n || dofs + dl > n) continue;
        if (strncmp((const char*)d + ko, key, n - ko) != 0) continue;
        size_t k = dl < outn - 1 ? dl : outn - 1;
        memcpy(out, d + dofs, k);
        out[k] = 0;
        return;
    }
}

static int parse_sfo(const uint8_t* d, size_t n, di_info* info)
{
    if (n < 20 || memcmp(d, "\0PSF", 4) != 0) return -1;
    sfo_get(d, n, "TITLE_ID", info->title_id, sizeof info->title_id);
    sfo_get(d, n, "TITLE", info->title, sizeof info->title);
    sfo_get(d, n, "VERSION", info->version, sizeof info->version);
    for (char* p = info->title; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
    return info->title_id[0] ? 0 : -1;
}

/* ------------------------------------------------------------------------- */
/* ISO 9660 + PS3 disc encryption (installer/src/iso.rs)                     */
/* ------------------------------------------------------------------------- */

typedef struct { uint32_t lba; uint64_t len; } extent;

typedef struct {
    char*   path;       /* from the disc root, '/'-separated */
    int     dir;
    extent* ext;
    int     next;
    uint64_t size;
} entry;

typedef struct {
    int      fd;
    uint64_t sectors;
    uint32_t plain[200][2];
    int      nplain;
    int      decrypting;
    uint8_t  key[16];
    entry*   e;
    int      ne, cap;
} iso;

static int iso_plain(const iso* s, uint64_t sec)
{
    if (!s->nplain) return 1;
    for (int i = 0; i < s->nplain; i++)
        if (sec >= s->plain[i][0] && sec <= s->plain[i][1]) return 1;
    return 0;
}

static int iso_has_regions(const iso* s)
{
    return s->nplain && !(s->nplain == 1 && (uint64_t)s->plain[0][1] + 1 >= s->sectors);
}

static int iso_read(iso* s, uint64_t first, uint64_t count, uint8_t* buf)
{
    const size_t want = (size_t)(count * SECTOR);
    size_t got = 0;
    while (got < want) {
        const ssize_t r = pread(s->fd, buf + got, want - got, (off_t)(first * SECTOR + got));
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    if (!s->decrypting) return 0;
    for (uint64_t i = 0; i < count; i++) {
        const uint64_t sec = first + i;
        if (iso_plain(s, sec)) continue;
        uint8_t iv[16] = {0};
        for (int b = 0; b < 8; b++) iv[15 - b] = (uint8_t)(sec >> (8 * b));
        if (aes_cbc_dec(s->key, 16, iv, buf + i * SECTOR, SECTOR) != 0) return -1;
    }
    return 0;
}

static void iso_free(iso* s)
{
    for (int i = 0; i < s->ne; i++) { free(s->e[i].path); free(s->e[i].ext); }
    free(s->e);
    if (s->fd >= 0) close(s->fd);
    memset(s, 0, sizeof *s);
    s->fd = -1;
}

static entry* iso_add(iso* s, const char* path, int dir)
{
    if (s->ne == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 256;
        s->e = (entry*)realloc(s->e, sizeof(entry) * (size_t)s->cap);
    }
    entry* e = &s->e[s->ne++];
    memset(e, 0, sizeof *e);
    e->path = strdup(path);
    e->dir = dir;
    return e;
}

static void ext_push(entry* e, uint32_t lba, uint64_t len)
{
    e->ext = (extent*)realloc(e->ext, sizeof(extent) * (size_t)(e->next + 1));
    e->ext[e->next].lba = lba;
    e->ext[e->next].len = len;
    e->next++;
    e->size += len;
}

/* UCS-2 big-endian -> UTF-8 */
static void ucs2_utf8(const uint8_t* s, size_t n, char* out, size_t outn)
{
    size_t o = 0;
    for (size_t i = 0; i + 1 < n && o + 4 < outn; i += 2) {
        const unsigned c = be16(s + i);
        if (c < 0x80) out[o++] = (char)c;
        else if (c < 0x800) { out[o++] = (char)(0xC0 | c >> 6); out[o++] = (char)(0x80 | (c & 63)); }
        else { out[o++] = (char)(0xE0 | c >> 12); out[o++] = (char)(0x80 | ((c >> 6) & 63)); out[o++] = (char)(0x80 | (c & 63)); }
    }
    out[o] = 0;
}

static int iso_list(iso* s)
{
    uint8_t vd[SECTOR];
    uint32_t root_lba = 0, root_len = 0;
    int have = 0, joliet = 0;
    for (uint64_t sec = 16; sec < 64; sec++) {
        if (iso_read(s, sec, 1, vd) != 0 || memcmp(vd + 1, "CD001", 5) != 0) return -1;
        if (vd[0] == 255) break;
        const uint8_t* rec = vd + 156;
        if (vd[0] == 1 && !have) { root_lba = le32(rec + 2); root_len = le32(rec + 10); have = 1; }
        if (vd[0] == 2 && vd[88] == '%' && vd[89] == '/' && (vd[90] == '@' || vd[90] == 'C' || vd[90] == 'E')) {
            root_lba = le32(rec + 2); root_len = le32(rec + 10); have = 1; joliet = 1;
        }
    }
    if (!have) return -1;

    typedef struct { char* prefix; uint32_t lba, len; int depth; } job;
    int nj = 1, capj = 64;
    job* st = (job*)malloc(sizeof(job) * (size_t)capj);
    st[0].prefix = strdup(""); st[0].lba = root_lba; st[0].len = root_len; st[0].depth = 0;
    int rc = 0;
    while (nj && rc == 0) {
        job j = st[--nj];
        if (j.depth > 32 || j.len > (64u << 20)) { free(j.prefix); rc = -1; break; }
        const uint64_t nsec = (j.len + SECTOR - 1) / SECTOR;
        uint8_t* d = (uint8_t*)malloc((size_t)(nsec * SECTOR));
        if (!d || iso_read(s, j.lba, nsec, d) != 0) { free(d); free(j.prefix); rc = -1; break; }
        const size_t dn = (size_t)(nsec * SECTOR);
        size_t pos = 0;
        entry* pending = NULL;
        while (pos < dn) {
            const size_t rl = d[pos];
            if (rl == 0) { pos = (pos / SECTOR + 1) * SECTOR; continue; }
            if (pos + rl > dn || rl < 34) break;
            const uint8_t* r = d + pos;
            pos += rl;
            const size_t nl = r[32];
            if (33 + nl > rl) continue;
            const uint8_t* raw = r + 33;
            if (nl == 1 && (raw[0] == 0 || raw[0] == 1)) continue;
            char name[512];
            if (joliet) ucs2_utf8(raw, nl, name, sizeof name);
            else { size_t k = nl < sizeof name - 1 ? nl : sizeof name - 1; memcpy(name, raw, k); name[k] = 0; }
            char* semi = strchr(name, ';');
            if (semi) *semi = 0;
            size_t L = strlen(name);
            if (L && name[L - 1] == '.') name[--L] = 0;
            if (!L || strchr(name, '/') || strchr(name, '\\') || !strcmp(name, "..")) continue;
            const uint32_t elba = le32(r + 2), elen = le32(r + 10);
            const uint8_t flags = r[25];
            char full[2048];
            if (j.prefix[0]) snprintf(full, sizeof full, "%s/%s", j.prefix, name);
            else snprintf(full, sizeof full, "%s", name);
            if (flags & 2) {
                iso_add(s, full, 1);
                if (nj == capj) { capj *= 2; st = (job*)realloc(st, sizeof(job) * (size_t)capj); }
                st[nj].prefix = strdup(full); st[nj].lba = elba; st[nj].len = elen; st[nj].depth = j.depth + 1;
                nj++;
                continue;
            }
            /* Multi-extent files: records with flag 0x80 continue in the next one. */
            if (pending && strcmp(pending->path, full) == 0) {
                ext_push(pending, elba, elen);
            } else {
                pending = iso_add(s, full, 0);
                ext_push(pending, elba, elen);
            }
            if (!(flags & 0x80)) pending = NULL;
        }
        free(d);
        free(j.prefix);
    }
    for (int i = 0; i < nj; i++) free(st[i].prefix);
    free(st);
    return rc;
}

static entry* iso_find(iso* s, const char* path)
{
    for (int i = 0; i < s->ne; i++)
        if (!s->e[i].dir && strcasecmp(s->e[i].path, path) == 0) return &s->e[i];
    return NULL;
}

/* Read a whole (small) file from the image. */
static uint8_t* iso_file(iso* s, const entry* e, size_t* n)
{
    if (e->size > (64u << 20)) return NULL;
    uint8_t* out = (uint8_t*)malloc((size_t)e->size + SECTOR);
    size_t o = 0;
    for (int i = 0; out && i < e->next; i++) {
        const uint64_t nsec = (e->ext[i].len + SECTOR - 1) / SECTOR;
        uint8_t* b = (uint8_t*)malloc((size_t)(nsec * SECTOR) + 1);
        if (!b || iso_read(s, e->ext[i].lba, nsec, b) != 0) { free(b); free(out); return NULL; }
        memcpy(out + o, b, (size_t)e->ext[i].len);
        o += (size_t)e->ext[i].len;
        free(b);
    }
    *n = o;
    return out;
}

static const char* signature(const char* path)
{
    const size_t L = strlen(path);
#define ENDS(x) (L >= sizeof(x) - 1 && strcasecmp(path + L - (sizeof(x) - 1), x) == 0)
    if (ENDS(".sfo")) return "\0PSF";
    if (ENDS("eboot.bin") || ENDS(".self") || ENDS(".sprx")) return "SCE\0";
    if (ENDS(".png")) return "\x89PNG";
    if (ENDS(".elf") || ENDS(".prx")) return "\x7f" "ELF";
#undef ENDS
    if (strcasestr(path, ".psarc")) return "PSAR";
    return NULL;
}

/* 1 = the regions really are encrypted, 0 = stored in the clear, -1 = can't tell. */
static int iso_detect(iso* s, int decrypt)
{
    const int saved = s->decrypting;
    s->decrypting = decrypt;
    int verdict = -1;
    uint8_t b[SECTOR];
    for (int i = 0; i < s->ne; i++) {
        const entry* e = &s->e[i];
        if (e->dir || e->size < 4 || !e->next || iso_plain(s, e->ext[0].lba)) continue;
        const char* sig = signature(e->path);
        if (!sig) continue;
        if (iso_read(s, e->ext[0].lba, 1, b) != 0) { verdict = -1; break; }
        verdict = memcmp(b, sig, 4) == 0;
        if (!decrypt) verdict = !verdict;
        break;
    }
    s->decrypting = saved;
    return verdict;
}

static int parse_key_text(const uint8_t* d, size_t n, uint8_t key[16])
{
    char hex[33];
    size_t h = 0;
    for (size_t i = 0; i < n; i++) {
        const char c = (char)d[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (h == 32 || !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            h = 99;
            break;
        }
        hex[h++] = c;
    }
    if (h == 32) {
        for (int i = 0; i < 16; i++) { char t[3] = { hex[2 * i], hex[2 * i + 1], 0 }; key[i] = (uint8_t)strtoul(t, NULL, 16); }
        return 0;
    }
    if (n == 16) { memcpy(key, d, 16); return 0; }
    return -1;
}

static int find_dkey(const char* isop, char* out, size_t n)
{
    char base[2048];
    snprintf(base, sizeof base, "%s", isop);
    char* dot = strrchr(base, '.');
    char* sl = strrchr(base, '/');
    if (dot && (!sl || dot > sl)) *dot = 0;
    static const char* ext[] = { ".dkey", ".key", ".DKEY", ".KEY" };
    for (int i = 0; i < 4; i++) {
        snprintf(out, n, "%s%s", base, ext[i]);
        if (is_file(out)) return 0;
    }
    return -1;
}

/* Open an image and get it ready to read (iso.rs open_ready). */
static di_status iso_open(const char* path, const char* dkey, iso* s, di_info* info, char* err, size_t en)
{
    memset(s, 0, sizeof *s);
    s->fd = open(path, O_RDONLY);
    if (s->fd < 0) return fail(err, en, DI_ERR_OPEN, "Can't open the disc image (%s).", strerror(errno));
    struct stat st;
    fstat(s->fd, &st);
    s->sectors = (uint64_t)st.st_size / SECTOR;
    if (s->sectors < 18) return fail(err, en, DI_ERR_NOT_GAME, "This file is too small to be a disc image.");
    uint8_t s0[SECTOR];
    if (iso_read(s, 0, 1, s0) != 0) return fail(err, en, DI_ERR_OPEN, "Can't read the disc image.");
    const uint32_t n = be32(s0);
    if (n >= 1 && n <= 200 && 8 + 8 * n <= SECTOR) {
        for (uint32_t k = 0; k < n; k++) {
            const uint32_t a = be32(s0 + 8 + 8 * k), b = be32(s0 + 12 + 8 * k);
            if (a > b || b >= s->sectors + 1) { s->nplain = 0; break; }
            s->plain[s->nplain][0] = a;
            s->plain[s->nplain][1] = b;
            s->nplain++;
        }
        if (s->nplain && s->plain[0][0] != 0) s->nplain = 0;
        for (int k = 1; k < s->nplain; k++)
            if (s->plain[k][0] <= s->plain[k - 1][1]) { s->nplain = 0; break; }
    }
    if (iso_list(s) != 0)
        return fail(err, en, DI_ERR_NOT_GAME, "This is not a readable disc image (ISO 9660).");

    info->is_iso = 1;
    info->encrypted = iso_has_regions(s) ? iso_detect(s, 0) == 1 : 0;
    if (info->encrypted) {
        char kp[2048];
        uint8_t* kd = NULL;
        size_t kn = 0;
        if (!dkey && find_dkey(path, kp, sizeof kp) == 0) dkey = kp;
        if (!dkey)
            return fail(err, en, DI_ERR_NEED_KEY,
                        "This disc image is encrypted (a 1:1 copy). Choose its disc key file (.dkey) too.");
        if (read_whole(dkey, &kd, &kn) != 0 || parse_key_text(kd, kn, s->key) != 0) {
            free(kd);
            return fail(err, en, DI_ERR_BAD_KEY, "The disc key file is not a disc key (32 hex digits).");
        }
        free(kd);
        if (iso_detect(s, 1) == 0)
            return fail(err, en, DI_ERR_BAD_KEY, "The disc key does not fit this disc image.");
        s->decrypting = 1;
        info->have_key = 1;
    }
    for (int i = 0; i < s->ne; i++)
        if (!s->e[i].dir && strncasecmp(s->e[i].path, "PS3_UPDATE/", 11) != 0) info->bytes += s->e[i].size;
    return DI_OK;
}

/* ------------------------------------------------------------------------- */
/* folder sources                                                            */
/* ------------------------------------------------------------------------- */

/* The folder that holds PS3_GAME: src itself, its parent if src is PS3_GAME,
 * or one level down (a folder holding the game folder). */
static int folder_root(const char* src, char* root, size_t n)
{
    char p[2048];
    snprintf(p, sizeof p, "%s/PS3_GAME/USRDIR", src);
    if (is_dir(p)) { snprintf(root, n, "%s", src); return 0; }
    snprintf(p, sizeof p, "%s/USRDIR", src);
    const char* last = strrchr(src, '/');
    if (is_dir(p) && last && strcmp(last + 1, "PS3_GAME") == 0) {
        snprintf(root, n, "%.*s", (int)(last - src), src);
        return 0;
    }
    char* const paths[] = { (char*)src, NULL };
    FTS* f = fts_open(paths, FTS_PHYSICAL | FTS_NOCHDIR, NULL);
    if (!f) return -1;
    FTSENT* e;
    int found = -1;
    while (found && (e = fts_read(f))) {
        if (e->fts_info != FTS_D) continue;
        if (e->fts_level > 1) { fts_set(f, e, FTS_SKIP); continue; }
        if (e->fts_level == 1) {
            snprintf(p, sizeof p, "%s/PS3_GAME/USRDIR", e->fts_path);
            if (is_dir(p)) { snprintf(root, n, "%s", e->fts_path); found = 0; }
        }
    }
    fts_close(f);
    return found;
}

static int skip_name(const char* name)
{
    return !strcmp(name, ".DS_Store") || !strncmp(name, "._", 2);
}

/* What a folder dump contributes: the disc's own top level, nothing the
 * player or another tool put next to it (and no system update). */
static int disc_top(const char* name)
{
    return !strcmp(name, "PS3_GAME") || !strcmp(name, "PS3_DISC.SFB");
}

static uint64_t folder_bytes(const char* root)
{
    char* const paths[] = { (char*)root, NULL };
    FTS* f = fts_open(paths, FTS_PHYSICAL | FTS_NOCHDIR, NULL);
    if (!f) return 0;
    uint64_t total = 0;
    FTSENT* e;
    while ((e = fts_read(f))) {
        if (e->fts_level == 1 && !disc_top(e->fts_name)) { fts_set(f, e, FTS_SKIP); continue; }
        if (e->fts_info == FTS_F && !skip_name(e->fts_name)) total += (uint64_t)e->fts_statp->st_size;
    }
    fts_close(f);
    return total;
}

/* ------------------------------------------------------------------------- */
/* SELF -> ELF (RPCS3 unself.cpp)                                            */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint64_t offset, size;
    uint32_t type, program_idx, hashed, sha1_idx, encrypted, key_idx, iv_idx, compressed;
} meta_section;

static di_status self_check(const uint8_t* d, size_t n, char* err, size_t en)
{
    if (n < 0x100 || memcmp(d, "SCE\0", 4) != 0)
        return fail(err, en, DI_ERR_DECRYPT, "EBOOT.BIN is not a SELF file.");
    if (be16(d + 10) != 1)
        return fail(err, en, DI_ERR_DECRYPT, "EBOOT.BIN is not a SELF (type %u).", be16(d + 10));
    if (be16(d + 8) & 0x8000)
        return fail(err, en, DI_ERR_DECRYPT, "EBOOT.BIN is a debug SELF; not supported.");
    const uint64_t hsize = be64(d + 16);
    const uint32_t meta = be32(d + 12);
    if (hsize > n || (uint64_t)meta + 0x20 + 0x40 + 0x20 > hsize)
        return fail(err, en, DI_ERR_DECRYPT, "EBOOT.BIN has a broken header.");
    return DI_OK;
}

di_status di_self_meta_key(const char* self_path, const uint8_t erk[32], const uint8_t riv[16],
                           uint8_t meta_key[16], uint8_t meta_iv[16], char* err, size_t en)
{
    uint8_t* d = NULL;
    size_t n = 0;
    if (read_whole(self_path, &d, &n) != 0) return fail(err, en, DI_ERR_OPEN, "Can't read %s.", self_path);
    di_status s = self_check(d, n, err, en);
    if (s == DI_OK) {
        uint8_t info[0x40];
        memcpy(info, d + 0x20 + be32(d + 12), sizeof info);
        if (aes_cbc_dec(erk, 32, riv, info, sizeof info) != 0 || info[16] != 0 || info[48] != 0) {
            s = fail(err, en, DI_ERR_DECRYPT, "The APP key does not open this SELF's metadata.");
        } else {
            memcpy(meta_key, info, 16);
            memcpy(meta_iv, info + 32, 16);
        }
    }
    free(d);
    return s;
}

di_status di_self_to_elf(const char* self_path, const char* elf_path,
                         const uint8_t* meta_key, const uint8_t* meta_iv, char* err, size_t en)
{
#if DI_HAVE_KEY
    if (!meta_key) { meta_key = k_eboot_meta_key; meta_iv = k_eboot_meta_iv; }
#endif
    if (!meta_key || !meta_iv)
        return fail(err, en, DI_ERR_NO_EBOOT_KEY, "This build has no key for EBOOT.BIN.");
    uint8_t* d = NULL;
    size_t n = 0;
    if (read_whole(self_path, &d, &n) != 0) return fail(err, en, DI_ERR_OPEN, "Can't read EBOOT.BIN.");
    uint8_t* out = NULL;
    uint8_t* hdrs = NULL;
    meta_section* ms = NULL;
    di_status s = self_check(d, n, err, en);
    if (s != DI_OK) goto done;
    {
        const uint64_t hsize   = be64(d + 16);
        const uint64_t mh_off  = 0x20 + (uint64_t)be32(d + 12) + 0x40;
        const uint64_t elf_off = be64(d + 0x30);
        const uint64_t ph_off  = be64(d + 0x38);
        const uint64_t sh_off  = be64(d + 0x40);
        if (elf_off + 64 > n || memcmp(d + elf_off, "\x7f" "ELF", 4) != 0 || d[elf_off + 4] != 2) {
            s = fail(err, en, DI_ERR_DECRYPT, "EBOOT.BIN does not hold a 64-bit ELF.");
            goto done;
        }
        const uint8_t* eh = d + elf_off;
        const uint64_t e_phoff = be64(eh + 32), e_shoff = be64(eh + 40);
        const uint16_t phnum = be16(eh + 56), shnum = be16(eh + 60);
        if (ph_off + 56ull * phnum > n || (sh_off && sh_off + 64ull * shnum > n)) {
            s = fail(err, en, DI_ERR_DECRYPT, "EBOOT.BIN has broken program headers.");
            goto done;
        }

        /* metadata header + section headers + data keys */
        const size_t hn = (size_t)(hsize - mh_off);
        hdrs = (uint8_t*)malloc(hn);
        memcpy(hdrs, d + mh_off, hn);
        if (aes_ctr(meta_key, meta_iv, hdrs, hn) != 0) { s = fail(err, en, DI_ERR_DECRYPT, "AES failed."); goto done; }
        const uint32_t nsec = be32(hdrs + 12), nkeys = be32(hdrs + 16);
        if (nsec == 0 || nsec > 256 || nkeys > 1024 || 0x20 + 0x30ull * nsec + 16ull * nkeys > hn) {
            s = fail(err, en, DI_ERR_DECRYPT, "The key does not fit this EBOOT.BIN.");
            goto done;
        }
        ms = (meta_section*)calloc(nsec, sizeof *ms);
        for (uint32_t i = 0; i < nsec; i++) {
            const uint8_t* h = hdrs + 0x20 + 0x30 * i;
            ms[i].offset = be64(h);           ms[i].size = be64(h + 8);
            ms[i].type = be32(h + 16);        ms[i].program_idx = be32(h + 20);
            ms[i].hashed = be32(h + 24);      ms[i].sha1_idx = be32(h + 28);
            ms[i].encrypted = be32(h + 32);   ms[i].key_idx = be32(h + 36);
            ms[i].iv_idx = be32(h + 40);      ms[i].compressed = be32(h + 44);
            if (ms[i].offset + ms[i].size > n || (ms[i].type == 2 && ms[i].program_idx >= phnum)) {
                s = fail(err, en, DI_ERR_DECRYPT, "The key does not fit this EBOOT.BIN.");
                goto done;
            }
        }
        const uint8_t* keys = hdrs + 0x20 + 0x30 * nsec;

        /* output size: everything WriteElf touches */
        uint64_t size = 64;
        if (e_phoff + 56ull * phnum > size) size = e_phoff + 56ull * phnum;
        if (sh_off && e_shoff + 64ull * shnum > size) size = e_shoff + 64ull * shnum;
        for (uint32_t i = 0; i < nsec; i++) {
            if (ms[i].type != 2) continue;
            const uint8_t* ph = d + ph_off + 56 * ms[i].program_idx;
            const uint64_t end = be64(ph + 8) + (ms[i].compressed == 2 ? be64(ph + 32) : ms[i].size);
            if (end > size) size = end;
        }
        if (size > (1ull << 30)) { s = fail(err, en, DI_ERR_DECRYPT, "EBOOT.BIN is too large."); goto done; }
        out = (uint8_t*)calloc(1, (size_t)size);

        memcpy(out, eh, 64);   /* WriteEhdr */
        for (uint32_t i = 0; i < nsec; i++) {
            if (ms[i].type != 2) continue;
            uint8_t* sec = d + ms[i].offset;
            if (ms[i].encrypted == 3) {
                if (ms[i].key_idx >= nkeys || ms[i].iv_idx >= nkeys) {
                    s = fail(err, en, DI_ERR_DECRYPT, "EBOOT.BIN section %u has no key.", i);
                    goto done;
                }
                if (aes_ctr(keys + 16 * ms[i].key_idx, keys + 16 * ms[i].iv_idx, sec, (size_t)ms[i].size) != 0) {
                    s = fail(err, en, DI_ERR_DECRYPT, "AES failed.");
                    goto done;
                }
            }
            const uint8_t* ph = d + ph_off + 56 * ms[i].program_idx;
            const uint64_t p_offset = be64(ph + 8), p_filesz = be64(ph + 32);
            if (ms[i].compressed == 2) {
                uLongf got = (uLongf)p_filesz;
                const int zr = uncompress(out + p_offset, &got, sec, (uLong)ms[i].size);
                if (zr != Z_OK && zr != Z_BUF_ERROR) {
                    s = fail(err, en, DI_ERR_DECRYPT, "EBOOT.BIN section %u did not unpack (zlib %d): wrong key?", i, zr);
                    goto done;
                }
            } else {
                memcpy(out + p_offset, sec, (size_t)ms[i].size);
            }
        }
        if (sh_off) memcpy(out + e_shoff, d + sh_off, 64ull * shnum);   /* section headers */
        memcpy(out + e_phoff, d + ph_off, 56ull * phnum);              /* program headers, last */

        if (write_whole(elf_path, out, (size_t)size) != 0)
            s = fail(err, en, DI_ERR_WRITE, "Can't write EBOOT.elf (%s).", strerror(errno));
    }
done:
    free(ms);
    free(hdrs);
    free(out);
    free(d);
    return s;
}

int di_have_eboot_key(void) { return DI_HAVE_KEY; }

/* ------------------------------------------------------------------------- */
/* probe / import                                                            */
/* ------------------------------------------------------------------------- */

static di_status check_title(const di_info* info, char* err, size_t en)
{
    if (strcmp(info->title_id, DI_TITLE_ID) != 0)
        return fail(err, en, DI_ERR_WRONG_GAME,
                    "This is \"%s\" (%s). This app needs inFamous, the European disc (" DI_TITLE_ID ").",
                    info->title, info->title_id);
    return DI_OK;
}

static di_status check_self_hash(const char* hex, char* err, size_t en)
{
    if (strcmp(hex, k_self_sha256) != 0)
        return fail(err, en, DI_ERR_WRONG_GAME,
                    "This inFamous disc has a different EBOOT.BIN from the one this app was made for. "
                    "It needs the European disc (" DI_TITLE_ID "), version 01.01, without updates.");
    return DI_OK;
}

di_status di_probe(const char* src, const char* dkey, di_info* info, char* err, size_t en)
{
    memset(info, 0, sizeof *info);
    if (err && en) err[0] = 0;
    if (is_dir(src)) {
        char root[2048], p[2300];
        if (folder_root(src, root, sizeof root) != 0)
            return fail(err, en, DI_ERR_NOT_GAME, "This folder has no PS3_GAME folder in it.");
        snprintf(p, sizeof p, "%s/PS3_GAME/PARAM.SFO", root);
        uint8_t* d = NULL;
        size_t n = 0;
        if (read_whole(p, &d, &n) != 0 || parse_sfo(d, n, info) != 0) {
            free(d);
            return fail(err, en, DI_ERR_NOT_GAME, "This folder has no readable PS3_GAME/PARAM.SFO.");
        }
        free(d);
        di_status s = check_title(info, err, en);
        if (s != DI_OK) return s;
        char hex[65];
        snprintf(p, sizeof p, "%s/PS3_GAME/USRDIR/EBOOT.elf", root);
        if (di_sha256_file(p, hex) == 0 && strcmp(hex, k_elf_sha256) == 0) {
            info->bytes = folder_bytes(root);
            return DI_OK;
        }
        snprintf(p, sizeof p, "%s/PS3_GAME/USRDIR/EBOOT.BIN", root);
        if (di_sha256_file(p, hex) != 0)
            return fail(err, en, DI_ERR_NOT_GAME, "This folder has no PS3_GAME/USRDIR/EBOOT.BIN.");
        if ((s = check_self_hash(hex, err, en)) != DI_OK) return s;
        if (!DI_HAVE_KEY)
            return fail(err, en, DI_ERR_NO_EBOOT_KEY,
                        "This build can't decrypt EBOOT.BIN. Use a folder that already has EBOOT.elf.");
        info->bytes = folder_bytes(root) + (8u << 20);
        return DI_OK;
    }

    iso s;
    di_status st = iso_open(src, dkey, &s, info, err, en);
    if (st == DI_OK) {
        entry* e = iso_find(&s, "PS3_GAME/PARAM.SFO");
        size_t n = 0;
        uint8_t* d = e ? iso_file(&s, e, &n) : NULL;
        if (!d || parse_sfo(d, n, info) != 0) st = fail(err, en, DI_ERR_NOT_GAME, "This is not a PS3 game disc (no PS3_GAME/PARAM.SFO).");
        free(d);
    }
    if (st == DI_OK) st = check_title(info, err, en);
    if (st == DI_OK) {
        entry* e = iso_find(&s, "PS3_GAME/USRDIR/EBOOT.BIN");
        size_t n = 0;
        uint8_t* d = e ? iso_file(&s, e, &n) : NULL;
        char hex[65];
        if (!d) st = fail(err, en, DI_ERR_NOT_GAME, "The disc has no PS3_GAME/USRDIR/EBOOT.BIN.");
        else { sha256_hex(d, n, hex); st = check_self_hash(hex, err, en); }
        free(d);
    }
    if (st == DI_OK && !DI_HAVE_KEY)
        st = fail(err, en, DI_ERR_NO_EBOOT_KEY, "This build can't decrypt EBOOT.BIN, so it can't import a disc image.");
    if (st == DI_OK) info->bytes += 8u << 20;
    iso_free(&s);
    return st;
}

typedef struct {
    const di_callbacks* cb;
    uint64_t done, total;
    double   last;
} prog;

static int tick(prog* p, uint64_t add, const char* what)
{
    p->done += add;
    if (p->cb && p->cb->cancel && *p->cb->cancel) return -1;
    double f = p->total ? (double)p->done / (double)p->total : 0;
    if (f > 0.999) f = 0.999;
    if (p->cb && p->cb->progress && (f - p->last >= 0.002 || !add)) {
        p->last = f;
        p->cb->progress(p->cb->ctx, f, what);
    }
    return 0;
}

static di_status copy_file(const char* from, const char* to, prog* p, const char* label, char* err, size_t en)
{
    struct stat st;
    if (stat(from, &st) != 0) return fail(err, en, DI_ERR_OPEN, "Can't read %s.", label);
    unlink(to);
    if (clonefile(from, to, 0) == 0)   /* same APFS volume: instant */
        return tick(p, (uint64_t)st.st_size, label) ? DI_ERR_CANCELLED : DI_OK;
    const int in = open(from, O_RDONLY);
    if (in < 0) return fail(err, en, DI_ERR_OPEN, "Can't read %s (%s).", label, strerror(errno));
    const int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { close(in); return fail(err, en, DI_ERR_WRITE, "Can't write %s (%s).", label, strerror(errno)); }
    enum { B = 4 << 20 };
    uint8_t* b = (uint8_t*)malloc(B);
    di_status s = DI_OK;
    for (;;) {
        const ssize_t r = read(in, b, B);
        if (r < 0) { s = fail(err, en, DI_ERR_OPEN, "Can't read %s (%s).", label, strerror(errno)); break; }
        if (r == 0) break;
        if (write(out, b, (size_t)r) != r) {
            s = fail(err, en, errno == ENOSPC ? DI_ERR_SPACE : DI_ERR_WRITE, "Can't write %s (%s).", label, strerror(errno));
            break;
        }
        if (tick(p, (uint64_t)r, label)) { s = DI_ERR_CANCELLED; break; }
    }
    free(b);
    close(in);
    if (close(out) != 0 && s == DI_OK) s = fail(err, en, DI_ERR_WRITE, "Can't write %s.", label);
    return s;
}

static di_status copy_folder(const char* root, const char* dst, prog* p, char* err, size_t en)
{
    char* const paths[] = { (char*)root, NULL };
    FTS* f = fts_open(paths, FTS_PHYSICAL | FTS_NOCHDIR, NULL);
    if (!f) return fail(err, en, DI_ERR_OPEN, "Can't read the game folder.");
    const size_t rl = strlen(root);
    di_status s = DI_OK;
    FTSENT* e;
    while (s == DI_OK && (e = fts_read(f))) {
        if (e->fts_level == 0) continue;
        if (e->fts_level == 1 && !disc_top(e->fts_name)) { if (e->fts_info == FTS_D) fts_set(f, e, FTS_SKIP); continue; }
        if (skip_name(e->fts_name)) continue;
        char to[4096];
        snprintf(to, sizeof to, "%s%s", dst, e->fts_path + rl);
        if (e->fts_info == FTS_D) {
            if (mkdirs(to) != 0) s = fail(err, en, DI_ERR_WRITE, "Can't create %s (%s).", to, strerror(errno));
        } else if (e->fts_info == FTS_F) {
            s = copy_file(e->fts_path, to, p, e->fts_path + rl + 1, err, en);
        }
    }
    fts_close(f);
    return s;
}

static di_status extract_iso(iso* s, const char* dst, prog* p, char* err, size_t en)
{
    enum { CHUNK = 512 };   /* sectors, 1 MiB */
    uint8_t* b = (uint8_t*)malloc(CHUNK * SECTOR);
    di_status st = DI_OK;
    for (int i = 0; st == DI_OK && i < s->ne; i++) {
        const entry* e = &s->e[i];
        if (!strncasecmp(e->path, "PS3_UPDATE", 10) && (e->path[10] == '/' || !e->path[10])) continue;
        char to[4096];
        snprintf(to, sizeof to, "%s/%s", dst, e->path);
        if (e->dir) {
            if (mkdirs(to) != 0) st = fail(err, en, DI_ERR_WRITE, "Can't create %s (%s).", e->path, strerror(errno));
            continue;
        }
        if (mkparent(to) != 0) { st = fail(err, en, DI_ERR_WRITE, "Can't create the folder for %s.", e->path); break; }
        const int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out < 0) { st = fail(err, en, DI_ERR_WRITE, "Can't write %s (%s).", e->path, strerror(errno)); break; }
        for (int x = 0; st == DI_OK && x < e->next; x++) {
            uint64_t left = e->ext[x].len, sec = e->ext[x].lba;
            while (left) {
                uint64_t ns = (left + SECTOR - 1) / SECTOR;
                if (ns > CHUNK) ns = CHUNK;
                if (iso_read(s, sec, ns, b) != 0) {
                    st = fail(err, en, DI_ERR_OPEN, "Can't read %s from the disc image (is it complete?).", e->path);
                    break;
                }
                const size_t take = (size_t)(left < ns * SECTOR ? left : ns * SECTOR);
                if (write(out, b, take) != (ssize_t)take) {
                    st = fail(err, en, errno == ENOSPC ? DI_ERR_SPACE : DI_ERR_WRITE, "Can't write %s (%s).", e->path, strerror(errno));
                    break;
                }
                left -= take;
                sec += ns;
                if (tick(p, take, e->path)) { st = DI_ERR_CANCELLED; break; }
            }
        }
        if (close(out) != 0 && st == DI_OK) st = fail(err, en, DI_ERR_WRITE, "Can't write %s.", e->path);
    }
    free(b);
    return st;
}

di_status di_import(const char* src, const char* dkey, const char* dest_root,
                    const di_callbacks* cb, char* err, size_t en)
{
    di_info info;
    di_status st = di_probe(src, dkey, &info, err, en);
    if (st != DI_OK) return st;

    char tmp[2048], fin[2048];
    snprintf(tmp, sizeof tmp, "%s/" DI_TITLE_ID ".importing", dest_root);
    snprintf(fin, sizeof fin, "%s/" DI_TITLE_ID, dest_root);
    if (mkdirs(dest_root) != 0) return fail(err, en, DI_ERR_WRITE, "Can't create %s (%s).", dest_root, strerror(errno));
    remove_tree(tmp);

    const uint64_t need = info.bytes + (256u << 20);
    const uint64_t have = free_bytes(dest_root);
    if (have < need)
        return fail(err, en, DI_ERR_SPACE, "Not enough free space: the game needs %.1f GB, %.1f GB is free.",
                    need / 1e9, have / 1e9);
    if (mkdirs(tmp) != 0) return fail(err, en, DI_ERR_WRITE, "Can't create %s (%s).", tmp, strerror(errno));

    prog p = { cb, 0, info.bytes, -1 };
    tick(&p, 0, "Starting");
    if (info.is_iso) {
        iso s;
        di_info again;
        st = iso_open(src, dkey, &s, &again, err, en);
        if (st == DI_OK) st = extract_iso(&s, tmp, &p, err, en);
        iso_free(&s);
    } else {
        char root[2048];
        folder_root(src, root, sizeof root);
        st = copy_folder(root, tmp, &p, err, en);
    }

    char bin[2300], elf[2300], hex[65];
    snprintf(bin, sizeof bin, "%s/PS3_GAME/USRDIR/EBOOT.BIN", tmp);
    snprintf(elf, sizeof elf, "%s/PS3_GAME/USRDIR/EBOOT.elf", tmp);
    if (st == DI_OK) {
        const int elf_ok = di_sha256_file(elf, hex) == 0 && strcmp(hex, k_elf_sha256) == 0;
        if (!elf_ok) {
            tick(&p, 0, "Decrypting EBOOT.BIN");
            st = di_self_to_elf(bin, elf, NULL, NULL, err, en);
            if (st == DI_OK) {
                tick(&p, 8u << 20, "Checking EBOOT.elf");
                if (di_sha256_file(elf, hex) != 0 || strcmp(hex, k_elf_sha256) != 0)
                    st = fail(err, en, DI_ERR_VERIFY, "EBOOT.BIN decrypted, but the result is not the expected file.");
            }
        }
    }
    if (st == DI_OK) {
        remove_tree(fin);   /* an earlier, unfinished or broken copy */
        if (rename(tmp, fin) != 0) st = fail(err, en, DI_ERR_WRITE, "Can't finish the import (%s).", strerror(errno));
    }
    if (st != DI_OK) {
        if (st == DI_ERR_CANCELLED) fail(err, en, st, "Import stopped.");
        remove_tree(tmp);
        return st;
    }
    if (cb && cb->progress) cb->progress(cb->ctx, 1.0, "Done");
    return DI_OK;
}

int di_installed(const char* dest_root, char* elf_out, size_t n)
{
    char elf[2300], hex[65];
    snprintf(elf, sizeof elf, "%s/" DI_TITLE_ID "/PS3_GAME/USRDIR/EBOOT.elf", dest_root);
    if (di_sha256_file(elf, hex) != 0 || strcmp(hex, k_elf_sha256) != 0) return 0;
    if (elf_out && n) snprintf(elf_out, n, "%s", elf);
    return 1;
}

const char* di_status_name(di_status s)
{
    switch (s) {
    case DI_OK: return "ok";
    case DI_ERR_OPEN: return "open";
    case DI_ERR_NOT_GAME: return "not-game";
    case DI_ERR_WRONG_GAME: return "wrong-game";
    case DI_ERR_NEED_KEY: return "need-key";
    case DI_ERR_BAD_KEY: return "bad-key";
    case DI_ERR_SPACE: return "space";
    case DI_ERR_WRITE: return "write";
    case DI_ERR_NO_EBOOT_KEY: return "no-eboot-key";
    case DI_ERR_DECRYPT: return "decrypt";
    case DI_ERR_VERIFY: return "verify";
    case DI_ERR_CANCELLED: return "cancelled";
    }
    return "?";
}

#endif /* __APPLE__ */
