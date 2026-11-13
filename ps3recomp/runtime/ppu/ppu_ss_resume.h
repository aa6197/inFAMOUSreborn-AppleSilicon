/*
 * Save states phase 1: resume points in the lifted PPU code.
 *
 * Shared by the runtime (ppu_ss_resume.cpp) and the lifted TUs that
 * ps3recomp/tools/ss_resume_pass.py writes at build time (CMake
 * PORT_SAVE_STATES, Apple only, default OFF). Nothing includes it when the
 * option is off.
 *
 * A saved PPU thread is its registers plus its chain: one frame per lifted
 * function on its host stack, outermost first. On load the thread calls the
 * outermost function with g_ss_resume set; each function's resume block takes
 * its frame (ss_resume_frame), refills its _cs_N snapshots from the guest
 * stack, calls the next function (ss_resume_call) and jumps to the label after
 * that call. The innermost function jumps into or past its sc / HLE call.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ppu_context;

/* Where a frame is. Key = SS_KEY(kind, address). */
enum {
    SS_K_NONE    = 0,  /* site table: function present but not resumable */
    SS_K_CALL    = 1,  /* address = guest return address of a bl / bctrl / blrl */
    SS_K_SC_IN   = 2,  /* address = the sc; resume runs the system call again */
    SS_K_SC_OUT  = 3,  /* address = the sc; resume goes on after it (results in ctx) */
    SS_K_HLE_IN  = 4,  /* address = the import stub; resume calls the HLE function again */
    SS_K_HLE_OUT = 5,  /* address = the import stub; resume returns from the stub */
    SS_K_TAIL    = 6   /* address = the function; a bctr tail jump made as a host call */
};
#define SS_KEY(kind, addr) (((uint64_t)(uint32_t)(kind) << 32) | (uint32_t)(addr))

/* delta: guest r1 at the site minus r1 at function entry (<= 0).
 * SS_DELTA_DYN: the frame size is not known statically; use the back chain. */
#define SS_DELTA_DYN 0x7FFFFFFF

typedef struct ss_site {
    uint32_t fn;     /* guest address of the lifted function */
    uint32_t addr;   /* see SS_K_* */
    uint32_t kind;
    int32_t  delta;
} ss_site;

typedef struct ss_rframe {
    uint64_t key;       /* SS_KEY(kind, address) of the place this frame is at */
    uint32_t fn;        /* guest address of this frame's function */
    uint32_t entry_sp;  /* guest r1 when the function was entered */
    uint32_t next_fn;   /* guest address of the function it calls (0 for the innermost) */
    uint32_t sp;        /* guest r1 at the site */
} ss_rframe;

/* Nonzero while any thread rebuilds its chain. Plain global: one load and a
 * not-taken branch at each lifted function entry. */
extern int g_ss_resume;

/* This thread's next frame if it is resuming and the frame belongs to fn, else
 * NULL. A frame for another function is a broken chain: it stops the load. */
const ss_rframe* ss_resume_frame(struct ppu_context* ctx, uint32_t fn);
/* Call the next function of the chain (f->next_fn). */
void ss_resume_call(struct ppu_context* ctx, const ss_rframe* f);
/* The function has no resume point for f->key: stop the load. Does not return. */
void ss_resume_fail(struct ppu_context* ctx, const ss_rframe* f);
/* Site tables, one per lifted TU, registered by static constructors. */
void ss_register_sites(const ss_site* s, uint32_t n);

/* --- save side: the exact chain of one stopped thread ---------------------- */

/* One host stack frame (innermost first), as the save side classifies it. */
enum { SS_H_FUNC = 1, SS_H_IC = 2, SS_H_OUTLINED = 3, SS_H_SC = 4, SS_H_HLE = 5, SS_H_OTHER = 6 };
typedef struct ss_hframe { uint32_t cls; uint32_t fn; /* guest address for SS_H_FUNC */ } ss_hframe;
/* Guest memory [lo, hi) as raw (big-endian) bytes: a copy, or vm_base + lo. */
typedef struct ss_gwin { uint32_t lo, hi; const uint8_t* p; } ss_gwin;
typedef struct ss_chain_info {
    const char* why;   /* failure reason (NULL when it worked) */
    int fail_k;        /* frame (0 = innermost) where it failed */
    uint32_t fail_fn;
    int amb;           /* frames where more than one stack slot fitted (first one taken) */
    /* first such frame: which slots (0 = live LR, 1 = own LR slot, 2 = back-chain LR slot) */
    int amb_k, amb_took, amb_other;
    uint32_t amb_fn, amb_ra_took, amb_ra_other;
    int top;           /* 1 if the outermost frame's entry r1 ends the guest back chain */
} ss_chain_info;
/* Fills out[] outermost first; returns the frame count, or -1 (see info->why).
 * parked_where: the ppu_ss_point where the thread is parked (1-4), or 0. */
int ss_chain_build(const ss_hframe* h, int nh, uint32_t r1, uint32_t lr, uint32_t cia, int parked_where,
                   const ss_gwin* g, ss_rframe* out, int max, ss_chain_info* info);

#ifdef __cplusplus
}
#endif
