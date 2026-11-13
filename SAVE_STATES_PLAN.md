# Save states (save anywhere): plan

2026-11-13. Plan only: no code yet.

## Goal

- Save the game at any moment. Load it later, even after the app was
  closed.
- Later: save on its own when the app goes to the background, and offer
  "Resume" at the next start.

## Why switching apps works but a fresh start does not

- Switching apps: iOS only stops giving the app CPU time. Everything stays
  in RAM. Coming back lets it run again. Nothing is saved.
- App closed (memory, restart, swipe away): the RAM is freed. A fresh
  start has new threads, new addresses, new GPU objects, new files.
- So a save state must turn the running game into plain data, and rebuild
  everything else from it.

## What makes it hard here

- Every game function is a real C++ function (27,305 of them). A game
  call is a real C++ call: 74,751 direct calls, 6,647 indirect calls.
  So "where each thread is" lives on the native thread stack, which
  cannot be saved and rebuilt.
  (`ps3recomp/tools/ppu_lifter.py:1444`, `:1550`, `:360-366`)
- Each PS3 thread (PPU and SPU) is a real host thread. Nothing can stop
  them all at one safe moment today. The iOS background pause only stops
  the frame clock (`infamous-recomp/main.cpp:252-260`).
- 78,743 C++ locals (`_cs_N`) hold saved registers across calls
  (`ppu_lifter.py:680-844`). The same values are also on the game's own
  stack, so they can be refilled from there.
- Rendered pictures live only in Metal textures, not in game memory.
  They must be read back from the GPU.
- `cellGcmGetReportDataAddress` returns a host pointer that can change
  each run (`ps3recomp/libs/video/cellGcmSys.c:3641`). Check in phase 0
  if the game keeps it; if so, move it into game memory first.
- No save-state code exists anywhere yet.

## What a save must hold

| Part | Size | Easy? |
|---|---|---|
| Game memory (one 4 GB area; only used pages) | measure; maybe 200–600 MB compressed | Yes, plain bytes |
| PPU threads: registers (up to 64) | ~1 KB each | Yes |
| PPU threads: where each one is (call chain) | small | **Hard** (see option A) |
| SPU threads: local store + registers | ~262 KB each | Yes, if the SPU is idle |
| Kernel objects: mutexes, conds, semaphores, events, timers, lwmutex | tables | Values yes; waiting threads rebuilt |
| Open files | path, mode, offset | Yes, reopen |
| RSX state, method registers, draw engine | ~0.4 MB | Yes |
| Render-target pictures | GPU readback | Medium |
| Audio ring indices | small | Yes; rebuild mixer and stream |
| Clock | one offset | Yes; add an offset to the time base |
| Pads, sysutil callbacks | small | Yes |
| Shader, pipeline and texture caches, Metal objects, workers | – | Not saved: rebuilt |

## Options

### A. Resume chains (recommended)

- Normal play stays as today: native calls, full speed.
- Save:
  1. Stop every thread at a safe point: PPU threads at their next system
     call, SPU work idle, GPU queue empty.
  2. For each PPU thread, read its call chain from the game's own stack
     (each frame stores its return address).
  3. Write game memory, registers, chains and system tables to a file.
- Load:
  1. Fill game memory and tables back in.
  2. Start each thread at its outermost function in "resume" mode. Each
     function jumps straight to the call it was in, and calls the next
     function in the chain. The last one goes back into its wait.
- Needs:
  - A new opt-in lifter mode (flag, default off). Today's output stays
    the same, so Windows does not change.
  - A label before every call, a resume jump at each function start,
    and `_cs_N` refilled from the stack.
  - A resume path for each blocking system call (about 20–30).
- Speed cost: one well-predicted check per function call. Expect under
  1%; to be measured.
- Saves only load in the same app build.

### B. Full dispatcher (not recommended)

- Every call returns to one central loop, so no native stack is needed.
- Simple to save, but much slower (maybe 20–40%). The iPhone is already
  CPU-bound.

### C. Open-world quick save (stopgap)

- Use the game's own save, plus Cole's position, health and energy. Put
  him back there after loading.
- The trainer already finds Cole's object (`infamous-recomp/trainer.cpp`).
  Teleport is not mapped yet.
- Only works in free roam. Missions and cutscenes restart at the game's
  checkpoint. Enemies and the world reset.
- About 3–5 hours. Game-specific and fragile.

### D. Quick wins for switching apps

- When going to the background, free memory that can be rebuilt: GPU
  caches, pipelines and the 256 MB geometry arena
  (`rsx_draw_engine.c:3401-3411`).
- Smaller apps are less likely to be closed by iOS. This helps, but does
  not guarantee anything.
- About 1–2 hours, with before/after memory numbers.

## Order and time

Times are hands-on working time (code, builds, test runs), not
people-days. Builds and test runs take most of it: a full rebuild of the
lifted code is about 15 CPU-minutes (5–10 min on the Mac), an iPad
build-and-run cycle about 15–20 min.

| Step | What | Time | Result |
|---|---|---|---|
| 1 | D: free memory in the background (iPad/iPhone) | 1–2 h | Fewer lost sessions |
| 2 | A phase 0 on the Mac: safe-point stop + call-chain check at 100 random moments in a 30-min run, no loading yet | 2–3 h | Go / no-go, save size, stop time |
| 3 | A phase 1 on the Mac: lifter resume mode, save and load, GPU readback | 6–12 h | Save, quit, load works on the Mac |
| 4 | A phase 2: iPad and iPhone, Save/Load in the pause overlay, auto-save on background | 2–3 h | Full feature |

- Option A in total: about 10–20 hours. Step 3 is the uncertain one:
  each lifter change means a regenerate and rebuild, and load bugs
  take test cycles. Phase 0 makes the estimate firmer.
- C is only worth it if A fails in phase 0.

## Phase 1 design (11-13)

Build side (CMake `PORT_SAVE_STATES`, Apple only, default OFF):

- `ps3recomp/tools/ss_resume_pass.py` runs last on each lifted file the
  Mac/iOS build compiles. It does not change the lifter, so there is no
  regenerate and Windows output is untouched.
- It adds:
  - `ctx->lr = <return address>` before each bctrl/blrl. The address
    comes from the game ELF (n-th bctrl between two known addresses).
  - `ctx->cia = <address>` before each system call.
  - a label after each call, and before/after each system call and HLE
    call.
  - a resume block at the start of each function: refill the saved
    registers from the guest stack, call the next function of the
    chain, jump to the label after that call.
  - a site table per file: function, address, kind, stack offset.
- All 27,305 lifted functions get a resume block. (The first count,
  27,021, missed 284 functions whose opening line ends in a comment.)
  One file is +30% object size (speed still to measure).
- The `_cs_N` snapshots are declared before the resume block and their
  lines become plain assignments (a jump may not skip a declaration with
  a value). 3 functions have a hand-added hook line before them; a
  resume skips it.

Save side (`ppu_ss_resume.cpp`, `ss_chain_build`):

- Host frames give the lifted functions, innermost first. The guest
  stack gives the return address of each one. Each must be a known site
  of that function, or the save is refused.
- A thread inside an HLE call: the import stub tail-calls the HLE
  code, so the stub has no host frame. The save reads the `bl` just
  before the return address to find the stub and adds it to the chain.
- A thread inside a guest callback (game code under an HLE call) is
  refused for now.

Load side: set `g_ss_resume`, give the thread its saved chain, call the
outermost function. Each resume block takes its frame and calls the
next. The innermost one re-runs (or skips past) its system call or HLE
call.

Still to do after the chains (most of the work):

- Host state outside guest memory: lv2 objects (mutex, cond, semaphore,
  event queues and ports, timers, threads), lwmutex/lwcond host slots,
  memory allocators, file handles and offsets, SPU threads and SPURS
  tables, RSX/GCM state and render targets, audio ports, pads, sysutil
  callbacks, the clock offset.
- Waits that cannot simply be re-run: `sys_cond_wait` and `lwcond_wait`
  (they unlock first) need a "wait only" resume.
- Revised time: chains and resume about 3–5 h; the rest of the state
  about 10–20 h more.

## Re-entry test (11-13, done)

`SS_REENTER=1|2` with `SS_PROBE` on `build-mac-ss`: live threads jump
back to their start and rebuild their stack from the chain. 1,947
re-entries per 240 s run, none skipped, registers the same at every
re-run call entry (1,947/1,947), no crash. Fixed on the way: cold parts
(`func_X.cold.N`) folded into their function; HLE entry points with two
HLE host frames. The load side works on live threads; the save file only
has to add host state.

## Save and load design (11-13, after the chain test)

Where threads are when the save stops them (100 checks, 36 threads):

- Most PPU threads sit in a blocking call: `sys_lwcond_wait` (most),
  `sys_event_queue_receive`, `sys_timer_usleep`, `sys_mutex_lock`, and
  one HLE wait (`WaitOnAddress`).
- On load each one runs its call again, but some calls cannot simply
  run twice:
  - `lwcond_wait` / `cond_wait` unlock their mutex first. Load needs a
    "wait only" form: wait, then lock again. Simplest: lock again and
    return OK at once (a spurious wake-up). The port already does this
    every 1 ms for most `lwcond_wait` sites (the 1 ms poll cap), so the
    game copes with it there; the "strict" sites need a real wait.
  - `timer_usleep`: sleep only the time that was left.
  - `event_queue_receive`, `mutex_lock`, `sem_wait`: safe to run again
    (the queue, owner and count come back from the save).

Save file (one per slot, device only):

- Header: format version and a build id. Chains only fit the exact
  build that made them, so a different build refuses the file.
- Game memory: used pages only, LZ4.
- PPU threads: registers, chain (from `ss_chain_build`), which call to
  re-enter and how.
- Kernel tables: mutex, cond, rwlock, semaphore, event queues and
  ports, timers, lwmutex/lwcond host slots, memory allocators, threads.
- Files: path, mode, offset. Reopened on load.
- SPU: see below.
- RSX: method registers, FIFO pointers, display buffers, render-target
  pictures (GPU readback).
- Audio ports, pads, sysutil callbacks, the clock offset.

SPU (the hardest part):

- SPURS policy modules run in short passes. The save waits for the
  current pass to end, so nothing is mid-way.
- SPURS tasks (for example the file task) and the raw SPU thread park
  in the middle of their code (`WAIT_SIGNAL`, channel waits). Their
  whole state is in local store + registers + pc, but their host
  frames are not.
- First try: resume them with the existing SPU interpreter from the
  saved pc. It shares the same context and calls lifted code for any
  known function, so it is correct, but the task's outer loop stays
  interpreted (slower) until the task ends. Measure the cost; if too
  slow, add SPU resume chains like the PPU ones.

Load (fresh start, `SS_LOAD=<file>`):

1. Boot as normal up to just before the first game thread runs (game
   file loaded, system modules, Metal and audio up).
2. Fill game memory, rebuild kernel tables and host objects.
3. Start one host thread per saved PPU thread. Each sets its chain and
   calls its outermost function; the resume blocks rebuild the stack.
4. Start SPU threads and SPURS, audio, then the frame clock.

Test ladder (Mac first; each step finds missing state):

1. Save at ~5 s (few threads and objects), load, does the intro go on?
2. Save at the main menu, load.
3. Save in play, load, play 5 minutes.
4. iPad, then Save/Load in the in-game menu.

## Risks

- The stack walk may miss frames (functions that keep the return
  address in a register, or "frame drift" bugs). Phase 0 finds out.
  Fallback: a shadow call stack (one push and pop per call, maybe 1–3%
  slower).
- A thread inside a host→game callback (sysutil, savedata) cannot be
  saved: wait until it returns.
- A thread in a busy loop never reaches a system call: the save waits,
  then says "try again".
- No saving during movies (Bink on SPU) or while the game writes its own
  save.
- Save files hold game memory, so they count as game content: kept on
  the device only, never in git or uploads.

## Rules kept

- Windows: the lifter mode is opt-in (default = today's output). Runtime
  code under `#if defined(__APPLE__)`.
- No JIT.
- The game's own saves stay as they are.
