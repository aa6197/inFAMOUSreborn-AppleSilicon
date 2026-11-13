/*
 * Save states: the save file (SAVE_STATES_PLAN.md, "Save file: how it is built").
 * Apple only, CMake PORT_SAVE_STATES (default OFF).
 *
 * Layout: header (magic "PS3SAVE1", u32 version, 16-byte build UUID, u64 time),
 * then named sections { u32 name length, name, u64 size, data }. A file only
 * loads into the exact build that wrote it (the UUID must match), so tables are
 * stored as raw bytes.
 * Save files hold game memory: device only, never in git or uploads.
 *
 * The save writes "mem" and the tables while every PPU thread is suspended, so
 * nothing here may take a lock another thread could hold: no malloc, no stdio,
 * no logging. ss_file_create allocates every buffer up front.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ss_out ss_out;

/* Section "ppu" (written after the threads run again): u32 count, then per
 * thread one ss_ppu_rec, ctx_size bytes of its ppu_context, and n ss_rframe
 * (ppu_ss_resume.h), outermost first. */
typedef struct ss_ppu_rec {
    uint32_t t;                  /* guest thread id (ppu_context.thread_id) */
    uint32_t parked, where;      /* parked at ppu_ss_point(where), else blocked inside a call */
    int32_t  dsc, dhle;          /* syscall / HLE call depth */
    uint32_t sc, hle, park_num;  /* last syscall number, last HLE nid, parked point's number */
    uint32_t ctx_size, n;
} ss_ppu_rec;

typedef struct ss_mem_stats {
    uint32_t ranges;
    uint64_t mapped, nonzero, packed;   /* bytes */
} ss_mem_stats;

/* Writer, used by the save (ppu_ss_probe.cpp, SS_SAVE_AT). */
ss_out* ss_file_create(const char* path);   /* NULL on error; allocates (call before suspending) */
void    ss_file_sec_begin(ss_out* o, const char* name);
void    ss_file_put(ss_out* o, const void* p, size_t n);
void    ss_file_sec_end(ss_out* o);
int     ss_file_mem(ss_out* o, ss_mem_stats* st);   /* section "mem": ranges + non-zero pages (LZ4) */
int     ss_file_tables(ss_out* o, uint64_t* bytes); /* one section "tab:<name>" per kernel table */
int     ss_file_sync(ss_out* o);    /* write out the buffer (no locks); 1 when every write worked */
int     ss_file_close(ss_out* o);   /* 1 when every write worked; frees */
uint64_t ss_file_size(const ss_out* o);

/* Reader. ss_file_in_new allocates everything; the other calls use only system
 * calls, so a check can run while the threads are suspended. */
typedef struct ss_in ss_in;
ss_in*  ss_file_in_new(void);
int     ss_file_in_map(ss_in* in, const char* path);   /* open, map, check header and build */
const uint8_t* ss_file_sec(const ss_in* in, const char* name, uint64_t* size);   /* NULL if absent */
/* apply = 0: compare the saved memory with live guest memory ("differ" = pages
 * that do not match, including touched non-zero pages the file lacks).
 * apply = 1: write it into guest memory (ranges made read/write, saved pages
 * copied, other touched pages zeroed, saved protection set back). */
int     ss_file_mem_load(ss_in* in, int apply, ss_mem_stats* st, uint64_t* differ);
/* Compare (apply = 0) or copy (apply = 1) every "tab:" section with its table.
 * differ = tables whose bytes differ. */
int     ss_file_tables_load(ss_in* in, int apply, uint32_t* differ);
/* After a check: up to max of the first pages that differed (guest addresses),
 * and the first table that differed (NULL if none) with its first differing byte. */
uint32_t ss_file_diff_pages(const ss_in* in, uint32_t* addr, uint32_t max);
const char* ss_file_diff_table(const ss_in* in, uint32_t* off);
const char* ss_file_why(const ss_in* in);   /* last failure reason */
void    ss_file_in_free(ss_in* in);

#ifdef __cplusplus
}
#endif
