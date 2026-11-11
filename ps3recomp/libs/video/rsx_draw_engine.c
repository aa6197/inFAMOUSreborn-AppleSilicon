/*
 * ps3recomp - platform-neutral RSX draw engine (see rsx_draw_engine.h)
 *
 * The orchestration carried across from libs/video/rsx_live_draw.c, the
 * NV4097 -> D3D12 engine a title has shipped on. Line numbers in the comments
 * below are that file's, so the two can be read side by side; the file itself
 * is vendored and stays exactly as it is.
 *
 * What is deliberately left out, because it is not renderer behaviour a title
 * needs: the shader disk cache, movie mode and its compositor, the a010
 * probe, and every YZ_PERF_PROFILE block.
 */
#include "rsx_draw_engine.h"

#include "rsx_fp_decompiler.h"
#include "rsx_primitives.h"
#include "rsx_restart_cuts.h"
#include "rsx_vertex_formats.h"
#include "rsx_vp_decompiler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define XXH_INLINE_ALL
#define XXH_STATIC_LINKING_ONLY
#include "xxhash.h"

/* Guest memory and the RSX offset resolvers, declared the way every backend
 * that reads guest data declares them (rsx_vertex_fetch.h). */
extern u8* vm_base;
u32 cellGcmResolveLocated(int local, u32 offset);
/* Non-zero only when the offset's page is in the IO table, so a caller can
 * tell IO-mapped main memory from "assume VRAM". */
u32 cellGcmResolveIO(u32 offset);
/* The guest VM's OOB guard: the size a host backed, or 0 for "the whole 32-bit
 * space is backed and no check is needed" (runtime/memory/vm.h). */
extern u32 ppu_vm_size;

/* ------------------------------------------------------------------------- */

#define ENG_MAX_SURFACES   64
#define ENG_MAX_ZDEPTHS    64
#define ENG_MAX_TEXTURES   RSX_DRAW_ENGINE_TEXTURE_CACHE
#define ENG_MAX_VTEXTURES  64
#define ENG_MAX_PIPELINES  8192
#define ENG_MAX_BATCHES    256
#define ENG_VERT_STRIDE    (RSX_DSP_NUM_VERTEX_ATTR * 16u)
#define ENG_INVALID        0xFFFFFFFFu
/* Guest textures larger than this are a misdecoded register, not an image;
 * rsx_live_draw.c rejects the same 4096 bound (2033). */
#define ENG_MAX_TEX_DIM    4096u
/* A surface declaration beyond this is a guest pointer read as clip
 * dimensions. rsx_live_draw.c's surface-guard (2625) preserves the existing
 * target rather than destroying a live one. */
#define ENG_MAX_SURFACE_DIM 8192u

#define ENG_VP_CB_BYTES    ((RSX_DSP_NUM_CONSTANTS + 2) * 16u)
#define ENG_FP_MAX_BYTES   0x10000u

#define M_ALPHA_TEST_ENABLE   0x0304
#define M_ALPHA_FUNC          0x0308
#define M_ALPHA_REF           0x030C
#define M_BLEND_ENABLE        0x0310
#define M_BLEND_SFACTOR       0x0314
#define M_BLEND_DFACTOR       0x0318
#define M_BLEND_EQUATION      0x0320
#define M_COLOR_MASK          0x0324
#define M_STENCIL_TEST_ENABLE 0x0328
#define M_STENCIL_MASK        0x032C
#define M_STENCIL_FUNC        0x0330
#define M_STENCIL_FUNC_REF    0x0334
#define M_STENCIL_FUNC_MASK   0x0338
#define M_STENCIL_OP_FAIL     0x033C
#define M_STENCIL_OP_ZFAIL    0x0340
#define M_STENCIL_OP_ZPASS    0x0344
#define M_TWO_SIDED_STENCIL   0x0348
#define M_BACK_STENCIL_FUNC   0x0350
#define M_BACK_STENCIL_OP_FAIL  0x035C
#define M_BACK_STENCIL_OP_ZFAIL 0x0360
#define M_BACK_STENCIL_OP_ZPASS 0x0364
#define M_SCISSOR_HORIZONTAL  0x08C0
#define M_SCISSOR_VERTICAL    0x08C4
#define M_DEPTH_FUNC          0x0A6C
#define M_DEPTH_WRITE         0x0A70
#define M_DEPTH_TEST_ENABLE   0x0A74
#define M_ZSTENCIL_CLEAR      0x1D8C

/* ---- module state -------------------------------------------------------- */

typedef struct {
    u32 location, offset;
    u32 w, h;
    rsx_be_format fmt;
    u32 handle;
    int cwritten;          /* a draw with a colour mask != 0 targeted it   */
    u32 feedback;          /* copy sampled by a draw that writes it, or 0  */
    u32 fb_serial;         /* the bind that last refreshed `feedback`      */
    u32 fb_last;           /* FEEDBACK_RUN: the bind that last sampled it  */
    u32 fb_run;            /*   draws since the snapshot was refreshed     */
    u32 fb_fp;             /*   that draw's fragment program               */
    u32 fb_frame;          /*   and its frame                              */
} eng_surface;

typedef struct {
    u32 location, offset;
    u32 w, h;
    u32 handle;
    u32 snapshot;          /* sampleable copy, or 0                        */
    u32 snap_w, snap_h;    /* the region the snapshot holds                */
    int cleared;
    int had_write;
    int snapshot_valid;
    u32 ccopy;             /* A8R8G8B8 colour copy, or 0                   */
    u32 cc_w, cc_h;        /* the region the colour copy holds             */
    int ccopy_valid;
} eng_zdepth;

typedef struct {
    u32 location, offset, format, width, height, pitch, remap, cubemap;
    u32 handle;
    u64 content_hash;
    u32 last_hash_frame;
    u64 last_use_serial;
    u32 span, span_levels; /* eng_texture_span for these levels (0 = none)   */
} eng_texture;

/* cuse: the vertex constants the pipeline's vertex shader reads (bit per
 * vp_c[i]); cpart: cuse is complete (no relatively indexed read), so only
 * those need staging (D3D12 VCB_PARTIAL, rsx_d3d12_backend.c:3360). */
typedef struct { u64 key; u32 handle; u8 fixed; u8 cpart; u64 ps_hash; u32 cuse[16]; } eng_pipeline;

typedef struct { u32 first, count; } eng_batch;

typedef struct {
    u32 location, offset, pitch, width, height;
    int valid;
} eng_display_buffer;

static struct {
    const rsx_draw_backend* be;
    int ready;
    int default_on;
    u32 width, height;

    rsx_dispatch rsx;

    eng_surface surfaces[ENG_MAX_SURFACES];
    u32 n_surfaces;
    eng_zdepth zdepths[ENG_MAX_ZDEPTHS];
    u32 n_zdepths;
    /* One bit per hash of the (location, offset) of every colour and depth
     * target (eng_rt_note). Bits are only added: a slot keeps its key for
     * life, so a set bit can be stale but a clear bit is exact. */
    u64 rt_filter[16];
    /* This draw's shadow-compare units, as the D3D12 backend keys them: bits
     * 0-3 the units, bits 4-15 a 3-bit zfunc per unit. */
    u32 shkey;
    /* This draw's TEX_XF modes: 2 bits per unit 0-7 (bit0 bx2, bit1 sRGB). */
    u32 texk;
    /* This draw tests depth bounds: a depth snapshot sits on ENG_DBT_UNIT. */
    u32 dbt;
    /* The bound pipeline's pixel-shader HLSL hash (the shader-dump name). */
    u64 ps_hash;
    /* The bound pipeline's used vertex constants, NULL = all (VCB_PARTIAL). */
    const u32* vs_cuse;
    eng_texture textures[ENG_MAX_TEXTURES];
    u32 n_textures;
    eng_pipeline pipelines[ENG_MAX_PIPELINES];
    u32 n_pipelines;
    /* Open-addressed index over pipelines[] by key: slot = pipeline + 1. */
    u16 pipe_index[ENG_MAX_PIPELINES * 2u];
    eng_display_buffer display_buffers[8];

    u32 frames;
    u64 texture_use_serial;
    u32 guest_draws;
    u32 last_guest_draws;
    u32 last_present_surface;
    u32 last_flip_buffer;

    /* staging: decoded texture levels, the constant blocks, the index list */
    u8* tex_staging;
    u32 tex_staging_cap;
    u8* vp_cb;
    u8* fp_cb;
    u32 fp_cb_cap;
    u32* indices;
    u32 index_cap;

    rsx_fp_constant_block fp_constants;
} g;

/* The target filter. A texture unit whose address has no bit set reads no
 * colour or depth target, so sink_bind_textures skips both table scans for
 * it: an exact shortcut, the scans would find nothing. iPad profile: the two
 * scans, per bound unit per draw, were ~5% of the RSX thread. */
static inline u32 eng_rt_bit(u32 location, u32 offset)
{
    return ((offset ^ (location << 31)) * 2654435761u) >> 22;   /* 0..1023 */
}
static inline void eng_rt_note(u32 location, u32 offset)
{
    const u32 b = eng_rt_bit(location, offset);
    g.rt_filter[b >> 6] |= 1ull << (b & 63u);
}
static inline int eng_rt_maybe(u32 location, u32 offset)
{
    const u32 b = eng_rt_bit(location, offset);
    return (int)((g.rt_filter[b >> 6] >> (b & 63u)) & 1u);
}

/* The decompilers' HLSL. The VP decompiler builds bodies up to 192 KB. */
static char s_vs_hlsl[256 * 1024];
static char s_ps_hlsl[256 * 1024];

/* The draw accumulator: one BEGIN_END group's batches, references and cuts. */
static struct {
    eng_batch arr[ENG_MAX_BATCHES];
    u32 n_arr;
    eng_batch idx[ENG_MAX_BATCHES];
    u32 n_idx;
    u32 n_packets;

    rsx_vertex_ref* refs;
    u32 n_refs, cap_refs, n_source_refs;
    int refs_remapped;
    rsx_vertex_remap ref_remap;
    /* Set by the fast index read when it produced every reference: their
     * vertex-id range (all share one base index). */
    int rng_known;
    u32 rng_lo, rng_hi;

    u32* cuts;
    u32 n_cuts, cap_cuts;

    u8* verts;
    u64 verts_cap;
    u32 n_verts;

    rsx_vertex_layout_plan layout;
    rsx_vertex_fetch_plan fetch_plan;
    int fetch_ok;

    /* INLINE_ARRAY stream for this group, when the vertices came down the
     * FIFO rather than from a vertex array. */
    const u8* inl;
    u32 inl_bytes;
    const float (*imm)[RSX_DSP_NUM_VERTEX_ATTR][4];   /* sink_immediate */
    u32 imm_count;
} dc;

/* ---- guest memory -------------------------------------------------------- */

/* rsx_dsp_* locations are RSX_LOCATION_LOCAL = 0 / _MAIN = 1;
 * cellGcmResolveLocated's argument is the opposite sense (1 = local), and
 * getting that backwards resolves every VRAM object through the IO table. */
static rsx_vertex_guest_ptr_fn s_guest_reader;
static void* s_guest_user;

void rsx_draw_engine_set_guest_memory(rsx_vertex_guest_ptr_fn reader, void* user)
{
    s_guest_reader = reader;
    s_guest_user = user;
}

static const u8* eng_guest_ptr(void* user, u32 location, u32 offset,
                               u32 min_bytes)
{
    (void)user;
    if (!min_bytes || min_bytes > (256u << 20)) return NULL;
    if (s_guest_reader) return s_guest_reader(s_guest_user, location, offset, min_bytes);
    if (!vm_base) return NULL;
    const u32 ea = cellGcmResolveLocated(location == RSX_LOCATION_LOCAL, offset);
    if (!ea) return NULL;
    if (ppu_vm_size && (u64)ea + min_bytes > ppu_vm_size) return NULL;
    return vm_base + ea;
}

static u64 eng_fnv1a(const void* data, u32 n, u64 hash)
{
    const u8* p = (const u8*)data;
    for (u32 i = 0; i < n; i++) { hash ^= p[i]; hash *= 1099511628211ull; }
    return hash;
}

/* ---- render state -------------------------------------------------------- */

void rsx_draw_engine_decode_render_state(const rsx_dispatch* rsx,
                                         rsx_be_render_state* rs)
{
    memset(rs, 0, sizeof(*rs));
    rs->alpha_test_enable = rsx_dsp_reg(rsx, M_ALPHA_TEST_ENABLE) & 1;
    rs->alpha_func   = rsx_dsp_reg(rsx, M_ALPHA_FUNC);
    rs->alpha_ref_raw = rsx_dsp_reg(rsx, M_ALPHA_REF);
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(rsx, &sf);
    rs->alpha_ref_format = sf.color_format;
    rs->rt_fp16 = sf.color_format == RSX_SURFACE_FMT_F_W16Z16Y16X16;
    rs->blend_enable = rsx_dsp_reg(rsx, M_BLEND_ENABLE) & 1;
    const u32 sfac = rsx_dsp_reg(rsx, M_BLEND_SFACTOR);
    const u32 dfac = rsx_dsp_reg(rsx, M_BLEND_DFACTOR);
    const u32 eq   = rsx_dsp_reg(rsx, M_BLEND_EQUATION);
    rs->sf_rgb = sfac & 0xFFFF; rs->sf_a = sfac >> 16;
    rs->df_rgb = dfac & 0xFFFF; rs->df_a = dfac >> 16;
    rs->eq_rgb = eq & 0xFFFF;   rs->eq_a = eq >> 16;
    rs->depth_test  = rsx_dsp_reg(rsx, M_DEPTH_TEST_ENABLE) & 1;
    rs->depth_write = rsx_dsp_reg(rsx, M_DEPTH_WRITE) & 1;
    rs->depth_func  = rsx_dsp_reg(rsx, M_DEPTH_FUNC);
    rs->cull_enable = rsx_dsp_reg(rsx, 0x183C) & 1;
    rs->cull_face   = rsx_dsp_reg(rsx, 0x1830);
    rs->front_face  = rsx_dsp_reg(rsx, 0x1834);
    /* The RAW register: a game-written 0 is a real "write no colour channel"
     * (a depth-prime pass), and rsx_dispatch_init seeds the nv40 reset value
     * so never-written reads as all-on. */
    rs->color_mask  = rsx_dsp_reg(rsx, M_COLOR_MASK);
    rs->stencil_enable    = rsx_dsp_reg(rsx, M_STENCIL_TEST_ENABLE) & 1;
    rs->stencil_two_sided = rsx_dsp_reg(rsx, M_TWO_SIDED_STENCIL) & 1;
    rs->s_func       = rsx_dsp_reg(rsx, M_STENCIL_FUNC);
    rs->s_func_mask  = rsx_dsp_reg(rsx, M_STENCIL_FUNC_MASK) & 0xFF;
    rs->s_write_mask = rsx_dsp_reg(rsx, M_STENCIL_MASK) & 0xFF;
    rs->s_fail       = rsx_dsp_reg(rsx, M_STENCIL_OP_FAIL);
    rs->s_zfail      = rsx_dsp_reg(rsx, M_STENCIL_OP_ZFAIL);
    rs->s_zpass      = rsx_dsp_reg(rsx, M_STENCIL_OP_ZPASS);
    rs->bs_func      = rsx_dsp_reg(rsx, M_BACK_STENCIL_FUNC);
    rs->bs_fail      = rsx_dsp_reg(rsx, M_BACK_STENCIL_OP_FAIL);
    rs->bs_zfail     = rsx_dsp_reg(rsx, M_BACK_STENCIL_OP_ZFAIL);
    rs->bs_zpass     = rsx_dsp_reg(rsx, M_BACK_STENCIL_OP_ZPASS);
}

/* The pipeline-identity fields of a render state as words, each named so
 * struct padding can never be identity (rsx_live_draw.c:3151-3185). The alpha
 * REFERENCE is deliberately absent: it lives in the fragment constant block,
 * so a title animating a fade must not compile a new pipeline per frame.
 * Enable and compare mode still select a variant. */
#define ENG_RS_WORDS 29u
static void eng_rs_words(const rsx_be_render_state* rs, u32* w)
{
    w[0] = rs->alpha_test_enable;
    w[1] = rs->alpha_func;
    w[2] = rs->blend_enable;
    w[3] = rs->sf_rgb;
    w[4] = rs->df_rgb;
    w[5] = rs->sf_a;
    w[6] = rs->df_a;
    w[7] = rs->eq_rgb;
    w[8] = rs->eq_a;
    w[9] = rs->depth_test;
    w[10] = rs->depth_write;
    w[11] = rs->depth_func;
    w[12] = rs->cull_enable;
    w[13] = rs->cull_face;
    w[14] = rs->front_face;
    w[15] = rs->color_mask;
    w[16] = rs->rt_fp16;
    w[17] = rs->stencil_enable;
    w[18] = rs->stencil_two_sided;
    w[19] = rs->s_func;
    w[20] = rs->s_func_mask;
    w[21] = rs->s_write_mask;
    w[22] = rs->s_fail;
    w[23] = rs->s_zfail;
    w[24] = rs->s_zpass;
    w[25] = rs->bs_func;
    w[26] = rs->bs_fail;
    w[27] = rs->bs_zfail;
    w[28] = rs->bs_zpass;
}

/* One XXH3 over the words: the byte-at-a-time FNV over the same fields was
 * ~3.5% of the RSX thread (it runs on every draw). */
u64 rsx_draw_engine_hash_render_state(const rsx_be_render_state* rs, u64 hash)
{
    u32 w[ENG_RS_WORDS];
    eng_rs_words(rs, w);
    return XXH3_64bits_withSeed(w, sizeof w, hash);
}

/* ---- topology expansion -------------------------------------------------- */

/* Every expansion is bounded by the restart cuts, so a strip that the guest
 * broke with the sentinel index gets no connecting triangle across the break.
 * Missing that decode is what upstream's aa7fb63 records as "the exploded
 * spiky mesh that occluded the scene". */
u32 rsx_draw_engine_topology_index_count(u32 primitive, u32 source_refs,
                                         const u32* cuts, u32 cut_count)
{
    const u32 segments = cut_count + 1;
    switch (primitive) {
    case RSX_PRIMITIVE_TRIANGLES:
        return source_refs - source_refs % 3u;
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_POLYGON: {
        u32 total = 0;
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            (void)begin;
            if (count >= 3) total += (count - 2) * 3;
        }
        return total;
    }
    case RSX_PRIMITIVE_QUAD_STRIP: {
        /* Two vertices per quad after the first pair, so a segment of n
         * carries (n - 2) / 2 quads and an odd trailing vertex is dropped. */
        u32 total = 0;
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            (void)begin;
            if (count >= 4) total += ((count - 2u) / 2u) * 6u;
        }
        return total;
    }
    case RSX_PRIMITIVE_QUADS:
        return (source_refs / 4u) * 6u;
    default:
        return 0;
    }
}

static u32 eng_topology_vertex(const u32* occurrence_to_unique, u32 occurrence)
{
    return occurrence_to_unique ? occurrence_to_unique[occurrence] : occurrence;
}

void rsx_draw_engine_write_topology_indices(u32 primitive, u32 source_refs,
                                            const u32* cuts, u32 cut_count,
                                            const u32* occurrence_to_unique,
                                            u32* indices)
{
    u32 write = 0;
    const u32 segments = cut_count + 1;
    switch (primitive) {
    case RSX_PRIMITIVE_TRIANGLES: {
        const u32 count = source_refs - source_refs % 3u;
        for (u32 o = 0; o < count; o++)
            indices[write++] = eng_topology_vertex(occurrence_to_unique, o);
        break;
    }
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            if (count < 3) continue;
            /* Odd triangles swap their first two vertices, so the whole strip
             * keeps one winding. */
            for (u32 i = 0; i + 2 < count; i++) {
                indices[write++] = eng_topology_vertex(
                    occurrence_to_unique, begin + i + (i & 1u));
                indices[write++] = eng_topology_vertex(
                    occurrence_to_unique, begin + i + 1u - (i & 1u));
                indices[write++] = eng_topology_vertex(
                    occurrence_to_unique, begin + i + 2u);
            }
        }
        break;
    /* A polygon is a convex fan around its own first vertex, which is what
     * both of this tree's other renderers make of one. */
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_POLYGON:
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            if (count < 3) continue;
            for (u32 i = 1; i + 1 < count; i++) {
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + i);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + i + 1u);
            }
        }
        break;
    case RSX_PRIMITIVE_QUAD_STRIP:
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            if (count < 4) continue;
            /* Every quad is split along the diagonal between the two pairs,
             * so each one keeps the strip's winding without alternating the
             * way a triangle strip has to; a cut simply starts the pairing
             * again from the segment's own first vertex. This is the
             * expansion the vtable path's emit_vertices performs. */
            for (u32 q = 0; q + 3 < count; q += 2) {
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q + 1u);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q + 2u);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q + 1u);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q + 3u);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q + 2u);
            }
        }
        break;
    case RSX_PRIMITIVE_QUADS:
        for (u32 q = 0; q < source_refs / 4u; q++) {
            const u32 base = q * 4u;
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base);
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base + 1u);
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base + 2u);
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base + 2u);
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base + 3u);
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base);
        }
        break;
    default:
        break;
    }
}

/* Does this primitive become an indexed triangle list, and is the index buffer
 * needed at all? rsx_vertex_topology_plan answers that for the reference
 * engine, whose own expansion stops at quads. Quad strips and polygons are
 * this engine's, so their two cases are decided here rather than in the
 * shared helper, which the vendored rsx_live_draw.c also calls. */
static int eng_topology_rebuild(u32 primitive, int refs_remapped, int* indexed)
{
    if (primitive == RSX_PRIMITIVE_QUAD_STRIP ||
        primitive == RSX_PRIMITIVE_POLYGON) {
        (void)refs_remapped;
        *indexed = 1;
        return 1;
    }
    return rsx_vertex_topology_plan(primitive, refs_remapped, indexed);
}

/* ---- surfaces ------------------------------------------------------------ */

static rsx_be_format eng_surface_format(u32 color_format)
{
    switch (color_format & 0x1Fu) {
    case RSX_SURFACE_FMT_F_W16Z16Y16X16: return RSX_BE_FMT_R16G16B16A16F;
    case 0x0C: return RSX_BE_FMT_R32G32B32A32F;
    case 0x0D: return RSX_BE_FMT_R32F;
    default:   return RSX_BE_FMT_R8G8B8A8;
    }
}

/* Decode the guest's own bytes behind a colour surface into host R,G,B,A rows
 * so a freshly created target starts with what the title CPU-initialised it
 * to. Returns the staging pointer, or NULL when the bytes are unreadable or
 * the format is not one the decoder handles. */
static const void* eng_surface_seed(u32 location, u32 offset, u32 w, u32 h,
                                    rsx_be_format fmt, u32* out_row_bytes)
{
    *out_row_bytes = 0;
    if (fmt != RSX_BE_FMT_R8G8B8A8) return NULL;
    /* Only a surface in IO-mapped MAIN memory is seeded. RSX local memory is
     * reserved and not backed by this tree's guest VM -- runtime/memory/vm.h
     * maps the whole space PROT_NONE and commits main memory and the stacks,
     * leaving a runner to commit what its own title needs -- so reading a VRAM
     * surface's backing would fault long before it could help. Both halves are
     * checked: the DMA context says which space the offset is in, and the IO
     * table says the page is really mapped. */
    if (location != RSX_LOCATION_MAIN ||
        (!s_guest_reader && !cellGcmResolveIO(offset))) return NULL;
    rsx_tex_layout tl;
    /* A8R8G8B8 with the LN bit: a surface is linear, never swizzled. */
    rsx_texture_layout(0x85u | 0x20u, w, h, &tl);
    if (!tl.face_bytes || !tl.dst_row_bytes) return NULL;
    const u8* src = eng_guest_ptr(NULL, location, offset, tl.face_bytes);
    if (!src) return NULL;
    const u32 bytes = tl.dst_row_bytes * tl.rows;
    if (g.tex_staging_cap < bytes) {
        u8* n = (u8*)realloc(g.tex_staging, bytes);
        if (!n) return NULL;
        g.tex_staging = n;
        g.tex_staging_cap = bytes;
    }
    rsx_texture_decode(g.tex_staging, tl.dst_row_bytes, src, w, h, &tl,
                       rsx_texture_argb_is_rgba());
    *out_row_bytes = tl.dst_row_bytes;
    return g.tex_staging;
}

/* The colour target for this (location, offset), created or reallocated when
 * the size or format moved. Returns a slot index, or ENG_INVALID.
 * rsx_live_draw.c's surface_get (2605-2690). */
static u32 eng_surface_get(u32 location, u32 offset, u32 want_w, u32 want_h,
                          rsx_be_format want_fmt)
{
    if (!want_w) want_w = g.width;
    if (!want_h) want_h = g.height;

    u32 slot = ENG_MAX_SURFACES;
    for (u32 i = 0; i < g.n_surfaces; i++)
        if (g.surfaces[i].location == location && g.surfaces[i].offset == offset) {
            if (g.surfaces[i].w == want_w && g.surfaces[i].h == want_h &&
                g.surfaces[i].fmt == want_fmt)
                return i;
            slot = i;
            break;
        }

    /* Never destroy a usable render target because one malformed command
     * decoded a guest pointer as clip dimensions. */
    if (want_w > ENG_MAX_SURFACE_DIM || want_h > ENG_MAX_SURFACE_DIM) {
        static u32 logs = 0;
        if ((logs < 8 && logs++ < 8))
            fprintf(stderr, "[rsx engine] rejected implausible surface 0x%X %ux%u;"
                            " keeping the %s target\n", offset, want_w, want_h,
                    slot < ENG_MAX_SURFACES ? "existing" : "absent");
        return slot < ENG_MAX_SURFACES ? slot : ENG_INVALID;
    }
    if (slot == ENG_MAX_SURFACES) {
        if (g.n_surfaces >= ENG_MAX_SURFACES) return ENG_INVALID;
        slot = g.n_surfaces;
    }

    u32 seed_row = 0;
    const void* seed = eng_surface_seed(location, offset, want_w, want_h,
                                        want_fmt, &seed_row);
    const u32 handle = g.be->color_target_create(g.be->user, want_fmt,
                                                 want_w, want_h, seed, seed_row);
    if (!handle)
        return (slot < g.n_surfaces && g.surfaces[slot].handle) ? slot : ENG_INVALID;

    eng_surface* s = &g.surfaces[slot];
    if (s->handle) g.be->color_target_release(g.be->user, s->handle);
    if (s->feedback && g.be->texture_release) g.be->texture_release(g.be->user, s->feedback);
    s->location = location; s->offset = offset;
    eng_rt_note(location, offset);
    s->w = want_w; s->h = want_h; s->fmt = want_fmt;
    s->handle = handle;
    s->cwritten = 0;
    s->feedback = 0;
    s->fb_serial = 0;
    if (slot == g.n_surfaces) g.n_surfaces++;
    { static u32 logs = 0; if ((logs < 16 && logs++ < 16))
        fprintf(stderr, "[rsx engine] surface %u:0x%08X %ux%u fmt %d%s\n",
                location, offset, want_w, want_h, (int)want_fmt,
                seed ? " (seeded from guest memory)" : ""); }
    return slot;
}

/* The colour targets SET_SURFACE_COLOR_TARGET names, as slots into
 * g.surfaces, with A first. One for an ordinary draw; an MRT set adds B, C
 * and D, each created at target A's size and format, which is the rule the
 * vtable path's target_for binds by. Resolution stops at the first member
 * that cannot be created, as that path stops at the first gap. Returns how
 * many were resolved, or 0. */
static u32 eng_current_target_set(u32 slots[RSX_BE_MAX_COLOR_TARGETS])
{
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(&g.rsx, &sf);
    /* SET_SURFACE_COLOR_TARGET 2 selects B alone; every other value starts at
     * A, and the MRT selectors add B, C and D on top of it. */
    const u32 sel = (sf.color_target == CELL_GCM_SURFACE_TARGET_1) ? 1u : 0u;
    const rsx_be_format fmt = eng_surface_format(sf.color_format);
    const u32 first = eng_surface_get(sf.color_location[sel], sf.color_offset[sel],
                                      sf.clip_w, sf.clip_h, fmt);
    if (first == ENG_INVALID) return 0;
    slots[0] = first;
    u32 n = 1;

    static const u32 mrt_from[3] = {
        CELL_GCM_SURFACE_TARGET_MRT1, CELL_GCM_SURFACE_TARGET_MRT2,
        CELL_GCM_SURFACE_TARGET_MRT3
    };
    for (u32 i = 0; i < 3; i++) {
        if (sf.color_target < mrt_from[i]) break;
        const u32 slot = eng_surface_get(sf.color_location[i + 1],
                                         sf.color_offset[i + 1],
                                         sf.clip_w, sf.clip_h, fmt);
        if (slot == ENG_INVALID) break;
        /* A set naming one buffer twice would attach the same target twice,
         * which no host API allows. */
        int seen = 0;
        for (u32 k = 0; k < n; k++) if (slots[k] == slot) seen = 1;
        if (seen) break;
        slots[n++] = slot;
    }
    return n;
}

static u32 eng_current_surface(void)
{
    u32 slots[RSX_BE_MAX_COLOR_TARGETS];
    return eng_current_target_set(slots) ? slots[0] : ENG_INVALID;
}

/* ---- per-zeta depth ------------------------------------------------------ */

/* One depth target per guest zeta address. Sharing a single resource across a
 * title's shadow, scene and post passes cross-contaminates later depth tests;
 * upstream's ef3271b records that as "the black player-character mass". */
static u32 eng_zdepth_get(u32 location, u32 offset, u32 rt_w, u32 rt_h)
{
    /* The attachment must cover the whole canvas: an early pass declaring a
     * smaller clip than the live viewport otherwise gets a target the host
     * API rejects when a later, larger pass binds it. */
    u32 want_w = rt_w > g.width ? rt_w : g.width;
    u32 want_h = rt_h > g.height ? rt_h : g.height;
    if (want_w > ENG_MAX_SURFACE_DIM || want_h > ENG_MAX_SURFACE_DIM)
        return ENG_INVALID;

    u32 slot = ENG_MAX_ZDEPTHS;
    for (u32 i = 0; i < g.n_zdepths; i++) {
        eng_zdepth* z = &g.zdepths[i];
        if (z->location == location && z->offset == offset) {
            if (z->w >= want_w && z->h >= want_h) return i;
            slot = i;
            if (want_w < z->w) want_w = z->w;
            if (want_h < z->h) want_h = z->h;
            break;
        }
    }
    if (slot == ENG_MAX_ZDEPTHS) {
        if (g.n_zdepths >= ENG_MAX_ZDEPTHS) return ENG_INVALID;
        slot = g.n_zdepths;
    }

    const u32 handle = g.be->depth_target_create(g.be->user, want_w, want_h);
    if (!handle)
        return (slot < g.n_zdepths && g.zdepths[slot].handle) ? slot : ENG_INVALID;

    eng_zdepth* z = &g.zdepths[slot];
    if (z->handle) g.be->depth_target_release(g.be->user, z->handle);
    if (z->snapshot && g.be->texture_release) g.be->texture_release(g.be->user, z->snapshot);
    if (z->ccopy && g.be->texture_release) g.be->texture_release(g.be->user, z->ccopy);
    z->location = location; z->offset = offset;
    eng_rt_note(location, offset);
    z->w = want_w; z->h = want_h;
    z->handle = handle;
    z->snapshot = 0;
    z->ccopy = 0;
    z->ccopy_valid = 0;
    z->cleared = 0;
    z->had_write = 0;
    z->snapshot_valid = 0;
    if (slot == g.n_zdepths) g.n_zdepths++;
    return slot;
}

/* A depth target read back as a texture, but only once the pass has executed
 * a depth-WRITING draw: a write-enable bit alone does not prove the pass
 * produced a usable depth map, and a clear-only zeta falls through to guest
 * memory instead (rsx_live_draw.c:6016-6018, 6265-6269). */
/* w x h is the region the texture reads, from the top-left corner. The depth
 * target is grown to cover the canvas (see eng_zdepth_get), so a 1024x1024
 * shadow map lives in a larger target, and its 0..1 texture coordinates only
 * address the map if the copy holds exactly that region. The copy it replaces
 * is retired, not destroyed: earlier draws in the stream still name it. */
static u32 eng_zdepth_snapshot(u32 slot, u32 w, u32 h)
{
    eng_zdepth* z = &g.zdepths[slot];
    if (!z->handle || !z->had_write) return 0;
    if (!w || w > z->w) w = z->w;
    if (!h || h > z->h) h = z->h;
    if (z->snapshot_valid && z->snapshot && z->snap_w == w && z->snap_h == h)
        return z->snapshot;
    if (!g.be->depth_snapshot) return 0;
    const u32 tex = g.be->depth_snapshot(g.be->user, z->handle, w, h);
    if (!tex) return 0;
    if (z->snapshot && g.be->texture_release) g.be->texture_release(g.be->user, z->snapshot);
    z->snapshot = tex;
    z->snap_w = w; z->snap_h = h;
    z->snapshot_valid = 1;
    return tex;
}

/* A zeta sampled as A8R8G8B8: inFamous' deferred lighting, fog and post read
 * 0x01010000 that way to rebuild positions (and stencil bit 0) from the depth
 * buffer. Like the D3D12 backend's colour copy it is refreshed whenever depth
 * or stencil changed since the last one (v49), and the zeta the draw is
 * testing against qualifies too: the copy is taken before the draw. */
static u32 eng_zdepth_color_copy(u32 slot, u32 w, u32 h)
{
    eng_zdepth* z = &g.zdepths[slot];
    if (!z->handle || !(z->cleared || z->had_write) || !g.be->depth_color_copy) return 0;
    if (!w || w > z->w) w = z->w;
    if (!h || h > z->h) h = z->h;
    if (z->ccopy_valid && z->ccopy && z->cc_w == w && z->cc_h == h) return z->ccopy;
    const u32 tex = g.be->depth_color_copy(g.be->user, z->handle, w, h);
    if (!tex) return 0;
    if (z->ccopy && g.be->texture_release) g.be->texture_release(g.be->user, z->ccopy);
    z->ccopy = tex;
    z->cc_w = w; z->cc_h = h;
    z->ccopy_valid = 1;
    { static int n = 0; if ((n < 6 && n++ < 6))
        fprintf(stderr, "[rsx engine] zeta 0x%08X sampled as A8R8G8B8 -> colour copy %ux%u\n",
                z->offset, w, h); }
    return tex;
}

static u32 s_bind_serial;

/* FEEDBACK_RUN=<N>, FEEDBACK_RUN_ANYFP, read as the D3D12 backend reads them
 * (rsx_d3d12_backend.c v156/v157): 0 or 1 refreshes the snapshot before every
 * such draw (exact); N > 1 lets a run of consecutive draws that sample the
 * target they write share one snapshot for up to N draws. inFamous' G-buffer
 * decals are ~170 such draws a city frame, each sampling two targets, and the
 * shipped launch env sets 1000. A copy is also a pass break on a tile GPU. */
static int s_fb_run = -1, s_fb_anyfp = 1;
static int eng_feedback_run(void)
{
    if (s_fb_run < 0) {
        const char* e = getenv("FEEDBACK_RUN");
        s_fb_run = e ? atoi(e) : 0;
        if (s_fb_run < 0) s_fb_run = 0;
        const char* a = getenv("FEEDBACK_RUN_ANYFP");
        s_fb_anyfp = (a && *a == '0') ? 0 : 1;
        fprintf(stderr, "[rsx engine] feedback snapshot refreshed every %d draw(s) of a %s run "
                        "(FEEDBACK_RUN; FEEDBACK_RUN_ANYFP=%d)\n",
                s_fb_run > 1 ? s_fb_run : 1, s_fb_anyfp ? "consecutive-feedback-draw" : "same-FP",
                s_fb_anyfp);
    }
    return s_fb_run;
}

/* The input of a draw that samples a target it is writing: a copy made right
 * before it, once per draw however many units read it (the D3D12 backend's
 * feedback snapshot), or the run's copy under FEEDBACK_RUN. */
static u32 eng_surface_feedback(u32 slot)
{
    eng_surface* s = &g.surfaces[slot];
    if (!s->handle || !g.be->surface_copy) return 0;
    if (s->feedback && s->fb_serial == s_bind_serial) return s->feedback;
    const int run = eng_feedback_run();
    u32 fp = 0;
    if (run > 1) {
        u32 fp_loc = 0;
        fp = rsx_dsp_fragment_program(&g.rsx, &fp_loc) | 1u;
        if (s->feedback && s->fb_frame == g.frames && s->fb_last + 1u == s_bind_serial &&
            (s_fb_anyfp || s->fb_fp == fp) && s->fb_run + 1u < (u32)run) {
            s->fb_run++;
            s->fb_last = s->fb_serial = s_bind_serial;
            s->fb_fp = fp;
            return s->feedback;
        }
    }
    s->fb_run = 0;
    s->fb_last = s_bind_serial;
    s->fb_fp = fp;
    s->fb_frame = g.frames;
    const u32 c = g.be->surface_copy(g.be->user, s->handle, s->feedback);
    if (!c) return 0;
    if (!s->feedback) {
        static int n = 0; if ((n < 6 && n++ < 6))
            fprintf(stderr, "[rsx engine] feedback copy of surface 0x%08X %ux%u\n",
                    s->offset, s->w, s->h);
    }
    s->feedback = c;
    s->fb_serial = s_bind_serial;
    return c;
}

/* ---- shadow maps --------------------------------------------------------- */

/* The D3D12 backend's switches, read the same way (rsx_d3d12_backend.c
 * shadows_on, v73c ZS_CWRITTEN, v28 SHADOW_LIT): SHADOWS=0 turns the compare
 * off, ZS_CWRITTEN=0 lets a never-written colour target win, SHADOW_LIT=0
 * leaves a depth unit with no depth target on its guest-memory texture. */
static int eng_env_flag(const char* name, int def)
{
    const char* e = getenv(name);
    if (!e || !*e) return def;
    return *e != '0';
}
static int eng_shadows_on(void)
{
    static int v = -1;
    if (v < 0) {
        v = eng_env_flag("SHADOWS", 1);
        fprintf(stderr, "[rsx engine] shadow maps %s (SHADOWS)\n", v ? "compared" : "off");
    }
    return v;
}
static int eng_feedback_on(void)
{
    static int v = -1;
    if (v < 0) {
        v = eng_env_flag("FEEDBACK_COPY", 1);
        fprintf(stderr, "[rsx engine] a draw sampling its own target reads a copy (FEEDBACK_COPY) %s\n",
                v ? "ON" : "off");
    }
    return v;
}

static int eng_zconv_on(void)
{
    static int v = -1;
    if (v < 0) {
        v = eng_env_flag("ZCONV", 1);
        fprintf(stderr, "[rsx engine] zeta sampled as A8R8G8B8 reads a colour copy (ZCONV) %s\n",
                v ? "ON" : "off");
    }
    return v;
}

static int eng_zs_cwritten(void)
{
    static int v = -1;
    if (v < 0) v = eng_env_flag("ZS_CWRITTEN", 1);
    return v;
}
static int eng_shadow_lit(void)
{
    static int v = -1;
    if (v < 0) v = eng_env_flag("SHADOW_LIT", 1);
    return v;
}

/* The compare and its 2x2 PCF, the D3D12 backend's s_shadow_fn (v34, v92,
 * v94, v148) unchanged: the RSX compares TEXEL func REF, so zfunc GREATER is
 * lit where the stored depth lies beyond the fragment. The texture is the
 * depth snapshot (R32Float) rather than an R24 view; both hold 0..1. */
static const char s_eng_shadow_fn[] =
    "float4 rsx_shadow(Texture2D t, SamplerState s, float4 c, int proj, int zf) {\n"
    "    float3 p = proj ? c.xyz / c.w : c.xyz;\n"
    "    uint W, H; t.GetDimensions(W, H);\n"
    "    float2 ts = float2(W, H);\n"
    "    float2 f = frac(p.xy * ts - 0.5);\n"
    "    float4 d = t.GatherRed(s, p.xy);\n"
    "#if RSX_SHRP\n"
    "    float3 dx = ddx(p), dy = ddy(p);\n"
    "    float det = dx.x * dy.y - dx.y * dy.x;\n"
    "    float2 g = (abs(det) > 1e-24) ? float2(dx.z * dy.y - dx.y * dy.z, dx.x * dy.z - dy.x * dx.z) / det : (float2)0;\n"
    "    float2 o = f / ts;\n"
    "    float4 du = float4(-o.x, 1.0 / ts.x - o.x, 1.0 / ts.x - o.x, -o.x);\n"
    "    float4 dv = float4(1.0 / ts.y - o.y, 1.0 / ts.y - o.y, -o.y, -o.y);\n"
    "    float4 rp = g.x * du + g.y * dv;\n"
    "    if (any(abs(rp) > RSX_SHRPMAX) || any(isnan(rp))) rp = (float4)0;\n"
    "    float4 z = p.zzzz + rp - RSX_SHBIAS;\n"
    "#else\n"
    "    float zs = max(abs(ddx(p.z)), abs(ddy(p.z)));\n"
    "    float4 z = p.zzzz - (RSX_SHBIAS + RSX_SHSLOPE * min(zs, 0.01));\n"
    "#endif\n"
    "    float4 r = (float4)0;\n"
    "    if (zf == 1) r = (float4)(d < z); else if (zf == 2) r = (float4)(d == z);\n"
    "    else if (zf == 3) r = (float4)(d <= z); else if (zf == 4) r = (float4)(d > z);\n"
    "    else if (zf == 5) r = (float4)(d != z); else if (zf == 6) r = (float4)(d >= z);\n"
    "    else if (zf == 7) r = (float4)1;\n"
    "    float lit = lerp(lerp(r.w, r.z, f.x), lerp(r.x, r.y, f.x), f.y);\n"
    "    return lit.xxxx;\n"
    "}\n";

/* Every 2D sample of a compare unit becomes rsx_shadow(...): the D3D12
 * backend's fp_shadow_rewrite, with the same SHADOW_FLIP, SHADOW_RP,
 * SHADOW_RP_BIAS, SHADOW_RP_MAX, SHADOW_BIAS and SHADOW_SLOPE switches.
 * Returns how many samples it rewrote, or -1 when the program would not fit. */
static int eng_fp_shadow_rewrite(char* hlsl, size_t cap, u32 shkey)
{
    int patched = 0;
    static int flip = -1;
    if (flip < 0) flip = eng_env_flag("SHADOW_FLIP", 0);
    for (int u = 0; u < 4; u++) {
        if (!((shkey >> u) & 1u)) continue;
        int zf = (int)((shkey >> (4 + 3 * u)) & 7u);
        if (flip) { static const int sw[8] = {0, 4, 2, 6, 1, 5, 3, 7}; zf = sw[zf]; }
        char needles[2][48];
        snprintf(needles[0], sizeof needles[0], "rsx_tex[%d].Sample(rsx_samp[%d], ", u, u);
        snprintf(needles[1], sizeof needles[1], "rsx_tex%d.Sample(rsx_samp[%d], ", u, u);
        for (int ni = 0; ni < 2; ni++) {
            char* from = hlsl;
            for (;;) {
                char* at = strstr(from, needles[ni]);
                if (!at) break;
                char* a = at + strlen(needles[ni]);          /* the coordinate */
                if (*a != '(') { from = a; continue; }
                char* q = a; int dep = 0;
                for (; *q; q++) { if (*q == '(') dep++; else if (*q == ')') { if (--dep == 0) break; } }
                if (!*q) break;
                char* g1e = q + 1;                           /* one past group 1 */
                int proj = 0; char* tail = NULL;
                if (strncmp(g1e, ".xy)", 4) == 0) tail = g1e + 4;
                else if (strncmp(g1e, ".xy / (", 7) == 0) {
                    char* r = g1e + 6; dep = 0;
                    for (; *r; r++) { if (*r == '(') dep++; else if (*r == ')') { if (--dep == 0) break; } }
                    if (!*r || strncmp(r + 1, ".w)", 3) != 0) { from = g1e; continue; }
                    proj = 1; tail = r + 4;
                } else { from = g1e; continue; }
                char head[64], trail[32], rbuf[1024];
                snprintf(head, sizeof head, ni ? "rsx_shadow(rsx_tex%d, rsx_samp[%d], " : "rsx_shadow(rsx_tex[%d], rsx_samp[%d], ", u, u);
                snprintf(trail, sizeof trail, ", %d, %d)", proj, zf);
                const size_t hl = strlen(head), gl = (size_t)(g1e - a), tl = strlen(trail);
                const size_t newlen = hl + gl + tl, oldlen = (size_t)(tail - at), used = strlen(hlsl);
                if (newlen >= sizeof rbuf) { from = tail; continue; }
                if (used - oldlen + newlen + 1 > cap) return -1;
                memcpy(rbuf, head, hl); memcpy(rbuf + hl, a, gl); memcpy(rbuf + hl + gl, trail, tl); rbuf[newlen] = 0;
                memmove(at + newlen, tail, strlen(tail) + 1);
                memcpy(at, rbuf, newlen);
                from = at + newlen;
                patched++;
            }
        }
    }
    if (patched) {   /* the helper references no globals: put it first */
        static char bias_hdr[192] = { 0 };
        if (!bias_hdr[0]) {
            if (eng_env_flag("SHADOW_RP", 1)) {
                const char* e = getenv("SHADOW_RP_BIAS"); double b = e ? atof(e) : 0.0003;
                const char* em = getenv("SHADOW_RP_MAX"); double mx = em ? atof(em) : 0.003;
                snprintf(bias_hdr, sizeof bias_hdr, "#define RSX_SHRP 1\n#define RSX_SHBIAS %.7f\n#define RSX_SHRPMAX %.7f\n", b, mx);
            } else {
                const char* e = getenv("SHADOW_BIAS"); double b = e ? atof(e) : 0.0015;
                const char* e2 = getenv("SHADOW_SLOPE"); double sl = e2 ? atof(e2) : 1.5;
                snprintf(bias_hdr, sizeof bias_hdr, "#define RSX_SHRP 0\n#define RSX_SHBIAS %.7f\n#define RSX_SHSLOPE %.4f\n", b, sl);
            }
            fprintf(stderr, "[rsx engine] shadow compare: %s", bias_hdr);
        }
        const size_t bl = strlen(bias_hdr);
        const size_t fl = sizeof(s_eng_shadow_fn) - 1, used = strlen(hlsl);
        if (used + fl + bl + 1 > cap) return -1;
        memmove(hlsl + fl + bl, hlsl, used + 1);
        memcpy(hlsl, bias_hdr, bl);
        memcpy(hlsl + bl, s_eng_shadow_fn, fl);
    }
    return patched;
}

/* Every occurrence of find in buf replaced with repl, while it fits. */
static void eng_hlsl_replace_all(char* buf, size_t cap, const char* find, const char* repl)
{
    const size_t fl = strlen(find), rl = strlen(repl);
    if (!fl) return;
    char* at = buf;
    while ((at = strstr(at, find)) != NULL) {
        const size_t used = strlen(buf) + 1;
        if (used + rl - fl > cap) return;
        memmove(at + rl, at + fl, used - (size_t)(at - buf) - fl);
        memcpy(at, repl, rl);
        at += rl;
    }
}

/* The D3D12 backend's v170 SUN_NDL_CLAMP (default 1; 0 = off). inFamous' sun-
 * cascade light FPs compute shadow * dot(N,-L) with N.L not clamped, and rely
 * on a back-facing surface comparing as shadowed. The shadow compare here is
 * biased toward lit (the same v92/v148 biases), so a wall facing away from the
 * sun got a negative sun term: navy walls. max(N.L,0) gives the shadowed result
 * there and changes nothing on sun-facing surfaces. Called only for draws with
 * shadow units, as D3D12 does. */
static void eng_fp_sun_ndl_clamp(char* hlsl, size_t cap)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("SUN_NDL_CLAMP"); on = (e && *e == '0') ? 0 : 1; }
    static const char from[] = "(((r[2]).xyzw) * ((h[0]).xyzw)); h[4].w = _v.w;";
    static const char to[]   = "(((r[2]).xyzw) * max((h[0]).xyzw, 0.0)); h[4].w = _v.w;";
    if (!on || !strstr(hlsl, from) || !strstr(hlsl, "dot(((h[4]).xyzw).xyz, (-(")) return;
    eng_hlsl_replace_all(hlsl, cap, from, to);
    static u32 logs = 0;
    if (logs < 8) { logs++; fprintf(stderr, "[rsx engine] SUN_NDL_CLAMP: sun term clamped to shadow * max(N.L,0)\n"); }
}

/* ---- depth bounds ---------------------------------------------------------- */

/* The D3D12 backend's v35 DEPTH_BOUNDS (default 1; 0 = ignore the guest's test).
 * DEPTH_BOUNDS_TEST_ENABLE (0x0380) passes a fragment only where the depth
 * ALREADY STORED at its pixel lies in [DEPTH_BOUNDS_MIN, MAX] (0x0384/0x0388).
 * inFamous' deferred point lights rely on it: each light volume is limited to
 * the depth slice the light reaches. D3D12 has the test in hardware
 * (OMSetDepthBounds); Metal has none, so a draw that enables it gets a snapshot
 * of its zeta on a spare unit and the fragment program discards outside the
 * bounds, which ride in fp_alpha.y/z. */
#define ENG_DBT_UNIT 15u
static int eng_dbt_on(void)
{
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("DEPTH_BOUNDS");
        v = (e && *e == '0') ? 0 : 1;
        fprintf(stderr, "[rsx engine] depth bounds test %s (DEPTH_BOUNDS)\n", v ? "ON" : "off");
    }
    return v;
}

/* The test as the first statement of main. 0 = done, -1 = no room / no main. */
static int eng_fp_dbt_rewrite(char* hlsl, size_t cap, u32 cube_mask)
{
    if ((cube_mask >> ENG_DBT_UNIT) & 1u) return -1;
    char* m = strstr(hlsl, " main(PSInput input)");
    if (!m) return -1;
    char* body = strchr(m, '{');
    if (!body) return -1;
    body++;
    if (*body == '\n') body++;
    char ins[192];
    snprintf(ins, sizeof ins,
             "    { float _rsx_dz = %s%u%s.Load(int3(int2(input.position.xy), 0)).r;"
             " if (_rsx_dz < fp_alpha.y || _rsx_dz > fp_alpha.z) discard; }\n",
             cube_mask ? "rsx_tex" : "rsx_tex[", ENG_DBT_UNIT, cube_mask ? "" : "]");
    const size_t il = strlen(ins), used = strlen(hlsl);
    if (used + il + 1 > cap) return -1;
    memmove(body + il, body, used + 1 - (size_t)(body - hlsl));
    memcpy(body, ins, il);
    return 0;
}

/* ---- texture value transforms -------------------------------------------- */

/* The D3D12 backend's v89 TEX_XF (rsx_d3d12_backend.c s_txf_fn,
 * fp_txf_rewrite, d3d12_bind_texture), same switch, same default (on).
 * TEXTURE_ADDRESS bits 12-15 unsigned_remap (1 = BIASED: texel * 2 - 1 on
 * RGB, the "_bx2" expand inFamous' deferred lights use for G-buffer normals)
 * and bits 20-23 gamma (sRGB -> linear on RGB). Without them albedo is read
 * as linear: a washed-out, too-bright, desaturated scene. Per draw, a 2-bit
 * mode per unit 0-7 (bit0 expand, bit1 sRGB) is part of the pipeline key. */
static int eng_tex_xf_on(void)
{
    static int v = -1;
    if (v < 0) {
        v = eng_env_flag("TEX_XF", 1);
        fprintf(stderr, "[rsx engine] texture unsigned-remap (bx2) + gamma (sRGB) %s (TEX_XF)\n",
                v ? "ON" : "off");
    }
    return v;
}

/* One unit's mode from its TEXTURE_ADDRESS word; depth compares never. */
static u32 eng_tex_xf_mode(u32 unit, const rsx_dsp_texture* t)
{
    if (!eng_tex_xf_on()) return 0;
    const u32 base_fmt = t->format & RSX_TEX_FMT_BASE_MASK & ~(u32)RSX_TEX_FMT_UNNORM & ~(u32)RSX_TEX_FMT_LINEAR;
    const u32 ur = (t->wrap >> 12) & 0xFu, ga = (t->wrap >> 20) & 0xFu, sr = (t->wrap >> 24) & 0xFu;
    u32 m = (ur == 1u ? 1u : 0u) | (ga ? 2u : 0u);
    if (base_fmt >= RSX_TEX_FMT_DEPTH24_D8 && base_fmt <= RSX_TEX_FMT_DEPTH24_D8 + 3u) m = 0;
    if (ur || ga || sr) {
        static u32 seen[48]; static int ns = 0;
        const u32 key = (unit << 24) | (base_fmt << 16) | (ur << 8) | (ga << 4) | sr;
        int known = 0;
        for (int i = 0; i < ns; i++) if (seen[i] == key) known = 1;
        if (!known && ns < 48) {
            seen[ns++] = key;
            fprintf(stderr, "[rsx engine] TEX_XF unit %u fmt 0x%02X %ux%u: unsigned_remap %u gamma 0x%X"
                            " signed_remap 0x%X (address 0x%08X) -> mode %u\n",
                    unit, base_fmt, t->width, t->height, ur, ga, sr, t->wrap, m);
        }
    }
    return m;
}

static const char s_eng_txf_fn[] =
    "float4 rsx_txf(float4 c, int m) {\n"
    "  if (m & 2) c.rgb = (c.rgb <= 0.04045) ? c.rgb / 12.92 : pow(max((c.rgb + 0.055) / 1.055, 0.0), 2.4);\n"
    "  if (m & 1) c.rgb = c.rgb * 2.0 - 1.0;\n"
    "  return c;\n}\n";

/* Every Sample / SampleBias / SampleLevel of a unit with a mode becomes
 * rsx_txf(<sample>, mode). Returns how many it wrapped, or -1 when the
 * program would not fit. */
static int eng_fp_txf_rewrite(char* hlsl, size_t cap, u32 texk)
{
    int patched = 0;
    static const char* const meth[3] = { "Sample(", "SampleBias(", "SampleLevel(" };
    for (int u = 0; u < 8; u++) {
        const int m = (int)((texk >> (2 * u)) & 3u);
        if (!m) continue;
        for (int form = 0; form < 2; form++)
        for (int mi = 0; mi < 3; mi++) {
            char nd[48];
            snprintf(nd, sizeof nd, form ? "rsx_tex%d.%s" : "rsx_tex[%d].%s", u, meth[mi]);
            char* from = hlsl;
            for (;;) {
                char* at = strstr(from, nd);
                if (!at) break;
                /* rsx_tex1 never matches rsx_tex10..15: the needle ends in '.' */
                char* q = at + strlen(nd) - 1;              /* the '(' */
                int dep = 0;
                for (; *q; q++) { if (*q == '(') dep++; else if (*q == ')') { if (--dep == 0) break; } }
                if (!*q) break;
                char* end = q + 1;
                char trail[16];
                snprintf(trail, sizeof trail, ", %d)", m);
                const size_t hl = 8 /* "rsx_txf(" */, tl = strlen(trail), used = strlen(hlsl);
                if (used + hl + tl + 1 > cap) return -1;
                memmove(end + tl, end, strlen(end) + 1);
                memcpy(end, trail, tl);
                memmove(at + hl, at, strlen(at) + 1);
                memcpy(at, "rsx_txf(", hl);
                from = end + hl + tl;
                patched++;
            }
        }
    }
    if (patched) {
        const size_t fl = sizeof(s_eng_txf_fn) - 1, used = strlen(hlsl);
        if (used + fl + 1 > cap) return -1;
        memmove(hlsl + fl, hlsl, used + 1);
        memcpy(hlsl, s_eng_txf_fn, fl);
    }
    return patched;
}

/* ---- textures ------------------------------------------------------------ */

static rsx_be_format eng_texfmt(rsx_texfmt f)
{
    switch (f) {
    case RSX_TEXFMT_R8:              return RSX_BE_FMT_R8;
    case RSX_TEXFMT_R8G8:            return RSX_BE_FMT_R8G8;
    case RSX_TEXFMT_R8G8B8A8:        return RSX_BE_FMT_R8G8B8A8;
    case RSX_TEXFMT_BC1:             return RSX_BE_FMT_BC1;
    case RSX_TEXFMT_BC2:             return RSX_BE_FMT_BC2;
    case RSX_TEXFMT_BC3:             return RSX_BE_FMT_BC3;
    case RSX_TEXFMT_R16:             return RSX_BE_FMT_R16;
    case RSX_TEXFMT_R16G16:          return RSX_BE_FMT_R16G16;
    case RSX_TEXFMT_R16G16F:         return RSX_BE_FMT_R16G16F;
    case RSX_TEXFMT_R16G16B16A16F:   return RSX_BE_FMT_R16G16B16A16F;
    case RSX_TEXFMT_R32F:            return RSX_BE_FMT_R32F;
    case RSX_TEXFMT_R32G32B32A32F:   return RSX_BE_FMT_R32G32B32A32F;
    default:                         return RSX_BE_FMT_R8;
    }
}

/* How many guest bytes a texture occupies: the whole mip chain, times six for
 * a cube map. That is what the content hash has to cover, or a title
 * animating one face or one lower level keeps its stale upload. */
static u32 eng_texture_span(u32 fmt, u32 w, u32 h, u32 levels, u32 pitch, int cube)
{
    if (cube) return rsx_texture_cube_face_stride(fmt, w, h, levels, pitch) * 6u;
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 n = rsx_texture_mip_chain(fmt, w, h, levels, pitch, lv);
    if (!n) return 0;
    return lv[n - 1].offset + lv[n - 1].tl.face_bytes;
}

/* One check per cached texture per presented frame -- a mutation detector, not
 * a content id (rsx_live_draw.c:2002-2026).
 *
 * Default: the D3D12 backend's sampled checksum (rsx_d3d12_backend.c tex_csum /
 * tex_csum_dense / tex_csum_faces, the path inFamous ships on): ~1024 words of
 * level 0, of every face for a cube, and 16x denser for 8-bit planes (Bink
 * Y/U/V video frames). M7 profile: hashing every byte of every bound texture
 * every frame was ~10% of the render thread.
 * PS3RECOMP_ENGINE_TEX_FULLHASH=1: the old word-at-a-time FNV over the whole
 * span (all levels, all faces), for chasing a stale texture. */
/* Eight loads in flight (the D3D12 backend's TEX_CSUM_MLP, rsx_d3d12_backend.c
 * v234, on in the shipped Windows launch env): the same sample offsets and the
 * same FNV order, but eight independent words are loaded before any is hashed,
 * so a large texture's ~1024 cache-line misses overlap instead of forming one
 * load/hash chain. The scalar tail keeps the result bit-identical. iPad
 * profile: the one-at-a-time sample load was ~10% of the RSX thread.
 * Apple default on; PS3RECOMP_ENGINE_TEX_CSUM_MLP=0 off (=1 on elsewhere). */
static int eng_csum_mlp_on(void)
{
    static int on = -1;
    if (__builtin_expect(on < 0, 0)) {
        const char* e = getenv("PS3RECOMP_ENGINE_TEX_CSUM_MLP");
#if defined(__APPLE__)
        on = !(e && *e == '0');
#else
        on = (e && *e == '1');
#endif
    }
    return on;
}
static u32 eng_csum_step(const u8* base, u32 nbytes, u32 step)
{
    u32 h = 2166136261u, i = 0;
    if (eng_csum_mlp_on()) {
        const u64 span = (u64)step * 7u + 3u;
        for (; (u64)i + span < nbytes; i += step * 8u) {
            u32 w0, w1, w2, w3, w4, w5, w6, w7;
            memcpy(&w0, base + i,            sizeof w0);
            memcpy(&w1, base + i + step,     sizeof w1);
            memcpy(&w2, base + i + step * 2, sizeof w2);
            memcpy(&w3, base + i + step * 3, sizeof w3);
            memcpy(&w4, base + i + step * 4, sizeof w4);
            memcpy(&w5, base + i + step * 5, sizeof w5);
            memcpy(&w6, base + i + step * 6, sizeof w6);
            memcpy(&w7, base + i + step * 7, sizeof w7);
            h ^= w0; h *= 16777619u;
            h ^= w1; h *= 16777619u;
            h ^= w2; h *= 16777619u;
            h ^= w3; h *= 16777619u;
            h ^= w4; h *= 16777619u;
            h ^= w5; h *= 16777619u;
            h ^= w6; h *= 16777619u;
            h ^= w7; h *= 16777619u;
        }
    }
    for (; i + 3 < nbytes; i += step) {
        u32 w32; memcpy(&w32, base + i, sizeof w32);
        h ^= w32; h *= 16777619u;
    }
    return h;
}
static u32 eng_csum(const u8* base, u32 nbytes)
{
    return eng_csum_step(base, nbytes, nbytes > 4096u ? nbytes / 1024u : 4u);
}
static u32 eng_csum_dense(const u8* base, u32 nbytes)
{
    u32 step = nbytes > 65536u ? (nbytes / 16384u) & ~3u : 4u;
    if (!step) step = 4u;
    return eng_csum_step(base, nbytes, step);
}
static int eng_tex_fullhash_on(void)
{
    static int full = -1;
    if (full < 0) {
        const char* e = getenv("PS3RECOMP_ENGINE_TEX_FULLHASH");
        full = (e && *e && *e != '0') ? 1 : 0;
    }
    return full;
}
/* The sampled checksum of a resolved source. Pure: it reads src[0..span) and
 * nothing else, so a worker thread computes exactly what the RSX thread would
 * (TEXCS below). */
static u32 eng_texture_csum_src(const u8* src, u32 fmt, u32 w, u32 h,
                                u32 levels, u32 pitch, int cube, u32 span)
{
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 n = rsx_texture_mip_chain(fmt, w, h, levels, pitch, lv);
    u32 nb = n ? lv[0].tl.face_bytes : span;
    if (nb > span) nb = span;
    if ((fmt & 0x9Fu) == 0x81u) return eng_csum_dense(src, nb);     /* B8 */
    u32 hash = eng_csum(src, nb);
    if (cube) {
        const u32 fs = rsx_texture_cube_face_stride(fmt, w, h, levels, pitch);
        for (u32 f = 1; f < 6; f++)
            if ((u64)f * fs + nb <= span)
                hash = (hash ^ eng_csum(src + (size_t)f * fs, nb)) * 16777619u;
    }
    return hash;
}
static u64 eng_texture_content_hash(u32 location, u32 offset, u32 fmt, u32 w,
                                    u32 h, u32 levels, u32 pitch, int cube,
                                    u32 span, int* readable)
{
    const u8* src = span ? eng_guest_ptr(NULL, location, offset, span) : NULL;
    if (!src) { *readable = 0; return 0; }
    *readable = 1;
    if (eng_tex_fullhash_on()) {
        u64 hash = 1469598103934665603ull;
        u32 i = 0;
        for (; i + 8 <= span; i += 8) {
            u64 word;
            memcpy(&word, src + i, sizeof(word));
            hash ^= word;
            hash *= 1099511628211ull;
        }
        for (; i < span; i++) { hash ^= src[i]; hash *= 1099511628211ull; }
        return hash;
    }
    return eng_texture_csum_src(src, fmt, w, h, levels, pitch, cube, span);
}

/* ---- TEXCS: the frame's texture checksums on worker threads (Apple) --------
 * The D3D12 backend's v228 TEXCS (rsx_d3d12_backend.c, tcs_*). Every cached
 * texture bound in a frame is re-validated once per frame by the sampled
 * checksum above: ~1000 textures x ~1024 scattered loads, issued one texture
 * after another by the RSX thread, which sets the pace on the iPad (profile:
 * ~15% of it). D3D12 runs the same checksum functions on workers at its
 * prepass, once the whole frame is recorded. This engine has no prepass (it
 * validates at the first bind), so the job list is the textures validated in
 * the previous frame, and a worker's value is used only when it was computed
 * from data at least as new as what the bind may rely on:
 *  - rsx_draw_engine_data_epoch() is called by the FIFO walker when it reads a
 *    new put (it then executes only commands up to that put) and at every
 *    NV406E semaphore acquire (SPU-released data; the same two points as the
 *    D3D12 VC_WW epoch). The RSX's own 2D writes (NV308A, NV3089) count too.
 *  - Textures whose content changed in the last 16 frames get no job: the
 *    game rewrites a few every frame between two puts (eng_tcs_volatile).
 *  - The first validation of an epoch (and of each frame) publishes the jobs
 *    (seq_cst, after the walker's acquire of put / the label), so every
 *    worker load happens after the data published by that put or label.
 *  - A bind takes a value only if its job is of the current epoch and has
 *    exactly the bind's inputs (pointer, format, size, levels, pitch, cube,
 *    span); otherwise it computes inline, as before.
 * Values are bit-identical to the inline path (same function, same bytes);
 * the read only moves earlier within the same epoch, as in D3D12. The RSX
 * thread never waits for a worker.
 * PS3RECOMP_ENGINE_TEXCS=0 off (Apple default on; not built elsewhere),
 * PS3RECOMP_ENGINE_TEXCS_THREADS=<n> (default 3, utility QoS = E cores),
 * PS3RECOMP_ENGINE_TEXCS_VERIFY=1 recomputes every taken value and counts
 * differences, PS3RECOMP_ENGINE_TEXCS_STATS=1 logs [TEXCS] every 600 frames. */
#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#include <stdatomic.h>
typedef struct {
    const u8* src;
    u32 fmt, w, h, levels, pitch, span, cube;
    _Atomic u32 state;          /* 0 pending, 1 claimed, 2 done (res valid) */
    u32 res;
} eng_tcs_job;
static eng_tcs_job s_tcs_j[2][ENG_MAX_TEXTURES];   /* by generation parity */
static _Atomic u64 s_tcs_ctr;   /* gen (bits 32..63) | n (16..31) | next (0..15) */
static _Atomic int s_tcs_in[2]; /* workers inside the array of that parity */
static pthread_mutex_t s_tcs_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_tcs_cv = PTHREAD_COND_INITIALIZER;
static u32 s_tcs_wake;          /* under s_tcs_mx */
static int s_tcs_cfg = -1, s_tcs_nthr = 3, s_tcs_verify, s_tcs_stats;
static _Atomic u32 s_tcs_epoch = 1;
/* RSX thread only from here */
static u32 s_tcs_kick_epoch, s_tcs_kick_frame, s_tcs_gen;
static u32 s_tcs_slot_gen[ENG_MAX_TEXTURES];
static u16 s_tcs_slot_idx[ENG_MAX_TEXTURES];
static u32 s_tcs_chg[ENG_MAX_TEXTURES];         /* frame + 1 of the slot's last content change */
static u16 s_tcs_used[2][ENG_MAX_TEXTURES];        /* slots validated, by frame parity */
static u32 s_tcs_used_n[2], s_tcs_used_frame[2] = { ~0u, ~0u };
static struct { u64 kicks, jobs, take, self, busy, stale, miss, skip_in, checked, differ; } s_tcs_st;
static _Atomic u64 s_tcs_byw;

static void eng_tcs_run(u32 gen)
{
    eng_tcs_job* const a = s_tcs_j[gen & 1u];
    for (;;) {
        u64 v = atomic_load(&s_tcs_ctr);
        if ((u32)(v >> 32) != gen) return;
        const u32 nx = (u32)v & 0xFFFFu, n = (u32)(v >> 16) & 0xFFFFu;
        if (nx >= n) return;
        if (!atomic_compare_exchange_weak(&s_tcs_ctr, &v, v + 1)) continue;
        eng_tcs_job* const j = &a[nx];
        u32 exp = 0;
        if (!atomic_compare_exchange_strong(&j->state, &exp, 1u)) continue;
        j->res = eng_texture_csum_src(j->src, j->fmt, j->w, j->h, j->levels,
                                      j->pitch, (int)j->cube, j->span);
        atomic_store_explicit(&j->state, 2u, memory_order_release);
        atomic_fetch_add_explicit(&s_tcs_byw, 1, memory_order_relaxed);
    }
}

static void* eng_tcs_worker(void* p)
{
    (void)p;
    pthread_setname_np("eng texcs");
    u32 seen = 0;
    for (;;) {
        pthread_mutex_lock(&s_tcs_mx);
        while (s_tcs_wake == seen) pthread_cond_wait(&s_tcs_cv, &s_tcs_mx);
        const u32 gen = s_tcs_wake;
        pthread_mutex_unlock(&s_tcs_mx);
        seen = gen;
        atomic_fetch_add(&s_tcs_in[gen & 1u], 1);
        eng_tcs_run(gen);
        atomic_fetch_sub(&s_tcs_in[gen & 1u], 1);
    }
    return NULL;
}

static int eng_tcs_on(void)
{
    if (__builtin_expect(s_tcs_cfg < 0, 0)) {
        const char* e = getenv("PS3RECOMP_ENGINE_TEXCS");
        const char* t = getenv("PS3RECOMP_ENGINE_TEXCS_THREADS");
        const char* v = getenv("PS3RECOMP_ENGINE_TEXCS_VERIFY");
        const char* s = getenv("PS3RECOMP_ENGINE_TEXCS_STATS");
        s_tcs_cfg = !(e && *e == '0') && !eng_tex_fullhash_on();
        if (t && *t) { s_tcs_nthr = atoi(t); if (s_tcs_nthr < 0) s_tcs_nthr = 0; if (s_tcs_nthr > 8) s_tcs_nthr = 8; }
        s_tcs_verify = v && *v == '1';
        s_tcs_stats = s && *s == '1';
        (void)eng_csum_mlp_on();   /* its lazy init runs here, before any worker */
        if (s_tcs_cfg) {
            for (int i = 0; i < s_tcs_nthr; i++) {
                pthread_attr_t at; pthread_t th;
                pthread_attr_init(&at);
                pthread_attr_setstacksize(&at, 256u << 10);
                pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
                pthread_attr_set_qos_class_np(&at, QOS_CLASS_UTILITY, 0);
                const int rc = pthread_create(&th, &at, eng_tcs_worker, NULL);
                pthread_attr_destroy(&at);
                if (rc) { s_tcs_nthr = i; break; }
            }
        }
        fprintf(stderr, "[TEXCS] parallel texture checksums: %s, %d workers "
                "(PS3RECOMP_ENGINE_TEXCS, _THREADS)%s\n", s_tcs_cfg ? "ON" : "off",
                s_tcs_cfg ? s_tcs_nthr : 0, s_tcs_verify ? ", VERIFY" : "");
    }
    return s_tcs_cfg;
}

/* a slot validated this frame: next frame's job list */
static void eng_tcs_note(u32 slot)
{
    const u32 p = g.frames & 1u;
    if (s_tcs_used_frame[p] != g.frames) { s_tcs_used_frame[p] = g.frames; s_tcs_used_n[p] = 0; }
    if (s_tcs_used_n[p] < ENG_MAX_TEXTURES) s_tcs_used[p][s_tcs_used_n[p]++] = (u16)slot;
}

/* Textures whose content changed in the last 16 frames are validated inline
 * only: a texture the game rewrites every frame (verify run: a 64x64 Y16_X16
 * normal map and a 64x36 map, rewritten mid-frame with no put or semaphore
 * between) would otherwise be checked against a snapshot older than the bind. */
static int eng_tcs_volatile(u32 slot)
{
    return s_tcs_chg[slot] && g.frames + 1u - s_tcs_chg[slot] < 16u;
}

/* publish this epoch's jobs: last frame's validated slots not yet validated
 * in this frame (except the one the caller computes now) */
static void eng_tcs_kick(u32 except)
{
    const u32 gen = (s_tcs_gen + 1u) ? s_tcs_gen + 1u : 1u;
    const u32 par = gen & 1u;
    if (atomic_load(&s_tcs_in[par])) {   /* a worker from two kicks ago is still inside */
        atomic_store(&s_tcs_ctr, (u64)s_tcs_gen << 32);   /* close the stale generation */
        s_tcs_st.skip_in++;
        return;
    }
    s_tcs_gen = gen;
    s_tcs_kick_epoch = atomic_load_explicit(&s_tcs_epoch, memory_order_relaxed);
    s_tcs_kick_frame = g.frames;
    eng_tcs_job* const a = s_tcs_j[par];
    u32 n = 0;
    const u32 pf = g.frames - 1u, pp = pf & 1u;
    if (s_tcs_used_frame[pp] == pf) {
        for (u32 k = 0; k < s_tcs_used_n[pp]; k++) {
            const u32 i = s_tcs_used[pp][k];
            if (i == except || i >= g.n_textures || s_tcs_slot_gen[i] == gen) continue;
            const eng_texture* e = &g.textures[i];
            if (!e->handle || !e->span || e->last_hash_frame == g.frames || eng_tcs_volatile(i)) continue;
            const u8* src = eng_guest_ptr(NULL, e->location, e->offset, e->span);
            if (!src) continue;
            eng_tcs_job* const j = &a[n];
            j->src = src; j->fmt = e->format; j->w = e->width; j->h = e->height;
            j->levels = e->span_levels; j->pitch = e->pitch; j->span = e->span;
            j->cube = e->cubemap;
            atomic_store_explicit(&j->state, 0u, memory_order_relaxed);
            s_tcs_slot_gen[i] = gen; s_tcs_slot_idx[i] = (u16)n;
            n++;
        }
    }
    atomic_store(&s_tcs_ctr, ((u64)gen << 32) | ((u64)n << 16));
    s_tcs_st.kicks++; s_tcs_st.jobs += n;
    if (n && s_tcs_nthr) {
        pthread_mutex_lock(&s_tcs_mx);
        s_tcs_wake = gen;
        pthread_cond_broadcast(&s_tcs_cv);
        pthread_mutex_unlock(&s_tcs_mx);
    }
}

/* slot i's checksum if a worker computed it for exactly these inputs in this
 * epoch; 0 = compute it inline */
static int eng_tcs_take(u32 i, const u8* src, u32 fmt, u32 w, u32 h, u32 levels,
                        u32 pitch, int cube, u32 span, u32* cs)
{
    if (atomic_load_explicit(&s_tcs_epoch, memory_order_relaxed) != s_tcs_kick_epoch ||
        g.frames != s_tcs_kick_frame) {   /* new data or a new frame: new jobs */
        eng_tcs_kick(i); s_tcs_st.stale++;
        return 0;
    }
    if (s_tcs_slot_gen[i] != s_tcs_gen) { s_tcs_st.miss++; return 0; }
    s_tcs_slot_gen[i] = 0;
    eng_tcs_job* const j = &s_tcs_j[s_tcs_gen & 1u][s_tcs_slot_idx[i]];
    if (j->src != src || j->fmt != fmt || j->w != w || j->h != h ||
        j->levels != levels || j->pitch != pitch || j->span != span ||
        j->cube != (u32)(cube != 0)) { s_tcs_st.miss++; return 0; }
    u32 st = atomic_load_explicit(&j->state, memory_order_acquire);
    if (st == 0u && atomic_compare_exchange_strong(&j->state, &st, 1u)) {
        s_tcs_st.self++;                     /* not started: computed inline */
        return 0;
    }
    if (st == 2u) { *cs = j->res; s_tcs_st.take++; return 1; }
    s_tcs_st.busy++;                         /* a worker is on it: no waiting */
    return 0;
}

static void eng_tcs_frame(void)
{
    if (!s_tcs_stats || s_tcs_cfg <= 0 || (g.frames % 600u) != 599u) return;
    fprintf(stderr, "[TEXCS] 600 frames: %llu kicks, %llu jobs (%llu by workers), "
            "%llu taken, %llu inline (%llu not started, %llu busy, %llu first of "
            "epoch/frame, %llu no job), %llu straggler skips, verify %llu/%llu differ\n",
            (unsigned long long)s_tcs_st.kicks, (unsigned long long)s_tcs_st.jobs,
            (unsigned long long)atomic_exchange(&s_tcs_byw, 0),
            (unsigned long long)s_tcs_st.take,
            (unsigned long long)(s_tcs_st.self + s_tcs_st.busy + s_tcs_st.stale + s_tcs_st.miss),
            (unsigned long long)s_tcs_st.self, (unsigned long long)s_tcs_st.busy,
            (unsigned long long)s_tcs_st.stale, (unsigned long long)s_tcs_st.miss,
            (unsigned long long)s_tcs_st.skip_in,
            (unsigned long long)s_tcs_st.differ, (unsigned long long)s_tcs_st.checked);
    memset(&s_tcs_st, 0, sizeof s_tcs_st);
}
#endif /* __APPLE__ */

void rsx_draw_engine_data_epoch(void)
{
#if defined(__APPLE__)
    atomic_fetch_add_explicit(&s_tcs_epoch, 1u, memory_order_relaxed);
#endif
}

/* The once-per-frame validation checksum of cached slot `slot`; same contract
 * as eng_texture_content_hash. */
static u64 eng_texture_validate_hash(u32 slot, u32 location, u32 offset, u32 fmt,
                                     u32 w, u32 h, u32 levels, u32 pitch,
                                     int cube, u32 span, int* readable)
{
#if defined(__APPLE__)
    if (eng_tcs_on()) {
        eng_tcs_note(slot);
        const u8* src = span ? eng_guest_ptr(NULL, location, offset, span) : NULL;
        if (!src) { *readable = 0; return 0; }
        *readable = 1;
        u32 cs;
        if (eng_tcs_take(slot, src, fmt, w, h, levels, pitch, cube, span, &cs)) {
            if (s_tcs_verify) {
                s_tcs_st.checked++;
                if (cs != eng_texture_csum_src(src, fmt, w, h, levels, pitch, cube, span)) {
                    if (s_tcs_st.differ < 4 && (g.frames % 600u) < 300u)
                        fprintf(stderr, "[TEXCS] differ: frame %u slot %u loc %u off 0x%08X "
                                "fmt 0x%02X %ux%u lv %u cube %d span %u\n", g.frames, slot,
                                location, offset, fmt, w, h, levels, cube, span);
                    s_tcs_st.differ++;
                }
            }
            return cs;
        }
        return eng_texture_csum_src(src, fmt, w, h, levels, pitch, cube, span);
    }
#else
    (void)slot;
#endif
    return eng_texture_content_hash(location, offset, fmt, w, h, levels, pitch,
                                    cube, span, readable);
}

static int eng_staging_reserve(u32 bytes)
{
    if (g.tex_staging_cap >= bytes) return 1;
    u8* n = (u8*)realloc(g.tex_staging, bytes);
    if (!n) return 0;
    g.tex_staging = n;
    g.tex_staging_cap = bytes;
    return 1;
}

/* Decode a guest texture out of guest memory and hand every face and level to
 * the backend. Returns the backend handle, or 0. */
static u32 eng_texture_upload(u32 location, u32 offset, u32 fmt, u32 w, u32 h,
                              u32 levels, u32 pitch, int cube, u32 remap)
{
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 nlv = rsx_texture_mip_chain(fmt, w, h, levels, pitch, lv);
    if (!nlv || !lv[0].tl.face_bytes) return 0;
    const u32 faces = cube ? 6u : 1u;
    const u32 face_stride = cube
        ? rsx_texture_cube_face_stride(fmt, w, h, levels, pitch) : 0u;
    const u32 span = eng_texture_span(fmt, w, h, levels, pitch, cube);
    const u8* src = eng_guest_ptr(NULL, location, offset, span);
    if (!src) return 0;

    const u32 handle = g.be->texture_create(g.be->user, eng_texfmt(lv[0].tl.fmt),
                                            w, h, nlv, faces, remap, fmt);
    if (!handle) return 0;

    for (u32 f = 0; f < faces; f++) {
        const u8* face = src + (size_t)f * face_stride;
        for (u32 m = 0; m < nlv; m++) {
            const rsx_tex_layout* tl = &lv[m].tl;
            if (!eng_staging_reserve(tl->dst_row_bytes * tl->rows)) {
                g.be->texture_release(g.be->user, handle);
                return 0;
            }
            rsx_texture_decode(g.tex_staging, tl->dst_row_bytes,
                               face + lv[m].offset, lv[m].w, lv[m].h, tl,
                               rsx_texture_argb_is_rgba());
            g.be->texture_upload(g.be->user, handle, f, m, lv[m].w, lv[m].h,
                                 g.tex_staging, tl->dst_row_bytes, tl->rows);
        }
    }
    return handle;
}

#define ENG_TEX_ASSOC_SETS 4096u
#define ENG_TEX_ASSOC_WAYS 4
static int s_eng_tex_assoc[ENG_TEX_ASSOC_SETS][ENG_TEX_ASSOC_WAYS];   /* slot + 1, 0 = empty */
static inline u32 eng_tex_bucket(u32 location, u32 offset, u32 fmt, u32 w, u32 h,
                                 u32 pitch, u32 remap, int cube)
{
    u32 x = (offset * 2654435761u) ^ (w * 40503u) ^ (h * 2246822519u) ^
            (fmt * 3266489917u) ^ (pitch * 668265263u) ^ (remap * 374761393u) ^
            (location << 7) ^ (u32)(cube != 0);
    x ^= x >> 15; x *= 2246822519u; x ^= x >> 13;
    return x & (ENG_TEX_ASSOC_SETS - 1u);
}
static inline int eng_tex_same(const eng_texture* e, u32 location, u32 offset,
                               u32 fmt, u32 w, u32 h, u32 pitch, u32 remap, int cube)
{
    return e->location == location && e->offset == offset &&
           e->format == fmt && e->width == w && e->height == h &&
           e->pitch == pitch && e->remap == remap &&
           e->cubemap == (u32)(cube != 0);
}
/* Move (or insert) slot i to the front of its bucket. */
static inline void eng_tex_assoc_note(u32 hb, int i)
{
    int* ways = s_eng_tex_assoc[hb];
    const int v = i + 1;
    int k = 0;
    while (k < ENG_TEX_ASSOC_WAYS - 1 && ways[k] != v) k++;
    for (; k > 0; k--) ways[k] = ways[k - 1];
    ways[0] = v;
}

/* The cache slot for a texture unit's bytes, keyed on where they are and what
 * the registers say they are. An already-cached entry is re-hashed at most
 * once per presented frame and re-decoded when the guest changed it, which is
 * what makes an animated UI or a video texture update at all. A miss with the
 * cache full evicts the least recently used entry rather than returning
 * nothing: returning white "made the recovered orphanage render as flat
 * green/black geometry" (rsx_live_draw.c:2326-2334). */
static u32 eng_texture_slot(u32 location, u32 offset, u32 fmt, u32 w, u32 h,
                            u32 levels, u32 pitch, int cube, u32 remap)
{
    if (!w || !h || w > ENG_MAX_TEX_DIM || h > ENG_MAX_TEX_DIM) return 0;
    /* The span is a function of fmt, w, h, levels, pitch and cube; all but
     * levels are the entry's identity, so a hit with the same levels reuses
     * the span it stored. Same early return as before when it is 0. */
    u32 span = 0;

    /* Hashed hints first (the D3D12 backend's TEX_ASSOC, rsx_d3d12_backend.c
     * v227A): four recent slots per bucket, each verified against the full
     * identity; a miss still does the authoritative scan. Only a lookup
     * accelerator -- cache contents and uploads are unchanged. M7 profile: the
     * linear scan over up to 1024 entries per bound unit per draw was ~18% of
     * the render thread. */
    const u32 hb = eng_tex_bucket(location, offset, fmt, w, h, pitch, remap, cube);
    int found = -1;
    for (int way = 0; way < ENG_TEX_ASSOC_WAYS && found < 0; way++) {
        const int i = s_eng_tex_assoc[hb][way] - 1;
        if (i >= 0 && (u32)i < g.n_textures &&
            eng_tex_same(&g.textures[i], location, offset, fmt, w, h, pitch, remap, cube))
            found = i;
    }
    if (found < 0) {
        span = eng_texture_span(fmt, w, h, levels, pitch, cube);
        if (!span) return 0;
    }
    for (u32 i = 0; found < 0 && i < g.n_textures; i++)
        if (eng_tex_same(&g.textures[i], location, offset, fmt, w, h, pitch, remap, cube))
            found = (int)i;
    if (found >= 0) {
        eng_texture* e = &g.textures[found];
        if (!span) {
            if (e->span && e->span_levels == levels) span = e->span;
            else {
                span = eng_texture_span(fmt, w, h, levels, pitch, cube);
                if (!span) return 0;
            }
        }
        e->span = span; e->span_levels = levels;
        eng_tex_assoc_note(hb, found);
        if (e->handle && e->last_hash_frame != g.frames) {
            int readable = 0;
            const u64 hash = eng_texture_validate_hash((u32)found, location,
                                                       offset, fmt, w, h, levels,
                                                       pitch, cube, span, &readable);
            e->last_hash_frame = g.frames;
            if (readable && hash != e->content_hash) {
#if defined(__APPLE__)
                s_tcs_chg[found] = g.frames + 1u;   /* TEXCS: inline for a while */
#endif
                const u32 fresh = eng_texture_upload(location, offset, fmt, w, h,
                                                     levels, pitch, cube, remap);
                if (fresh) {
                    g.be->texture_release(g.be->user, e->handle);
                    e->handle = fresh;
                    e->content_hash = hash;
                }
            }
        }
        e->last_use_serial = ++g.texture_use_serial;
        return e->handle;
    }

    u32 index;
    u32 evicted = 0;
    if (g.n_textures < ENG_MAX_TEXTURES) {
        index = g.n_textures++;
    } else {
        index = 0;
        for (u32 i = 1; i < g.n_textures; i++)
            if (g.textures[i].last_use_serial < g.textures[index].last_use_serial)
                index = i;
        evicted = g.textures[index].handle;
    }

    eng_texture e;
    memset(&e, 0, sizeof(e));
    e.location = location; e.offset = offset; e.format = fmt;
    e.width = w; e.height = h; e.pitch = pitch;
    e.remap = remap; e.cubemap = (u32)(cube != 0);
    e.last_hash_frame = g.frames;
    e.last_use_serial = ++g.texture_use_serial;
    e.span = span; e.span_levels = levels;
    { int readable = 0;
      e.content_hash = eng_texture_content_hash(location, offset, fmt, w, h,
                                                levels, pitch, cube, span,
                                                &readable); }
    e.handle = eng_texture_upload(location, offset, fmt, w, h, levels, pitch,
                                  cube, remap);
    if (e.handle && evicted) g.be->texture_release(g.be->user, evicted);
    if (e.handle || !evicted) { g.textures[index] = e; eng_tex_assoc_note(hb, (int)index); }
#if defined(__APPLE__)
    if (e.handle && s_tcs_cfg > 0) { s_tcs_chg[index] = 0; eng_tcs_note(index); }   /* TEXCS: validated next frame */
#endif
    return e.handle;
}

/* SET_TEXTURE_FILTER's min field is [18:16] (1 NEAREST, 2 LINEAR, then 3..6,
 * the four combinations of a nearest/linear minification with a
 * nearest/linear mip filter) and mag [26:24]; SET_TEXTURE_CONTROL0 carries
 * max LOD at [18:7] and min LOD at [30:19], both 4.8 fixed point. RSX's LOD
 * bias, SET_TEXTURE_FILTER [12:0], is not applied: the reference engine
 * leaves it at zero too. */
static void eng_decode_sampler(u32 filter, u32 wrap, u32 control0,
                               rsx_be_sampler_desc* out)
{
    const u32 minf = (filter >> 16) & 7u;
    const u32 magf = (filter >> 24) & 7u;
    out->min_linear  = (u8)(minf == 2 || minf == 4 || minf == 6);
    out->mag_linear  = (u8)(magf == 2);
    out->mip_present = (u8)(minf >= 3);
    out->mip_linear  = (u8)(minf == 5 || minf == 6);
    out->wrap_s = (u8)(wrap & 0xFu);
    out->wrap_t = (u8)((wrap >> 8) & 0xFu);
    out->wrap_r = (u8)((wrap >> 16) & 0xFu);
    out->min_lod = (float)((control0 >> 19) & 0xFFFu) / 256.0f;
    out->max_lod = out->mip_present ? (float)((control0 >> 7) & 0xFFFu) / 256.0f
                                    : 0.0f;
    if (out->max_lod < out->min_lod) out->max_lod = out->min_lod;
}

/* ---- pipelines ----------------------------------------------------------- */

static u32 eng_vtex_mask(void)
{
    u32 mask = 0;
    for (u32 u = 0; u < RSX_DSP_NUM_VERTEX_TEXTURES; u++) {
        rsx_dsp_vertex_texture vt;
        rsx_dsp_get_vertex_texture(&g.rsx, u, &vt);
        if (vt.enabled && vt.width && vt.height) mask |= 1u << u;
    }
    return mask;
}

/* PROG_MEMO: what the per-draw program scans derive from the program bytes --
 * the fragment program's size, texture mask and structure hash, the vertex
 * program's length, input mask and hash -- kept per program address and reused
 * only while the bytes still equal a stored copy (memcmp). Each of those is a
 * function of exactly the bytes compared (every scan stops at its END
 * instruction), so a hit returns what the scan would. iPad profile: the scans
 * were ~7% of the RSX thread, redone on every draw.
 * PS3RECOMP_ENGINE_PROG_MEMO=0 turns it off; PS3RECOMP_ENGINE_PROG_VERIFY=1
 * redoes every scan on a hit and counts differences. RSX thread only. */
typedef struct {
    u32 instrs;                 /* 0 = empty */
    u32 cap;
    u8* bytes;
    u32 in_mask;
    u8  mask_ok, hash_ok;
    u64 hash;                   /* XXH3 of the program, pipeline-key seed */
} eng_vp_memo;
typedef struct {
    u32 loc, off, size;         /* size 0 = empty */
    u32 cap;
    u8* bytes;
    u32 tex_mask;
    u8  tex_ok, shash_ok;
    u64 shash_seed, shash;      /* eng_fp_struct_hash for that seed */
} eng_fp_memo;
#define ENG_FP_MEMO 256u
static eng_vp_memo s_vp_memo[RSX_DSP_VP_INSTR];
static eng_fp_memo s_fp_memo[ENG_FP_MEMO];

static int eng_prog_memo_on(void)
{
    static int on = -1;
    if (on < 0) on = eng_env_flag("PS3RECOMP_ENGINE_PROG_MEMO", 1);
    return on;
}
static int eng_prog_verify_on(void)
{
    static int on = -1;
    if (on < 0) on = eng_env_flag("PS3RECOMP_ENGINE_PROG_VERIFY", 0);
    return on;
}
static void eng_prog_verify(const char* what, u64 memo, u64 scan)
{
    static u64 checks, bad;
    checks++;
    if (memo != scan && bad++ < 16)
        fprintf(stderr, "[rsx engine] PROG_VERIFY: %s differs (memo 0x%llx, scan 0x%llx)\n",
                what, (unsigned long long)memo, (unsigned long long)scan);
    if ((checks % 100000u) == 0)
        fprintf(stderr, "[rsx engine] PROG_VERIFY: %llu checked, %llu differ\n",
                (unsigned long long)checks, (unsigned long long)bad);
}
static int eng_memo_store(u8** bytes, u32* cap, const u8* src, u32 n)
{
    if (*cap < n) {
        u8* p = (u8*)realloc(*bytes, n);
        if (!p) return 0;
        *bytes = p;
        *cap = n;
    }
    memcpy(*bytes, src, n);
    return 1;
}

/* The memo entry for the vertex program at slot start, or NULL (memo off, no
 * END instruction, out of memory): the caller then scans as before. */
static eng_vp_memo* eng_vp_memo_get(u32 start, const u8* uc)
{
    if (!eng_prog_memo_on()) return NULL;
    const u32 max_bytes = (RSX_DSP_VP_INSTR - start) * 16u;
    eng_vp_memo* m = &s_vp_memo[start];
    if (m->instrs && !memcmp(uc, m->bytes, (size_t)m->instrs * 16u)) {
        if (eng_prog_verify_on())
            eng_prog_verify("vp length", m->instrs, rsx_vp_program_size_instrs(uc, max_bytes));
        return m;
    }
    m->instrs = 0;
    m->mask_ok = m->hash_ok = 0;
    const u32 instrs = rsx_vp_program_size_instrs(uc, max_bytes);
    if (!instrs || !eng_memo_store(&m->bytes, &m->cap, uc, instrs * 16u)) return NULL;
    m->instrs = instrs;
    return m;
}

/* The same for the fragment program at (loc, off). *io_uc is the program's
 * first 16 bytes on entry and, on success, the program's full span. */
static eng_fp_memo* eng_fp_memo_get(u32 loc, u32 off, const u8** io_uc)
{
    if (!eng_prog_memo_on()) return NULL;
    eng_fp_memo* m = &s_fp_memo[((off ^ (loc << 30)) * 2654435761u) >> 24];
    if (m->size && m->off == off && m->loc == loc) {
        const u8* p = eng_guest_ptr(NULL, loc, off, m->size);
        if (p && !memcmp(p, m->bytes, m->size)) {
            if (eng_prog_verify_on())
                eng_prog_verify("fp size", m->size,
                                rsx_fp_program_size(*io_uc, ENG_FP_MAX_BYTES));
            *io_uc = p;
            return m;
        }
    }
    m->size = 0;
    m->tex_ok = m->shash_ok = 0;
    const u32 size = rsx_fp_program_size(*io_uc, ENG_FP_MAX_BYTES);
    const u8* p = size ? eng_guest_ptr(NULL, loc, off, size) : NULL;
    if (!p || !eng_memo_store(&m->bytes, &m->cap, p, size)) return NULL;
    m->loc = loc;
    m->off = off;
    m->size = size;
    *io_uc = p;
    return m;
}

/* Are both of the guest's own programs resident? One answer, used by the
 * layout and by the pipeline, because they must not disagree: the built-in
 * program declares all sixteen inputs, so narrowing the layout for it would
 * leave the pipeline's vertex descriptor short of what the shader reads.
 * out_vm / out_fm get the PROG_MEMO entries (NULL when there is none). */
static int eng_guest_programs(const u8** out_vp, u32* out_vp_instrs,
                              const u8** out_fp, u32* out_fp_size,
                              eng_vp_memo** out_vm, eng_fp_memo** out_fm)
{
    const u8* vp_uc = NULL;
    u32 vp_instrs = 0;
    eng_vp_memo* vm = NULL;
    eng_fp_memo* fm = NULL;
    const u32 start = rsx_dsp_vp_start(&g.rsx);
    if (start < RSX_DSP_VP_INSTR) {
        vp_uc = (const u8*)(g.rsx.vp + start * 4);
        vm = eng_vp_memo_get(start, vp_uc);
        vp_instrs = vm ? vm->instrs : rsx_vp_program_size_instrs(
            vp_uc, (RSX_DSP_VP_INSTR - start) * 16u);
    }

    /* A zero SET_SHADER_PROGRAM is "none", not "the program at offset 0": the
     * register carries the location in its low two bits, so a real program can
     * never read back as zero. */
    const u8* fp_uc = NULL;
    u32 fp_size = 0;
    if (rsx_dsp_reg(&g.rsx, 0x08E4)) {
        u32 fp_loc = 0;
        const u32 fp_off = rsx_dsp_fragment_program(&g.rsx, &fp_loc);
        fp_uc = eng_guest_ptr(NULL, fp_loc, fp_off, 16);
        fm = fp_uc ? eng_fp_memo_get(fp_loc, fp_off, &fp_uc) : NULL;
        if (fm) {
            fp_size = fm->size;
        } else {
            fp_size = fp_uc ? rsx_fp_program_size(fp_uc, ENG_FP_MAX_BYTES) : 0;
            if (fp_size) fp_uc = eng_guest_ptr(NULL, fp_loc, fp_off, fp_size);
            if (!fp_uc) fp_size = 0;
        }
    }

    if (out_vp) *out_vp = vp_uc;
    if (out_vp_instrs) *out_vp_instrs = vp_instrs;
    if (out_fp) *out_fp = fp_uc;
    if (out_fp_size) *out_fp_size = fp_size;
    if (out_vm) *out_vm = vm;
    if (out_fm) *out_fm = fm;
    return vp_instrs && fp_size;
}

/* rsx_fp_texture_mask through the memo. */
static u32 eng_fp_tex_mask(eng_fp_memo* fm, const u8* uc, u32 size)
{
    if (fm && fm->tex_ok && !eng_prog_verify_on()) return fm->tex_mask;
    const u32 mask = rsx_fp_texture_mask(uc, size);
    if (fm) {
        if (fm->tex_ok) eng_prog_verify("fp textures", fm->tex_mask, mask);
        fm->tex_mask = mask;
        fm->tex_ok = 1;
    }
    return mask;
}

/* The layout the active vertex program actually reads. An uncertain analysis
 * falls back to all sixteen registers (rsx_live_draw.c:3625-3643). */
static void eng_vertex_layout(rsx_vertex_layout_plan* layout)
{
    u32 mask = 0xFFFFu;
    const u8* uc = NULL;
    u32 instrs = 0;
    eng_vp_memo* vm = NULL;
    if (eng_guest_programs(&uc, &instrs, NULL, NULL, &vm, NULL)) {
        if (vm && vm->mask_ok && !eng_prog_verify_on()) {
            mask = vm->in_mask;
        } else {
            rsx_vp_input_analysis analysis = { 0xFFFFu, 0 };
            if (rsx_vp_analyze_inputs(uc, instrs * 16u, &analysis) == (int)instrs &&
                analysis.exact && analysis.input_mask)
                mask = analysis.input_mask;
            if (vm) {
                if (vm->mask_ok) eng_prog_verify("vp inputs", vm->in_mask, mask);
                vm->in_mask = mask;
                vm->mask_ok = 1;
            }
        }
    }
    rsx_vertex_layout_plan_init(layout, mask);
}

/* What a draw runs before a title has loaded programs of its own.
 *
 * The reference engine has no fallback and drops such a draw, because a title
 * always has both programs resident by the time it draws anything. A general
 * toolkit does not get to assume that: the host harness's fixed-function modes
 * are exactly this case, and so is the first frame of a title that clears and
 * flips before its first SET_TRANSFORM_PROGRAM. It is written in the same HLSL
 * shape the decompilers emit -- ATTRn inputs, the COLOR0/TEXCOORD varyings, the
 * VPConst block and the viewport epilogue -- so it goes through the one
 * pipeline_create the backend already implements rather than needing a second
 * entry point for a built-in shader.
 *
 * Vertex constant rows 0..3 are the transform when the guest programmed them
 * and identity when it did not: an all-zero matrix would collapse every vertex
 * onto the origin. */
static const char* const kEngFixedVS =
"struct VSInput {\n"
"    float4 a0:ATTR0;  float4 a1:ATTR1;  float4 a2:ATTR2;  float4 a3:ATTR3;\n"
"    float4 a4:ATTR4;  float4 a5:ATTR5;  float4 a6:ATTR6;  float4 a7:ATTR7;\n"
"    float4 a8:ATTR8;  float4 a9:ATTR9;  float4 a10:ATTR10; float4 a11:ATTR11;\n"
"    float4 a12:ATTR12; float4 a13:ATTR13; float4 a14:ATTR14; float4 a15:ATTR15;\n"
"};\n"
"struct VSOutput {\n"
"    float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1;\n"
"    float4 fog:FOG;\n"
"    float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
"    float4 t4:TEXCOORD4; float4 t5:TEXCOORD5; float4 t6:TEXCOORD6; float4 t7:TEXCOORD7;\n"
"};\n"
"cbuffer VPConst : register(b0) {\n"
"    float4 vp_c[512];\n"
"    float4 vp_posscale;\n"
"    float4 vp_posoffset;\n"
"};\n"
"VSOutput main(VSInput input) {\n"
"    float4 _p = float4(dot(vp_c[0], input.a0), dot(vp_c[1], input.a0),\n"
"                       dot(vp_c[2], input.a0), dot(vp_c[3], input.a0));\n"
"    float _m = dot(abs(vp_c[0]), 1.0) + dot(abs(vp_c[1]), 1.0)\n"
"             + dot(abs(vp_c[2]), 1.0) + dot(abs(vp_c[3]), 1.0);\n"
"    if (_m == 0.0) _p = input.a0;\n"
"    VSOutput Out;\n"
"    Out.pos = float4(_p.xyz * vp_posscale.xyz + _p.w * vp_posoffset.xyz, _p.w);\n"
"    Out.col0 = input.a3; Out.col1 = float4(0,0,0,1); Out.fog = (float4)0;\n"
"    Out.t0 = input.a8; Out.t1 = (float4)0; Out.t2 = (float4)0; Out.t3 = (float4)0;\n"
"    Out.t4 = (float4)0; Out.t5 = (float4)0; Out.t6 = (float4)0; Out.t7 = (float4)0;\n"
"    return Out;\n"
"}\n";

static const char* const kEngFixedPS =
"struct PSInput {\n"
"    float4 position : SV_POSITION; float4 col0 : COLOR0; float4 col1 : COLOR1;\n"
"    float4 fog : FOG;\n"
"    float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2; float4 tc3:TEXCOORD3;\n"
"    float4 tc4:TEXCOORD4; float4 tc5:TEXCOORD5; float4 tc6:TEXCOORD6; float4 tc7:TEXCOORD7;\n"
"};\n"
"float4 main(PSInput input) : SV_TARGET { return input.col0; }\n";

/* rsx_fp_structural_hash's identity -- every instruction's 16 bytes and the
 * inline-constant count, the constant VALUES left out (they are hoisted into
 * the buffered block) -- gathered and hashed by one XXH3 call instead of FNV
 * a byte at a time (that was ~4% of the RSX thread: it runs on every draw).
 * 0 = malformed, as there. RSX thread only (static buffer). */
static u64 eng_fp_struct_hash(const u8* uc, u32 max_bytes, u64 seed)
{
    static u8 buf[ENG_FP_MAX_BYTES + 4];
    if (!uc) return 0;
    u32 off = 0, n = 0, consts = 0;
    while (off + 16 <= max_bytes && n + 16 <= ENG_FP_MAX_BYTES) {
        const u32 w0 = rsx_fp_read_word(uc + off);
        const u32 w1 = rsx_fp_read_word(uc + off + 4);
        const u32 w2 = rsx_fp_read_word(uc + off + 8);
        const u32 w3 = rsx_fp_read_word(uc + off + 12);
        memcpy(buf + n, uc + off, 16);
        n += 16;
        off += 16;
        /* Register type 2 = an inline constant follows (FP_REG_TYPE_CONST). */
        if ((w1 & 3u) == 2u || (w2 & 3u) == 2u || (w3 & 3u) == 2u) {
            if (off + 16 > max_bytes || consts >= RSX_FP_MAX_INLINE_CONSTANTS) return 0;
            consts++;
            off += 16;
        }
        if (w0 & 1u) {                  /* FP_END */
            memcpy(buf + n, &consts, 4);
            const u64 h = XXH3_64bits_withSeed(buf, n + 4u, seed ^ 0x31535046u);
            return h ? h : 1u;
        }
    }
    return 0;
}

/* The pipeline for this draw, built once per distinct key. A negative result
 * is cached too (handle 0), so a program pair that will not translate is not
 * retried on every draw of every frame. */
static u32 eng_pipeline_get(const rsx_vertex_layout_plan* layout,
                            const rsx_be_render_state* rs,
                            rsx_be_format rt_fmt, u32 rt_count,
                            u32 cube_mask, u32 vtex_mask, int* out_fixed)
{
    *out_fixed = 1;
    const u8* vp_uc = NULL;
    const u8* fp_uc = NULL;
    u32 vp_instrs = 0, fp_size = 0;
    eng_vp_memo* vm = NULL;
    eng_fp_memo* fm = NULL;
    /* No resident program pair means the built-in one; see kEngFixedVS. */
    const int fixed = !eng_guest_programs(&vp_uc, &vp_instrs, &fp_uc, &fp_size, &vm, &fm);

    const u32 fp_ctrl  = rsx_dsp_shader_control(&g.rsx);
    /* Only units the program samples: a stale depth binding on an unused
     * unit must not make a second pipeline. */
    u32 shkey = 0;
    if (!fixed && g.shkey) {
        const u32 used = eng_fp_tex_mask(fm, fp_uc, fp_size);
        for (u32 u = 0; u < 4; u++)
            if (((g.shkey >> u) & 1u) && ((used >> u) & 1u))
                shkey |= (1u << u) | (g.shkey & (7u << (4 + 3 * u)));
    }
    u32 texk = 0;
    if (!fixed && g.texk) {
        const u32 used = eng_fp_tex_mask(fm, fp_uc, fp_size);
        for (u32 u = 0; u < 8; u++)
            if ((used >> u) & 1u) texk |= g.texk & (3u << (2 * u));
    }

    const u32 dbt = (!fixed && g.dbt) ? 1u : 0u;

    /* Every reader stops at count, so only count is reset (the block is
     * 32 KB, and clearing it twice a draw was ~6% of this thread). */
    g.fp_constants.count = 0;
    if (!fixed && rsx_fp_collect_constants_lean(fp_uc, fp_size, &g.fp_constants) < 0)
        return 0;

    /* Identity: the vertex program's own bytes, the fragment program's
     * STRUCTURE (its inline constants are hoisted into the buffered block, so
     * a constant change must not be a new pipeline), the export-width bit of
     * SHADER_CONTROL, the cube and vertex-texture masks, the input layout,
     * and the structural render state. */
    u64 key = 1469598103934665603ull;
    /* The small fields go in one XXH3 call at fixed positions (0 where a
     * field does not apply), not field by field through byte-wise FNV. */
    u32 kw[10u + ENG_RS_WORDS];
    memset(kw, 0, 6u * sizeof kw[0]);
    if (fixed) {
        key = XXH3_64bits_withSeed("XIFN", 4, key);
    } else {
        /* XXH3 over the program words, seeded with the running key: the
         * byte-wise FNV here was ~3% of the RSX thread. */
        /* Both through PROG_MEMO: the vertex hash always starts from the
         * same seed, the fragment one is kept for the seed it was made with. */
        if (vm && vm->hash_ok && !eng_prog_verify_on()) {
            key = vm->hash;
        } else {
            const u64 h = XXH3_64bits_withSeed(vp_uc, (size_t)vp_instrs * 16u, key);
            if (vm) {
                if (vm->hash_ok) eng_prog_verify("vp hash", vm->hash, h);
                vm->hash = h;
                vm->hash_ok = 1;
            }
            key = h;
        }
        if (fm && fm->shash_ok && fm->shash_seed == key && !eng_prog_verify_on()) {
            key = fm->shash;
        } else {
            const u64 h = eng_fp_struct_hash(fp_uc, fp_size, key);
            if (fm) {
                if (fm->shash_ok && fm->shash_seed == key)
                    eng_prog_verify("fp hash", fm->shash, h);
                fm->shash_seed = key;
                fm->shash = h;
                fm->shash_ok = 1;
            }
            key = h;
        }
        if (!key) return 0;
        kw[0] = fp_ctrl & 0x40u;
        kw[1] = cube_mask;
        kw[2] = vtex_mask;
        kw[3] = shkey;
        kw[4] = dbt;
        kw[5] = texk;
    }
    kw[6] = layout->mask;
    kw[7] = layout->stride;
    kw[8] = (u32)rt_fmt;
    /* How many colour attachments the pass will have is pipeline identity:
     * a host API matches the two against each other. */
    kw[9] = rt_count;
    eng_rs_words(rs, kw + 10);
    key = XXH3_64bits_withSeed(kw, sizeof kw, key);

    const u32 pmask = ENG_MAX_PIPELINES * 2u - 1u;
    u32 pslot = (u32)(key ^ (key >> 29) ^ (key >> 47)) & pmask;
    for (u32 n; (n = g.pipe_index[pslot]) != 0; pslot = (pslot + 1u) & pmask)
        if (g.pipelines[n - 1u].key == key) {
            *out_fixed = g.pipelines[n - 1u].fixed;
            g.ps_hash = g.pipelines[n - 1u].ps_hash;
            g.vs_cuse = g.pipelines[n - 1u].cpart ? g.pipelines[n - 1u].cuse : NULL;
            return g.pipelines[n - 1u].handle;
        }
    g.vs_cuse = NULL;
    if (g.n_pipelines >= ENG_MAX_PIPELINES) return 0;

    u32 handle = 0;
    int vi = 1, fi = 1;
    u32 nconst = 0;
    if (fixed) {
        snprintf(s_vs_hlsl, sizeof s_vs_hlsl, "%s", kEngFixedVS);
        snprintf(s_ps_hlsl, sizeof s_ps_hlsl, "%s", kEngFixedPS);
    } else {
        vi = rsx_vp_decompile_compact_ex(vp_uc, vp_instrs * 16u, vtex_mask,
                                         layout->mask, s_vs_hlsl,
                                         sizeof s_vs_hlsl);
        fi = rsx_fp_decompile_buffered_ex(fp_uc, fp_size, fp_ctrl, cube_mask,
                                          s_ps_hlsl, sizeof s_ps_hlsl, &nconst);
        if (fi > 0 && nconst != g.fp_constants.count) fi = -1;
        if (fi > 0 && rs->alpha_test_enable &&
            rsx_fp_apply_alpha_test_buffered(s_ps_hlsl, sizeof s_ps_hlsl,
                                             rs->alpha_func) < 0)
            fi = -1;
        if (fi > 0 && shkey) eng_fp_sun_ndl_clamp(s_ps_hlsl, sizeof s_ps_hlsl);
        if (fi > 0 && shkey &&
            eng_fp_shadow_rewrite(s_ps_hlsl, sizeof s_ps_hlsl, shkey) < 0)
            fi = -1;
        if (fi > 0 && texk) {
            const int nx = eng_fp_txf_rewrite(s_ps_hlsl, sizeof s_ps_hlsl, texk);
            if (nx < 0) fi = -1;
            static u32 logs = 0;
            if (logs < 12 || nx < 0) {
                logs++;
                fprintf(stderr, "[rsx engine] TEX_XF: %d sample(s) wrapped (unit modes 0x%04X)\n", nx, texk);
            }
        }
        /* A failed insert keeps the draw, untested, as before this switch. */
        if (fi > 0 && dbt) {
            const int r = eng_fp_dbt_rewrite(s_ps_hlsl, sizeof s_ps_hlsl, cube_mask);
            static u32 logs = 0;
            if (logs < 6 || r < 0) {
                logs++;
                fprintf(stderr, "[rsx engine] DEPTH_BOUNDS: test %s in the fragment program\n",
                        r < 0 ? "NOT inserted" : "inserted");
            }
        }
    }
    if (vi > 0 && fi > 0)
        handle = g.be->pipeline_create(g.be->user, s_vs_hlsl, s_ps_hlsl, rs,
                                       layout, layout->stride, rt_fmt, rt_count);
    { static u32 logs = 0; if ((logs < 32 && logs++ < 32))
        fprintf(stderr, "[rsx engine] pipeline %016llx: %s vp %d, fp %d,"
                        " %u constants -> %s\n",
                (unsigned long long)key, fixed ? "built-in" : "guest",
                vi, fi, nconst, handle ? "ok" : "FAILED (draw dropped)"); }

    g.pipelines[g.n_pipelines].key = key;
    g.pipelines[g.n_pipelines].handle = handle;
    g.pipelines[g.n_pipelines].fixed = (u8)fixed;
    {   /* Which constants the vertex shader reads, from the HLSL it was built
         * from, as the D3D12 backend does: every read is "vp_c[<n>]", or
         * "vp_c[(" for a relatively indexed one, which needs them all. The
         * declaration "vp_c[512]" is past the last entry. */
        eng_pipeline* p = &g.pipelines[g.n_pipelines];
        memset(p->cuse, 0, sizeof p->cuse);
        int rel = 0;
        for (const char* q = strstr(s_vs_hlsl, "vp_c["); q; q = strstr(q + 5, "vp_c[")) {
            const char* r = q + 5;
            if (*r >= '0' && *r <= '9') {
                const unsigned long v = strtoul(r, NULL, 10);
                if (v < 512u) p->cuse[v >> 5] |= 1u << (v & 31u);
            } else {
                rel = 1;
            }
        }
        p->cpart = (u8)(!rel && eng_env_flag("VCB_PARTIAL", 1));
        g.vs_cuse = p->cpart ? p->cuse : NULL;
    }
    g.pipelines[g.n_pipelines].ps_hash = g.ps_hash =
        eng_fnv1a(s_ps_hlsl, (u32)strlen(s_ps_hlsl), 1469598103934665603ull);
    g.n_pipelines++;
    g.pipe_index[pslot] = (u16)g.n_pipelines;
    *out_fixed = fixed;
    return handle;
}

/* ---- draw accumulation --------------------------------------------------- */

static void dc_reset(void)
{
    dc.n_arr = dc.n_idx = dc.n_packets = 0;
    dc.n_refs = dc.n_source_refs = dc.n_verts = dc.n_cuts = 0;
    dc.refs_remapped = 0;
    dc.rng_known = 0;
    dc.fetch_ok = 1;
    dc.inl = NULL;
    dc.inl_bytes = 0;
    dc.imm = NULL;
    dc.imm_count = 0;
}

static int dc_push_ref(u32 vertex_id, u32 base_index)
{
    if (dc.n_refs >= dc.cap_refs) {
        const u32 next = dc.cap_refs ? dc.cap_refs * 2u : 4096u;
        rsx_vertex_ref* r = (rsx_vertex_ref*)realloc(dc.refs,
                                                     (size_t)next * sizeof(*r));
        if (!r) return 0;
        dc.refs = r;
        dc.cap_refs = next;
    }
    dc.refs[dc.n_refs].vertex_id = vertex_id;
    dc.refs[dc.n_refs].base_index = base_index;
    dc.n_refs++;
    return 1;
}

static int dc_reserve_verts(u64 bytes)
{
    if (bytes <= dc.verts_cap) return 1;
    u64 next = dc.verts_cap ? dc.verts_cap : (1u << 20);
    while (next < bytes) next *= 2u;
    u8* v = (u8*)realloc(dc.verts, (size_t)next);
    if (!v) return 0;
    dc.verts = v;
    dc.verts_cap = next;
    return 1;
}

static int dc_reserve_indices(u32 count)
{
    if (count <= g.index_cap) return 1;
    u32 next = g.index_cap ? g.index_cap : 4096u;
    while (next < count) next *= 2u;
    u32* p = (u32*)realloc(g.indices, (size_t)next * sizeof(u32));
    if (!p) return 0;
    g.indices = p;
    g.index_cap = next;
    return 1;
}

/* ---- fast vertex path ---------------------------------------------------
 *
 * Same bytes as rsx_vertex_remap_build + rsx_vertex_fetch_one, done in bulk.
 * The D3D12 path converts vertices off the FIFO thread and dedupes indexed
 * draws over the index range (v55 "range" vs "hashed"); this is the range
 * dedupe and a per-attribute fetch with the type switch hoisted out of the
 * vertex loop. Profile: fetch + decode + remap were ~40% of sink_end.
 *   PS3RECOMP_ENGINE_VTX_FAST=0    the per-vertex path, as before
 *   PS3RECOMP_ENGINE_VTX_VERIFY=1  run both and log any difference */
static int eng_vtx_fast_mode(void)
{
    static int v = -1;
    if (v < 0) {
        v = eng_env_flag("PS3RECOMP_ENGINE_VTX_FAST", 1);
        if (v && eng_env_flag("PS3RECOMP_ENGINE_VTX_VERIFY", 0)) v = 2;
        fprintf(stderr, "[rsx engine] vertex fetch: %s (PS3RECOMP_ENGINE_VTX_FAST)\n",
                v == 2 ? "fast, verified" : v ? "fast" : "per vertex");
    }
    return v;
}

/* Index-range dedupe: when every reference shares one base index and the
 * vertex ids span a small range, a direct table replaces the hash probe.
 * First-occurrence order and occurrence_to_unique match the hash version. */
static u64* s_rng_tab;
static u32  s_rng_cap, s_rng_gen;
static int eng_remap_range(rsx_vertex_remap* remap, rsx_vertex_ref* refs, u32 count,
                           u32* unique_count, int known, u32 known_lo, u32 known_hi)
{
    if (count < 2) return 0;
    u32 lo = known_lo, hi = known_hi;
    if (!known) {
        const u32 base = refs[0].base_index;
        lo = hi = refs[0].vertex_id;
        for (u32 i = 1; i < count; i++) {
            const u32 v = refs[i].vertex_id;
            if (refs[i].base_index != base) return 0;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
    }
    const u64 range = (u64)hi - lo + 1u;
    if (range > (u64)count * 4u + 1024u || range > (1u << 22)) return 0;
    if (count > remap->occurrence_capacity) {
        u32* occ = (u32*)realloc(remap->occurrence_to_unique, (size_t)count * sizeof(u32));
        if (!occ) return 0;
        remap->occurrence_to_unique = occ;
        remap->occurrence_capacity = count;
    }
    if (range > s_rng_cap) {
        u64* t = (u64*)calloc((size_t)range, sizeof(u64));
        if (!t) return 0;
        free(s_rng_tab);
        s_rng_tab = t;
        s_rng_cap = (u32)range;
        s_rng_gen = 0;
    }
    if (++s_rng_gen == 0) {
        memset(s_rng_tab, 0, (size_t)s_rng_cap * sizeof(u64));
        s_rng_gen = 1;
    }
    const u64 tag = (u64)s_rng_gen << 32;
    u32* occ = remap->occurrence_to_unique;
    u32 unique = 0;
    for (u32 o = 0; o < count; o++) {
        const rsx_vertex_ref ref = refs[o];
        u64* e = &s_rng_tab[ref.vertex_id - lo];
        if ((u32)(*e >> 32) != s_rng_gen) {
            *e = tag | unique;
            refs[unique] = ref;
            occ[o] = unique++;
        } else {
            occ[o] = (u32)*e;
        }
    }
    *unique_count = unique;
    return 1;
}

static int eng_remap(rsx_vertex_remap* remap, rsx_vertex_ref* refs, u32 count, u32* unique_count)
{
    const int mode = eng_vtx_fast_mode();
    if (!mode) return rsx_vertex_remap_build(remap, refs, count, unique_count);
    if (mode == 1) {
        if (eng_remap_range(remap, refs, count, unique_count,
                            dc.rng_known, dc.rng_lo, dc.rng_hi)) return 1;
        return rsx_vertex_remap_build(remap, refs, count, unique_count);
    }
    /* verify: hash on a copy, range on the real list, compare */
    static rsx_vertex_ref* cp; static u32 cp_cap; static rsx_vertex_remap ref_map;
    if (count > cp_cap) {
        rsx_vertex_ref* n = (rsx_vertex_ref*)realloc(cp, (size_t)count * sizeof(*cp));
        if (!n) return rsx_vertex_remap_build(remap, refs, count, unique_count);
        cp = n; cp_cap = count;
    }
    memcpy(cp, refs, (size_t)count * sizeof(*cp));
    u32 u_ref = 0;
    const int ok_ref = rsx_vertex_remap_build(&ref_map, cp, count, &u_ref);
    if (!eng_remap_range(remap, refs, count, unique_count, 0, 0, 0))
        return rsx_vertex_remap_build(remap, refs, count, unique_count);
    if (ok_ref && (u_ref != *unique_count ||
                   memcmp(cp, refs, (size_t)u_ref * sizeof(*cp)) ||
                   memcmp(ref_map.occurrence_to_unique, remap->occurrence_to_unique,
                          (size_t)count * sizeof(u32)))) {
        static int n = 0;
        if (n++ < 8) fprintf(stderr, "[rsx engine] VTX_VERIFY: remap differs (count %u, unique %u vs %u)\n",
                             count, *unique_count, u_ref);
    }
    return 1;
}

/* One attribute of one vertex, exactly as rsx_vertex_fetch_one does it. */
static int eng_fetch_attr_slow(const rsx_vertex_fetch_plan* plan, u32 attr,
                               const rsx_vertex_ref* ref, float* value)
{
    const rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
    const u32 element = rsx_vertex_element_index(
        ref->vertex_id, ref->base_index, fetch->desc.frequency,
        (plan->divider_mask >> attr) & 1u);
    const u8* source = NULL;
    if (fetch->stable_base &&
        element >= fetch->stable_first && element <= fetch->stable_last) {
        source = fetch->stable_base + (u64)(element - fetch->stable_first) * fetch->stride;
    } else if (plan->inline_data) {
        source = NULL;
    } else if (plan->guest_ptr) {
        const u32 offset = plan->base_offset + fetch->desc.offset + element * fetch->stride;
        source = plan->guest_ptr(plan->guest_user, fetch->desc.location, offset,
                                 fetch->elem_size);
    }
    if (source && rsx_vertex_decode_element(fetch->desc.type, fetch->desc.size, source, value))
        return 1;
    if (attr == 0) return 0;
    memcpy(value, fetch->default_value, 16);
    return 1;
}

/* Half to float bits, branch-free, same bits as the per-component decode:
 * exponent 0 gives signed zero (denormals flushed), 31 gives inf/NaN with
 * the mantissa kept, the rest rebias by 112. */
static inline u32 eng_half_bits(u32 h)
{
    const u32 sign = (h & 0x8000u) << 16;
    const u32 em = h & 0x7FFFu, ex = em >> 10;
    const u32 w = sign | ((em << 13) + (ex == 31u ? (224u << 23) : (112u << 23)));
    return ex == 0u ? sign : w;
}

/* Division tables for eng_fetch_batch. Each entry is the same expression
 * the per-vertex code evaluated, so the values are bit for bit the same;
 * a load replaces a divide. CMP32 x/y are indexed by the raw 11-bit field
 * (sign folded in), z by the raw 10-bit field. */
static float s_eng_u8n[256], s_eng_cmp_xy[2048], s_eng_cmp_z[1024];
static int s_eng_div_ready;
static void eng_div_tables_init(void)
{
    for (u32 i = 0; i < 256u; i++) s_eng_u8n[i] = (u8)i / 255.0f;
    for (u32 r = 0; r < 2048u; r++) {
        s32 x = (s32)r;
        if (x & 0x400) x -= 0x800;
        s_eng_cmp_xy[r] = (float)x / 1023.0f;
    }
    for (u32 r = 0; r < 1024u; r++) {
        s32 z = (s32)r;
        if (z & 0x200) z -= 0x400;
        s_eng_cmp_z[r] = (float)z / 511.0f;
    }
    s_eng_div_ready = 1;
}

/* All refs, one attribute at a time. Returns 0 where fetch_one would. */
static int eng_fetch_batch(const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs,
                           u32 n, u8* out)
{
    const u32 vstride = plan->layout.stride;
    if (!s_eng_div_ready) eng_div_tables_init();
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        const rsx_vertex_fetch_attr* f = &plan->attr[attr];
        u8* o = out + slot * 16u;
        if (plan->imm_verts) {
            for (u32 i = 0; i < n; i++)
                memcpy(o + (size_t)i * vstride, refs[i].vertex_id < plan->imm_count
                           ? plan->imm_verts[refs[i].vertex_id][attr] : f->default_value, 16);
            continue;
        }
        if (!f->desc.type || !f->desc.size) {
            for (u32 i = 0; i < n; i++) memcpy(o + (size_t)i * vstride, f->default_value, 16);
            continue;
        }
        const u32 type = f->desc.type, sz = f->desc.size < 4u ? f->desc.size : 4u;
        const u32 freq = f->desc.frequency, mod = (plan->divider_mask >> attr) & 1u;
        const u8* base = f->stable_base;
        const u32 first = base ? f->stable_first : 1u, last = base ? f->stable_last : 0u;
        const size_t stride = f->stride;
        /* One loop per type (and per size for FLOAT, the common case), so
         * the type switch is not taken per vertex. Same values bit for bit:
         * x/y/z default 0, w 1, then the first sz components from guest. */
#define ENG_VLOOP(...) \
        for (u32 i = 0; i < n; i++) { \
            float* v = (float*)(o + (size_t)i * vstride); \
            const u32 e = freq <= 1u ? ((refs[i].vertex_id + refs[i].base_index) & 0x000FFFFFu) \
                : rsx_vertex_element_index(refs[i].vertex_id, refs[i].base_index, freq, mod); \
            if (e < first || e > last) { \
                if (!eng_fetch_attr_slow(plan, attr, &refs[i], v)) return 0; \
                continue; \
            } \
            const u8* s = base + (size_t)(e - first) * stride; \
            __VA_ARGS__ \
        }
        switch (type) {
        case RSX_VTX_TYPE_FLOAT:
            if (sz == 4) {
                ENG_VLOOP(u32 w[4]; memcpy(w, s, 16);
                          w[0] = __builtin_bswap32(w[0]); w[1] = __builtin_bswap32(w[1]);
                          w[2] = __builtin_bswap32(w[2]); w[3] = __builtin_bswap32(w[3]);
                          memcpy(v, w, 16);)
            } else if (sz == 3) {
                ENG_VLOOP(u32 w[4]; memcpy(w, s, 12);
                          w[0] = __builtin_bswap32(w[0]); w[1] = __builtin_bswap32(w[1]);
                          w[2] = __builtin_bswap32(w[2]); w[3] = 0x3F800000u;
                          memcpy(v, w, 16);)
            } else if (sz == 2) {
                ENG_VLOOP(u32 w[4]; memcpy(w, s, 8);
                          w[0] = __builtin_bswap32(w[0]); w[1] = __builtin_bswap32(w[1]);
                          w[2] = 0u; w[3] = 0x3F800000u;
                          memcpy(v, w, 16);)
            } else {
                ENG_VLOOP(u32 w[4]; memcpy(w, s, 4);
                          w[0] = __builtin_bswap32(w[0]); w[1] = 0u; w[2] = 0u;
                          w[3] = 0x3F800000u;
                          memcpy(v, w, 16);)
            }
            break;
        case RSX_VTX_TYPE_HALF:
            /* One loop per size, so the component loop has a fixed count. */
#define ENG_HALF_LOOP(N) \
            ENG_VLOOP(u32 w[4] = {0u, 0u, 0u, 0x3F800000u}; \
                for (u32 c = 0; c < (N); c++) \
                    w[c] = eng_half_bits(((u32)s[c * 2u] << 8) | s[c * 2u + 1u]); \
                memcpy(v, w, 16);)
            if (sz == 4) { ENG_HALF_LOOP(4u) }
            else if (sz == 3) { ENG_HALF_LOOP(3u) }
            else if (sz == 2) { ENG_HALF_LOOP(2u) }
            else { ENG_HALF_LOOP(1u) }
#undef ENG_HALF_LOOP
            break;
        case RSX_VTX_TYPE_UNORM8:
            ENG_VLOOP(v[0] = v[1] = v[2] = 0.0f; v[3] = 1.0f;
                for (u32 c = 0; c < sz; c++) v[c] = s_eng_u8n[s[c]];)
            break;
        case RSX_VTX_TYPE_UINT8:
            ENG_VLOOP(v[0] = v[1] = v[2] = 0.0f; v[3] = 1.0f;
                for (u32 c = 0; c < sz; c++) v[c] = (float)s[c];)
            break;
        case RSX_VTX_TYPE_SNORM16:
            ENG_VLOOP(v[0] = v[1] = v[2] = 0.0f; v[3] = 1.0f;
                for (u32 c = 0; c < sz; c++)
                    v[c] = (s16)((s[c * 2u] << 8) | s[c * 2u + 1u]) / 32767.0f;)
            break;
        case RSX_VTX_TYPE_SINT16:
            ENG_VLOOP(v[0] = v[1] = v[2] = 0.0f; v[3] = 1.0f;
                for (u32 c = 0; c < sz; c++)
                    v[c] = (float)(s16)((s[c * 2u] << 8) | s[c * 2u + 1u]);)
            break;
        case RSX_VTX_TYPE_CMP32:
            ENG_VLOOP(
                const u32 w = ((u32)s[0] << 24) | ((u32)s[1] << 16) | ((u32)s[2] << 8) | s[3];
                v[0] = s_eng_cmp_xy[w & 0x7FFu];
                v[1] = s_eng_cmp_xy[(w >> 11) & 0x7FFu];
                v[2] = s_eng_cmp_z[(w >> 22) & 0x3FFu];
                v[3] = 1.0f;)
            break;
        default:
            for (u32 i = 0; i < n; i++)
                if (!eng_fetch_attr_slow(plan, attr, &refs[i],
                                         (float*)(o + (size_t)i * vstride)))
                    return 0;
            break;
        }
#undef ENG_VLOOP
    }
    return 1;
}

static int dc_reserve_refs(u64 n)
{
    if (n <= dc.cap_refs) return 1;
    if (n > 0x80000000ull) return 0;
    u64 next = dc.cap_refs ? dc.cap_refs : 4096u;
    while (next < n) next *= 2u;
    rsx_vertex_ref* r = (rsx_vertex_ref*)realloc(dc.refs, (size_t)next * sizeof(*r));
    if (!r) return 0;
    dc.refs = r;
    dc.cap_refs = (u32)next;
    return 1;
}

/* 16-bit index -> (generation << 16) | unique slot, for the fused dedupe. */
static u32* s_idx16_tab;
static u32  s_idx16_gen;

/* VTX_FAST: every index batch in one pass, with one reserve and word loads
 * instead of dc_push_ref per index. Restart cuts land where the per-index
 * loop puts them. When a 16-bit list starts the reference list and the
 * topology may share references, the dedupe happens in the same pass on a
 * direct table, in the first-occurrence order eng_remap_range and
 * rsx_vertex_remap_build give. Returns 0 (nothing done) when a batch is not
 * one contiguous guest run: the caller reads index by index, as before;
 * 1 = references read; 2 = read and deduplicated. */
static int dc_fetch_indexed_fast(const rsx_dsp_index_array* ia, u32 ia_loc,
                                 u32 base_index, int restart_en,
                                 u32 restart_val, int allow_remap)
{
    const u32 esz = ia->is_u32 ? 4u : 2u;
    u64 total = 0;
    for (u32 b = 0; b < dc.n_idx; b++) {
        const u32 count = dc.idx[b].count;
        if (!count) continue;
        const u64 start = (u64)ia->offset + (u64)dc.idx[b].first * esz;
        const u64 bytes = (u64)count * esz;
        if (start > 0xFFFFFFFFull || bytes > 0xFFFFFFFFull ||
            start + bytes > 0x100000000ull ||
            !eng_guest_ptr(NULL, ia_loc, (u32)start, (u32)bytes))
            return 0;
        total += count;
    }

    if (!ia->is_u32 && allow_remap && dc.n_refs == 0 && total > 1) {
        rsx_vertex_remap* rm = &dc.ref_remap;
        if (total > 0xFFFFFFFFull) return 0;
        if (total > rm->occurrence_capacity) {
            u32* occ = (u32*)realloc(rm->occurrence_to_unique, (size_t)total * sizeof(u32));
            if (!occ) return 0;
            rm->occurrence_to_unique = occ;
            rm->occurrence_capacity = (u32)total;
        }
        /* One spare ref: the loop below writes refs[unique] on every index,
         * hit or miss, so a 16-bit list with all 65536 values needs 65537. */
        if (!dc_reserve_refs((total < 65536u ? total : 65536u) + 1u)) return 0;
        if (!s_idx16_tab) {
            s_idx16_tab = (u32*)calloc(65536u, sizeof(u32));
            if (!s_idx16_tab) return 0;
            s_idx16_gen = 0;
        }
        if (++s_idx16_gen > 0xFFFFu) {
            memset(s_idx16_tab, 0, 65536u * sizeof(u32));
            s_idx16_gen = 1;
        }
        const u32 gen = s_idx16_gen, tag = gen << 16;
        u32* tab = s_idx16_tab;
        u32* occ = rm->occurrence_to_unique;
        rsx_vertex_ref* refs = dc.refs;
        u32 n_occ = 0, unique = 0;
        /* A 16-bit index never equals 0xFFFFFFFF, so this one compare stands
         * for "restart enabled and index == restart value". */
        const u32 restart_cmp = restart_en ? restart_val : 0xFFFFFFFFu;
        for (u32 b = 0; b < dc.n_idx; b++) {
            const u32 count = dc.idx[b].count;
            if (!count) continue;
            const u8* run = eng_guest_ptr(NULL, ia_loc,
                                          ia->offset + dc.idx[b].first * 2u, count * 2u);
            for (u32 i = 0; i < count; i++) {
                u16 w;
                memcpy(&w, run + (size_t)i * 2u, 2);
                const u32 index = __builtin_bswap16(w);
                if (__builtin_expect(index == restart_cmp, 0)) {
                    if (!rsx_restart_cut_push(&dc.cuts, &dc.n_cuts, &dc.cap_cuts, n_occ)) {
                        dc.fetch_ok = 0;
                        return 2;
                    }
                    continue;
                }
                /* Branch-free: a repeat (about 5 in 6 in a triangle list)
                 * and a first occurrence take the same path. The refs slot
                 * past the last unique one is scratch until a miss claims it. */
                const u32 e = tab[index];
                const u32 hit = (e >> 16) == gen;
                const u32 slot = hit ? (e & 0xFFFFu) : unique;
                tab[index] = tag | slot;
                refs[unique].vertex_id = index;
                refs[unique].base_index = base_index;
                occ[n_occ++] = slot;
                unique += hit ^ 1u;
            }
        }
        dc.n_source_refs = n_occ;
        dc.n_refs = unique;
        dc.refs_remapped = unique < n_occ;
        return 2;
    }

    if (!dc_reserve_refs((u64)dc.n_refs + total)) return 0;
    const int whole = dc.n_refs == 0;
    u32 lo = 0xFFFFFFFFu, hi = 0;
    rsx_vertex_ref* refs = dc.refs;
    u32 n = dc.n_refs;
    for (u32 b = 0; b < dc.n_idx; b++) {
        const u32 count = dc.idx[b].count;
        if (!count) continue;
        const u8* run = eng_guest_ptr(NULL, ia_loc, ia->offset + dc.idx[b].first * esz,
                                      count * esz);
        for (u32 i = 0; i < count; i++) {
            u32 index;
            if (ia->is_u32) {
                u32 w; memcpy(&w, run + (size_t)i * 4u, 4); index = __builtin_bswap32(w);
            } else {
                u16 w; memcpy(&w, run + (size_t)i * 2u, 2); index = __builtin_bswap16(w);
            }
            if (restart_en && index == restart_val) {
                if (!rsx_restart_cut_push(&dc.cuts, &dc.n_cuts, &dc.cap_cuts, n)) {
                    dc.n_refs = n;
                    dc.fetch_ok = 0;
                    return 1;
                }
                continue;
            }
            refs[n].vertex_id = index;
            refs[n].base_index = base_index;
            n++;
            if (index < lo) lo = index;
            if (index > hi) hi = index;
        }
    }
    dc.n_refs = n;
    if (whole && n) {
        dc.rng_known = 1;
        dc.rng_lo = lo;
        dc.rng_hi = hi;
    }
    return 1;
}

/* Turn this group's batches into a reference list, recording a CUT rather
 * than fetching a phantom vertex wherever the restart sentinel appears, then
 * collapse repeated references and fetch each unique vertex once.
 * rsx_live_draw.c's fetch_batches_hoisted (4165-4292). */
/* VTX_VERIFY: a copy of what dc_fetch_indexed_fast produced, checked
 * against the per-index path. */
static struct {
    int mode;                   /* dc_fetch_indexed_fast's return, 0 = none */
    u32 n_src, n_refs, n_cuts;
    int remapped;
    rsx_vertex_ref* refs; u32* cuts; u32* occ;
    u32 cap_refs, cap_cuts, cap_occ;
} s_ivf;

static int eng_ivf_copy(void** buf, u32* cap, const void* src, u32 n, size_t elem)
{
    if (n > *cap) {
        void* b = realloc(*buf, (size_t)n * elem);
        if (!b) return 0;
        *buf = b;
        *cap = n;
    }
    if (n) memcpy(*buf, src, (size_t)n * elem);
    return 1;
}

static void eng_ivf_check(int stage)
{
    if (s_ivf.mode != stage) return;
    int same = dc.n_refs == s_ivf.n_refs && dc.n_cuts == s_ivf.n_cuts &&
               !memcmp(dc.refs, s_ivf.refs, (size_t)dc.n_refs * sizeof(*dc.refs)) &&
               !memcmp(dc.cuts, s_ivf.cuts, (size_t)dc.n_cuts * sizeof(u32));
    if (stage == 2)
        same = same && dc.n_source_refs == s_ivf.n_src &&
               dc.refs_remapped == s_ivf.remapped &&
               (!dc.refs_remapped ||
                !memcmp(dc.ref_remap.occurrence_to_unique, s_ivf.occ,
                        (size_t)dc.n_source_refs * sizeof(u32)));
    static u32 checks, logs;
    checks++;
    if (!same && logs++ < 8)
        fprintf(stderr, "[rsx engine] VTX_VERIFY: index read differs (stage %d: refs %u vs %u,"
                        " source %u vs %u, cuts %u vs %u)\n", stage, dc.n_refs, s_ivf.n_refs,
                dc.n_source_refs, s_ivf.n_src, dc.n_cuts, s_ivf.n_cuts);
    if ((checks & 0xFFFFu) == 0)
        fprintf(stderr, "[rsx engine] VTX_VERIFY: %u index reads checked, %u differ\n",
                checks, logs);
    s_ivf.mode = 0;
}

/* The fetch plan's register part: descriptors, defaults, sources. Nothing
 * in it depends on the references, so it is set up before dc_fetch (the
 * geometry memo keys on it) and dc_fetch only adds the element ranges. */
static void dc_plan_init(const rsx_vertex_layout_plan* layout)
{
    dc.layout = *layout;
    rsx_vertex_fetch_plan_init(&dc.fetch_plan, &g.rsx, layout, eng_guest_ptr, NULL);
    rsx_vertex_fetch_plan_set_inline(&dc.fetch_plan, &g.rsx, dc.inl, dc.inl_bytes);
    rsx_vertex_fetch_plan_set_immediate(&dc.fetch_plan, dc.imm, dc.imm_count);
}

static void dc_fetch(const rsx_vertex_layout_plan* layout, int allow_remap)
{
    int deduped_fast = 0;   /* dc_fetch_indexed_fast: 0 not used, 1 read, 2 read + deduped */
    s_ivf.mode = 0;
    for (u32 b = 0; b < dc.n_arr && dc.fetch_ok; b++)
        for (u32 i = 0; i < dc.arr[b].count && dc.fetch_ok; i++)
            if (!dc_push_ref(dc.arr[b].first + i, 0)) dc.fetch_ok = 0;

    if (dc.n_idx && dc.fetch_ok) {
        const u32 base_index = rsx_dsp_vertex_data_base_index(&g.rsx);
        rsx_dsp_index_array ia;
        rsx_dsp_get_index_array(&g.rsx, &ia);
        const u32 esz = ia.is_u32 ? 4u : 2u;
        const int restart_en = rsx_dsp_restart_index_enabled(&g.rsx, ia.is_u32);
        const u32 restart_val = rsx_dsp_restart_index(&g.rsx);
        /* The index array's location field is a DMA selector, not the
         * rsx_dsp location enum: 0 selects local memory. */
        const u32 ia_loc = ia.location ? RSX_LOCATION_MAIN : RSX_LOCATION_LOCAL;

        if (eng_vtx_fast_mode() == 1)
            deduped_fast = dc_fetch_indexed_fast(&ia, ia_loc, base_index, restart_en,
                                                 restart_val, allow_remap);
        if (eng_vtx_fast_mode() == 2) {
            /* Run the fast read, keep its result, then rewind and let the
             * per-index path below produce the reference result. */
            const u32 n_refs0 = dc.n_refs, n_cuts0 = dc.n_cuts;
            const int r = dc_fetch_indexed_fast(&ia, ia_loc, base_index, restart_en,
                                                restart_val, allow_remap);
            if (r && dc.fetch_ok &&
                eng_ivf_copy((void**)&s_ivf.refs, &s_ivf.cap_refs, dc.refs, dc.n_refs,
                             sizeof(*dc.refs)) &&
                eng_ivf_copy((void**)&s_ivf.cuts, &s_ivf.cap_cuts, dc.cuts, dc.n_cuts,
                             sizeof(u32)) &&
                (r != 2 || !dc.refs_remapped ||
                 eng_ivf_copy((void**)&s_ivf.occ, &s_ivf.cap_occ,
                              dc.ref_remap.occurrence_to_unique, dc.n_source_refs,
                              sizeof(u32)))) {
                s_ivf.mode = r;
                s_ivf.n_src = dc.n_source_refs;
                s_ivf.n_refs = dc.n_refs;
                s_ivf.n_cuts = dc.n_cuts;
                s_ivf.remapped = dc.refs_remapped;
            }
            dc.n_refs = n_refs0;
            dc.n_cuts = n_cuts0;
            dc.n_source_refs = 0;
            dc.refs_remapped = 0;
            dc.rng_known = 0;
            dc.fetch_ok = 1;
        }
        for (u32 b = 0; !deduped_fast && b < dc.n_idx && dc.fetch_ok; b++) {
            const u32 first = dc.idx[b].first, count = dc.idx[b].count;
            const u64 start = (u64)ia.offset + (u64)first * esz;
            const u64 bytes = (u64)count * esz;
            const u8* run = NULL;
            if (count && start <= 0xFFFFFFFFull && bytes <= 0xFFFFFFFFull &&
                start + bytes <= 0x100000000ull)
                run = eng_guest_ptr(NULL, ia_loc, (u32)start, (u32)bytes);
            for (u32 i = 0; i < count && dc.fetch_ok; i++) {
                const u8* p = run ? run + (size_t)i * esz
                    : eng_guest_ptr(NULL, ia_loc, ia.offset + (first + i) * esz, esz);
                if (!p) { dc.fetch_ok = 0; break; }
                const u32 index = ia.is_u32
                    ? (((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3])
                    : (u32)((p[0] << 8) | p[1]);
                if (restart_en && index == restart_val) {
                    if (!rsx_restart_cut_push(&dc.cuts, &dc.n_cuts, &dc.cap_cuts,
                                              dc.n_refs))
                        dc.fetch_ok = 0;
                    continue;
                }
                if (!dc_push_ref(index, base_index)) dc.fetch_ok = 0;
            }
        }
    }
    if (!dc.fetch_ok) return;
    eng_ivf_check(1);

    if (deduped_fast != 2) dc.n_source_refs = dc.n_refs;
    /* Only a topology this engine rebuilds through an index buffer may share
     * repeated references. A line strip or a point list is drawn in the order
     * it arrived, so collapsing duplicates would reorder it. */
    if (deduped_fast != 2 && allow_remap && dc.n_refs > 1) {
        u32 unique = dc.n_refs;
        if (eng_remap(&dc.ref_remap, dc.refs, dc.n_refs, &unique)) {
            dc.refs_remapped = unique < dc.n_refs;
            dc.n_refs = unique;
        }
    }
    eng_ivf_check(2);

    /* dc_plan_init already set up dc.fetch_plan for this layout. */
    rsx_vertex_fetch_plan_prepare(&dc.fetch_plan, dc.refs, dc.n_refs);
    if (!dc_reserve_verts((u64)dc.n_refs * layout->stride)) {
        dc.fetch_ok = 0;
        return;
    }
    const int fast = eng_vtx_fast_mode();
    if (fast && layout->stride) {
        if (!eng_fetch_batch(&dc.fetch_plan, dc.refs, dc.n_refs, dc.verts)) {
            dc.fetch_ok = 0;
            return;
        }
        if (fast == 2) {
            static u8* chk; static u64 chk_cap;
            const u64 bytes = (u64)dc.n_refs * layout->stride;
            if (bytes > chk_cap) {
                u8* c = (u8*)realloc(chk, (size_t)bytes);
                if (c) { chk = c; chk_cap = bytes; }
            }
            if (bytes <= chk_cap) {
                int ok = 1;
                for (u32 i = 0; i < dc.n_refs && ok; i++)
                    ok = rsx_vertex_fetch_one(&dc.fetch_plan, &dc.refs[i],
                                              chk + (u64)i * layout->stride);
                if (!ok || memcmp(chk, dc.verts, (size_t)bytes)) {
                    static int nd = 0;
                    if (nd++ < 8) fprintf(stderr, "[rsx engine] VTX_VERIFY: fetch differs (%u verts, %u attrs, ok %d)\n",
                                          dc.n_refs, (unsigned)layout->count, ok);
                }
                static u64 nchk = 0;
                if ((++nchk % 20000u) == 0)
                    fprintf(stderr, "[rsx engine] VTX_VERIFY: %llu draws checked\n",
                            (unsigned long long)nchk);
            }
        }
    } else {
        for (u32 i = 0; i < dc.n_refs; i++) {
            u8* dst = layout->stride ? dc.verts + (u64)i * layout->stride : NULL;
            if (!rsx_vertex_fetch_one(&dc.fetch_plan, &dc.refs[i], dst)) {
                dc.fetch_ok = 0;
                return;
            }
        }
    }
    dc.n_verts = dc.n_refs;
}

/* ---- GEO_MEMO: converted vertices and indices kept across frames --------
 * The D3D12 backend's VCACHE (rsx_d3d12_backend.c, v60) measured 86-89% of
 * the city's vertices byte-identical to the frame before. Here a draw whose
 * inputs hash to a draw seen before reuses that draw's converted vertices and
 * rebuilt index list, instead of reading, deduplicating and converting again.
 * Host memory only: the backend copies what it is handed during draw(), as it
 * does with dc.verts, so nothing about GPU lifetimes changes. The key is XXH3
 * over
 *   - the shape: primitive, batches, index-array registers, base index, restart;
 *   - the guest index bytes;
 *   - the fetch plan: layout, base offset, divider, every descriptor and default;
 *   - the guest vertex bytes every attribute reads, over the element range the
 *     index list spans (remembered per shape + index hash from the draw that
 *     converted it, so a hit never scans the indices).
 * Draws it skips: immediate or inline vertices, a frequency divider, a source
 * that is not one readable span, and the VTX_FAST verify/off modes. A draw
 * whose vertices change every frame (skinning) stops hashing them for a while.
 * PS3RECOMP_ENGINE_GEO_MEMO=0 turns it off; PS3RECOMP_ENGINE_GEO_MB sets the
 * arena (default 256, 16..2048); PS3RECOMP_ENGINE_GEO_STATS=1 logs every 600
 * frames. */
#define GEO_SET_BITS 16u     /* 2-way sets: 128K entries per level */
typedef struct { u64 key; u32 elo, ehi, last; u8 vol, used; } geo_l1;
typedef struct { u64 key; u64 pos; u32 vbytes, n_verts, n_draw, stride, last; u8 indexed, used; } geo_l2;
/* The way holding key, else the way to replace (empty, then least recent). */
static geo_l1* eng_geo_way_l1(geo_l1* t, u64 k)
{
    geo_l1* s = &t[(k & ((1u << GEO_SET_BITS) - 1u)) * 2u];
    if (s[0].used && s[0].key == k) return &s[0];
    if (s[1].used && s[1].key == k) return &s[1];
    if (!s[0].used) return &s[0];
    if (!s[1].used) return &s[1];
    return (s[0].last <= s[1].last) ? &s[0] : &s[1];
}
static geo_l2* eng_geo_way_l2(geo_l2* t, u64 k)
{
    geo_l2* s = &t[(k & ((1u << GEO_SET_BITS) - 1u)) * 2u];
    if (s[0].used && s[0].key == k) return &s[0];
    if (s[1].used && s[1].key == k) return &s[1];
    if (!s[0].used) return &s[0];
    if (!s[1].used) return &s[1];
    return (s[0].last <= s[1].last) ? &s[0] : &s[1];
}
static geo_l1* s_geo_l1;
static geo_l2* s_geo_l2;
static u8* s_geo_arena;
static u64 s_geo_size, s_geo_head;
static int s_geo_on = -1, s_geo_stats;
/* GEO_DIRECT: the arena is the backend's GPU-visible buffer (arena_create)
 * and a hit is drawn from it as it is (draw_arena), as the D3D12 VCACHE binds
 * its cache buffer. PS3RECOMP_ENGINE_GEO_DIRECT=0: hits are copied into the
 * staging like any other draw. */
static int s_geo_gpu;
static struct { u64 draws, elig, hit, miss1, miss2, volskip, stored, hashb, reuseb, storeb, refresh, direct, refused; } s_geo_st;

static int eng_geo_on(void)
{
    if (s_geo_on >= 0) return s_geo_on;
    s_geo_on = eng_env_flag("PS3RECOMP_ENGINE_GEO_MEMO", 1);
    s_geo_stats = eng_env_flag("PS3RECOMP_ENGINE_GEO_STATS", 0);
    u64 mb = 256;
    const char* e = getenv("PS3RECOMP_ENGINE_GEO_MB");
    if (e && *e) { mb = strtoull(e, NULL, 10); if (mb < 16) mb = 16; if (mb > 2048) mb = 2048; }
    if (s_geo_on) {
        s_geo_size = mb << 20;
        s_geo_l1 = (geo_l1*)calloc((size_t)2 << GEO_SET_BITS, sizeof(geo_l1));
        s_geo_l2 = (geo_l2*)calloc((size_t)2 << GEO_SET_BITS, sizeof(geo_l2));
        if (g.be && g.be->arena_create && g.be->arena_retire && g.be->draw_arena &&
            eng_env_flag("PS3RECOMP_ENGINE_GEO_DIRECT", 1)) {
            s_geo_arena = (u8*)g.be->arena_create(g.be->user, s_geo_size);
            s_geo_gpu = s_geo_arena != NULL;
        }
        if (!s_geo_arena) s_geo_arena = (u8*)malloc((size_t)s_geo_size);
        if (!s_geo_l1 || !s_geo_l2 || !s_geo_arena) {
            free(s_geo_l1); free(s_geo_l2);
            if (!s_geo_gpu) free(s_geo_arena);
            s_geo_l1 = NULL; s_geo_l2 = NULL; s_geo_arena = NULL;
            s_geo_on = 0; s_geo_gpu = 0;
        }
    }
    fprintf(stderr, "[rsx engine] geometry memo: %s (PS3RECOMP_ENGINE_GEO_MEMO, %llu MB%s)\n",
            s_geo_on ? "on" : "off", (unsigned long long)(s_geo_on ? mb : 0),
            s_geo_gpu ? ", hits drawn from it: PS3RECOMP_ENGINE_GEO_DIRECT" : "");
    return s_geo_on;
}

/* Arena: a byte ring addressed by absolute position. Data at pos is intact
 * while the head has not gone a full ring past it. NULL (and nothing moves)
 * when GEO_DIRECT draws still to be submitted read the bytes it would hand
 * out; the backend has waited for any submitted ones. */
static u8* eng_geo_alloc(u32 bytes, u64* pos)
{
    u64 head = s_geo_head;
    const u64 off = head % s_geo_size;
    if (off + bytes > s_geo_size) head += s_geo_size - off;
    if (s_geo_gpu && head + bytes > s_geo_size &&
        !g.be->arena_retire(g.be->user, head + bytes - s_geo_size)) {
        s_geo_st.refused++;
        return NULL;
    }
    *pos = head;
    s_geo_head = head + bytes;
    return s_geo_arena + (head % s_geo_size);
}

/* A hit drawn from the arena must stay intact until its frame is on the GPU
 * and done. One in the oldest eighth is copied instead: the next stores
 * would soon need its bytes, and arena_retire would have to wait for it. */
static int eng_geo_direct(const geo_l2* l2)
{
    return s_geo_gpu && s_geo_head + s_geo_size / 8u <= l2->pos + s_geo_size;
}

typedef struct {
    int ok;             /* eligible for the memo */
    u64 shape;          /* shape + index bytes */
    u64 plan;           /* fetch plan */
    geo_l1* l1;
    geo_l2* l2;
    u64 key;            /* full key, when the vertex hash was taken */
    int have_key;
} eng_geo_probe;

/* Hash the guest bytes every laid-out attribute reads for elements
 * [elo, ehi]: one range per attribute, overlapping ranges of one location
 * merged so an interleaved buffer is read once. 0 = a range is unreadable. */
static int eng_geo_vtx_hash(const rsx_vertex_fetch_plan* plan, u32 elo, u32 ehi, u64* out)
{
    u32 loc[RSX_DSP_NUM_VERTEX_ATTR];
    u64 lo[RSX_DSP_NUM_VERTEX_ATTR], hi[RSX_DSP_NUM_VERTEX_ATTR];
    u32 n = 0;
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const rsx_vertex_fetch_attr* f = &plan->attr[plan->layout.attrs[slot]];
        if (!f->desc.type || !f->desc.size || !f->elem_size) continue;
        const u64 prefix = (u64)plan->base_offset + (u64)f->desc.offset;
        const u64 a = prefix + (u64)elo * f->stride;
        const u64 b = prefix + (u64)ehi * f->stride + f->elem_size;
        if (a > 0xFFFFFFFFull || b > 0x100000000ull || b <= a) return 0;
        /* insertion sort by (location, start) */
        u32 k = n++;
        while (k > 0 && (loc[k - 1] > f->desc.location ||
                         (loc[k - 1] == f->desc.location && lo[k - 1] > a))) {
            loc[k] = loc[k - 1]; lo[k] = lo[k - 1]; hi[k] = hi[k - 1]; k--;
        }
        loc[k] = f->desc.location; lo[k] = a; hi[k] = b;
    }
    u64 h = 0x9E3779B97F4A7C15ull;
    for (u32 i = 0; i < n;) {
        u64 a = lo[i], b = hi[i];
        u32 j = i + 1;
        while (j < n && loc[j] == loc[i] && lo[j] <= b) { if (hi[j] > b) b = hi[j]; j++; }
        const u8* src = eng_guest_ptr(NULL, loc[i], (u32)a, (u32)(b - a));
        if (!src) return 0;
        h = XXH3_64bits_withSeed(src, (size_t)(b - a), h ^ ((u64)loc[i] << 40) ^ a);
        s_geo_st.hashb += b - a;
        i = j;
    }
    *out = h;
    return 1;
}

/* Every field of the plan the conversion reads, packed without padding:
 * u32 words only, laid-out slots only. */
static u64 eng_geo_plan_hash(const rsx_vertex_fetch_plan* plan)
{
    u32 k[4 + RSX_DSP_NUM_VERTEX_ATTR * 13];
    u32 n = 0;
    k[n++] = plan->layout.mask;
    k[n++] = plan->layout.stride;
    k[n++] = plan->base_offset;
    k[n++] = plan->divider_mask;
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        const rsx_vertex_fetch_attr* f = &plan->attr[attr];
        k[n++] = attr;
        k[n++] = f->desc.type; k[n++] = f->desc.size; k[n++] = f->desc.stride;
        k[n++] = f->desc.frequency; k[n++] = f->desc.offset; k[n++] = f->desc.location;
        k[n++] = f->elem_size; k[n++] = f->stride;
        memcpy(&k[n], f->default_value, 16); n += 4;
    }
    return XXH3_64bits(k, (size_t)n * 4u);
}

/* Before dc_fetch. Returns the cached entry on a hit, else NULL with the
 * probe filled in for eng_geo_store. */
static const geo_l2* eng_geo_lookup(u32 prim, eng_geo_probe* pr)
{
    memset(pr, 0, sizeof *pr);
    if (!eng_geo_on() || eng_vtx_fast_mode() != 1) return NULL;
    s_geo_st.draws++;
    const rsx_vertex_fetch_plan* plan = &dc.fetch_plan;
    if (dc.imm_count || dc.inl || plan->inline_data || plan->imm_verts || !plan->layout.stride)
        return NULL;
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const rsx_vertex_fetch_attr* f = &plan->attr[plan->layout.attrs[slot]];
        if (f->desc.type && f->desc.size && f->desc.frequency > 1u) return NULL;
    }

    struct {
        u32 prim, n_arr, n_idx, base_index, ia_off, ia_loc, is_u32, restart_en, restart_val;
    } sh;
    memset(&sh, 0, sizeof sh);
    sh.prim = prim;
    sh.n_arr = dc.n_arr;
    sh.n_idx = dc.n_idx;
    u64 h = 0;
    if (dc.n_idx) {
        rsx_dsp_index_array ia;
        rsx_dsp_get_index_array(&g.rsx, &ia);
        sh.base_index = rsx_dsp_vertex_data_base_index(&g.rsx);
        sh.ia_off = ia.offset;
        sh.ia_loc = ia.location ? RSX_LOCATION_MAIN : RSX_LOCATION_LOCAL;
        sh.is_u32 = ia.is_u32;
        sh.restart_en = (u32)rsx_dsp_restart_index_enabled(&g.rsx, ia.is_u32);
        sh.restart_val = rsx_dsp_restart_index(&g.rsx);
        h = XXH3_64bits(&sh, sizeof sh);
        const u32 esz = ia.is_u32 ? 4u : 2u;
        for (u32 b = 0; b < dc.n_idx; b++) {
            const u32 count = dc.idx[b].count;
            if (!count) continue;
            const u64 start = (u64)ia.offset + (u64)dc.idx[b].first * esz;
            const u64 bytes = (u64)count * esz;
            if (start + bytes > 0x100000000ull) return NULL;
            const u8* run = eng_guest_ptr(NULL, sh.ia_loc, (u32)start, (u32)bytes);
            if (!run) return NULL;
            h = XXH3_64bits_withSeed(run, (size_t)bytes, h ^ start);
            s_geo_st.hashb += bytes;
        }
    } else {
        h = XXH3_64bits(&sh, sizeof sh);
    }
    h = XXH3_64bits_withSeed(dc.idx, dc.n_idx * sizeof(dc.idx[0]), h);
    h = XXH3_64bits_withSeed(dc.arr, dc.n_arr * sizeof(dc.arr[0]), h);
    pr->ok = 1;
    pr->shape = h;
    pr->plan = eng_geo_plan_hash(plan);
    s_geo_st.elig++;

    geo_l1* l1 = eng_geo_way_l1(s_geo_l1, h);
    pr->l1 = l1;
    if (!l1->used || l1->key != h) { s_geo_st.miss1++; return NULL; }
    l1->last = g.frames;
    /* Vertices that changed the last three times: probe 1 frame in 16. */
    if (l1->vol >= 3 && (g.frames & 15u)) { s_geo_st.volskip++; pr->ok = 0; return NULL; }
    u64 hv;
    if (!eng_geo_vtx_hash(plan, l1->elo, l1->ehi, &hv)) { pr->ok = 0; return NULL; }
    u64 key = XXH3_64bits_withSeed(&pr->plan, sizeof pr->plan, h ^ hv);
    if (!key) key = 1;
    pr->key = key;
    pr->have_key = 1;
    geo_l2* l2 = eng_geo_way_l2(s_geo_l2, key);
    pr->l2 = l2;
    if (!l2->used || l2->key != key || l2->stride != plan->layout.stride ||
        s_geo_head > l2->pos + s_geo_size) {
        if (l1->vol < 255) l1->vol++;
        s_geo_st.miss2++;
        return NULL;
    }
    l1->vol = 0;
    l2->last = g.frames;
    const u32 ib = l2->indexed ? l2->n_draw * 4u : 0u;
    const u32 bytes = ((l2->vbytes + 15u) & ~15u) + ib;
    /* Older than half the ring: move it up front, so data drawn every frame
     * is never the next to be overwritten. The copy may skip to the ring's
     * start and then needs 2 x bytes; it must end before the head reaches
     * the source's slot again. Too close to that: draw it from where it is. */
    const u64 age = s_geo_head - l2->pos, room = ((u64)bytes + 15u) & ~15ull;
    if (age > s_geo_size / 2u && age + 2u * room <= s_geo_size) {
        const u8* src = s_geo_arena + (l2->pos % s_geo_size);
        u64 pos;
        u8* dst = eng_geo_alloc((u32)room, &pos);
        if (dst) {
            memcpy(dst, src, bytes);
            l2->pos = pos;
            s_geo_st.refresh++;
        }
    }
    s_geo_st.hit++;
    s_geo_st.reuseb += bytes;
    return l2;
}

/* After a miss converted the draw: remember the element range and the data. */
static void eng_geo_store(eng_geo_probe* pr, int indexed, u32 n_draw, const u32* indices)
{
    if (!pr->ok || !dc.fetch_ok || !dc.n_verts) return;
    const rsx_vertex_fetch_plan* plan = &dc.fetch_plan;
    u32 elo = 0, ehi = 0;
    int have = 0;
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const rsx_vertex_fetch_attr* f = &plan->attr[plan->layout.attrs[slot]];
        if (!f->desc.type || !f->desc.size || !f->elem_size) continue;
        if (!f->stable_base) return;    /* read element by element: not memoized */
        if (!have) { elo = f->stable_first; ehi = f->stable_last; have = 1; }
        else if (f->stable_first != elo || f->stable_last != ehi) return;
    }
    if (!have) return;  /* no array at all: defaults only, cheap anyway */
    geo_l1* l1 = pr->l1;
    if (!l1->used || l1->key != pr->shape || l1->elo != elo || l1->ehi != ehi) {
        l1->key = pr->shape; l1->elo = elo; l1->ehi = ehi; l1->vol = 0; l1->used = 1;
        pr->have_key = 0;
    }
    l1->last = g.frames;
    if (!pr->have_key) {
        u64 hv;
        if (!eng_geo_vtx_hash(plan, elo, ehi, &hv)) return;
        pr->key = XXH3_64bits_withSeed(&pr->plan, sizeof pr->plan, pr->shape ^ hv);
        if (!pr->key) pr->key = 1;
    }
    const u32 vbytes = dc.n_verts * plan->layout.stride;
    const u32 ib = indexed ? n_draw * 4u : 0u;
    const u64 total = (((u64)vbytes + 15u) & ~15ull) + ib;
    if (total > s_geo_size / 8u) return;
    u64 pos;
    u8* dst = eng_geo_alloc((u32)((total + 15u) & ~15ull), &pos);
    if (!dst) return;
    memcpy(dst, dc.verts, vbytes);
    if (ib) memcpy(dst + ((vbytes + 15u) & ~15u), indices, ib);
    geo_l2* l2 = eng_geo_way_l2(s_geo_l2, pr->key);
    l2->key = pr->key;
    l2->last = g.frames;
    l2->pos = pos;
    l2->vbytes = vbytes;
    l2->n_verts = dc.n_verts;
    l2->n_draw = n_draw;
    l2->stride = plan->layout.stride;
    l2->indexed = (u8)(indexed != 0);
    l2->used = 1;
    s_geo_st.stored++;
    s_geo_st.storeb += total;
}

/* PS3RECOMP_ENGINE_GEO_VERIFY=1: every hit also converts the draw the long
 * way and compares vertices, index list and counts with what it reused. */
static void eng_geo_verify_hit(u32 prim, int rebuild, const rsx_vertex_layout_plan* layout,
                               const geo_l2* hit)
{
    static int on = -1;
    if (on < 0) on = eng_env_flag("PS3RECOMP_ENGINE_GEO_VERIFY", 0);
    if (!on) return;
    static u64 checks, bad;
    const u8* verts = s_geo_arena + (hit->pos % s_geo_size);
    const u32* idx = (const u32*)(verts + ((hit->vbytes + 15u) & ~15u));
    dc_fetch(layout, rebuild);
    int ok = dc.n_verts && dc.fetch_ok, indexed = 0;
    u32 n_draw = dc.n_source_refs;
    if (ok && rebuild) {
        ok = eng_topology_rebuild(prim, dc.refs_remapped, &indexed) != 0;
        n_draw = indexed ? rsx_draw_engine_topology_index_count(prim, dc.n_source_refs,
                                                                dc.cuts, dc.n_cuts)
                         : dc.n_source_refs - dc.n_source_refs % 3u;
    }
    if (ok && indexed) {
        ok = dc_reserve_indices(n_draw);
        if (ok)
            rsx_draw_engine_write_topology_indices(
                prim, dc.n_source_refs, dc.cuts, dc.n_cuts,
                dc.refs_remapped ? dc.ref_remap.occurrence_to_unique : NULL, g.indices);
    }
    const int same = ok && dc.n_verts == hit->n_verts && indexed == (int)hit->indexed &&
                     n_draw == hit->n_draw &&
                     !memcmp(dc.verts, verts, (size_t)hit->n_verts * layout->stride) &&
                     (!indexed || !memcmp(g.indices, idx, (size_t)n_draw * 4u));
    checks++;
    if (!same && bad++ < 16)
        fprintf(stderr, "[rsx engine] GEO_VERIFY: hit differs (prim %u verts %u/%u draw %u/%u"
                        " indexed %d/%d ok %d)\n", prim, dc.n_verts, hit->n_verts, n_draw,
                hit->n_draw, indexed, (int)hit->indexed, ok);
    if ((checks % 100000u) == 0)
        fprintf(stderr, "[rsx engine] GEO_VERIFY: %llu hits checked, %llu differ\n",
                (unsigned long long)checks, (unsigned long long)bad);
}

static void eng_geo_frame(void)
{
    if (!s_geo_on || !s_geo_stats || (g.frames % 600u) != 599u) return;
    const double f = 600.0;
    fprintf(stderr, "[rsx engine] GEO_MEMO per frame: draws %.0f eligible %.0f hit %.0f"
                    " miss(new %.0f changed %.0f) volatile-skip %.0f | hashed %.1f MB"
                    " reused %.1f MB stored %.1f MB refreshed %.0f | direct %.0f refused %.1f\n",
            s_geo_st.draws / f, s_geo_st.elig / f, s_geo_st.hit / f, s_geo_st.miss1 / f,
            s_geo_st.miss2 / f, s_geo_st.volskip / f, s_geo_st.hashb / f / 1048576.0,
            s_geo_st.reuseb / f / 1048576.0, s_geo_st.storeb / f / 1048576.0,
            s_geo_st.refresh / f, s_geo_st.direct / f, s_geo_st.refused / f);
    memset(&s_geo_st, 0, sizeof s_geo_st);
}

/* ---- one-frame trace and target dump (debug, opt-in) -------------------- */

/* PS3RECOMP_ENGINE_TRACE_AT=<n>: log every draw of presented frame n (target,
 * texture sources, drops). PS3RECOMP_ENGINE_SURFACE_DUMP=<dir> with it: write
 * every colour target as a PPM when frame n is presented, with per-channel
 * mean/max, so a dark frame can be followed back to the pass that went dark. */
/* PS3RECOMP_ENGINE_TRACE_AT=<frame>, or <seconds>s for the first frame that
 * starts that long after the first present: the flip rate swings from
 * hundreds a second in a movie to a dozen in a 3D scene, so a frame number
 * lands on a different scene from run to run. */
static long   s_trace_at = -2;
static double s_trace_at_sec = -1.0;
static long eng_trace_at(void)
{
    if (s_trace_at == -2) {
        const char* e = getenv("PS3RECOMP_ENGINE_TRACE_AT");
        char* end = NULL;
        s_trace_at = -1;
        if (e && *e) {
            const double v = strtod(e, &end);
            if (end && *end == 's') s_trace_at_sec = v;
            else s_trace_at = (long)v;
        }
    }
    return s_trace_at;
}
static void eng_trace_tick(u32 frame)
{
    if (eng_trace_at() >= 0 || s_trace_at_sec < 0.0) return;
    static double t0 = -1.0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const double now = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
    if (t0 < 0.0) t0 = now;
    if (now - t0 >= s_trace_at_sec) {
        s_trace_at = (long)frame + 1;
        fprintf(stderr, "[eng-trace] tracing frame %ld (%.1fs)\n", s_trace_at, now - t0);
    }
}
#define ENG_TRACING() (eng_trace_at() >= 0 && (long)g.frames == eng_trace_at())

static char s_tex_kind[RSX_BE_MAX_TEXTURES];   /* 'S' surface, 'Z' depth, 'T' texture */
static u32  s_tex_src[RSX_BE_MAX_TEXTURES];    /* surface slot or guest offset          */
static u32  s_tex_fmt[RSX_BE_MAX_TEXTURES];    /* SET_TEXTURE_FORMAT byte               */

/* A float register (depth bounds and the like) as the guest wrote it. */
static float eng_reg_f32(u32 method)
{
    const u32 bits = rsx_dsp_reg(&g.rsx, method);
    float f; memcpy(&f, &bits, 4);
    return f;
}

static float eng_half(u16 h)
{
    const u32 s = (u32)(h >> 15) << 31, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    u32 bits;
    if (e == 0) {
        if (!m) bits = s;
        else { float f = (float)m / 16777216.0f; memcpy(&bits, &f, 4); bits |= s; }
    } else if (e == 31) bits = s | 0x7F800000u | (m << 13);
    else bits = s | ((e + 112u) << 23) | (m << 13);
    float f; memcpy(&f, &bits, 4);
    return f;
}

/* One surface to <dir>/f<frame>_<tag>_s<slot>_<offset>_<w>x<h>_fmt<n>.ppm,
 * with its channel means and maxima on stderr. The caller has submitted. */
static void eng_dump_one(const char* dir, u32 i, const char* tag)
{
    {
        const eng_surface* s = &g.surfaces[i];
        u32 bpp = 0, ch = 0, kind = 0;   /* kind 0 unorm8, 1 unorm16, 2 half, 3 float */
        switch (s->fmt) {
        case RSX_BE_FMT_R8:            bpp = 1;  ch = 1; kind = 0; break;
        case RSX_BE_FMT_R8G8:          bpp = 2;  ch = 2; kind = 0; break;
        case RSX_BE_FMT_R8G8B8A8:      bpp = 4;  ch = 4; kind = 0; break;
        case RSX_BE_FMT_R16:           bpp = 2;  ch = 1; kind = 1; break;
        case RSX_BE_FMT_R16G16:        bpp = 4;  ch = 2; kind = 1; break;
        case RSX_BE_FMT_R16G16F:       bpp = 4;  ch = 2; kind = 2; break;
        case RSX_BE_FMT_R16G16B16A16F: bpp = 8;  ch = 4; kind = 2; break;
        case RSX_BE_FMT_R32F:          bpp = 4;  ch = 1; kind = 3; break;
        case RSX_BE_FMT_R32G32B32A32F: bpp = 16; ch = 4; kind = 3; break;
        default: break;
        }
        if (!s->handle || !bpp || !s->w || !s->h) return;
        u8* raw = (u8*)malloc((size_t)s->w * s->h * bpp);
        u8* rgb = (u8*)malloc((size_t)s->w * s->h * 3);
        if (!raw || !rgb) { free(raw); free(rgb); return; }
        g.be->readback(g.be->user, s->handle, 0, 0, s->w, s->h, raw, s->w * bpp);
        double sum[4] = {0, 0, 0, 0};
        float mx[4] = {-1e30f, -1e30f, -1e30f, -1e30f};
        const size_t n = (size_t)s->w * s->h;
        for (size_t p = 0; p < n; p++) {
            float v[4] = {0, 0, 0, 1};
            const u8* px = raw + p * bpp;
            for (u32 c = 0; c < ch; c++) {
                if (kind == 0) v[c] = px[c] / 255.0f;
                else if (kind == 1) { u16 q; memcpy(&q, px + c * 2, 2); v[c] = q / 65535.0f; }
                else if (kind == 2) { u16 q; memcpy(&q, px + c * 2, 2); v[c] = eng_half(q); }
                else memcpy(&v[c], px + c * 4, 4);
            }
            for (u32 c = 0; c < 4; c++) { sum[c] += v[c]; if (v[c] > mx[c]) mx[c] = v[c]; }
            for (u32 c = 0; c < 3; c++) {
                const float x = ch == 1 ? v[0] : v[c];
                rgb[p * 3 + c] = (u8)(x <= 0.0f ? 0 : x >= 1.0f ? 255 : (int)(x * 255.0f + 0.5f));
            }
        }
        char path[1024];
        snprintf(path, sizeof path, "%s/f%u_%s_s%02u_%08X_%ux%u_fmt%d.ppm",
                 dir, g.frames, tag, i, s->offset, s->w, s->h, (int)s->fmt);
        FILE* f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", s->w, s->h);
            fwrite(rgb, 1, n * 3, f);
            fclose(f);
        }
        fprintf(stderr, "[eng-dump] f%u %s s%u loc=%u off=%08X %ux%u fmt=%d mean=(%.3f %.3f %.3f %.3f) max=(%.3f %.3f %.3f %.3f)%s\n",
                g.frames, tag, i, s->location, s->offset, s->w, s->h, (int)s->fmt,
                sum[0] / n, sum[1] / n, sum[2] / n, sum[3] / n, mx[0], mx[1], mx[2], mx[3],
                g.last_present_surface == i ? " PRESENTED" : "");
        free(raw); free(rgb);
    }
}

static const char* eng_dump_dir(void)
{
    const char* dir = getenv("PS3RECOMP_ENGINE_SURFACE_DUMP");
    return (dir && *dir && g.be->readback) ? dir : NULL;
}

static void eng_dump_surfaces(void)
{
    const char* dir = eng_dump_dir();
    if (!dir) return;
    if (g.be->submit_and_wait) g.be->submit_and_wait(g.be->user, 0);
    for (u32 i = 0; i < g.n_surfaces; i++) eng_dump_one(dir, i, "end");
}

/* In the traced frame, each pass's target as the pass ends: called before a
 * draw resolves its targets, so a pass that reuses the address at another
 * size cannot have replaced the surface yet. */
static u32 s_trace_prev = ENG_INVALID, s_trace_prev_off, s_trace_prev_w, s_trace_prev_h;
static u32 s_trace_pass;
static void eng_trace_pass_boundary(int frame_end)
{
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(&g.rsx, &sf);
    const u32 sel = (sf.color_target == CELL_GCM_SURFACE_TARGET_1) ? 1u : 0u;
    const u32 off = sf.color_offset[sel];
    if (!frame_end && s_trace_prev != ENG_INVALID && off == s_trace_prev_off &&
        sf.clip_w == s_trace_prev_w && sf.clip_h == s_trace_prev_h)
        return;
    const char* dir = eng_dump_dir();
    if (dir && s_trace_prev != ENG_INVALID && s_trace_prev < g.n_surfaces) {
        char tag[16];
        snprintf(tag, sizeof tag, "p%03u", s_trace_pass);
        if (g.be->submit_and_wait) g.be->submit_and_wait(g.be->user, 0);
        eng_dump_one(dir, s_trace_prev, tag);
    }
    s_trace_pass++;
    s_trace_prev = ENG_INVALID;
    if (frame_end) return;
    s_trace_prev_off = off; s_trace_prev_w = sf.clip_w; s_trace_prev_h = sf.clip_h;
}

/* ---- sink ---------------------------------------------------------------- */

static void sink_begin(void* user, const rsx_dispatch* r, u32 prim)
{
    (void)user; (void)r; (void)prim;
    dc_reset();
}

static void sink_draw_arrays(void* user, const rsx_dispatch* r, u32 first, u32 count)
{
    (void)user; (void)r;
    dc.n_packets++;
    if (dc.n_arr >= ENG_MAX_BATCHES) return;
    dc.arr[dc.n_arr].first = first;
    dc.arr[dc.n_arr].count = count;
    dc.n_arr++;
}

/* INLINE_ARRAY delivers one finished stream rather than a (first, count)
 * range; queue it as an ordinary consecutive batch and let the fetch plan
 * read the vertices out of it (rsx_vertex_fetch_plan_set_inline). */
static void sink_inline_array(void* user, const rsx_dispatch* r,
                              const u8* data, u32 bytes)
{
    (void)user;
    const u32 stride = rsx_vertex_inline_layout(r, NULL);
    if (!stride || bytes < stride) return;
    dc.inl = data;
    dc.inl_bytes = bytes;
    dc.n_packets++;
    if (dc.n_arr >= ENG_MAX_BATCHES) return;
    dc.arr[dc.n_arr].first = 0;
    dc.arr[dc.n_arr].count = bytes / stride;
    dc.n_arr++;
}

/* Immediate-mode vertices (SET_VERTEX_DATA*_M inside BEGIN/END): queue them as
 * one consecutive batch and let the fetch plan read them out of the captured
 * snapshot (rsx_vertex_fetch_plan_set_immediate). The vtable path draws the
 * same vertices (rsx_commands.c, BEGIN_END(0)); RSX_NO_IMMEDIATE=1 turns both
 * off. */
static void sink_immediate(void* user, const rsx_dispatch* r,
                           const float (*verts)[RSX_DSP_NUM_VERTEX_ATTR][4], u32 count)
{
    (void)user; (void)r;
    static int off = -1;
    if (off < 0) off = getenv("RSX_NO_IMMEDIATE") ? 1 : 0;
    if (off || !verts || !count) return;
    dc.imm = verts;
    dc.imm_count = count;
    dc.n_packets++;
    if (dc.n_arr >= ENG_MAX_BATCHES) return;
    dc.arr[dc.n_arr].first = 0;
    dc.arr[dc.n_arr].count = count;
    dc.n_arr++;
}

static void sink_draw_index(void* user, const rsx_dispatch* r, u32 first, u32 count)
{
    (void)user; (void)r;
    dc.n_packets++;
    if (dc.n_idx >= ENG_MAX_BATCHES) return;
    dc.idx[dc.n_idx].first = first;
    dc.idx[dc.n_idx].count = count;
    dc.n_idx++;
}

/* Which texture units this draw binds, and from where: a unit naming a
 * registered colour surface samples that live target rather than guest
 * memory (every post-process, shadow and reflection in a PS3 title is a
 * render-to-texture read back through a texture unit), a unit naming a
 * tracked zeta in DEPTH24_D8 samples its snapshot, and everything else is a
 * guest upload (rsx_live_draw.c:5986-6062). */
static u32 sink_bind_textures(const u32* target_slots, u32 n_targets,
                              u32 current_zslot,
                              u32 textures[RSX_BE_MAX_TEXTURES],
                              rsx_be_sampler_desc samplers[RSX_BE_MAX_TEXTURES],
                              u32* out_cube_mask)
{
    u32 mask = 0, cube_mask = 0;
    g.shkey = 0;
    g.texk = 0;
    s_bind_serial++;
    for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
        /* The enable bit alone first: SET_TEXTURE_CONTROL0 bit 31, the bit
         * rsx_dsp_get_texture reports as t.enabled. Most of the 16 units are
         * off, and decoding all of a unit's registers was ~3% of the RSX
         * thread on the iPad. */
        if (!(rsx_dsp_reg(&g.rsx, 0x1A0Cu + u * 0x20u) >> 31)) continue;
        rsx_dsp_texture t;
        rsx_dsp_get_texture(&g.rsx, u, &t);
        if (!t.enabled) continue;
        mask |= 1u << u;
        /* The pipeline's cube mask, from the same registers. A cube face is
         * square by construction, so a non-square image cannot be one; the
         * fragment program is compiled against this mask and a texturecube
         * slot filled with a 2D texture is a validation failure, not a wrong
         * pixel. */
        if (t.cubemap && t.width && t.width == t.height) cube_mask |= 1u << u;
        s_tex_kind[u] = 'T'; s_tex_src[u] = t.offset; s_tex_fmt[u] = t.format;
        eng_decode_sampler(t.filter, t.wrap, t.control0, &samplers[u]);
        /* From the register, whatever the unit ends up reading (guest upload,
         * live target, copy), as the D3D12 backend sets it at bind time. */
        if (u < 8) g.texk |= eng_tex_xf_mode(u, &t) << (2 * u);

        /* The depth target at this address (zc), and the same if this draw
         * is not testing against it (zi). */
        u32 zi = ENG_INVALID, zc = ENG_INVALID;
        int sampled = -1, feedback = 0;
        const int maybe_rt = eng_rt_maybe(t.location, t.offset);
        for (u32 i = 0; maybe_rt && i < g.n_zdepths; i++)
            if (g.zdepths[i].handle && g.zdepths[i].location == t.location &&
                g.zdepths[i].offset == t.offset) {
                if (zc == ENG_INVALID) zc = i;
                if (current_zslot != i) { zi = i; break; }
            }

        for (u32 i = 0; maybe_rt && i < g.n_surfaces; i++) {
            if (!g.surfaces[i].handle || g.surfaces[i].location != t.location ||
                g.surfaces[i].offset != t.offset)
                continue;
            /* Not a colour target only ever drawn with colour mask 0 when a
             * depth target shares its address: that pass exported depth.
             * inFamous' 0x0F340000 is bound as colour (mask 0) AND zeta by
             * its shadow pass; the light pass then samples it as a depth
             * texture. The D3D12 backend's v73c rule (ZS_CWRITTEN). */
            if (zc != ENG_INVALID && !g.surfaces[i].cwritten && eng_zs_cwritten()) break;
            /* One this draw writes (an MRT set writes more than one) cannot
             * be read while it is written on any API: it reads a copy made
             * right before the draw, as the D3D12 backend does. */
            for (u32 k = 0; k < n_targets; k++) if (target_slots[k] == i) feedback = 1;
            if (feedback && !eng_feedback_on()) break;
            sampled = (int)i;
            break;
        }
        if (sampled >= 0) {
            u32 src = g.surfaces[sampled].handle;
            if (feedback) src = eng_surface_feedback((u32)sampled);
            if (src) {
                const u32 view = g.be->surface_view
                    ? g.be->surface_view(g.be->user, src, t.remap & 0xFFFFu, t.format)
                    : 0;
                textures[u] = view ? view : src;
                s_tex_kind[u] = feedback ? 'F' : 'S'; s_tex_src[u] = (u32)sampled;
                continue;
            }
        }
        const u32 base_fmt = t.format & RSX_TEX_FMT_BASE_MASK & ~(u32)RSX_TEX_FMT_UNNORM;
        /* A zeta read as A8R8G8B8 reads the colour copy of its depth and
         * stencil, never the stale guest bytes at that address (v49). */
        if (base_fmt == RSX_TEX_FMT_A8R8G8B8 && zc != ENG_INVALID && !t.cubemap &&
            eng_zconv_on()) {
            const u32 cc = eng_zdepth_color_copy(zc, t.width, t.height);
            if (cc) {
                const u32 view = g.be->surface_view
                    ? g.be->surface_view(g.be->user, cc, t.remap & 0xFFFFu, t.format)
                    : 0;
                textures[u] = view ? view : cc;
                s_tex_kind[u] = 'C'; s_tex_src[u] = zc;
                continue;
            }
        }
        /* A depth-format sampler (DEPTH24_D8 .. DEPTH16_FLOAT) is a shadow-map
         * lookup: the RSX texture unit compares the stored depth with the
         * coordinate's z (TEXTURE_ADDRESS zfunc) and the program reads the
         * 0..1 result. As the D3D12 backend does (v34 SHADOWS): units 0-3
         * that read a depth target get the depth itself and the compare in
         * the fragment program; with no depth target, SHADOW_LIT gives 1.0. */
        if (base_fmt >= RSX_TEX_FMT_DEPTH24_D8 && base_fmt <= RSX_TEX_FMT_DEPTH24_D8 + 3u) {
            const int cmp = u < 4 && !t.cubemap && eng_shadows_on();
            const u32 snap = (zi != ENG_INVALID)
                ? eng_zdepth_snapshot(zi, t.width, t.height) : 0;
            if (snap) {
                textures[u] = snap; s_tex_kind[u] = 'Z'; s_tex_src[u] = zi;
                if (cmp) g.shkey |= (1u << u) | (((t.wrap >> 28) & 7u) << (4 + 3 * u));
                continue;
            }
            if (cmp && eng_shadow_lit()) {
                g.shkey |= (1u << u) | (7u << (4 + 3 * u));   /* ALWAYS: lit */
                s_tex_kind[u] = 'L';
            }
        }
        textures[u] = eng_texture_slot(t.location, t.offset, t.format,
                                       t.width, t.height, t.mipmaps, t.pitch,
                                       (int)t.cubemap, t.remap & 0xFFFFu);
    }
    *out_cube_mask = cube_mask;
    return mask;
}

static u32 sink_bind_vertex_textures(
    u32 vtex_mask, u32 textures[RSX_BE_MAX_VERTEX_TEXTURES],
    rsx_be_sampler_desc samplers[RSX_BE_MAX_VERTEX_TEXTURES])
{
    u32 bound = 0;
    for (u32 u = 0; u < RSX_BE_MAX_VERTEX_TEXTURES; u++) {
        if (!((vtex_mask >> u) & 1u)) continue;
        rsx_dsp_vertex_texture vt;
        rsx_dsp_get_vertex_texture(&g.rsx, u, &vt);
        eng_decode_sampler(vt.filter, vt.wrap, vt.control0, &samplers[u]);
        /* A vertex unit has no crossbar, so its remap is the identity. */
        textures[u] = eng_texture_slot(vt.location, vt.offset, vt.format,
                                       vt.width, vt.height, vt.mipmaps,
                                       vt.pitch, 0, 0xAAE4u);
        if (textures[u]) bound |= 1u << u;
    }
    return bound;
}

static void sink_end(void* user, const rsx_dispatch* r)
{
    (void)user; (void)r;
    if (!g.ready || !dc.n_packets) return;
    if (ENG_TRACING()) eng_trace_pass_boundary(0);

    const u32 prim = g.rsx.current_primitive;
    /* Everything that becomes triangles -- lists, strips, fans, quads, quad
     * strips and polygons -- is REBUILT into one triangle list through an
     * index buffer. A host strip topology cannot express a restart cut, which
     * is the whole reason the reference engine rebuilds them, and rebuilding
     * is also what lets repeated references share an uploaded vertex. Points
     * and lines pass through as the guest issued them, and anything left that
     * would need an expansion this engine does not have is dropped rather
     * than drawn as something else. */
    rsx_topology topology = RSX_TOPOLOGY_TRIANGLES;
    int scratch = 0;
    const int rebuild = eng_topology_rebuild(prim, 0, &scratch) != 0;
    if (!rebuild) {
        if (rsx_primitive_needs_expansion(prim)) return;
        topology = rsx_primitive_topology(prim);
        if (topology == RSX_TOPOLOGY_UNSUPPORTED) return;
    }

    rsx_vertex_layout_plan layout;
    eng_vertex_layout(&layout);
    dc_plan_init(&layout);

    /* GEO_MEMO: the same inputs as a draw already converted take its
     * vertices and index list as they are. */
    eng_geo_probe geo;
    const geo_l2* geo_hit = eng_geo_lookup(prim, &geo);
    const u8* verts;
    const u32* draw_indices = NULL;
    u32 n_verts;
    int indexed = 0;
    u32 n_draw;
    if (geo_hit) {
        eng_geo_verify_hit(prim, rebuild, &layout, geo_hit);
        verts = s_geo_arena + (geo_hit->pos % s_geo_size);
        n_verts = geo_hit->n_verts;
        indexed = geo_hit->indexed;
        n_draw = geo_hit->n_draw;
        if (indexed)
            draw_indices = (const u32*)(verts + ((geo_hit->vbytes + 15u) & ~15u));
    } else {
        dc_fetch(&layout, rebuild);
        if (!dc.n_verts || !dc.fetch_ok) return;
        verts = dc.verts;
        n_verts = dc.n_verts;
        n_draw = dc.n_source_refs;
        if (rebuild) {
            if (!eng_topology_rebuild(prim, dc.refs_remapped, &indexed)) return;
            n_draw = indexed
                ? rsx_draw_engine_topology_index_count(prim, dc.n_source_refs,
                                                       dc.cuts, dc.n_cuts)
                : dc.n_source_refs - dc.n_source_refs % 3u;
        }
    }
    if (!n_draw) return;

    rsx_be_render_state rs;
    rsx_draw_engine_decode_render_state(&g.rsx, &rs);

    u32 targets[RSX_BE_MAX_COLOR_TARGETS];
    const u32 n_targets = eng_current_target_set(targets);
    if (!n_targets) return;
    const u32 target = targets[0];
    if (ENG_TRACING()) s_trace_prev = target;

    rsx_dsp_surface sf;
    rsx_dsp_viewport vp;
    rsx_dsp_get_surface(&g.rsx, &sf);
    rsx_dsp_get_viewport(&g.rsx, &vp);
    const u32 zslot = eng_zdepth_get(sf.zeta_location, sf.zeta_offset,
                                     sf.clip_w, sf.clip_h);

    /* Textures before any pipeline or target binding: a cache refresh can
     * replace a resource, and the backend is free to submit while doing it. */
    u32 textures[RSX_BE_MAX_TEXTURES];
    rsx_be_sampler_desc samplers[RSX_BE_MAX_TEXTURES];
    u32 vtextures[RSX_BE_MAX_VERTEX_TEXTURES];
    rsx_be_sampler_desc vsamplers[RSX_BE_MAX_VERTEX_TEXTURES];
    memset(textures, 0, sizeof textures);
    memset(samplers, 0, sizeof samplers);
    memset(vtextures, 0, sizeof vtextures);
    memset(vsamplers, 0, sizeof vsamplers);
    u32 cube_mask = 0;
    u32 tex_mask = sink_bind_textures(targets, n_targets, zslot,
                                      textures, samplers, &cube_mask);
    /* Depth bounds: the zeta as stored before this draw, on the spare unit. */
    g.dbt = 0;
    if (eng_dbt_on() && (rsx_dsp_reg(&g.rsx, 0x0380u) & 1u) && zslot != ENG_INVALID &&
        !((tex_mask >> ENG_DBT_UNIT) & 1u)) {
        const u32 snap = eng_zdepth_snapshot(zslot, sf.clip_w, sf.clip_h);
        if (snap) {
            textures[ENG_DBT_UNIT] = snap; s_tex_kind[ENG_DBT_UNIT] = 'D'; s_tex_src[ENG_DBT_UNIT] = zslot;
            tex_mask |= 1u << ENG_DBT_UNIT;
            g.dbt = 1;
        }
    }
    const u32 vtex_raw = eng_vtex_mask();
    const u32 vtex_mask = sink_bind_vertex_textures(vtex_raw, vtextures,
                                                    vsamplers);

    /* A guest program pair that will not translate has no fallback: the draw
     * is dropped, as the reference engine drops it. Substituting the built-in
     * program would draw the geometry in the wrong colours, which is harder to
     * see than a missing object and hides the translation failure. */
    int pipeline_is_fixed = 1;
    const u32 pipeline = eng_pipeline_get(&layout, &rs,
                                          eng_surface_format(sf.color_format),
                                          n_targets, cube_mask, vtex_raw,
                                          &pipeline_is_fixed);
    if (!pipeline) {
        if (ENG_TRACING())
            fprintf(stderr, "[eng-trace] f%u DROP no pipeline prim=%u n=%u tgt=s%u\n",
                    g.frames, prim, n_draw, target);
        return;
    }

    if (indexed && !geo_hit) {
        if (!dc_reserve_indices(n_draw)) return;
        rsx_draw_engine_write_topology_indices(
            prim, dc.n_source_refs, dc.cuts, dc.n_cuts,
            dc.refs_remapped ? dc.ref_remap.occurrence_to_unique : NULL,
            g.indices);
        draw_indices = g.indices;
    }

    /* The transform constant block, then the viewport epilogue the vertex
     * program's tail multiplies by: RSX window coordinates mapped into the
     * host's clip space (rsx_live_draw.c:6143-6156). */
    const float W = sf.clip_w ? (float)sf.clip_w : (float)g.width;
    const float H = sf.clip_h ? (float)sf.clip_h : (float)g.height;
    float xf[8] = { 1, 1, 1, 0, 0, 0, 0, 0 };
    if (vp.scale[0] != 0.0f || vp.translate[0] != 0.0f) {
        xf[0] = vp.scale[0] / (W * 0.5f);
        xf[1] = -(vp.scale[1] / (H * 0.5f));
        xf[2] = vp.scale[2];
        xf[4] = (vp.translate[0] - W * 0.5f) / (W * 0.5f);
        xf[5] = -((vp.translate[1] - H * 0.5f) / (H * 0.5f));
        xf[6] = vp.translate[2];
    }

    /* The buffered fragment constants, then fp_alpha: the layout
     * rsx_fp_decompile_buffered_ex compiled the shader against. */
    const u32 nslots = g.fp_constants.count ? g.fp_constants.count : 1u;
    const u32 fp_bytes = (nslots + 1u) * 16u;
    if (g.fp_cb_cap < fp_bytes) {
        u8* n = (u8*)realloc(g.fp_cb, fp_bytes);
        if (!n) return;
        g.fp_cb = n;
        g.fp_cb_cap = fp_bytes;
    }
    memset(g.fp_cb, 0, fp_bytes);
    if (g.fp_constants.count)
        memcpy(g.fp_cb, g.fp_constants.values, g.fp_constants.count * 16u);
    { float* alpha = (float*)(g.fp_cb + nslots * 16u);
      alpha[0] = rsx_fp_alpha_ref(rs.alpha_ref_raw, rs.alpha_ref_format);
      alpha[1] = alpha[2] = alpha[3] = 0.0f;
      if (g.dbt) { alpha[1] = eng_reg_f32(0x0384u); alpha[2] = eng_reg_f32(0x0388u); } }

    /* The depth attachment is cleared the first time it is bound, so a title
     * that never clears its shadow zeta still tests against something. */
    const u32 depth_handle = (zslot != ENG_INVALID) ? g.zdepths[zslot].handle : 0;
    if (zslot != ENG_INVALID && !g.zdepths[zslot].cleared) {
        g.be->clear_depth_stencil(g.be->user, depth_handle,
                                  RSX_BE_CLEAR_DEPTH | RSX_BE_CLEAR_STENCIL,
                                  1.0f, 0);
        g.zdepths[zslot].cleared = 1;
        g.zdepths[zslot].had_write = 0;
    }

    u32 handles[RSX_BE_MAX_COLOR_TARGETS];
    for (u32 i = 0; i < n_targets; i++) handles[i] = g.surfaces[targets[i]].handle;
    g.be->bind_targets(g.be->user, handles, n_targets, depth_handle);
    g.be->bind_pipeline(g.be->user, pipeline);
    if (g.be->bind_vs_constants_split) {
        g.be->bind_vs_constants_split(g.be->user, g.rsx.constants,
                                      RSX_DSP_NUM_CONSTANTS * 16u, xf, sizeof xf,
                                      g.rsx.constants_gen, g.vs_cuse);
    } else {
        memcpy(g.vp_cb, g.rsx.constants, RSX_DSP_NUM_CONSTANTS * 16u);
        memcpy(g.vp_cb + RSX_DSP_NUM_CONSTANTS * 16u, xf, sizeof xf);
        g.be->bind_vs_constants(g.be->user, g.vp_cb, ENG_VP_CB_BYTES);
    }
    g.be->bind_ps_constants(g.be->user, g.fp_cb, fp_bytes);
    g.be->bind_textures(g.be->user, textures, samplers, tex_mask);
    g.be->bind_vertex_textures(g.be->user, vtextures, vsamplers, vtex_mask);
    g.be->set_viewport(g.be->user, 0.0f, 0.0f, W, H);

    /* The guest scissor intersected with the surface. The nv40 reset is a
     * full 4096x4096 window and a never-written register reads 0 here, which
     * the w == 0 test treats as no scissor, so only a real game scissor
     * narrows anything (rsx_live_draw.c:6180-6197). */
    {
        u32 sx = 0, sy = 0;
        u32 sw = g.surfaces[target].w, sh = g.surfaces[target].h;
        const u32 h = rsx_dsp_reg(&g.rsx, M_SCISSOR_HORIZONTAL);
        const u32 v = rsx_dsp_reg(&g.rsx, M_SCISSOR_VERTICAL);
        const u32 gx = h & 0xFFFFu, gw = h >> 16;
        const u32 gy = v & 0xFFFFu, gh = v >> 16;
        if (gw > 0 && gh > 0) {
            u32 right = sx + sw, bottom = sy + sh;
            if (gx > sx) sx = gx;
            if (gy > sy) sy = gy;
            if (gx + gw < right)  right = gx + gw;
            if (gy + gh < bottom) bottom = gy + gh;
            sw = right > sx ? right - sx : 0;
            sh = bottom > sy ? bottom - sy : 0;
        }
        g.be->set_scissor(g.be->user, sx, sy, sw, sh);
    }
    /* Dynamic, and deliberately outside the pipeline key. */
    g.be->set_stencil_ref(g.be->user,
                          rsx_dsp_reg(&g.rsx, M_STENCIL_FUNC_REF) & 0xFFu);

    const u32 uploaded = indexed ? n_verts : n_draw;
    if (ENG_TRACING()) {
        char zbuf[48]; zbuf[0] = 0;
        if (zslot != ENG_INVALID)
            snprintf(zbuf, sizeof zbuf, "(%08X %ux%u)", g.zdepths[zslot].offset,
                     g.zdepths[zslot].w, g.zdepths[zslot].h);
        char tx[1536]; int o = 0; tx[0] = 0;
        for (u32 u = 0; u < RSX_BE_MAX_TEXTURES && o < (int)sizeof tx - 32; u++)
            if (tex_mask & (1u << u))
            {
                o += snprintf(tx + o, sizeof tx - o, " u%u=%c%X/%02X%s", u, s_tex_kind[u],
                              s_tex_src[u], s_tex_fmt[u] & 0xFFu, textures[u] ? "" : "(none)");
                /* A guest texture: where it is, its size, its first texels. */
                rsx_dsp_texture t;
                rsx_dsp_get_texture(&g.rsx, u, &t);
                if (s_tex_kind[u] == 'T' && o < (int)sizeof tx - 64) {
                    const u8* gp = eng_guest_ptr(NULL, t.location, t.offset, 16);
                    o += snprintf(tx + o, sizeof tx - o, "[L%u %ux%u p%u", t.location,
                                  t.width, t.height, t.pitch);
                    if (gp) o += snprintf(tx + o, sizeof tx - o, " %02X%02X%02X%02X %02X%02X%02X%02X",
                                          gp[0], gp[1], gp[2], gp[3], gp[4], gp[5], gp[6], gp[7]);
                    o += snprintf(tx + o, sizeof tx - o, "]");
                }
            }
        /* A short program's inline constants (x of each), for tone/LUT passes. */
        if (g.fp_constants.count && g.fp_constants.count <= 8 && o < (int)sizeof tx - 160) {
            o += snprintf(tx + o, sizeof tx - o, " c=");
            for (u32 k = 0; k < g.fp_constants.count; k++) {
                float f[4]; memcpy(f, g.fp_constants.values[k], 16);
                o += snprintf(tx + o, sizeof tx - o, "%s%.4g/%.4g/%.4g", k ? "," : "", f[0], f[1], f[2]);
            }
        }
        fprintf(stderr, "[eng-trace] f%u draw prim=%u n=%u tgt=s%u(%08X %ux%u fmt%d)x%u z=%s%s"
                        " blend=%d dt=%d dw=%d cmask=%X fixed=%d sh=%X db=%u[%.5f,%.5f] st=%u pk=%X ps=%016llx%s\n",
                g.frames, prim, n_draw, target, g.surfaces[target].offset,
                g.surfaces[target].w, g.surfaces[target].h, (int)g.surfaces[target].fmt,
                n_targets, depth_handle ? "y" : "n", zbuf,
                (int)rs.blend_enable, (int)rs.depth_test, (int)rs.depth_write,
                (unsigned)rs.color_mask, pipeline_is_fixed, g.shkey,
                rsx_dsp_reg(&g.rsx, 0x0380u) & 1u, eng_reg_f32(0x0384u), eng_reg_f32(0x0388u),
                (unsigned)rs.stencil_enable, rsx_dsp_reg(&g.rsx, 0x1FECu), (unsigned long long)g.ps_hash, tx);
    }
    if (!geo_hit) eng_geo_store(&geo, indexed, n_draw, draw_indices);
    if (geo_hit && eng_geo_direct(geo_hit)) {
        s_geo_st.direct++;
        g.be->draw_arena(g.be->user, topology, geo_hit->pos, uploaded, layout.stride,
                         indexed ? geo_hit->pos + ((geo_hit->vbytes + 15u) & ~15u) : 0,
                         indexed ? n_draw : 0);
    } else {
        g.be->draw(g.be->user, topology, verts, uploaded, layout.stride,
                   indexed ? draw_indices : NULL, indexed ? n_draw : 0);
    }
    /* Counted only when the draw ran the guest's OWN programs, which is what
     * the test hook means: a draw through the built-in pair is a draw, not
     * evidence that the decompile-translate-compile path worked. */
    if (!pipeline_is_fixed) g.guest_draws++;

    /* COLOR_MASK keeps one byte per channel (A R G B, high to low). */
    if (rs.color_mask & 0x01010101u)
        for (u32 i = 0; i < n_targets; i++) g.surfaces[targets[i]].cwritten = 1;
    if (zslot != ENG_INVALID && rs.depth_test && rs.depth_write) {
        g.zdepths[zslot].had_write = 1;
        /* Any copy taken so far no longer holds this depth. */
        g.zdepths[zslot].snapshot_valid = 0;
        g.zdepths[zslot].ccopy_valid = 0;
    }
    /* The colour copy carries stencil too (v156), so stencil writes age it. */
    if (zslot != ENG_INVALID && rs.stencil_enable && (rs.s_write_mask & 0xFFu))
        g.zdepths[zslot].ccopy_valid = 0;
}

static void sink_clear(void* user, const rsx_dispatch* r, u32 mask)
{
    (void)user; (void)r;
    if (!g.ready) return;
    u32 targets[RSX_BE_MAX_COLOR_TARGETS];
    const u32 n_targets = eng_current_target_set(targets);
    if (!n_targets) return;

    if (mask & (RSX_CLEAR_COLOR_R | RSX_CLEAR_COLOR_G |
                RSX_CLEAR_COLOR_B | RSX_CLEAR_COLOR_A)) {
        const u32 c = rsx_dsp_clear_color(&g.rsx);
        const float rgba[4] = {
            (float)((c >> 16) & 0xFF) / 255.0f,
            (float)((c >>  8) & 0xFF) / 255.0f,
            (float)( c        & 0xFF) / 255.0f,
            (float)((c >> 24) & 0xFF) / 255.0f,
        };
        /* Every target the set names, not only A: a deferred pass clears its
         * whole G-buffer in one CLEAR_SURFACE, and B, C and D would otherwise
         * keep whatever the last frame left in them. */
        for (u32 i = 0; i < n_targets; i++)
            g.be->clear_color(g.be->user, g.surfaces[targets[i]].handle, rgba);
    }
    if (mask & (RSX_CLEAR_DEPTH | RSX_CLEAR_STENCIL)) {
        rsx_dsp_surface sf;
        rsx_dsp_get_surface(&g.rsx, &sf);
        const u32 zslot = eng_zdepth_get(sf.zeta_location, sf.zeta_offset,
                                         sf.clip_w, sf.clip_h);
        if (zslot == ENG_INVALID) return;
        u32 flags = 0;
        if (mask & RSX_CLEAR_DEPTH)   flags |= RSX_BE_CLEAR_DEPTH;
        if (mask & RSX_CLEAR_STENCIL) flags |= RSX_BE_CLEAR_STENCIL;
        /* ZSTENCIL_CLEAR_VALUE is Z24S8: depth24 << 8 | stencil8, with the
         * nv40 reset 0xFFFFFF00 seeded by rsx_dispatch_init, so a stream that
         * never writes it still clears to 1.0 / 0. */
        const u32 zs = rsx_dsp_reg(&g.rsx, M_ZSTENCIL_CLEAR);
        g.be->clear_depth_stencil(g.be->user, g.zdepths[zslot].handle, flags,
                                  zs ? (float)(zs >> 8) / 16777215.0f : 1.0f,
                                  (u8)(zs & 0xFFu));
        g.zdepths[zslot].cleared = 1;
        g.zdepths[zslot].ccopy_valid = 0;
        if (mask & RSX_CLEAR_DEPTH) {
            /* A clear invalidates the older published depth image; the next
             * texture consumer resolves the newly written pass exactly once. */
            g.zdepths[zslot].had_write = 0;
            g.zdepths[zslot].snapshot_valid = 0;
        }
    }
}

/* Resolve a flip's buffer id to a REGISTERED surface rather than whatever is
 * currently bound: the live target at that instant is often an offscreen
 * shadow or post-process surface, and copying it presents black despite the
 * scene's draws having executed (rsx_live_draw.c:7557-7570). */
static u32 eng_present_surface(u32 buffer_id)
{
    if (buffer_id < 8 && g.display_buffers[buffer_id].valid) {
        const eng_display_buffer* d = &g.display_buffers[buffer_id];
        for (u32 i = 0; i < g.n_surfaces; i++)
            if (g.surfaces[i].handle && g.surfaces[i].location == d->location &&
                g.surfaces[i].offset == d->offset)
                return i;
    }
    return eng_current_surface();
}

static void eng_present(u32 buffer_id)
{
    if (!g.ready) return;
    const u32 target = eng_present_surface(buffer_id);
    if (target == ENG_INVALID) return;
    if (getenv("PS3RECOMP_METAL_FRAME_DUMP") && g.frames % 120 == 0) {
        u32 current = eng_current_surface();
        fprintf(stderr, "[rsx capture] frame=%u buffer=%u selected=%u current=%u draws=%u surfaces=%u\n",
                g.frames, buffer_id, target, current, g.guest_draws, g.n_surfaces);
        for (u32 i = 0; i < g.n_surfaces; ++i)
            fprintf(stderr, "[rsx capture] surface %u loc=%u offset=%08X size=%ux%u handle=%u\n",
                    i, g.surfaces[i].location, g.surfaces[i].offset,
                    g.surfaces[i].w, g.surfaces[i].h, g.surfaces[i].handle);
    }
    g.last_present_surface = target;
    g.be->present(g.be->user, g.surfaces[target].handle);
    if (ENG_TRACING()) { eng_trace_pass_boundary(1); eng_dump_surfaces(); }
    eng_trace_tick(g.frames);
    eng_geo_frame();
#if defined(__APPLE__)
    eng_tcs_frame();
#endif
    g.frames++;
    g.last_guest_draws = g.guest_draws;
    g.guest_draws = 0;
}

static void sink_flip(void* user, const rsx_dispatch* r, u32 arg)
{
    (void)user; (void)r;
    g.last_flip_buffer = arg & 7u;
    eng_present(g.last_flip_buffer);
}

/* ---- public API ---------------------------------------------------------- */

void rsx_draw_engine_set_backend(const rsx_draw_backend* backend)
{
    g.be = backend;
}

void rsx_draw_engine_set_default(int on)
{
    g.default_on = on;
}

int rsx_draw_engine_enabled(void)
{
    static int cached = -1;
    static int env_seen = 0;
    static int env_on = 0;
    if (!env_seen) {
        const char* e = getenv("PS3RECOMP_RSX_ENGINE");
        env_seen = 1;
        if (e && *e) env_on = (strcmp(e, "dispatch") == 0) ? 1 : -1;
    }
    /* The environment is authoritative in both directions; without it the
     * backend's own default decides. Cached after the first backend has
     * registered, so the FIFO walker pays one compare. */
    const int on = env_on > 0 ? 1 : (env_on < 0 ? 0 : g.default_on);
    if (!g.be) return 0;
    if (cached < 0) cached = on;
    return cached;
}

int rsx_draw_engine_init(u32 width, u32 height)
{
    if (!g.be) return -1;
    g.width  = width  ? width  : 1280;
    g.height = height ? height : 720;
    g.vp_cb = (u8*)calloc(1, ENG_VP_CB_BYTES);
    if (!g.vp_cb) return -1;
    if (g.be->init && g.be->init(g.be->user, g.width, g.height) != 0) {
        free(g.vp_cb); g.vp_cb = NULL;
        return -1;
    }
    rsx_dispatch_sink sink;
    memset(&sink, 0, sizeof sink);
    sink.clear            = sink_clear;
    sink.begin            = sink_begin;
    sink.end              = sink_end;
    sink.draw_arrays      = sink_draw_arrays;
    sink.draw_index_array = sink_draw_index;
    sink.inline_array     = sink_inline_array;
    sink.immediate        = sink_immediate;
    sink.flip             = sink_flip;
    rsx_dispatch_init(&g.rsx, &sink);
    g.ready = 1;
    fprintf(stderr, "[rsx engine] register-file draw engine up (%ux%u)\n",
            g.width, g.height);
    return 0;
}

void rsx_draw_engine_shutdown(void)
{
    if (!g.be) return;
    if (g.ready && g.be->submit_and_wait)
        g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_SHUTDOWN);
    for (u32 i = 0; i < g.n_textures; i++)
        if (g.textures[i].handle) g.be->texture_release(g.be->user, g.textures[i].handle);
    for (u32 i = 0; i < g.n_pipelines; i++)
        if (g.pipelines[i].handle) g.be->pipeline_release(g.be->user, g.pipelines[i].handle);
    for (u32 i = 0; i < g.n_surfaces; i++)
        if (g.surfaces[i].handle) g.be->color_target_release(g.be->user, g.surfaces[i].handle);
    for (u32 i = 0; i < g.n_zdepths; i++)
        if (g.zdepths[i].handle) g.be->depth_target_release(g.be->user, g.zdepths[i].handle);
    if (g.ready && g.be->shutdown) g.be->shutdown(g.be->user);

    rsx_draw_engine_set_guest_memory(NULL, NULL);
    free(dc.refs); free(dc.cuts); free(dc.verts);
    rsx_vertex_remap_destroy(&dc.ref_remap);
    memset(&dc, 0, sizeof dc);
    free(g.tex_staging); free(g.vp_cb); free(g.fp_cb); free(g.indices);
    const rsx_draw_backend* be = g.be;
    memset(&g, 0, sizeof g);
    g.be = be;
}

void rsx_draw_engine_method(u32 method, u32 arg)
{
    if (!g.ready) return;
    /* The subchannel is a binding slot, not an engine selector, and a title
     * whose SPU-built command lists bind NV4097 elsewhere would otherwise
     * store its state in the wrong register bank.  Standard NV4097 methods
     * (< 0x2000) strip the subchannel with & 0x1FFC.  Driver methods
     * (0xE9xx, 0xEBxx) encode the subchannel bits as part of the address;
     * preserve them with the wider mask. */
    u32 m = (method >= 0xE000u) ? (method & 0xFFFCu) : (method & 0x1FFCu);
    rsx_dispatch_method(&g.rsx, m, arg);
}

void rsx_draw_engine_const_block(u32 method, const u8* be_words, u32 count)
{
    if (!g.ready) return;
    method &= 0x1FFCu;
    /* PS3RECOMP_ENGINE_TC_VERIFY=1: run the block both ways (bulk, then the
     * per-word rsx_dispatch_method from the same start) and compare the
     * registers, write counts, constants and generation. The per-word
     * result is kept. */
    static int verify = -1;
    if (verify < 0) {
        const char* e = getenv("PS3RECOMP_ENGINE_TC_VERIFY");
        verify = (e && *e == '1') ? 1 : 0;
        if (verify) fprintf(stderr, "[rsx engine] TC_VERIFY: constant blocks checked against the per-word path\n");
    }
    if (!verify || count > 33u || method < 0x1EFCu) {
        rsx_dispatch_const_block(&g.rsx, method, be_words, count);
        return;
    }
    enum { R0 = 0x1EFCu >> 2, NR = 33 };
    static u32 c_save[RSX_DSP_NUM_CONSTANTS][4], c_bulk[RSX_DSP_NUM_CONSTANTS][4];
    static unsigned long long n_blocks = 0, n_bad = 0;
    u32 regs_save[NR], seen_save[NR], regs_bulk[NR], seen_bulk[NR];
    rsx_dispatch* r = &g.rsx;
    memcpy(regs_save, &r->regs[R0], sizeof regs_save);
    memcpy(seen_save, &r->seen[R0], sizeof seen_save);
    memcpy(c_save, r->constants, sizeof c_save);
    const u64 gen_save = r->constants_gen;

    rsx_dispatch_const_block(r, method, be_words, count);
    memcpy(regs_bulk, &r->regs[R0], sizeof regs_bulk);
    memcpy(seen_bulk, &r->seen[R0], sizeof seen_bulk);
    memcpy(c_bulk, r->constants, sizeof c_bulk);
    const u64 gen_bulk = r->constants_gen;

    memcpy(&r->regs[R0], regs_save, sizeof regs_save);
    memcpy(&r->seen[R0], seen_save, sizeof seen_save);
    memcpy(r->constants, c_save, sizeof c_save);
    r->constants_gen = gen_save;
    for (u32 i = 0; i < count; i++) {
        const u8* w = be_words + 4u * i;
        rsx_dispatch_method(r, method + 4u * i,
                            ((u32)w[0] << 24) | ((u32)w[1] << 16) | ((u32)w[2] << 8) | (u32)w[3]);
    }
    const int bad = memcmp(regs_bulk, &r->regs[R0], sizeof regs_bulk) ||
                    memcmp(seen_bulk, &r->seen[R0], sizeof seen_bulk) ||
                    memcmp(c_bulk, r->constants, sizeof c_bulk) || gen_bulk != r->constants_gen;
    n_blocks++;
    if (bad && ++n_bad <= 16)
        fprintf(stderr, "[rsx engine] TC_VERIFY: block differs (method 0x%04X count %u)\n", method, count);
    if ((n_blocks % 1000000ull) == 0)
        fprintf(stderr, "[rsx engine] TC_VERIFY: %llu blocks checked, %llu differ\n", n_blocks, n_bad);
}

void rsx_draw_engine_set_display_buffer(u32 buffer_id, u32 location, u32 offset,
                                        u32 pitch, u32 width, u32 height)
{
    if (buffer_id >= 8) return;
    eng_display_buffer* d = &g.display_buffers[buffer_id];
    d->location = location;
    d->offset = offset;
    d->pitch = pitch;
    d->width = width;
    d->height = height;
    d->valid = width && height;
}

void rsx_draw_engine_flush(void)
{
    if (g.ready && g.be->submit_and_wait)
        g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
}

void rsx_draw_engine_present(void)
{
    /* A host that drives the flip itself gets whichever buffer the guest's
     * own last flip named, and display buffer 0 before there has been one:
     * a runner that both consumes 0xE944 and calls here would otherwise
     * re-present a double-buffered title's OTHER scanout. Presenting twice is
     * harmless either way -- the engine never clears the surface, so a second
     * present just blits the same image again. */
    eng_present(g.last_flip_buffer);
}

void rsx_draw_engine_present_buffer(u32 buffer_id)
{
    g.last_flip_buffer = buffer_id & 7u;
    eng_present(g.last_flip_buffer);
}

u32 rsx_draw_engine_guest_draws(void)
{
    return g.last_guest_draws;
}

u32 rsx_draw_engine_readback_center(void)
{
    if (!g.ready || !g.be->readback) return 0;
    const u32 slot = g.last_present_surface;
    if (slot >= g.n_surfaces) return 0;
    const eng_surface* s = &g.surfaces[slot];
    if (!s->handle || s->fmt != RSX_BE_FMT_R8G8B8A8 || !s->w || !s->h) return 0;
    u8 px[4] = { 0, 0, 0, 0 };
    g.be->readback(g.be->user, s->handle, s->w / 2u, s->h / 2u, 1, 1, px, 4);
    /* Decoded rows are R,G,B,A; the guest's own clear colour is A8R8G8B8, and
     * a host comparing the two wants them in the same order. */
    return ((u32)px[3] << 24) | ((u32)px[0] << 16) |
           ((u32)px[1] << 8)  |  (u32)px[2];
}
