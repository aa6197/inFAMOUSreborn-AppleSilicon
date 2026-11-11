/* test_spu_helpers_v130_neon.c -- arm64 only: the NEON v130 block of spu_helpers_vec.h (float compares,
 * conversions, fi, quadword bit shifts) against the scalar references in spu_helpers.h, plus the arm64
 * float core of spu_helpers.h (spu__xfq two-min clamp, NEON fma/fms/fnms) against scalar forms, and the
 * TBL frest / frsqest against the LUT references over all 2^32 inputs.
 * Every FPCR rounding mode, FZ on and off. g_spu_xfloat = 1 (the vector clamp is compile-time, as on x86).
 * Exit status 0 = all equal. Prints "N checks, M differ"; M must be 0.
 *
 * Build from ps3recomp/:
 *   cc -O2 -std=gnu17 -fno-strict-aliasing -w -I runtime/spu -I include -I runtime/ppu -I runtime/memory \
 *      -I runtime/syscalls -I runtime/platform runtime/spu/tests/test_spu_helpers_v130_neon.c -o test_v130_neon
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#if !defined(__aarch64__)
int main(void) { printf("arm64 only, skipped\n"); return 0; }
#else
#include "spu_helpers.h"
int g_spu_ls_watch_n = 0;
void spu_ls_watch_slow(uint32_t lsa, int w, const uint8_t* p, uint32_t pc, uint32_t lr) { (void)lsa; (void)w; (void)p; (void)pc; (void)lr; }
#define SPU_HELPERS_V130_NO_REDIRECT
#include "spu_helpers_vec.h"
#if !defined(SPU__V130)
int main(void) { printf("v130 NEON block not enabled\n"); return 0; }
#else
int g_spu_xfloat = 1;
uint32_t g_spu_xflim[8] = { 0xFF7FFFFFu, 0xFF7FFFFFu, 0xFF7FFFFFu, 0xFF7FFFFFu, 0x7F7FFFFFu, 0x7F7FFFFFu, 0x7F7FFFFFu, 0x7F7FFFFFu };

/* the scalar forms the arm64 build used before (spu_xfv clamp + fmaf per lane) */
static u128 S_fma(u128 a, u128 b, u128 c)  { u128 r; a=spu_xfv(a); b=spu_xfv(b); c=spu_xfv(c); for(int i=0;i<4;i++) r._f32[i]=fmaf(a._f32[i], b._f32[i], c._f32[i]); return r; }
static u128 S_fms(u128 a, u128 b, u128 c)  { u128 r; a=spu_xfv(a); b=spu_xfv(b); c=spu_xfv(c); for(int i=0;i<4;i++) r._f32[i]=fmaf(a._f32[i], b._f32[i], -c._f32[i]); return r; }
static u128 S_fnms(u128 a, u128 b, u128 c) { u128 r; a=spu_xfv(a); b=spu_xfv(b); c=spu_xfv(c); for(int i=0;i<4;i++) r._f32[i]=fmaf(a._f32[i], -b._f32[i], c._f32[i]); return r; }
static u128 S_fa(u128 a, u128 b) { u128 r; a=spu_xfv(a); b=spu_xfv(b); for(int i=0;i<4;i++) r._f32[i]=a._f32[i]+b._f32[i]; return r; }
static u128 S_fm(u128 a, u128 b) { u128 r; a=spu_xfv(a); b=spu_xfv(b); for(int i=0;i<4;i++) r._f32[i]=a._f32[i]*b._f32[i]; return r; }

static uint64_t s = 0x9E3779B97F4A7C15ull;
static uint32_t rnd32(void){ s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)(s >> 11); }
static uint32_t special(void){
    static const uint32_t sp[] = {0,0x80000000u,1,0x80000001u,0x007FFFFFu,0x807FFFFFu,0x00800000u,0x7F7FFFFFu,0xFF7FFFFFu,0x7F800000u,0xFF800000u,
        0x7FC00000u,0xFFC00000u,0x7F800001u,0x3F800000u,0xBF800000u,0x4F000000u,0xCF000000u,0x4EFFFFFFu,0x4F800000u,0x5F000000u,0x2F800000u,0x00000400u,0x06FFFFFFu,0x07000000u,0x07FFFFFFu};
    uint32_t r = rnd32();
    switch (r & 7) { case 0: case 1: return sp[(r >> 3) % (sizeof sp / 4)];
      case 2: return (r & 0x807FFFFFu) | ((uint32_t)((r >> 9) % 3 ? (r >> 12) & 0xFF : ((r>>12)&1 ? 0xFF : 0)) << 23);
      case 3: return (r & 0x80000000u) | (((r >> 4) & 0xFF) << 23) | (r & 0x7FFFF);
      default: return rnd32(); }
}
static u128 rv(void){ u128 r; for(int i=0;i<4;i++) r._u32[i] = special(); return r; }
static long bad = 0, tot = 0;
static void chk(const char* n, u128 x, u128 y, u128 a, u128 b, int k, uint64_t fpcr){
    tot++;
    if (memcmp(&x, &y, 16)) { if (bad++ < 40) printf("DIFF %s k=%d fpcr=%08llX a=%08X %08X %08X %08X b=%08X.. ref=%08X %08X %08X %08X vec=%08X %08X %08X %08X\n",
        n, k, (unsigned long long)fpcr, a._u32[0],a._u32[1],a._u32[2],a._u32[3], b._u32[0], x._u32[0],x._u32[1],x._u32[2],x._u32[3], y._u32[0],y._u32[1],y._u32[2],y._u32[3]); }
}
static uint64_t get_fpcr(void) { uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v) :: "memory"); return v; }
static void set_fpcr(uint64_t v) { __asm__ volatile("msr fpcr, %0" :: "r"(v) : "memory"); }

int main(int argc, char** argv){
    const long N = argc > 1 ? atol(argv[1]) : 200000;
    const uint64_t base = get_fpcr() & ~((3ull << 22) | (1ull << 24));
    /* spu__xfq (two-min clamp) vs the scalar spu_xf over every bit pattern, default FPCR */
    { long d = 0; for (uint64_t w = 0; w < (1ull << 32); w += 4) { u128 a; for (int i = 0; i < 4; i++) a._u32[i] = (uint32_t)(w + i);
        u128 v = spu__fst(spu__xfq(spu__fld(a))), r = spu_xfv(a); tot++; if (memcmp(&v, &r, 16)) { if (d++ < 5) printf("DIFF xfq %08X\n", a._u32[0]); bad++; } } }
#if defined(SPU__V130_FREST)
    /* frest / frsqest (TBL byte planes) vs the scalar LUT references over every bit pattern (no FP math in them) */
    { long d = 0; for (uint64_t w = 0; w < (1ull << 32); w += 4) { u128 a; for (int i = 0; i < 4; i++) a._u32[i] = (uint32_t)(w + i);
        u128 v = spu__v_frest(a), r = spu_frest(a); tot++; if (memcmp(&v, &r, 16)) { if (d++ < 5) printf("DIFF frest %08X\n", a._u32[0]); bad++; }
        v = spu__v_frsqest(a); r = spu_frsqest(a); tot++; if (memcmp(&v, &r, 16)) { if (d++ < 5) printf("DIFF frsqest %08X\n", a._u32[0]); bad++; } } }
#endif
    for (unsigned rm = 0; rm < 4; rm++) for (unsigned fz = 0; fz < 2; fz++) {
        const uint64_t fpcr = base | ((uint64_t)rm << 22) | ((uint64_t)fz << 24);
        set_fpcr(fpcr);
        for (long it = 0; it < N; it++) {
            u128 a = rv(), b = rv(), c = rv(); int i8 = (int)(rnd32() & 0xFF), sh = (int)(rnd32() & 0x7F);
            chk("fa", S_fa(a,b), spu_fa(a,b), a,b,0,fpcr);
            chk("fm", S_fm(a,b), spu_fm(a,b), a,b,0,fpcr);
            chk("fma", S_fma(a,b,c), spu_fma(a,b,c), a,b,0,fpcr);
            chk("fms", S_fms(a,b,c), spu_fms(a,b,c), a,b,0,fpcr);
            chk("fnms", S_fnms(a,b,c), spu_fnms(a,b,c), a,b,0,fpcr);
            chk("fcgt", spu_fcgt(a,b), spu__v_fcgt(a,b), a,b,0,fpcr);
            chk("fceq", spu_fceq(a,b), spu__v_fceq(a,b), a,b,0,fpcr);
            chk("fcmgt", spu_fcmgt(a,b), spu__v_fcmgt(a,b), a,b,0,fpcr);
            chk("fcmeq", spu_fcmeq(a,b), spu__v_fcmeq(a,b), a,b,0,fpcr);
            u128 bb = b; if (it & 1) for(int i=0;i<4;i++) bb._u32[i] = (rnd32() & 1) ? a._u32[i] : (a._u32[i] ^ 0x80000000u);
            chk("fceq=", spu_fceq(a,bb), spu__v_fceq(a,bb), a,bb,0,fpcr);
            chk("fcmeq=", spu_fcmeq(a,bb), spu__v_fcmeq(a,bb), a,bb,0,fpcr);
            chk("fcmgt=", spu_fcmgt(a,bb), spu__v_fcmgt(a,bb), a,bb,0,fpcr);
            chk("cflts", spu_cflts(a,i8), spu__v_cflts(a,i8), a,b,i8,fpcr);
            chk("cflts173", spu_cflts(a,173), spu__v_cflts(a,173), a,b,173,fpcr);
            chk("csflt", spu_csflt(a,i8), spu__v_csflt(a,i8), a,b,i8,fpcr);
            chk("cuflt", spu_cuflt(a,i8), spu__v_cuflt(a,i8), a,b,i8,fpcr);
            chk("csflt155", spu_csflt(a,155), spu__v_csflt(a,155), a,b,155,fpcr);
            chk("fi", spu_fi(a,b), spu__v_fi(a,b), a,b,0,fpcr);
            { u128 b2 = b; for(int i=0;i<4;i++) b2._u32[i] = (b2._u32[i] & 0x807FFFFFu) | ((14u + (rnd32() % 242u)) << 23);   /* force the vector path */
              chk("fi-vec", spu_fi(a,b2), spu__v_fi(a,b2), a,b2,0,fpcr); }
            chk("shlqbii", spu_shlqbii(a,sh), spu__v_shlqbii(a,sh), a,b,sh,fpcr);
            chk("rotqbii", spu_rotqbii(a,sh), spu__v_rotqbii(a,sh), a,b,sh,fpcr);
            chk("rotqmbii", spu_rotqmbii(a,-sh), spu__v_rotqmbii(a,-sh), a,b,-sh,fpcr);
            chk("rotqmbii+", spu_rotqmbii(a,sh), spu__v_rotqmbii(a,sh), a,b,sh,fpcr);
            chk("shlqbi", spu_shlqbi(a,b), spu__v_shlqbi(a,b), a,b,0,fpcr);
            chk("rotqbi", spu_rotqbi(a,b), spu__v_rotqbi(a,b), a,b,0,fpcr);
            chk("rotqmbi", spu_rotqmbi(a,b), spu__v_rotqmbi(a,b), a,b,0,fpcr);
        }
        /* every exponent x every i8 for the conversions; every exponent/step for fi */
        for (uint32_t e = 0; e < 256; e++) for (int k = 0; k < 256; k++) {
            u128 a; for(int i=0;i<4;i++) a._u32[i] = (e << 23) | (rnd32() & 0x807FFFFFu);
            chk("cflts-x", spu_cflts(a,k), spu__v_cflts(a,k), a,a,k,fpcr);
            u128 ia; for(int i=0;i<4;i++) ia._u32[i] = rnd32() >> (rnd32() & 31);
            if (k & 1) for(int i=0;i<4;i++) ia._u32[i] = ~ia._u32[i];
            chk("csflt-x", spu_csflt(ia,k), spu__v_csflt(ia,k), ia,ia,k,fpcr);
            chk("cuflt-x", spu_cuflt(ia,k), spu__v_cuflt(ia,k), ia,ia,k,fpcr);
            u128 fb; for(int i=0;i<4;i++) fb._u32[i] = (e << 23) | (rnd32() & 0x807FFFFFu);
            chk("fi-x", spu_fi(a,fb), spu__v_fi(a,fb), a,fb,(int)e,fpcr);
        }
    }
    set_fpcr(base);
    printf("v130 neon: %ld checks, %ld differ\n", tot, bad);
    return bad != 0;
}
#endif
#endif
