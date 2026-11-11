/* test_spu_vecargs.c -- SPU_U128_VECARGS (Apple arm64 + clang, opt-in): every helper that spu_helpers_vec.h turns
 * into a macro over a vector-typed body must give the same bytes as the u128 function it replaces.
 * `(name)(args)` calls the function (a function-like macro does not expand when the name is in parentheses),
 * `name(args)` the macro. The integer families and the v130 forms are covered by test_spu_helpers_vec.c and
 * test_spu_helpers_v130_neon.c built with -DSPU_U128_VECARGS; this file covers the rest (spu_helpers.h forms,
 * the hand-written vec forms, local store, spu_xfk.h's _k forms).
 *
 *   cc -O2 -std=gnu17 -fno-strict-aliasing -ffp-contract=off -w -DSPU_U128_VECARGS -I runtime/spu -I include \
 *      -I runtime/ppu -I runtime/memory -I runtime/syscalls -I runtime/platform \
 *      runtime/spu/tests/test_spu_vecargs.c -o test_spu_vecargs && ./test_spu_vecargs
 * Without -DSPU_U128_VECARGS it builds too (both sides are then the same function). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "spu_helpers.h"
#include "spu_helpers_vec.h"
#include "spu_xfk.h"

static uint64_t g_seed = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { g_seed ^= g_seed << 13; g_seed ^= g_seed >> 7; g_seed ^= g_seed << 17; return (uint32_t)(g_seed >> 16); }
static const uint32_t k_special[] = {
    0x00000000u, 0x80000000u, 0x00000001u, 0x007FFFFFu, 0x00800000u, 0x3F800000u, 0xBF800000u, 0x7F7FFFFFu,
    0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x7F800001u, 0xFFFFFFFFu, 0x0000001Fu, 0x00000020u, 0x0000003Fu,
    0x00000040u, 0xFFFFFFE0u, 0xFFFFFFC1u, 0x4B000000u, 0x00FF00FFu, 0x12345678u, 0x80000001u, 0x0000007Fu };
static uint32_t word(void) {
    const uint32_t r = rnd();
    switch (r & 7) {
    case 0: case 1: return k_special[rnd() % (sizeof k_special / sizeof k_special[0])];
    case 2: return rnd() & 0xFF;                       /* small counts / indices */
    case 3: return (uint32_t)-(int32_t)(rnd() & 0xFF); /* small negative counts */
    default: return rnd();
    }
}
static u128 vec(void) { u128 v; for (int i = 0; i < 4; i++) v._u32[i] = word(); return v; }

static long g_checks, g_diff;
static void check(const char* name, u128 ref, u128 got, u128 a, u128 b, long imm) {
    g_checks++;
    if (memcmp(&ref, &got, 16) == 0) return;
    if (g_diff++ < 20)
        printf("DIFF %s imm=%ld a=%08X %08X %08X %08X b=%08X.. ref=%08X %08X %08X %08X got=%08X %08X %08X %08X\n",
               name, imm, a._u32[0], a._u32[1], a._u32[2], a._u32[3], b._u32[0],
               ref._u32[0], ref._u32[1], ref._u32[2], ref._u32[3], got._u32[0], got._u32[1], got._u32[2], got._u32[3]);
}
#define C1(n)       check(#n, (n)(a), n(a), a, b, 0)
#define C2(n)       check(#n, (n)(a, b), n(a, b), a, b, 0)
#define C3(n)       check(#n, (n)(a, b, c), n(a, b, c), a, b, 0)
#define CI(n, i)    check(#n, (n)(a, (i)), n(a, (i)), a, b, (i))

int main(void)
{
    const u128 z = {0};
    for (int it = 0; it < 400000; it++) {
        const u128 a = vec(), b = vec(), c = vec();
        C3(spu_shufb);
        C2(spu_rotqby); C2(spu_shlqby); C2(spu_rotqmby); C2(spu_rotqbybi); C2(spu_shlqbybi); C2(spu_rotqmbybi);
        C2(spu_cbx); C2(spu_chx); C2(spu_cwx); C2(spu_cdx);
        C2(spu_shl); C2(spu_rotm); C2(spu_rotma); C2(spu_rot);
        C1(spu_clz); C1(spu_gb); C1(spu_gbh); C1(spu_gbb);
        C2(spu_fa); C2(spu_fs); C2(spu_fm); C3(spu_fma); C3(spu_fms); C3(spu_fnms);
        const int i7 = (int)(rnd() & 0x7F) - 64;   /* signed 7-bit immediates */
        CI(spu_rotqbyi, i7); CI(spu_shlqbyi, i7); CI(spu_rotqmbyi, i7);
        CI(spu_cbd, i7); CI(spu_chd, i7); CI(spu_cwd, i7); CI(spu_cdd, i7);
        { const uint16_t h = (uint16_t)rnd(); CI(spu__v_iohl, h); }
        C1(spu__v_fsm);
        { const uint32_t w = word();
          check("splat_u32", (spu__v_splat_u32)(w), spu__v_splat_u32(w), a, b, w);
          check("il", (spu__v_il)((int32_t)w), spu__v_il((int32_t)w), a, b, w);
          check("ila", (spu__v_ila)(w), spu__v_ila(w), a, b, w);
          check("ilhu", (spu__v_ilhu)((uint16_t)w), spu__v_ilhu((uint16_t)w), a, b, w);
          check("ilh", (spu__v_ilh)((uint16_t)w), spu__v_ilh((uint16_t)w), a, b, w);
          check("link", (spu__v_link)(w), spu__v_link(w), a, b, w);
          check("pref_u32", (spu__v_pref_u32)(w), spu__v_pref_u32(w), a, b, w); }
        /* spu_xfk.h: the _k forms over the macro spu__fld / spu__fst vs the same text over the functions */
        { const int m = (int)(rnd() & 7);
#define FLD(x) (spu__fld)(x)
#define FST(x) (spu__fst)(x)
          check("fa_k", FST(_mm_add_ps(SPU__XFKC(m, 0, FLD(a)), SPU__XFKC(m, 1, FLD(b)))), spu_fa_k(a, b, m), a, b, m);
          check("fs_k", FST(_mm_sub_ps(SPU__XFKC(m, 0, FLD(a)), SPU__XFKC(m, 1, FLD(b)))), spu_fs_k(a, b, m), a, b, m);
          check("fm_k", FST(_mm_mul_ps(SPU__XFKC(m, 0, FLD(a)), SPU__XFKC(m, 1, FLD(b)))), spu_fm_k(a, b, m), a, b, m);
#if defined(__aarch64__)
          check("fma_k", FST(spu__xfk_fmadd(SPU__XFKC(m, 0, FLD(a)), SPU__XFKC(m, 1, FLD(b)), SPU__XFKC(m, 2, FLD(c)))), spu_fma_k(a, b, c, m), a, b, m);
          check("fms_k", FST(spu__xfk_fmsub(SPU__XFKC(m, 0, FLD(a)), SPU__XFKC(m, 1, FLD(b)), SPU__XFKC(m, 2, FLD(c)))), spu_fms_k(a, b, c, m), a, b, m);
          check("fnms_k", FST(spu__xfk_fnmadd(SPU__XFKC(m, 0, FLD(a)), SPU__XFKC(m, 1, FLD(b)), SPU__XFKC(m, 2, FLD(c)))), spu_fnms_k(a, b, c, m), a, b, m);
#endif
        }
    }
    /* every immediate */
    for (int it = 0; it < 64; it++) {
        const u128 a = vec(), b = z;
        for (int i = -128; i < 128; i++) { CI(spu_rotqbyi, i); CI(spu_shlqbyi, i); CI(spu_rotqmbyi, i);
                                           CI(spu_cbd, i); CI(spu_chd, i); CI(spu_cwd, i); CI(spu_cdd, i); }
    }
    for (int32_t i = -70000; i < 70000; i++) {
        const u128 a = z, b = z;
        check("fsmbi", (spu_fsmbi)(i), spu_fsmbi(i), a, b, i);
    }
    /* every shift count, every lane */
    for (int it = 0; it < 2000; it++) {
        const u128 a = vec();
        for (uint32_t s = 0; s < 256; s++) {
            u128 b; for (int l = 0; l < 4; l++) b._u32[l] = (s + 61u * (uint32_t)l) | (rnd() & 0xFFFFFF00u & -(uint32_t)(it & 1));
            C2(spu_shl); C2(spu_rotm); C2(spu_rotma); C2(spu_rot);
            C2(spu_rotqby); C2(spu_shlqby); C2(spu_rotqmby); C2(spu_rotqbybi); C2(spu_shlqbybi); C2(spu_rotqmbybi);
        }
    }
#if !defined(SPU_RC_LS_PROBES)
    /* local store: read and write at every 16-byte line, plus addresses with low bits and above the mask */
    {
        static uint8_t ls_ref[0x40000 + 16], ls_got[0x40000 + 16];
        for (size_t i = 0; i < sizeof ls_ref; i++) ls_ref[i] = ls_got[i] = (uint8_t)rnd();
        for (int it = 0; it < 200000; it++) {
            const uint32_t lsa = rnd() ^ (rnd() << 16);
            const u128 a = vec(), b = z;
            check("ls_rd", (spu__v_ls_rd)(ls_ref, lsa), spu__v_ls_rd(ls_got, lsa), a, b, lsa);
            (spu__v_ls_wr)(ls_ref, lsa, a);
            spu__v_ls_wr(ls_got, lsa, a);
        }
        g_checks++;
        if (memcmp(ls_ref, ls_got, sizeof ls_ref) != 0) { g_diff++; printf("DIFF ls_wr: stores differ\n"); }
    }
#endif
    printf("vecargs: %ld checks, %ld differ (%s)\n", g_checks, g_diff,
#if defined(SPU__VECARGS) && SPU__VECARGS
           "macro forms on"
#else
           "macro forms off: both sides are the functions"
#endif
    );
    return g_diff != 0;
}
