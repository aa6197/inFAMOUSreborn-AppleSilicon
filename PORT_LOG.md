# Apple port log

Read this first every session. Newest entry on top.

## Now

- **End-user release (new):** prebuilt Mac app and iPad .ipa with no game
  files. The player picks their own disc image or game folder at first
  start; the app unpacks it, decrypts EBOOT.BIN and checks it. See entry
  "End-user release: in-app disc import". Make the files with
  `DEPS_DIR=$WORK_DIR/deps infamous-recomp/tools/release_apple.sh <ver> $WORK_DIR/dist/<ver>`
  (no args: `~/inFamous/dist/<ver>`, deps in `~/inFamous/deps`).
- **Legal points for the user's lawyer** (not decided by me): the
  prebuilt binary holds code translated from the game; it embeds the
  EBOOT metadata key (`import/eboot_key.h`, git-ignored, never commit);
  at3_standalone (LGPL 2.1) is linked statically; the repo has no
  top-level licence.
- Milestone: **M9 done on the real iPad** (iPad Pro 11 M4, iPadOS 27.2):
  gameplay + pause menu, 330 s, no crash. M10 (iPhone): Simulator
  (iPhone 17, iPhone 18 Pro), plus one test on the real iPhone 16 Pro
  (11-11, user's OK): runs, 12.6–15.8 flips/s. See entry "iPhone 16 Pro:
  first real-device test". Since 11-12 the user allows speed tests on
  the iPhone (A/B, profiling): `IOS_DEV=<iphone>` with
  the iOS tools. Nothing else on the iPhone.
- **iPhone speed now (11-12):** the long freezes (2–55 s) are fixed
  (d09eb0b, lock-line wait yields and sleeps). Lane workers now run at
  user-initiated QoS on 6-core devices (074495d, +8–12%). Main-loop fix
  (20678f5) and PASS_TRIM (c3ed3d0) added ~4%. Now ~23 flips/s hot
  (start: ~14), ~31 cool. Goal: high 30s. The hot phone is CPU-bound:
  GPU-only cuts no longer raise fps. Two short freezes came back in
  rounds 7–8 (open). See entries "iPhone: in-run A/B, profile, ..." and
  "iPhone: lane workers at higher priority". 11-13: full run at 720p
  **28.3 / 27.8** hot, no freezes (~+20%); 360p +28%; at 450p shadows
  off +7%, near-only +6%. The phone's Settings app has Resolution saved
  at 450p (the user's setting, leave it); tests force 720p with
  `RENDER_SCALE=100`.
- **In-game menu (11-13):** MENU on the touch pad, or Home / touchpad
  click on a gamepad: pauses the game; Shadows, Resolution, Upscaling.
  Tested in the Simulator only. See entry "In-game menu (iOS)".
- **11-12 evening (iPad only):** shadows cost ~10% on the iPad; new
  Shadows setting (On / Near only / Off, default On). Three exact RSX
  thread trims (plan memo, program reuse, dead-method skip), each small.
  See entry "iPad: effect costs, shadow setting, RSX thread trims".
- **11-12 night:** new Resolution setting (below 720p, MetalFX upscale;
  iPad 360p +16%) and the yellow/blue flash cause found: the particle
  buffer was read as depth. See entry "Lower resolution setting, present
  shots, yellow/blue flicker".
- **11-13:** the flash fix is always on (user's OK), no switch. Checked
  on the Mac and the iPad. See entry "Yellow/blue flash fix always on".
- **Save states (11-13):** the user wants save-anywhere. Plan in
  `SAVE_STATES_PLAN.md`. Phase 0 on the Mac: **go**. Every thread stops
  at a safe point in ≤1.3 ms; save ~233 MB (LZ4). Call chains are
  complete in 100/100 checks once bctrl sets the guest LR (now 0/100).
  Phase 1 (Mac, test build `PORT_SAVE_STATES`, default OFF): resume
  chains work, 3600/3600 threads in 100 checks. Re-entry test (live
  threads thrown back and rebuilt from their chains): 1,947/1,947,
  registers the same each time. Save file writer works (236 MB, 36/36
  threads, frozen ~0.6 s, the game runs on). Load, PPU core: works
  (memory, kernel, files, 36/36 threads back in 130 ms, no crash).
  Graphics (GCM / RSX) restore works: the walker runs on from the saved
  FIFO spot and a flip goes through. SPURS restore works: the title
  runs on after a load. HLE library state works. The SPURS taskset
  (FIOS) task is saved and restored. Audio restore works, and with it
  **a load from the title reaches gameplay** (START, cutscene,
  tutorial; 4 of 4 runs). Clocks: the guest clock goes on from the
  save. **iPad (11-13):** in-game menu "Save state" / "Load state"
  and a save on the way to the background work (save ~0.55 s, load
  ~0.3–0.55 s, gameplay after a load). **iPhone (11-13):** the same
  works (save ~0.8 s, load ~0.5 s). Movies after a load work again
  (the Bink decoder thread is saved and started again). Next: a save
  in the middle of the iPhone intro movie, then Mac. See entries
  "Save states: movies after a load", "Save states on iPad",
  "Save states: clocks", "Save states: audio",
  "Save states: SPURS taskset task",
  "Save states: HLE libraries", "Save states: SPURS",
  "Save states: graphics",
  "Save states: load, PPU core",
  "Save states: save file writer", "Save states: re-entry test" and
  "Save states phase 1: resume chains".
- **BG_TRIM (11-13, iOS, default on, user's OK):** lets go of the
  texture pool, stage ring and geometry memo in the background. iPhone:
  ~520 MB less in the background (1051 vs 1574 MB), clean resume.
  `BG_TRIM=0` turns it off.
- Signing works with my team (the PLA block is gone).
- The iPad may be busy with another task: run `ios_device.sh check` first.
- iPad speed now (11-11, round 5): **55.6 / 51.9 flips/s** with the hop gate,
  SPU vectors in registers and PGO profile v5 everywhere. Full list: "### iPad speed history" below.
  Older note: ~40 flips/s hot, ~43 warm in gameplay (hot ABBA,
  11-11). Was 27–28 hot on 11-10 and 20–21 on 11-09. Gains: short
  sleeps, NEON SPU helpers, RSX thread cuts. The iPad slows as it warms
  up (P cores ~1.8 GHz hot); only trust ABBA runs in the hot state. See
  entry "iPad speed: short sleeps, NEON, RSX cuts".
- Later on 11-11: texture and geometry hashes moved to 3 worker
  threads (TEXCS, GEOCS) and a texture pool. iPad hot 41–42 flips/s
  (39–40 on a hotter day), Mac static scene ~77–83.
- Later still on 11-11: one TLS block (4336c81, Mac +2.7%, iPad
  same), long main-thread waits park (05e2575, Mac +2.3% and −6% CPU,
  iPad same fps, −3..5% CPU), SPURS kick ported but off (7edc84e,
  `SPURS_KICK_APPLE=1`). A cooler iPad runs ~50–51 flips/s.
- The RSX thread is still the iPad limit, but now 87–92% busy
  (was 98–99%). Power is shared, so less work on any thread helps.
- PGO on Apple: profile `$WORK_DIR/pgo/apple-v4.profdata` (not in git).
  Mac +3%, iPad no gain. Build dirs `build-mac-pgo`, `build-ios-pgo`
  (installed on the iPad).
- Next speed work: see "### Next" in the 11-11 entry (main-thread
  spin wait, SPU job code, `sink_end`, Metal encode).
- **Mac keyboard + mouse (new):** same keys as Windows (WASD, mouse look,
  Space, Esc pause). See entry "Mac keyboard + mouse controls".
- Still needs the user: real taps on the touch pad, a real controller,
  and a real keyboard + mouse on the Mac (pointer hold, focus).
- M5 is **parked**: 3D is still too bright on near surfaces. Audio is
  now clean (it was choppy only because the game ran slow).
  Open items are in the M5 entry below.
- Run the static speed test with `$WORK_DIR/tools/prof.sh` and a
  movement soak with `$WORK_DIR/tools/soak.sh` (see the M7 entry).
- Run: `$WORK_DIR/tools/run_game.sh <secs> <tag> [VAR=val ...]` (see the port notes, "Run and observe").
- Build dir: `infamous-recomp/build-mac` (Release, `-O2`, arm64). Command is in the port notes.
- Game: extracted copy in `$WORK_DIR/game/BCES00220` (has `EBOOT.elf`).
  The ISO on an external drive is only needed to re-extract.
  - BCES00220, inFamous, Europe. PARAM.SFO VERSION 01.01, APP_VER 01.00.
  - Decrypted image: no .dkey. EBOOT.BIN is a retail SELF; RPCS3 decrypts it.
- Generated sources are in place in the repo (git-ignored). Don't regenerate
  unless needed; `install ... --no-build -y` re-checks them.


### iPad speed history

Flips/s on the iPad Pro (M4), same 330 s test, two runs per side,
mirrored order. The iPad slows as it heats, so compare before vs
after inside one row only. Data: `$WORK_DIR/perf/ipad_speed.csv`.
Table: `python3.12 $WORK_DIR/tools/speed_table.py`.

| # | Date | Change | Before | After | Gain | Notes |
|---|---|---|---|---|---|---|
| 1 | 11-08 | First iPad run |  | 14.6–15 |  |  |
| 2 | 11-08 | RSX thread at UI QoS |  | 15.5–15.8 |  |  |
| 3 | 11-08 | RSX trims (geo memo, const blocks, pipeline key, tex span) | 16.2–16.6 | 20.2 / 21.5 | +27.1% |  |
| 4 | 11-10 | Short sleeps yield |  | 27.9 / 26.3 |  | nanosleep mode was 24.0 |
| 5 | 11-10 | PGO (profile-guided build) | 27.3 / 27.0 | 30.8 / 29.9 | +11.8% |  |
| 6 | 11-10 | Program memo | 26.5 / 26.4 | 27.6 / 28.3 | +5.7% |  |
| 7 | 11-10 | Partial vertex constants | 25.8 / 26.5 | 27.7 / 28.0 | +6.5% |  |
| 8 | 11-10 | Inline loads/stores |  |  | +5.0% | +5%, run numbers not kept |
| 9 | 11-11 | NEON SPU helpers | 32.1 / 28.6 | 38.4 / 37.3 | +24.7% | the big one |
| 10 | 11-11 | RSX thread cuts | 38.1 / 37.0 | 40.6 / 40.4 | +7.9% | cool iPad hit 43.5 |
| 11 | 11-11 | PGO v4 (retrained) |  |  | 0.0% | no gain |
| 12 | 11-11 | Parallel hashes (TEXCS + GEOCS) |  | 41–42 |  | hot iPad |
| 13 | 11-11 | Thread-local block | 50.9 / 51.2 | 51.3 / 50.9 | +0.1% | same speed; cooler iPad |
| 14 | 11-11 | Park long waits | 49.3 / 51.5 | 51.0 / 50.3 | +0.5% | same speed, 3–5% less CPU |
| 15 | 11-11 | Round 2: stack guard off + mfc, lpgate, lane QoS 0 |  | 51.1 / 45.9 | 0.0% | all neutral; serial mode 35.3 / 33.4 (bad) |
| 16 | 11-11 | Round 3: hop gate | 43.6 / 43.5 | 46.3 / 46.6 | +6.7% | SPU PGO worth ~5.5% on top |
| 17 | 11-11 | Round 4: SPU vectors in registers | 47.4 / 46.9 | 48.3 / 49.1 | +3.3% | Mac same: 79.3 / 79.6 |
| 18 | 11-11 | Round 5: SPU PGO profile retrained (v5) | 54.4 / 47.7 | 55.6 / 51.9 | +5.3% | RSX thread 95% busy again; Mac 81.6 / 87.9 vs 80.1 / 71.5 (busy Mac, not reliable) |
| 19 | 11-11 | Round 6: Metal encoding on its own thread (not kept) | 54.2 / 55.6 | 54.1 / 55.0 | -0.6% | Drawing thread 93% -> 83% busy, but no faster. Something else also limits at ~55. |
| 20 | 11-11 | Round 7: geometry prefetch hints (not kept) | 54.9 / 53.0 | 53.1 / 53.1 | -1.6% | RSX thread 96% -> 94% busy, no gain. iPad warmer this round (P 1.86 -> 1.81 GHz). |
| 21 | 11-11 | Round 8: where the frame waits (diagnostic) | 55.6 (encode off) | 54.5 (encode on) |  | One run each. Game thread spends 25-30% in a wait loop, mostly on SPU jobs. |
| 22 | 11-11 | Round 9: SPU threads on slow cores (not kept) | 55.7 / 55.2 | 52.8 / 51.3 | -6.1% | Fast cores freed (272% -> 151%, 1.89 -> 2.34 GHz) but SPU work got slower. |
| 23 | 11-11 | Round 10: MetalFX upscaling off (option) | 53.8 / 52.2 | 54.9 / 54.8 | +3.5% | GPU busy 101% -> 82%. Costs the sharper upscale. Audio and job-wait tests: no gain. |
| 24 | 11-11 | Round 11: clear the shared depth (as D3D12) | 51.9 / 49.5 | 52.2 / 51.7 | +2.5% | Noisy: pairs +0.6% and +4.4%. Only ~2% of passes. Kept to match Windows. |
| 25 | 11-11 | Round 12: encode thread with MetalFX off (not kept) | 52.8 / 52.9 | 51.6 / 52.3 | -1.7% | Real GPU load with MetalFX off: ~70%. The GPU is not the limit; the 3 fast cores are. |

---

## 2026-11-13 (evening) — Save states: movies after a load (Bink SPU worker), iPhone results

### Problem

- iPhone: a menu save made during the intro movie (01_intro.bik, the
  iPhone plays it at boot) hung after the load (run 193710).
- iPad: after a load of a title save the billboard TV stood still.
- Cause: the Bink movie decoder is an SPU thread group ("RADG5", one
  thread, tid 0x2000) running lifted SPU code on its own host thread.
  Save / load did not include the SPU group and thread tables, so after
  a load there was no decoder and any movie waited forever.

### Fix (Apple only, lv2_register.c + ppu_ss_probe.cpp + ppu_ss_load.cpp)

- Save: `ss_sputhr_hold` holds each lifted receive worker between
  requests (a worker that is decoding finishes and sends its completion
  first). It counts as held once it is idle and took no request for
  5 ms. The "[ss] save:" line now says "SPU threads N held".
- The SPU group and thread tables are saved as raw bytes.
- Load: `ss_sputhr_restore` starts every thread that was running again
  from its entry point (as group_start does: image deployed again, same
  four args). Host pointers from the old process are cleared.
  Log: "[ss] load: SPU threads back (1 started, 0 not)".

### Results (iPad Pro 11 M4, save-state build)

| Test | Result |
|---|---|
| Title save at 30 s (run 195704, `SS_SAVE_CHECK=1`) | ok: SPU threads 1 held, frozen 714 ms, total 760 ms, 0 pages differ |
| Load of it (run 195757) | ok: SPU threads back (1 started), load 284 ms, decoder takes requests, billboard TV moves |
| Gameplay save at 55 s (run 200122 pair) | ok: frozen 578 ms, total 607 ms |
| Load of it | ok: load 289 ms, decoder back, gameplay runs on |
| Normal iOS build | compiles |

- Not tested yet: a save in the middle of a full-screen movie. The iPad
  skips 01_intro.bik (it goes to gameplay), so this needs the iPhone.

### iPhone 16 Pro results (save-state build, before the fix)

| Test | Result |
|---|---|
| Menu "Save state" from gameplay (run 194026) | ok: ~0.81 s |
| Background save (same run) | ok: 0.88–1.2 s |
| Menu "Load state" ×2, next start (runs 194338, 194426) | ok: load ~0.47 s, gameplay runs on |
| Menu fits the iPhone screen | ok (rows shrink to fit) |
| Save during the intro movie (runs 193429, 193710) | hung after the load: fixed above, re-test on the iPhone |

## 2026-11-13 (evening) — Save states on iPad: menu Save / Load, background save

### What is new (save-state build only; normal builds unchanged)

- Build: `IOS_BUILD_DIR=$REPO/infamous-recomp/build-ios-ss tools/ios_device.sh build ON
  -DPORT_SAVE_STATES=ON -DPORT_SAVE_STATES_ELF=<EBOOT.elf>`, then `install` with the
  same `IOS_BUILD_DIR`.
- In-game menu: two new rows.
  - "Save state": saves to Documents/save.ss while the game stays paused.
    Writes save.ss.part, renamed only when the save worked (a failed save
    keeps the old one). Button shows "Saving...", "Saved 19:20" or
    "Save failed, try again".
  - "Load state": two buttons, "Save <time>" (save.ss) and "Auto <time>"
    (auto.ss). First press arms it ("Press again to load", 4 s). Second
    press writes Documents/ss_load_next and closes the app. Opening the
    app again loads that save once (the marker is deleted first, so a
    load that crashes does not loop).
- Background: going to the background saves Documents/auto.ss inside a
  background task (`SS_AUTOSAVE=0` off).
- `SS_MENU=0` turns all of it off. Hooks are on from the start in these
  builds (needed for a save at any time).
- Fix: the menu pause held the frame clock in its own loop, so a save's
  graphics hold (ss_gcm_hold) would wait 10 s and fail. The pause loop
  now answers the park request (main.cpp, PORT_IOS only).
- Test hook: `MENU_SCRIPT="40:open,43:save,58:close,62:bg,80:fg"`
  (open, close, save, load0, load1, bg, fg) drives the menu and the
  app background events on a device with no touch.

### Results

iPad Pro 11 M4, save-state build, title screen:

| Test | Result |
|---|---|
| `SS_SAVE_AT=30` save (run 191645) | ok: 36 threads (0 without chain), safe in 0.9 ms, frozen 522 ms, total 569 ms, file 241 MB. Check: 0 pages differ (g_sys_timers byte 137: host cond var, harmless) |
| `SS_LOAD` of that save (run 191812) | ok: 36 threads back in 283 ms. START → intro cutscene → tutorial (gameplay) |
| Menu "Save now" (run 192233, `MENU_SCRIPT`) | ok: graphics held after 12 ms, frozen 531 ms, total 565 ms, 0 pages differ. Button shows "Saved 19:23" |
| Go to background (same run) | ok: auto.ss saved inside the background task, frozen 506 ms, total 531 ms. Back to the foreground: clock resumed |
| Menu "Load state" ×2 (run 192412) | first press: "Press again to load" + note; second press: marker written, app closed |
| Next start (run 192505) | marker read once ("this start loads save.ss"), load 551 ms, START → gameplay |

- Before the main.cpp fix the menu save would have waited 10 s for the
  frame clock and failed (the menu pause loop never answered the park).
- iPhone: not tested yet (locked during its test window). Next: the same
  tests on the iPhone (short screen: the rows now shrink to fit), then
  a save from gameplay, then Mac.

## 2026-11-13 (evening) — Save states: clocks (Mac)

### Guest clock goes on after a load (Mac, test build only)

- Problem: every guest clock was host `CLOCK_MONOTONIC` (time since the
  Mac booted). After a load the game saw time jump forward by the gap
  between save and load, and after a reboot it would jump backward.
- Guest clocks found: PPU timebase (`ppu_timebase_now`, every lifted
  `mftb`), `sys_time_get_system_time`, cellGcm flip and report times,
  the SPU decrementer (`dec_base_ns` is in the saved SPU contexts),
  cellAudio port timestamps (`s_audio_start_us`). Wall time
  (`sys_time_get_current_time`) stays real time.
- Now (Apple): these clocks add `g_ss_clock_off_ns` (sys_timer.c). It
  is 0 until a load, so normal runs give the same values as before.
- Save: `ss_clock_mark` notes the guest clock while every thread is
  stopped (blob `clock.guest_ns`). `ss_timer_hold` drops timer ticks
  during the save (they push events into the queues being copied).
- Load: `ss_clock_restore` sets the offset first, before graphics,
  SPURS, audio and the threads. `s_audio_start_us` is now a blob (it
  was set again at load).
- Results: save (`runs/20261113-184651-ss-csave1`): "timers held",
  0 pages differ. The timer table still differs at byte 137: that is
  inside its host condition variable (bytes 112-160), which the load
  makes again. Harmless.
- Load (`runs/20261113-184743-ss-cload1`, 21 s after the save): "guest
  clock goes on from 6389214.313 s (offset -21.385 s)". Load again
  3 min later (`runs/20261113-185040-ss-cload2`): offset -198.9 s. Both
  reach gameplay after START.
- Normal Mac build builds, boots and plays audio
  (`runs/20261113-184940-mac-clkchk`).
- Next: longer play after a load, then save from gameplay.

## 2026-11-13 (evening) — Save states: audio (Mac) — load reaches gameplay

### Audio restore (Mac, test build only)

- Cause of the black screen after a load: the cellAudio mixing thread
  did not exist in the new process. The game's audio loop waits on the
  audio event queue, so it never woke and the game stalled after the
  cutscene.
- Save: `ss_audio_hold` parks the mixing thread at the top of its loop
  (paced and plain loops). It writes guest read indexes and sends
  events, so it must not run while memory is saved.
- Saved as `mod:` blobs: init flag, ports (buffers and read indexes are
  guest memory), notify queues.
- Load (`ss_audio_restore`): each open port points at its guest buffer
  again, the host device opens again, the mixing thread starts.
- Results: save (`runs/20261113-180546-ss-asave1`, at 30 s): "audio
  held", frozen 736 ms, file 241 MB. Save check: **0 pages differ**
  (page 0x58100000 used to differ: that was the mixer). Only the timer
  table differs (clocks, next step).
- Load (`runs/20261113-180624-ss-aload1` and `-180852-ss-aload2`):
  "audio back (1 ports, mixing thread started)", 18 `[audio-rate]`
  lines (was 0). START at 8 s: cutscene, then **gameplay** (the
  tutorial with the "look up at the chopper" hint). Same in both runs.
- Normal Mac build builds, boots and plays audio
  (`runs/20261113-181027-mac-auchk`).
- Next: clocks (timer table), then longer play after a load.

## 2026-11-13 (evening) — Save states: SPURS taskset task (Mac)

### Taskset task restore (Mac, test build only)

- The game has one SPURS taskset task outside the policy modules: FIOS
  (taskset 0x405C2E00, task 0, image 2). It runs on its own host thread
  and waits in WAIT_SIGNAL.
- Save: `ss_spu_tasks_hold` takes the signal lock (try-lock, up to
  1 s) and keeps each task in its wait loop. Each waiting task is saved
  with its whole SPU context (section `spu.tasks`), plus the wake
  tables (`mod:spu.*` blobs).
- Load: the contexts come back (host pointers cleared, as for the
  policy contexts). A new thread per task enters the wait again
  (`spu_spurs_taskset_syscall`), then runs on from link r0. A resumed
  task handles its 0xA70 syscalls as the standalone case
  (`g_ss_spu_resumed_tls`, both interpreter loops).
- The Apple-only hooks cost one thread-local check per wait.
- Results: save (`runs/20261113-175755-ss-tsave1`, at 30 s): "SPU
  tasks 1 of 1 held", frozen 731 ms, file 241 MB. Load
  (`runs/20261113-175835-ss-tload1`): "1 of 1 SPU tasks started", the
  task waits again.
- Finding: in a normal run the FIOS task makes one syscall and never
  wakes (`runs/20261113-180131-ss-sysimg2`, `SPU_SYSCALL_IMG=2`). File
  loads after START run on the PPU. So this task was not why the screen
  went black after a load. Next: audio.
- Normal Mac build builds.

## 2026-11-13 (evening) — Save states: HLE libraries (Mac)

### Library state (Mac, test build only)

- Problem: after a load the title ran but START did nothing. cellPad
  was not initialised in the new process, so the game read no pad.
- Now saved as `mod:` blobs: pad, keyboard, mouse (init, ports),
  sysutil (callbacks, events, BGM, disc callback, queued completions),
  message dialog (open, callback, type, overlay text, progress),
  video out, trophies, sysmodule (loaded list), save data scratch,
  cellGame (all its state).
- Load: blobs come back, then the pad opens its host side again
  (`ss_pad_restore`), queued sysutil completions are queued again
  (`ss_sysutil_restore`), an open dialog shows again and gets a fresh
  no-button deadline (`ss_msgdialog_restore`).
- Save: the sysutil completion list is copied under its lock
  (try-lock; busy = the save fails, no wait).
- Limits: blob table 128 -> 512, sections 128 -> 1024. A dropped blob
  now fails the save (it used to say "save ok").
- **Name rule:** `ps3recomp/tools/gen_hle_nids.py` registers every
  non-static `cell*` / `sys*` / `sce*` / `_sys*` function in `libs/`,
  even inside `#if` blocks. Apple-only helpers there must start with
  `ss_` (not `cell*_ss_*`, not `sysutil_*`), or the Windows NID table
  names a function Windows does not have (link error).
- Results: save (`runs/20261113-173609-ss-libsave2`, at 30 s): all
  safe after 1.1 ms, frozen 547 ms, file 241 MB, check 1 page and the
  timer table differ (as before). Load
  (`runs/20261113-173641-ss-libload2`): "libraries back (pad open, 0
  sysutil completions, dialog none)", 36 threads in 169 ms. START at
  8 s works: the opening cutscene plays.
- Still broken: after the cutscene the screen stays black. A normal
  run (`runs/20261113-173832-ss-base2`) is black ~6-12 s, then
  gameplay. The loaded run opens no files (normal: 236) and its SPURS
  taskset task never runs. Audio is also not running (0 `[audio-rate]`
  lines, normal 28). Next: the taskset (FIOS) task, then audio.
- Normal Mac build builds and boots to the title
  (`runs/20261113-174212-mac-spurschk`).

## 2026-11-13 (evening) — Save states: SPURS (Mac)

### SPURS restore (Mac, test build only)

- The save parks every SPURS kernel thread at the top of its pass loop
  (`ss_spurs_hold`), after the PPU threads are suspended. Lane workers
  are joined inside a pass, so no policy module is running there.
- Saved: instance and workload tables, tasks, event queues, job chains
  (`mod:spurs.*` blobs), and every persistent policy-module context
  (section `spurs.pm`: 51 contexts, ~265 KB each, LS included).
- Load: the contexts come back (host lock and wait pointers cleared, a
  held reservation is reported as lost), then each instance starts its
  kernel threads again. `spurs_kernel_start` was moved out of
  `spurs_initialize_common` unchanged, so Windows runs the same code.
  `s_pm_ctx` / `s_pm_data` moved to file scope (same behaviour).
- Results: save (`runs/20261113-172230-ss-spsave1`, at 30 s): 12 of 12
  kernel threads parked after 1.2 ms; save check 1 page differs. Load
  (`runs/20261113-172411-ss-spload1`): "SPURS back (2 instances, 51
  policy contexts)". The title screen runs on (~117 flips/s headless;
  a normal run ~89), Cole walks across it.
- Still to do: the SPURS taskset task (runs on its own thread, not
  saved yet), audio, clocks.
- Normal Mac build builds and boots to the title.

## 2026-11-13 (evening) — Save states: graphics (Mac)

### Graphics restore (Mac, test build only)

- Save and load now carry the GCM / RSX state as "mod:" blobs
  (`ss_gcm_blobs`, `rsx_draw_engine_ss_blobs`): display buffers,
  IO map tables, handlers, labels, reports, the FIFO walker position and
  its RSX state, the draw engine's register file.
- Both hold the graphics still (`ss_gcm_hold`): the frame clock
  parks at the top of its loop (`main.cpp`, Apple only) and the walker
  lock is taken. On load this also waits for the backend to be up.
- The load does not run `cellGcmInit` again (it zeroes the control
  register). Host handler pointers, the SPU put hook and the engine's
  display buffers are set again; the context goes live last.
- Results: save (`runs/20261113-171205-ss-gsave1`, at 30 s): graphics
  held after 1.2 ms; save check 1 page differs (was 3). Load
  (`runs/20261113-171245-ss-gload1`): "graphics back (2 display
  buffers)"; the walker runs and one flip goes through, then the game
  waits on SPU jobs (SPURS not restored yet).
- Normal Mac build still builds (the walker's state moved to file scope,
  same behaviour; Windows code unchanged otherwise).

---

## 2026-11-13 (evening) — Save states: load, PPU core (Mac)

### Load, PPU core (Mac, test build only)

- New test: `SS_LOAD=<file>` (`build-mac-ss` only; `ppu_ss_load.cpp`).
  The main thread loads before the guest entry:
  - game memory, the lv2 tables, and new "mod:" blobs (module statics:
    lwmutex table, bump pointers, fd tables, ...);
  - every host lock in the tables is made again (mutex, cond, rwlock,
    semaphore, event queue / flag, timers restarted);
  - lwmutex host semaphores rebuilt from the guest owner word;
  - open files reopened at their saved position (each fd now keeps its
    flags and position; Apple only);
  - one host thread per saved PPU thread.
- Each thread takes its registers and chain, relocks the sys_mutexes it
  owned, waits for all others, then fixes up the call it was blocked in:
  `sys_lwcond_wait` and `sys_cond_wait` relock and return OK (a spurious
  wake-up); every other wait runs again from its start.
- Save files only load into the build that made them (old saves in
  `$WORK_DIR/saves` are from older builds).
- Result (`$WORK_DIR/runs/20261113-165515-ss-load1`, save at 30 s):
  - 5 mutexes, 7 event queues, 1 timer, 106 lwmutexes, 6 files.
  - All 36 threads ready after 126 ms, all resumed after 130 ms. No
    crash, no broken chain.
  - Then the game waits: GCM/RSX, audio, SPURS and the SPU task are not
    restored yet (main thread loops on a short sleep).
- Next: restore those modules (GCM first). SPU plan from a code survey:
  rebuild the one parked SPU task (FIOS) from its LS + registers and
  resume it in the interpreter at its link register; needs a small fix
  so the interpreter handles a standalone task's syscall at LS 0xA70.

---

## 2026-11-13 (evening) — Save states: save file writer (Mac)

### Save file writer (Mac, test build only)

- New test: `SS_SAVE_AT=<secs> SS_SAVE=<file>` (`build-mac-ss` only).
  Stops every thread like the probe, then writes one save file.
  `SS_SAVE_CHECK=1` reads the file back while the threads are still
  stopped and compares it with live memory. `SS_SAVE_EXIT=1` quits
  after the save.
- File (`ppu_ss_file.{h,cpp}`): header with the build id (the exe's
  UUID: a file only loads into the build that made it), game memory
  (non-zero pages only, LZ4), the lv2 kernel tables as raw bytes, then
  each PPU thread (registers + chain).
- While the threads are stopped the writer takes no locks (a stopped
  thread may hold the malloc or stdio lock): only system calls and
  buffers made before the stop. Logging waits until the threads run.
- Speed (Mac A/B, both -O2, 140 s windows): normal build 94.7 (cold
  first run) and 104.5 flips/s, save-state build 103.3 and 105.2. **The
  save-state build costs nothing measurable.**
- Result (`$WORK_DIR/runs/20261113-162812-ss-save2`, save at 90 s):
  - 36 threads, all with a chain. All safe after 0.2 ms, frozen
    573 ms, total 615 ms.
  - Memory: 69 ranges, 4095 MB mapped, 354 MB non-zero, 233 MB packed.
    Tables 2.9 MB. File 236 MB (in `$WORK_DIR/saves`, device only).
  - Read-back check: 1 page differs (0x58100000, the audio ring: the
    audio thread does not stop) and 1 table (`g_sys_timers`, a timer
    thread's host lock). Both are expected. The loader remakes host
    locks.
  - The game ran on after the save (flips 18,340 at 92 s → 24,640 at
    152 s).
- Open: LZ4 packs only 354 → 233 MB. Next: the loader.

---

## 2026-11-13 (evening) — Save states: re-entry test (Mac)

### Re-entry test (Mac, test build only)

- New test `SS_REENTER` (with `SS_PROBE`, `build-mac-ss` only). After
  each check, every thread stops at its next safe point and builds its
  chain. Then it jumps back (longjmp) to its start and rebuilds its
  stack from the chain through the resume blocks. The game goes on.
  This runs the load side on live threads, with no save file.
  - `SS_REENTER=1`: at any safe point (before or after a system call or
    HLE call).
  - `SS_REENTER=2`: at call entries only. The call runs again, and the
    registers at the second entry are compared with the first.
- How: thread starts (`ppu_thread_entry_trampoline`, `ppu_run`) call a
  hook (`g_ss_run_hook`, Apple only, NULL unless the test is on) that
  sets the jump point. `ppu_ss_icall_reset` clears the indirect-call
  depth counters the jump skips.
- Two fixes found:
  - Clang moves unlikely code, the resume block too, into
    `func_X.cold.N` (18,267 functions in the ss build, 60 in the normal
    build). After a re-entry the cold part has its own host frame, so
    the main thread showed `func_00010230` twice and failed every later
    check (35/36). The frame reader now folds a cold part into its
    function.
  - Every HLE entry point was skipped: `ps3_hle_call` and
    `ps3_hle_call_impl` both have a frame there, and the check took the
    second one for a nested call.
- Result, 240 s each, 60 checks
  (`$WORK_DIR/runs/20261113-154729-ss2-re3m1`, `...-155142-ss2-re3m2`):
  - Mode 1: 1,947 re-entries (sc in 4, sc out 362, HLE in 29, HLE out
    1,552), 0 skipped; registers the same 33/33.
  - Mode 2: 1,947 re-entries (sc in 152, HLE in 1,795), 0 skipped;
    **registers the same 1,947/1,947**.
  - Both: every check 36/36 threads, the game ran to the end, no crash.
- So chains and resume blocks rebuild live threads correctly. Left for
  save and load: host state outside game memory (kernel objects, files,
  SPU, RSX, audio).

---

## 2026-11-13 (late afternoon) — Save states phase 1: resume chains (Mac)

### Resume chains (Mac, test build only)

- New CMake option `PORT_SAVE_STATES` (Apple only, default OFF), build
  dir `build-mac-ss`. A build-time pass (`ps3recomp/tools/ss_resume_pass.py`)
  runs after register caching on each lifted file. It does not change
  the lifter, so Windows output is the same.
- The pass adds: the guest LR at each bctrl/blrl, `ctx->cia` at each
  system call, a label after each call, a resume block at the start of
  each function, and a site table per file. All 27,305 functions get a
  resume block.
- Save side (`ppu_ss_resume.cpp`): for each stopped thread, line up its
  host frames with return addresses on the guest stack and look each
  one up in the site tables. Any miss refuses the save.
- Two fixes found by the first run (9 of 36 threads ok):
  - 284 functions were skipped: their first line ends in a comment.
  - A thread inside an HLE call: the import stub tail-calls the HLE
    code, so the stub has no host frame. The save now reads the `bl`
    before the return address to find the stub.
- Result, `SS_PROBE=5 SS_PROBE_N=100 SS_PROBE_START=30`, 600 s
  (`$WORK_DIR/runs/20261113-151115-ss2-probe`):
  - **100/100 checks, 36/36 threads each: every chain complete**
    (3600/3600, 0 failures). Every chain reaches the end of the guest
    back chain.
  - Stop: all threads safe in 100/100; median 0.55 ms, max 0.76 ms.
  - Save size as phase 0: 355 MB non-zero, 233 MB LZ4 in ~390 ms.
  - Chains are 1–21 frames.
- Two stack slots can fit one frame (1,336 of 3,600 chains). Checked
  with the `[ss]  a` line, 240 s run (`$WORK_DIR/runs/20261113-152421-ss2-amb`,
  60 checks, 2160/2160 ok): all 972 cases are the caller of a frameless
  HLE stub. The live LR (taken) is right; the other slot is stale, left
  by an earlier call from the same function. Settled.
- Where threads wait at a stop: `sys_lwcond_wait` (most),
  `sys_event_queue_receive`, `sys_timer_usleep`, `sys_mutex_lock`.
  These decide the load design (`SAVE_STATES_PLAN.md`, "Save and load
  design").
- Not measured yet: speed of the `build-mac-ss` build against
  `build-mac`.
- iOS Simulator build with the option also builds (the stack reads use
  `vm_read_overwrite`; `mach_vm.h` is not allowed on iOS).

## 2026-11-13 (afternoon) — In-game menu (iOS)

### In-game menu (iOS/iPadOS)

- The user asked for a settings menu inside the game. Native UIKit
  overlay, `infamous-recomp/ios/game_menu.mm`.
- Opens from: a new **MENU** button on the touch pad (top middle, under
  SELECT/START), the gamepad Home/Guide button, or the DualSense
  touchpad click. SDL 3.4.18 turns off the system gesture on Home, so
  the app gets it.
- The game pauses: `port_app_set_paused(2)` parks the frame clock
  (`[ios] frame clock parked (in-game menu)`), without BG_TRIM. The
  touch pad hides while the menu is up.
- Rows: Shadows (On / Near only / Off), Resolution (720p … 360p),
  Upscaling (MetalFX / Simple), then Resume. Same keys as the Settings
  app page. Shadows and Upscaling change at once; Resolution is saved
  and a note says it changes at the next start.
- Gamepad: D-pad or left stick moves and changes values; Circle, START,
  Home or touchpad click closes; Cross on Resume closes. It closes on
  button release, so the game never sees that press.
- Coming back from the background with the menu open keeps the game
  paused.
- Test: iOS Simulator (iPhone 18 Pro), `TOUCH_PAD=1` (the Simulator
  sees the Mac's gamepad, which hides the pad). MENU → paused,
  Shadows Off → `shadow maps: 0 drawn` at once, 540p → note shown and
  saved, back to On/720p, Resume → `frame clock resumed`, pad back.
  `$WORK_DIR/runs/20261113-150927-menu-sim2`.
- Not tested yet: gamepad buttons (no way to press them from here) and
  a real device (iPad offline; iPhone off-limits until the user is
  back).

## 2026-11-13 — Save states phase 0, background trim, iPhone round 9

### Save states phase 0 (Mac, 77f9e84, test only)

- `SS_PROBE=17 SS_PROBE_START=30` run, 1900 s (`$WORK_DIR/runs/20261113-105913-ss-probe`),
  100 checks from 51 s on. Check with `ps3recomp/tools/ss_probe_check.py`.
- Stop: all threads at a safe point in 100/100. Stop time median 0.0 ms,
  p90 0.8, max 1.3. Suspend + read median 0.22 ms.
- Where threads stop: in an HLE call 2330, in a system call 900, at the
  HLE/system-call edge 370.
- Call chains (host frames lined up with guest LR words, in order):
  with bctrl setting the guest LR, **100/100 checks complete** (threads:
  2500 ok, 1100 only wrong at bctrl links, 0 missing, 0 extra). As the
  lifter is now: 0/100, because bctrl leaves the guest LR as it was and
  the callee saves an older one.
- Memory: 355 MB non-zero of 4 GB guest space; LZ4 233 MB in ~380 ms.
- No hang: flips kept rising. RSS 1668 → 2561 MB over the run; not yet
  compared with a run without the probe.
- RSX report memory: the game does not import
  `cellGcmGetReportDataAddress` or the Location calls (EBOOT NID
  search), so nothing extra to save there.
- Verdict: **go** for phase 1: an opt-in lifter flag so bctrl sets the
  guest LR (default = old output, Windows unchanged).
- Checker fix (same day): it now lines chains up one entry per frame and
  keeps recursion (was 97/100 from checker limits).

### Background memory trim (6e290c8, iOS; default on since 11-13)

- `BG_TRIM=1`: in the background, free the texture pool, stage ring and
  geometry memo (refilled on use), and ask malloc to give memory back.
  Logs `[ios-mem] background: footprint A -> B MB` and the foreground
  number. Default off.
- Builds for the Mac and iOS. iPhone test (450p, one run each, Settings
  app opened at 140 s, game back at ~180 s):
  - With trim (`20261113-121918-iphone-bgtrim1`): freed texture pool
    8 MB, stage ring 256 MB, geometry memo 256 MB; kept 197 MB of
    textures and targets. Footprint 1688 MB going to the background,
    **1051 MB** at the foreground line (before restore). Available
    memory 5092 MB.
  - Without (`20261113-122406-iphone-bgctl1`): 1703 → **1574 MB**,
    available 4569 MB.
  - About 520 MB less while in the background. Resume was clean in
    both (screenshot: textures right; ~36 flips/s after).
  - The `background: A -> B` line is taken before the RSX thread has
    parked, so the drop shows only at the foreground line. The event
    handler logs twice (old, harmless).
- Not tested on the iPad yet (offline).
- User's OK (11-13): **default on** for iOS; `BG_TRIM=0` turns it off.
  The code is iOS-only (`PORT_IOS`), so the Mac and Windows are
  unchanged.
- Check with no switch set (`20261113-132656-iphone-bgdef1`): log says
  `background trim: on`; 1692 MB going away, **1057 MB** while away;
  picture right on return, back to 1556 MB once drawing again.

### iPhone round 9 (in-run A/B, 300 s window, 18 pairs each)

- Resolution 720p → 360p: 37.3 → 47.6 flips/s (+27.7%), thermal fair.
- Shadows All → Near only (450p): 43.2 → 45.8 (+6.0%), thermal serious.
- Shadows All → Off (450p): 31.3 → 33.5 (+7.1%), thermal serious (re-run
  at 11:37; the first try had no data, the app was in the background).
- Both shadow tests ran at 450p: the Settings app had Resolution saved
  at 450p (set while the user had the phone). Env `RENDER_SCALE` wins
  over the saved value.
- Flash check on that run (`ZS_SUB_WATCH=1`): 0 bad reads at
  0x01AB8000, 122 kept as colour. Clean on the iPhone too.

### Speed now (11-13)

- Mac static scene 99.2 / 100.7 flips/s (was ~92).
- iPad 53.8 / 53.0 (11-11: 55.6 / 51.9, same within noise).
- iPhone full 330 s run at 720p (`RENDER_SCALE=100`), thermal serious:
  **28.3 / 27.8** steady, no freezes (11-12 hot: ~23, so ~+20%). Runs
  `20261113-122927-iphone-base720a` and `-123540-iphone-base720b`.
- iPhone at the saved 450p: 32.2 / 28.6 (`-114657-iphone-base1113a`,
  `-115309-iphone-base1113b`). Only a little faster than 720p on a hot
  phone: the CPU is the limit.

## 2026-11-13 — Yellow/blue flash fix always on

- User's OK: the fix from 62dda0c is now always on (Apple), with no
  switch. `ZS_CLEAR_CW` is gone: a colour clear always counts as a
  colour write for the v73c rule (`ZS_CWRITTEN`), so a target that was
  only cleared reads as colour, not as the depth at its address.
- Windows: unchanged (Apple-only code). The D3D12 backend has the same
  v73c rule, so it may show the same flash; not touched.
- Mac, 66 s, Start at 50 s, no switches (`ZS_SUB_WATCH=1` only):
  0 bad reads at 0x01AB8000 (was 44 without the fix), 46 kept as
  colour (`[zskeep]`); reads at 0x0F340000 (shadow maps) 373 log lines
  (377/378 before). Frame dump of the first kept frame looks normal (the
  washed-out wake-up scene, no yellow).
- iPad, 120 s, Start at 55 s, `ZS_SUB_WATCH=1` only: 0 bad reads at
  0x01AB8000, 43 kept as colour, 0x0F340000 reads 381 log lines. No
  crash, audio real time.
- Not checked: a long gameplay soak with panning on the iPad, and the
  user's own panning case.

## 2026-11-12 (night) — Lower resolution setting, present shots, yellow/blue flicker

### Resolution below 720p (7250177)

- `RENDER_SCALE=<percent>` (25–100, default 100 = no change): targets
  at the display size (1280x720) are drawn at k/80 of that size. WPOS
  reads, viewport, scissor, depth copies and readback follow the real
  size. The present upscales: MetalFX (up to 2x per axis), then linear.
- iOS Settings: **Resolution** (read at start, default 100%) and
  **Upscaling** (MetalFX on/off, live, default on). Opt-in: the default
  picture is unchanged.
- iPad, same run, both with MetalFX: 720p 47.36 → 360p 54.93 fps,
  **+8.20 ± 0.30 fps (+16%)**. 360p phases sat near the 60 Hz cap, so
  the real gain may be bigger.
- Picture: 360p is softer and a little flatter; brightness is the same.
  Comparison set (iPad with MetalFX, iPhone simulator with and without
  — the simulator has no MetalFX) is in the user's iCloud Drive,
  folder "inFamous low-res comparison". Not in git.

### Present shots (6520240, test)

- `PRESENT_SHOT=<secs>` saves the final presented image (after MetalFX
  and the letterbox, no touch controls) as BMP. `PRESENT_SHOT_SIZE=WxH`
  presents at a set size (e.g. iPhone screen on the Mac).

### Yellow/blue flicker (user report)

- User: while panning, the screen sometimes flashes yellow (sky) and
  blue (ground) for a moment, "normally when something's obscuring
  something". It happened before the resolution work too.
- Colour match: a D24S8 depth buffer read as ARGB8 gives A = depth
  high byte, R = mid, G = low, B = stencil. Far depth → yellow; near
  geometry with stencil → blue/lavender. Blurry = half-size buffer.
- `FP_CONST_WATCH=1` (6-minute pan): the final composite's
  desaturation constant (c7.x) never moved. Not the cause.
- `ZS_SUB_WATCH=1` caught it: the composite reads the 512x288 particle
  buffer at 0x01AB8000, and on some frames the v73c rule
  (`ZS_CWRITTEN`: a colour target nothing drew into reads the depth at
  its address) swaps it for the 1280x720 depth at the same address.
  Seen in short bursts (frames 13810–13854, 14922).
- Why: the surface is made new every frame (the address changes size),
  so `cwritten` resets; on frames with no particle draws the buffer is
  only cleared, and a clear did not count as a colour write.
- Proof picture (Mac, frame dump on the first bad frame): without the
  fix the final image is yellow sky over the normal ground (mean RGB
  200/200/55); with the fix a frame in the same window looks normal.
  Seen ~0.5 s right after Start (~51 s) and once at ~64 s in the pan
  test.

### Fix (62dda0c, opt-in)

- `ZS_CLEAR_CW=1`: a colour clear counts as a colour write for the
  v73c rule, so a target that was only cleared reads as colour.
- Mac pan test (6 min): all 48 bad reads at 0x01AB8000 became colour
  reads (`[zskeep]`); the reads at 0x0F340000 (shadow maps, soft smoke
  depth) are unchanged (154/267 vs 151/267 log lines). No speed cost.
- Was default off until the user's OK; always on since 11-13 (see
  entry "Yellow/blue flash fix always on").
- New test switches: `ZS_SUB_WATCH=1`, `ZS_SUB_DUMP=<hex>`,
  `TEX_KIND_WATCH=1`, `FP_CONST_WATCH=1` (see the port notes).
- Not checked yet: the iPad, and a long gameplay soak with the fix on.


## 2026-11-12 (evening) — iPad: effect costs, shadow setting, RSX thread trims

All device tests tonight on the iPad (the user said: no iPhone tonight).
In-run A/B (`ios_phase.sh`), 15 s phases. Noise: the "bit1" line (no
switch) moves by up to ±0.7 fps, so a gain under ~0.7 fps is not proven.
The iPad also cools/heats during a run (53 → 46 fps).

### What effects cost (iPad, in-run A/B)

| Switch | Skips | Paired gain |
|---|---|---|
| `SHADOWS=0` | all 4 sun shadow maps (~40% of draws) | +5.2 fps (+9.7%) |
| `SHADOW_MAPS=1` | 3 of 4 maps (keeps the one round the player) | +4.6 fps (+9.5%) |
| `FX_SKIP=1` (test) | 512x288 particle/smoke buffer | +1.1 fps (+2.0%) |
| `FX_SKIP=2` (test) | bloom/blur chain | +0.4 fps (noise) |

- New commits: skip switches (0402e42), Graphics settings for shadows
  (13bafce: iOS Settings page and Mac Graphics menu, On / Near only /
  Off, default On), pass dump (a9754fd), Mac window vsync switch
  (beb143f: `PS3RECOMP_METAL_DISPLAY_SYNC=0`).
- Mac window, gameplay: 50.5 flips/s with vsync, 80.6 without.
- Not checked yet: the iOS Settings page on the iPad itself.
- Open (user's call): bloom/particles as user settings; a render
  resolution below 720p plus upscale.

### Where the RSX thread goes (iPad profile prof6)

- RSX thread ~98.5% busy: it is the limit (3 fast cores).
- `sink_end` 66% (encode/commit 18.8%, fetch plan 5.4%, guest programs 4%,
  texture slot 4%, geometry hash + XXH3 ~7.5%, sampler 2.1%).
- FIFO parse ~25%: walker 10.1%, `rsx_dispatch_method` 6.7%,
  `rsx_process_method` 4.6%, const blocks 3.7%.
- FIFO words (Mac, 200M methods): BEGIN_END 12%, DRAW_INDEX_ARRAY 11%,
  INDEX_ARRAY_ADDRESS/DMA 6% each, VTXBUF_OFFSET 4.6%. 44 methods with
  no handler = 3.9% of words.

### Trims (all exact, Apple only, on by default)

| Commit | Change | Exact check | iPad |
|---|---|---|---|
| 08ab493 | `PLAN_MEMO`: reuse the vertex fetch plan while vertex registers are unchanged | `PS3RECOMP_ENGINE_PLAN_VERIFY=1`: 168M hits, 0 differ (98% hit rate) | +0.46 ± 0.19 fps |
| a97b8ee | Sampler hint table before the full sampler scan |  | Mac run clean, 92.3 flips/s |
| 3a49c16 | `GP_REUSE`: one guest program lookup per draw, not two | `PROG_VERIFY`: 1.37 billion, 0 differ | +0.32 ± 0.16 fps |
| 515dbb0 | `RSX_DEAD_SKIP`: methods with no handler skip the ~100-test chain | `RSX_DEAD_VERIFY=1`: 144.7M, 0 differ (Mac) | −0.13 ± 0.33 fps (no change) |

- Each is small. Plan memo and program reuse point up (inside the noise
  alone); dead-method skip shows no change. All kept: exact and cheap.
- Off switches: `PS3RECOMP_ENGINE_PLAN_MEMO=0`, `PS3RECOMP_ENGINE_GP_REUSE=0`,
  `RSX_DEAD_SKIP=0`. A/B: `PLAN_MEMO_PHASE`, `GP_REUSE_PHASE`,
  `RSX_DEAD_SKIP_PHASE` (=0|1).

### Next

- Skip `seen[idx]++` in `rsx_dispatch_method` on Apple (only Windows
  live draw and TC_VERIFY read it).
- Inline the walker → engine → dispatch calls.
- Encode/commit (18.8%), geometry hash memo.
- Render targets: the game reuses one address for targets of different
  sizes in a frame (0x01AB8000: 1280x720 → 1280x180 → 512x288), so
  `eng_surface_get` frees and makes new Metal textures every frame.
  ~1% of the RSX thread (kernel calls). A texture pool would fix it, but
  a reused texture holds old pixels instead of a fresh one: needs a clear
  on reuse and a frame check.
- SPU threads use the most CPU in total (3 lanes + kernels, ~2.5 cores):
  spread over many job functions, no single hot spot. `_tlv_get_addr`
  is 1.4–2% per SPU thread (callers: job code, `spu_drain_call`, MFC).
- SPU lifter register spills (opt-in flag), wait loops.

## 2026-11-12 — iPhone: in-run A/B, profile, main-loop fix, PASS_TRIM, GPU tests

### In-run A/B on Apple (ec54bd5)

- `PS3_AB=1` phase clock now runs on Apple too (pthread). Every
  `PS3_AB_SEC` s the phase number goes up. A switch with
  `<NAME>_PHASE=0|1` is on only in phases of that parity
  (`2|3` = bit 1). Both sides share the same heat, so heat drift
  cancels out.
- Tools in `$WORK_DIR/tools`: `ios_phase.sh <tag> VAR=val ...`
  (one 480 s run, 15 s phases, flips/s 150–450 s) and
  `abphase.py <run>` (paired diff: each phase vs its two neighbours).
- Use `PS3_AB_SEQ=0,0` (now the default in `ios_phase.sh`). Without it
  the clock also cycles the old phase mask 0–4, which turns some fast
  paths off in 4 of 5 phases (−0.4 to −0.8 flips/s in those phases).
- Noise: an empty switch can still read ±0.3–0.6 flips/s in one run.
  Trust a result only if it repeats.
- Only shows quick effects. Effects through heat need between-run ABBA.

### Phase results (iPhone, thermal 1–2, ~24–25 flips/s)

| run | switch (on = odd phases) | effect |
|---|---|---|
| ph1 | `V242A_SERIAL=1` | −6.2 flips/s (−26%), as on the iPad |
| ph1 | `V242A_NOBOOST` off (bit 1) | +0.45 for "on" = keep default (on) |
| ph2 | `V242A_NAP` | no-op on Apple (needs `SPURS_KICK_APPLE`); read +0.3 = noise |
| ph2 | `V242A_WIDTH` (bit 1) | 0.0 |
| aa1 | nothing (A/A) | −0.02 ± 0.20 |
| ph3 | `V242A_AUDSPIN` | +0.13 ± 0.06 (+0.5%), too small, stays off |
| ph3 | `V242A_LLSPIN` (bit 1) | 0.0 |
| ph4 | main-loop wait (`IOS_PUMP_PHASE=1`) | +0.77 ± 0.13 (+3%) |
| ph5 | same, clean mask | +0.32 ± 0.11 (+1.3%), kept |
| ph6 | MetalFX on (`METALFX_PHASE=1`, e484708) | −0.56 ± 0.08 (−2.4%) vs linear scaling |
| ph7 | `PS3RECOMP_METAL_PASS_TRIM_PHASE=1` | +0.83 ± 0.18 (+2.9%), now default (c3ed3d0) |

### Profile (prof4, 30 s of gameplay, thermal 2)

- Power/heat limited: P cores at their 1.26 GHz floor (~80% busy),
  E cores ~1.8 GHz, ~20 threads waiting for a core. A cool phone
  does ~29 flips/s, a hot one 21–25. Less work per frame = more fps.
- CPU per thread: rsx fifo 70%, SPURS lanes w0 49% + 49%, game main
  32%, audio SPURS kernel 27%, kernel 0 23%, other lanes/kernels
  10–16% each, 3 texture checksum workers 6–7% each, iOS main thread
  ~12%.
- rsx fifo: `sink_end` 58% (20% self), `eng_encode_and_commit_ex` 21%,
  `eng_encode_records` 18%, texture slots 6%, XXH3 3%.
- SPU job code: lifted functions spill registers to `ctx->gpr`
  around calls. Fixing that is a lifter change (not now).
- Spin waits are already short in lean mode. Most CPU time is real
  SPU job work and RSX encoding.

### Fix: iOS main thread no longer polls (20678f5)

- SDL3 has no real event wait on UIKit. `SDL_WaitEventTimeout` pumps
  the run loop and sleeps 1 ms, again and again: ~1000 wake-ups/s,
  ~12% of a core.
- Now: one `SDL_PollEvent`, then wait in the run loop for up to
  `IOS_PUMP_MS` (default 4) ms. Touches and other run-loop sources
  wake it at once. Gamepads are read at 250 Hz instead of 1000 Hz.
- `IOS_PUMP_MS=0` = old loop. `IOS_PUMP_PHASE=0|1` for in-run A/B.
- iOS only (`ios_shell.mm`). macOS and Windows unchanged.

### Power and GPU (pow1/pow2, thermal 2 "Serious")

- CPU (`IOS_CPU_LOG_SECS`): P cores held at 1.26 GHz (112% of 200%
  used), E cores 1.79 GHz (328% of 400%). App total ~412%.
- GPU (`PS3RECOMP_METAL_GPU_STATS=1`): busy 59%, 2 command buffers per
  frame, ~25 ms GPU time per frame. MetalFX 1280x720 -> 2144x1206.
- Power Profiler on a wired phone reads 0 %/hr and no per-process
  split (USB power). Use `inst:Power Profiler,Thermal State` in
  `ios_prof.sh`; the full template fails ("All Processes").
- The phone charges while wired: extra heat during every test.

### GPU trace (gpu1, Metal System Trace, thermal 2)

- iOS holds the GPU at its **minimum** performance state (heat).
  The "induced" flag is not an Xcode device condition: the
  InduceCondition signposts are empty.
- One frame on the GPU: game fragment ~18 ms and vertex ~9 ms
  (overlapping), MetalFX scale 3.0 ms + sharpen 0.9 ms, blits ~2.3 ms.
- ~53 render passes and ~8,300 draws per frame. Per second:
  1,468 Mpx loaded and 1,934 Mpx stored by render passes.
- Lesson: the phone is heat-limited. Less power per frame = higher
  clocks = more fps. Memory traffic is a big part of GPU power.

### PASS_TRIM: skip untouched attachments (c3ed3d0)

- Many passes attach a target they never touch: depth with every
  draw's depth and stencil test off, or colour with every write mask
  empty. Metal still loads and stores it each pass.
- Now such an attachment is a memoryless stand-in of the same format
  and size. The real surface keeps its contents. No visual change
  (Mac frames checked).
- Mac gameplay: load/store traffic −31%. iPhone: +2.9% flips/s.
- Default on for Apple GPUs. `PS3RECOMP_METAL_PASS_TRIM=0` = off.
  Metal file only, so Windows is unaffected.
- GPU stats (`PS3RECOMP_METAL_GPU_STATS=1`) now also print the depth
  share. After PASS_TRIM, depth is still ~42% of the traffic. Most of
  it is real: the same depth is reloaded after a pass break (depth
  snapshot or copy) mid-frame.
- Tried: skip the depth store when the next use is a clear
  (ZSTORE_TRIM). Mac: depth stores −7%, ~1.7% of all traffic. Too
  small to measure on the phone. Dropped.

### Checked, not worth it

- Bigger SDL audio buffer: cellAudio keeps only 4 blocks (1024
  frames) queued, so a 1024-frame device buffer would run dry.
  The audio wake-ups come from cellAudio's own 5.3 ms timer anyway.
- `-mcpu` above apple-a14: no gain on the iPad before.
- Profiles can't be read after a rebuild (symbols move).
  `ios_prof.sh` now keeps the binary that ran as `<run>/app.bin`.

### Colour targets compressed (round 7, between-run ABBA)

- Existing switch `PS3RECOMP_METAL_RT_PRIVATE=1` (colour targets in
  GPU-only memory, so the GPU can compress them).
- iPhone: base 31.3 / 23.7, on 29.0 / 26.2 flips/s. Average 27.5 vs
  27.6: no gain. Stays off.

### Compressed depth (ZPRIV): dropped

- Same idea for depth targets. On the Mac it turned a car grey (wrong
  colour). A split test showed compressed depth itself caused it, not
  the copy path. Code removed, never committed.

### Relaxed shader math (88f4957, round 8, between-run ABBA)

- `PS3RECOMP_METAL_MATH=relaxed|fast`: Metal math mode for guest
  fragment programs. Relaxed lets the compiler reorder and fuse float
  math but keeps NaN and Inf rules, so `x == x` checks still hold.
  Vertex programs always stay safe: depth-equal passes need the same
  bits. Default safe. Metal file only.
- Mac: GPU time −15%, frames look the same.
- iPhone: base 23.2 / 23.0, relaxed 23.6 / 22.6 flips/s (23.1 vs 23.1).
  GPU time per frame 25.2 → 17.9 ms, GPU busy 58% → 41%. No speed gain,
  so it stays off.

### Lesson: the hot iPhone is CPU-bound

- At thermal 2 the GPU is only ~58% busy at 23 flips/s. Cutting GPU work
  (PASS_TRIM traffic, compression, relaxed math) frees GPU time but
  barely moves fps. PASS_TRIM still helped (+2.9%), likely through
  less power and heat.
- The lever now is CPU work per frame: rsx fifo (`sink_end`, encode)
  and SPU job code.

### Two freezes came back (open)

- `rpon2`: 14.1 s at 124 s. `mrel1`: 4.7 s at 60 s. The other 22 iPhone
  runs since 15:00 had none. Both had a GPU-only test switch on, so
  the switch is probably not the cause.
- During the gap: all SPURS workers idle 500 ms with no queue change,
  and one "late pickup ... MISSED" line. Looks like a missed wake-up,
  like the old freezes.

### Start vs now (iPhone, thermal 2 "Serious")

- First test (11-11): 12.6–15.8 flips/s steady (avg 14.2), 52–60 s
  frozen per run, P cores ~2.1 GHz.
- Now (round 8 base): 23.0–23.2 steady (avg 23.1, +63%), no freezes,
  even with P cores held at 1.26 GHz. A cool phone does ~31.

### Next

- Find the cause of the two new freezes.
- Fresh Time Profiler at thermal 2 for CPU hot spots.
- Test over Wi‑Fi: wired, the phone charges, which adds heat. The phone
  is now paired over the local network (`devicectl` transport
  `localNetwork`).

## 2026-11-12 — iPhone: lane workers at higher priority (+8–12%)

Same 330 s script (`ios_ab.sh`, `IOS_DEV=<iPhone udid>`), build `build-ios-pgo`.

### Test

- `SPURS_LANE_QOS`: QoS of the SPURS lane worker threads. iOS default was
  `ut` (utility), picked on the iPad. Tried `in` (user-initiated) and
  `ui` (user-interactive).
- Round 4, ABCCBA (flips/s 150–250 s):

| run | setup | flips/s | thermal |
|---|---|---|---|
| 1 | normal (`ut`) | 28.9 | 0 |
| 2 | `in` | 30.2 | 1 |
| 3 | `ui` | 27.3 | 2 |
| 4 | `ui` | 27.1 | 2 |
| 5 | `in` | 25.8 | 2 |
| 6 | normal | 22.8 | 2 |

- Round 5, ABBAABBA, normal vs `in` (all at thermal 2):
  normal 21.0, 19.9, 19.8, 21.5 (avg 20.6);
  `in` 22.7, 22.4, 23.3, 24.0 (avg 23.1, +12%).
- Every `in` run beat its mirrored normal run in both rounds.
- No freezes and no `[LL-WAIT]` in any run. The only gaps were times
  the app went to the background (`[ios] frame clock parked`).

### Change (074495d)

- iOS default for `SPURS_LANE_QOS`: `in` with 6 or fewer CPUs
  (iPhone), `ut` above (iPad, so the iPad is unchanged). macOS unchanged.
- Check run with the new build, no switches: log says
  `SPURS_LANE_QOS=in`, 25.5 flips/s, no freezes.

### What else we learned

- The phone's state changes a lot. Same build (07:57) in the morning:
  game got ~300% CPU, lock waits of 50+ ms, 18–20 flips/s. In the
  afternoon: ~450–500% CPU, no lock waits, 20–30 flips/s. Probably other
  work on the phone in the morning (not proven). Compare runs only
  within one round.
- Hot, the P cores sit at their floor (1.26 GHz), E cores ~1.8 GHz.
  All 6 cores are busy; 11–21 threads wait for a core.
- The morning profile (`-prof1`) can't be read now: the app was rebuilt
  after it, so the symbols don't match.

### Next

- Fresh gameplay profile with this build: where does SPURS kernel time go
  (real SPU jobs or waiting)?
- Then try fewer SPURS threads / shorter spins on the 6-core phone.
- PGO profile is stale (`spu_coherency.c` changed). Regenerate later.
- A 30 fps cap might keep the phone cooler and steadier.

Runs: `runs/20261112-1[345]*-iphone-r4*`, `-r5*`, `-r6newdef`.
Speed chart page: "inFamous iPhone Speed".

---

## 2026-11-12 — iPhone speed: freezes fixed

Goal from the user: iPhone 16 Pro in the high 30s (iPad is ~50–55).
Same 330 s script as the iPad (`ios_ab.sh`, `IOS_DEV=<iPhone udid>`).
Build: `build-ios-pgo`. The phone is at thermal 2 ("serious") in every run.

### What was wrong

- Most runs froze for 2–55 s: no new frames, audio silent.
- Profile of a freeze: game-main and 7–9 SPURS kernel threads sat 100%
  in `spu_lockline_lock_ea`. The thread that held the lock-line stripe
  (a SPURS lane thread, utility QoS on iOS) got no CPU.
- Cause: the POSIX `ll_acq` in `spu_coherency.c` spun with no pause and
  no yield. 6 cores, all busy spinning, so the holder never ran. The iPad
  (10 cores) has spare cores, so it never froze.
- Windows already had a fix (v100: yield, then Sleep(1)).

### Fix (d09eb0b)

- Apple branch of `ll_acq`: pause, `sched_yield` after 4096 spins, and
  after `SPU_LL_SLEEP_MS` (default 4) a 1 ms sleep every 4th round.
  A sleep lets any thread run; a yield only same or higher priority.
- Waits >= 50 ms print `[LL-WAIT]` with waiter and holder names.
- Windows code unchanged (`_MSC_VER` branch).

### Results (flips/s 150–250 s; frozen = seconds with no frame)

| round | setup | flips/s | frozen s |
|---|---|---|---|
| 1 | base | 17.8 / 17.5 | 53 / 9 |
| 1 | MetalFX off | 19.3 / 15.2 | 12 / 34 |
| 1 | lower screen res (`IOS_VIEW_SCALE=2`) | 15.1 / 18.1 | 22 / 0 |
| 2 | base | 19.9 / (bg) | 0 / – |
| 2 | `V242A_LLSPIN` | 16.8 / 18.2 | 26 / (bg) |
| 2 | `V242A_NAP` | 18.6 / 18.0 | 0 / 0 |
| 2 | audio kernel utility QoS | 18.9 / 19.4 | 88 / 98 |
| 3 | **lock fix** | 20.0 | 0 |
| 3 | fix + `SPU_LL_SLEEP_MS=1` | 18.2 | 0 |
| 3 | fix + `SPURS_KERN0_QOS=ui` | 17.9 | 0 |
| 3 | fix + `SPURS_WKL_THREADS=3` | 17.5 / 19.2 | 0 / 0 |

- Before the fix 10 of 14 runs froze (32 s per run on average).
  After: 0 of 5. Freezes are gone; `[LL-WAIT]` waits now end by ~51 ms.
- Steady speed did not move: ~18–20 flips/s. No switch won.
- Round 3 has one run per side: the phone locked at ~08:30, so the
  mirrored half failed (launch refused while locked).
- Not kept (patches in `$WORK_DIR/patches/`): `IOS_VIEW_SCALE`
  (GPU is only ~25% busy, so fewer pixels gain nothing) and
  `SPURS_KERN0_QOS`. Never use `SPURS_KERN1_QOS=ut`: it causes long
  freezes (88–98 s per run before the fix).
- "(bg)": the app went to the background mid-run; not counted.

### Why the iPhone is slow

- CPU and power bound, not GPU: GPU ~20–35% busy.
- Hot all the time: P cores 1.4–2.1 GHz, E cores ~2.1 GHz. Only 2 P cores
  (iPad: 3–4 at ~1.8 GHz hot).
- ~5–7 threads wait for a core at any time.

### Next

- Lock holders are SPURS lane threads at utility QoS. Test
  `SPURS_LANE_QOS=in` on the iPhone.
- Finish round 3 (the mirrored half) with the phone kept unlocked.
- A 30 fps cap might keep the phone cooler and steadier.
- PGO: `spu_coherency.c` changed, so its profile is stale. Regenerate later.

Runs: `runs/20261112-06*-iphone-r1*`, `-r2*`, `-r3*`, `-stall1`, `-prof1`.
Speed chart page: "inFamous iPhone Speed".

---

## 2026-11-11 — iPhone 16 Pro: first real-device test

One install and test on the user's iPhone 16 Pro (iOS 27.2), with
their OK. Same app as the iPad (`build-ios-pgo`, PGO v5, MetalFX on).
Game folder copied in with devicectl (8.2 GB in 4 min). Same 330 s
script as `ios_ab.sh`. `ios_device.sh` now takes `IOS_DEV=<udid>`.

| run | flips/s (150–250 s) | min free memory | thermal |
|---|---|---|---|
| 1 (cool start) | 15.8 | 4267 MB of 6135 | 0 → 2 within ~1 min |
| 2 (hot) | 12.6 | 4320 MB | 2 the whole run |

- Works: boot, title, gameplay, pause menu, touch pad. No crash, no
  jetsam. MetalFX 1280x720 → 2144x1206 (Metal view 2622x1206).
- 6 cores (2 P + 4 E), so lean mode is on.
- CPU-bound and hot: thermal state 2 ("serious") within a minute.
  P cores only 1.3–1.4 GHz, E cores 2.1 GHz. ~6.6 threads waiting
  for a core. The iPad keeps 3 P cores at ~1.8 GHz.
- Memory is fine: over 4 GB still free.
- Runs: `runs/20261111-192231-iphone-perf1`, `runs/20261111-192853-iphone-perf2`.
- Ideas if the iPhone matters later: fewer SPU threads, or a 30 fps
  cap so the phone stays cooler.

---

## 2026-11-11 — README: iPad/iPhone install and build steps

- README: two ways to get the app (download, or build it yourself),
  then add your game in the app. Speeds updated (iPad ~50–55 fps).
- New "Build it yourself → iPad and iPhone" steps, with two scripts in
  the repo (the old ones live in `$WORK_DIR/tools` with fixed paths):
  - `infamous-recomp/tools/ios_deps.sh [iphoneos|iphonesimulator]`:
    gets the dependency sources with git (pinned tags) and builds them.
    `DEPS_DIR` (default `~/inFamous/deps`).
  - `infamous-recomp/tools/ios_build.sh <team id> [device id]`:
    configures (same flags as `ios_configure.sh`), builds with
    xcodebuild, installs with devicectl when a device id is given.
    `BUNDLE_ID` (default `local.<user>.infamous`), `BUILD_DIR`.
- A self-built app has no `eboot_key.h`, so it only imports a game
  folder that already has `PS3_GAME/USRDIR/EBOOT.elf`.

---

## 2026-11-11 — iPad speed: hop gate, SPU vectors in registers

All iPad numbers: `ios_ab.sh`-style 330 s gameplay runs, flips/s
150–250 s, mirrored order (ABBA / ABCCBA). The iPad heats during a
round, so the second half reads lower. Compare inside a round only.

### Commits

- 13c9205 `-fno-stack-protector` for the runtime on Apple (`PORT_NO_GS`).
- 7c5883b `mfc_for` cache: one thread-local block on Apple (one
  `_tlv_get_addr` call instead of two).
- 107e5fd CMake knobs for the lifted SPU sources (Apple only):
  `PORT_SPU_PGO=OFF` builds them without the PGO profile,
  `PORT_SPU_EXTRA_OPTS` adds flags to them only.
- daebe90 **hop gate**: the SPU hop path tested 3–4 globals per hop.
  Now one word, `g_spu_hop_gate` (bit 0 = XFER slow path, bit 1 =
  ibf off), rebuilt by `spu_hop_gate_sync()` after each writer.
  `-DSPU_NO_HOP_GATE` = old tests.

### Results

Round 2 (ABCDDCBA):

| Build | Run 1 | Run 2 |
|---|---|---|
| lpgate (laneprof test gated) | 50.1 | 46.8 |
| nsp + mfc | 51.1 | 45.9 |
| nsp + mfc, `SPURS_LANE_QOS=0` | 50.4 | 47.3 |
| `V242A_SERIAL=1` | 35.3 | 33.4 |

- All neutral except serial (bad: ~330–350% CPU, clocks go up but
  fps goes down). lpgate not committed.

Round 3 (ABCCBA):

| Build | Run 1 | Run 2 |
|---|---|---|
| nsp + mfc, PGO everywhere | 47.5 | 44.4 |
| hop gate, SPU without PGO | 46.3 | 46.6 |
| old hop tests, SPU without PGO | 43.6 | 43.5 |

- Hop gate **+6.7%** (same PGO setup on both sides).
- SPU PGO is worth ~5.5% (row 1 vs row 3). The profile is stale
  once SPU macros change, so retrain it after each SPU change.
- Mac round 3 is not usable: load went 8 → 22 (Photos, media
  analysis, storage scans).

### Root cause: u128 in GPRs on arm64

- AAPCS64 passes and returns the 16-byte `u128` union as two
  64-bit GPRs (`[2 x i64]`), not as a vector register.
- SROA then splits `u128` locals into GPR halves. Every helper call
  moves data q → x → q (`fmov`, `mov.d`).
- Fix, opt-in: `-DSPU_U128_VECARGS` (Apple arm64 + clang only).
  Helper names become statement-expression macros. They `memcpy`
  `u128` ↔ vector in the caller, around vector-typed bodies
  (`spu__w_*`). Bodies are unchanged; x86 never sees it.
- Bit-exact: new `tests/test_spu_vecargs.c`, 24.37M checks
  (every immediate, every shift count per lane, LS read/write),
  0 differ, with and without the flag.
- Hot SPU file (`jobmod_50f371dc382d_e3D90`), asm counts:

| | off | on |
|---|---|---|
| `fmov x,d` | 360 | 29 |
| `mov.d` | 205 | 36 |
| GPR spills (ldr / str) | 889 / 460 | 622 / 389 |
| vector spills | 987 | 1029 |
| instructions | 34400 | 32854 (−4.5%) |

- The remaining vector spills are real register pressure: hot SPU
  loops keep 50+ values live; arm64 has 32 q registers.

Round 4 (ABBA, SPU without PGO on both):

| | hop gate | + vecargs |
|---|---|---|
| iPad flips/s | 47.4 / 46.9 | **48.3 / 49.1** (+3.3%) |
| iPad RSX thread busy | 87% | 90–93% |
| Mac flips/s (load 12–16) | 74.9 / 79.2 | 79.3 / 79.6 (same) |

- ef0540f header + test, f5055f0 `PORT_SPU_VECARGS` (CMake, Apple,
  default ON). x86 / Windows preprocessed output unchanged.
- The RSX thread is close to the limit again on the iPad.

Round 5 (ABBA): PGO profile v5 everywhere, retrained with vectors in
registers (`$WORK_DIR/pgo/apple-v5.profdata`, not in git).

| | vecargs, SPU without PGO | + PGO v5 on SPU too |
|---|---|---|
| iPad flips/s | 54.4 / 47.7 | **55.6 / 51.9** (+5.3%) |
| iPad RSX thread busy | 90–96% | 95% |
| Mac flips/s (load 11 → 22) | 80.1 / 71.5 | 81.6 / 87.9 |

- PGO v5 is now the setup in `build-ios-pgo` and `build-mac-pgo`
  (`PORT_SPU_PGO=ON`, `PORT_PGO_PROFILE=.../apple-v5.profdata`).
- Mac numbers are not reliable (Photos and storage scans running).

Round 6 (ABBA): Metal encoding on its own thread
(`PS3RECOMP_METAL_ENC_THREAD=1`). Not kept.

| | off | on |
|---|---|---|
| iPad flips/s | 54.2 / 55.6 | 54.1 / 55.0 (−0.6%) |
| iPad RSX thread busy | 93–94% | 82–85% |
| iPad CPU total | 632–638% | 673–680% |

- The RSX thread gets ~10% free time, but speed stays the same.
  So something else also limits at ~55 flips/s (GPU, or the
  PPU/SPU side). Keep it off.

Round 7 (ABBA): geometry memo prefetch hints
(`PS3RECOMP_ENGINE_GEO_PF`, Apple only). Not kept.

| | off | on |
|---|---|---|
| iPad flips/s (redo) | 54.9 / 53.0 | 53.1 / 53.1 (−1.6%) |
| iPad RSX thread busy | 96% | 94% |

- On the Mac it cut `sink_end` from 22.9% to 17.4% of the RSX thread.
- **Not a real test.** The A/B script wants `<dir>/Release-iphoneos/inFamousRecomp.app`.
  The new app was copied without that folder, the install failed silently,
  and both sides ran the round 6 app. Fixed: the script now stops when the
  app is missing or the install fails, and prints the installed UUID.
- Useful anyway: the same app 4 times gave 54.3–55.7 flips/s. That is the noise.
- Redo with the right app: no gain. Dropped (patch kept outside the repo).

Round 8 (diagnostic, one run each): why ~55 flips/s?
New switch `IOS_RUNSTATE=1` (commit d417426): every 1 ms it checks if
a thread is running, ready but waiting for a core, or blocked.

| gameplay | encode thread off | on |
|---|---|---|
| flips/s | 55.6 | 54.5 |
| game-main on CPU | 42% | 45% |
| game-main ready but not running | 25% | 30% |
| game-main blocked | 33% | 26% |
| RSX thread on CPU (never waits) | 96% | 83% |
| RSX: game PUT → drained | 17.2 of 17.7 ms | 15.0 of 18.2 ms |

- Off: the RSX thread is behind for the whole frame.
- On: RSX is done 3 ms early, but the game thread now limits.
- **Correction (after round 9):** "ready but not running" is not
  core contention. The game waits for SPU jobs in a `sched_yield`
  loop (short sleeps, `sys_timer.c`), and a yielding thread counts as
  ready. So most of that 25–30% is waiting on SPU jobs.

Round 9 (screen, one run each): SPURS kernel threads to utility QoS.

| run | flips/s | P cores | P clock |
|---|---|---|---|
| A1 default | 55.7 | 272% | 1.89 GHz |
| B1 `SPURS_KERN_QOS=ut` | 52.8 | 151% | 2.34 GHz |
| C1 + `SPURS_KERN1_QOS=ut` | 51.3 | | |
| A2 default | 55.2 | 270% | 1.85 GHz |

- Fast cores freed and clocked up, but the SPU work on slow cores
  got slower, and the game waits on it. Not kept.
- The game thread still showed ~31% "ready" with the P cores half
  idle: that proved the metric is the yield loop, not contention.
- The iPad's P cores run at only ~1.9 GHz under this load (power
  limit). The M1 Max keeps ~3.2 GHz on 8 P cores. That gap, not the
  core count alone, is the most likely reason the M1 Max is faster.
- Mac reference, same pad script (this Mac, M5 Pro, PGO build,
  headless): 115 flips/s in gameplay (150–250 s).
- In gameplay the iPad GPU shows ~100% busy (`PS3RECOMP_METAL_GPU_STATS=1`,
  round 8 run: ~2 command buffers per frame, ~9 ms each). Earlier
  `METALFX=0` halved GPU busy at 28 fps; now at 55 fps it may be the limit.
- Next (round 10): MetalFX off, plus free P-core time without slowing the main SPU
  work: audio kernel only to E cores, shorter audio spin, and
  parking job waits sooner.

Round 10 (screen, one run each except A and M):

| run | flips/s | GPU busy |
|---|---|---|
| A default | 53.8 / 52.2 | 101% / 97% |
| M `METALFX=0` | 54.9 / 54.8 | 82% |
| B `SPURS_KERN1_QOS=ut` (audio to E) | 53.8 | 100% |
| C `V242A_AUDSPIN=1` (audio spins 50 us) | 52.3 | |
| D `PS3_SHORT_SLEEP_PARK=50:20` | 52.3 | |

- MetalFX costs ~1.5 ms of GPU per frame and ~3.5% speed. It is
  already a switch (`METALFX=0`). The user picked (11-11): **on** stays
  the default on iPad/iPhone. Sharper picture over ~2 fps.
- Audio and job-wait changes: no gain.
- The RSX thread is still 93–97% busy. The GPU sum is near 100%,
  but buffers can overlap, so the real GPU load is not known yet.
- New: `PS3RECOMP_METAL_ZFB_CLEAR` (default 1). Passes with no guest
  zeta clear the shared depth instead of load + store, as D3D12 does.
  Same picture on the Mac. Only ~2% of passes, so a small effect.

Round 11 (ABBA, `PS3RECOMP_METAL_GPU_STATS=1`, MetalFX on):

| run | flips/s | GPU busy |
|---|---|---|
| A1 `ZFB_CLEAR=0` | 51.9 | 99% |
| B1 `ZFB_CLEAR=1` | 52.2 | 98% |
| B2 `ZFB_CLEAR=1` | 51.7 | |
| A2 `ZFB_CLEAR=0` | 49.5 | |

- +2.5% on average, but the pairs say +0.6% and +4.4%.
  A2 looks like a slow outlier. Call it noise.
- Kept (default on): same as D3D12, same picture, not slower.
- All four runs are 2-3 flips/s under round 10. The GPU stats
  add some cost; the iPad may also have been warmer.

Round 12 (ABBA, `METALFX=0`, GPU stats + `RFBOUND=1` + `IOS_RUNSTATE=1`):

| run | flips/s | RSX thread | GPU busy (overlap once) |
|---|---|---|---|
| M1 encode on RSX thread | 52.8 | 96% | 68–72% |
| E1 `PS3RECOMP_METAL_ENC_THREAD=1` | 51.6 | 79% | 68–70% |
| E2 `PS3RECOMP_METAL_ENC_THREAD=1` | 52.3 | 81% | |
| M2 encode on RSX thread | 52.9 | 96% | |

- Encode thread: -1.7%. No gain again, even with GPU room.
- With MetalFX off the real GPU load is ~70%. The GPU is not
  the limit.
- With the encode thread on, the RSX FIFO walk gets slower
  (~10-11 -> ~13.6 ms per frame). The 3 fast cores are full
  (P 268-275% at 1.80 GHz, power-limited). Adding a thread
  only moves work around.
- So the iPad is CPU-bound on its 3 fast cores. The M1 Max
  has 8 fast cores at ~3.2 GHz, which is why it is faster.
- `RFBOUND-mtl` "main fence wait" reads 0 on Apple: that part
  of the report is not wired up there. The walk numbers work.

### Notes

- `test_spu_helpers_v130_neon` with `-ffp-contract=off` shows 203,829
  diffs with the old header too. Pre-existing, not from this work.
  (The lifted SPU sources use clang's default contract by design.)

---

## 2026-11-11 — iPad speed: short sleeps, NEON, RSX cuts

### Why

- The user's M1 Max and even a plain M1 MacBook beat the M4 iPad Pro.

### Found

- **Fire and effects** were the GPU cost. The Metal engine ignored
  `FEEDBACK_RUN` (D3D12 honours it). Fixed in 76ff9e4.
- After that the iPad is **CPU-bound**, not GPU-bound.
- **Short sleeps.** The guest main thread polls its job counters with
  `sys_timer_usleep(5)` (lr 0x00116824, ~30k/s) and `usleep(1)`
  (lr 0x000F2A74). On POSIX each was a `nanosleep`: kernel timer, two
  context switches, late wake-up. On the iPad that was ~6.4 s of 20 s
  main-thread CPU, inside the kernel.
- Windows never sleeps for these: under 1 ms it waits on the clock
  (QPC + `SwitchToThread` with `PS3_SHORT_SLEEP_SPIN=0`, the launch
  default; PAUSE spin up to 50 µs otherwise).

### Changed

- `sys_timer.c` (Apple): same split as Windows. Under 1 ms:
  `PS3_SHORT_SLEEP_SPIN=0` → `sched_yield` until the deadline;
  unset or 1 → `isb` spin up to 50 µs. `PS3_SHORT_SLEEP_NANO=1` brings
  back `nanosleep`. Logs one `[timer]` line at start.
- One rule for macOS and iOS, as on Windows (commit 20db509).
- Other commits this round:
  - c5f49a2 inline guest loads/stores in lifted PPU code (Apple).
  - 0f933df `PORT_PPU_MEMLEAN` = ALL on Apple.
  - 2da9260 `PORT_PGO=GEN` / `USE` on Apple.
  - bde4597 `spu_fi` on arm64 without libm `ldexpf`.
  - 4c0e3a2, c206170 name the PPU, SPU, RSX and SPURS host threads.
  - fc95af9 iOS CPU diagnostics (core split, clock probe, stress test).
  - 2b4e98c `SPURS_KERN_QOS` test switch, 4e32db2 Metal opt-ins.
  - 51adbeb iOS app marked as a game (Game Mode).
  - 11a016d cellPad: touch-pad hook links in builds without SDL3.
  - b260727 program memo, 9070eba partial vertex constants.
  - 6f9c267 Apple QoS fix (see "Tried, no gain").
  - 72e8460 SPU rc vector helpers on arm64 through sse2neon.
  - 31ed304 SPU float core and v130 helpers in native NEON.
  - 053e042 SPU `frest` / `frsqest` with NEON TBL.

### NEON SPU helpers (the big one)

- On arm64 the SPU job code used the scalar helpers: every float op
  went lane by lane, with a clamp per operand. x86 has had SSE4.1 /
  AVX2 forms for a long time.
- Now arm64 gets the same vector helpers: integer ops through
  sse2neon, and the float core (`fa`, `fm`, `fma`, compares,
  conversions, `fi`, bit shifts, `frest`) in native NEON.
- Bit-exact: `test_spu_helpers_vec` 24.44M checks and
  `test_spu_helpers_v130_neon` 3.26G checks (every rounding mode, FZ
  on/off, all 2^32 inputs for the clamp and `frest`), 0 differ.
- x86 output unchanged (same preprocessed source).
- Hot job d466: code 70% smaller.

| | old | NEON |
|---|---|---|
| iPad, hot ABBA (flips/s) | 32.1 / 28.6 | **38.4 / 37.3** |
| iPad total CPU | 646–661% | 597–599% |
| iPad RSX thread busy | 71–75% | **99%** |
| Mac (non-PGO, flips/s) | 60.6 | 65.0 |

- iPad **+18–25%**. The RSX thread is now the limit on the iPad.
- This iOS build had its SPU code un-PGO'd (profile from the old code).

### Results

iPad, gameplay, `ios_ab.sh` (flips/s 150–250 s), one build:

| short sleeps | run 1 | run 2 | mean |
|---|---|---|---|
| clock + yield (launch default) | 27.9 | 26.3 | **27.1** |
| spin ≤50 µs (`SPIN=1`) | 26.1 | 25.0 | 25.6 |
| old `nanosleep` (`NANO=1`) | 23.8 | 24.2 | 24.0 |

- Yield: **+13%** over the old sleep.
- Mac gameplay (same script, headless): yield 55.7 / 54.5, nanosleep
  52.1 / 55.2 flips/s. About +3%.
- Mac static scene (`ab_mac.sh`): yield 49.1 / 53.5, nanosleep
  51.5 / 56.2 flips/s. Yield is ~5% slower there (the RSX thread is the
  limit on the Mac, and yield takes CPU from it: 725% vs 677%).

Other gains this round:

| change | iPad | Mac |
|---|---|---|
| inline loads/stores | +5% | −7% CPU per frame |
| MEMLEAN = ALL | 0 | +8% |
| PGO (first profile) | +6% | — |
| PGO (retrained, MEMLEAN + inline) | +12% (30.8 / 29.9 vs 27.3 / 27.0) | — |
| PGO v3 (retrained on memo + VCB, new sleeps) | — | +9% (62.9 / 57.2 vs 54.2 / 55.9, Mac load 15–22) |
| program memo (b260727) | +6% (27.6 / 28.3 vs 26.5 / 26.4) | scans gone from profile |
| partial vertex constants (9070eba) | +6.5% hot, ABBA (27.7 / 28.0 vs 25.8 / 26.5) | ~6.5% of RSX thread was the copy |

Tried, no gain (opt-in only or dropped):

- `ENC_THREAD=1` (Metal encoder thread): iPad RSX thread 85% → 59%
  busy, same fps; Mac slower. With the PGO build it loses on the iPad:
  29.3 (and 26.6 with `ENC_QOS=ut`) vs 31.7 flips/s. Stays opt-in.
- `RT_PRIVATE=1` (private colour targets): GPU 22% → 20%, same fps.
- `SPURS_KERN_QOS`: no change.
- `METALFX=0` (no upscaling): GPU busy 43–52% → 22%, but no gain in
  flips/s or CPU clock (hot: 28.1 / P 1.67 GHz vs 29.1 / P 1.74 GHz
  with MetalFX). The GPU is not what heats the iPad. MetalFX stays on.
- Frame workgroup (`os_workgroup` on the game threads): no change,
  removed before commit.
- RSX thread at real UI QoS. Bug found: the shim set the sched
  priority first, which opts a thread out of QoS for good, so
  `RSX_QOS` never worked (iPad thread showed "--"). Fixed in 6f9c267.
  With the fix, iPad hot ABBA: 27.8 / 29.0 vs 28.6 / 27.6 flips/s
  (no real change). Mac `RSX_QOS=1`: 60.7 / 61.1 vs 62.7 / 61.9, so
  it stays off on macOS.

iPad clocks (stress test): 3 busy threads run at 3.54 GHz; 9 busy
threads drop to P 2.75 / E 2.81 GHz. More threads busy = slower cores.

### The iPad slows down as it warms up

- Cold (after 10 min idle) the iPad runs ~32 flips/s; after ~20 min of
  play it settles at ~27. Thermal state stays 0 ("nominal") the whole
  time. Each run is flat inside; the level drops between runs.
- `ios_ab.sh` keeps the game running between sides, so the first side
  of an ABAB set is cold and wins. Use ABBA, in the hot state.
- Hot state (`IOS_CPU_LOG_SECS=10`): P cores at **1.5 GHz** (max 3.54),
  E cores 2.16 GHz, ~675% CPU, ~5.5 threads waiting for a core. RSX
  thread 84% busy.
- The SPURS threads (~400% CPU) are real SPU job code, not spinning
  (iPad profile: 2–3% in yield/kernel). So the iPad is power-capped by
  real work. A Mac with a fan does not drop like this.
- What helps: less work per frame on any thread (power is shared), not
  only on the RSX thread.


### RSX thread cuts (11-11)

- After NEON the iPad RSX thread is 98–99% busy. It sets the pace.
- 0de5ea3: plain textures skip the render-target scans (bloom
  filter of target addresses); CONTROL0 enable bit checked first.
- 77398a3: no ARC retain/release in `eng_encode_draw`.
- c488cc3: GCM FIFO fast path in engine mode. Pure state words
  (texture units, transform program, vertex format, CONTROL3) are
  stored inline; the rest goes through `rsx_process_method` as
  before. `GCM_ENG_FAST=0` off, `=1` no inline words. Exactness:
  offline test of every inline word vs the handler, 0 differ.
  Frames match the baseline (same black fades).

| | old | new |
|---|---|---|
| Mac, RSX cuts (static scene) | 64.5 / 63.1 | **67.9 / 68.3** (+6.5%) |
| Mac, FIFO cut alone | 71.0 / 69.3 | 71.0 / 71.5 (~0) |
| iPad, all cuts, hot ABBA | 38.1 / 37.0 | **40.6 / 40.4** (+8%) |

- iPad warm side: 43.5 flips/s.

### PGO v4 (11-11)

- Profile `$WORK_DIR/pgo/apple-v4.profdata`, trained on the NEON code.
- iPad hot ABBA: 36.7 / 36.5 vs 37.5 / 35.8 (no-PGO NEON). No gain:
  the RSX thread is the limit and SPU PGO does not help it.
- Mac static scene: 78.0 / 76.2 vs 75.8 / 73.6 flips/s (+3%,
  Mac load 11–33 from other work).

### Parallel hashes: TEXCS + GEOCS (11-11)

- Idea: the RSX thread is the limit on the iPad. Move its hashing to
  3 worker threads (utility QoS). The RSX thread only checks results.
- 6927e13 `TEX_CSUM_MLP`: texture checksum with 8 loads in flight.
  iPad +1% (40.8 / 40.6 vs 40.1 / 40.5), Mac same.
- 7ac3303 **TEXCS**: workers checksum the textures the last frame used.
  - A worker value counts only if it was made after the last change to
    guest memory the RSX thread can see (put read, NV406E acquire, RSX
    2D write), in the same frame, for the same inputs.
  - Textures that changed in the last 16 frames get no job.
  - `PS3RECOMP_ENGINE_TEXCS=0` off, `_THREADS=<n>` (default 3),
    `_VERIFY=1`, `_STATS=1`.
- c94c571 **GEOCS**: same workers, same rule, for the geometry
  memo's vertex hash.
  - Jobs = last frame's probe range lists, one job per distinct list.
  - A probe takes a value only if pointer, start, length and location of
    every range match exactly.
  - A probe that finds its job not started does it inline and leaves the
    value for later probes of the same ranges.
  - `PS3RECOMP_ENGINE_GEOCS=0` off, `_VERIFY=1`, `_STATS=1`.
  - Verify (Mac): 33 windows, ~24M values taken, 0 differ. Inline
    hashing 15.5 → ~3.5 MB per frame (title), 5.9 MB at 8700 draws.

| | off | on |
|---|---|---|
| TEXCS, Mac static scene | 75.1 / 72.1 | **82.5 / 80.6** (+11%) |
| TEXCS, iPad hot BAAB | 40.6 / 40.2 | **42.1 / 42.1** (+4%) |
| GEOCS, Mac static scene | 82.9 / 82.7 | 82.4 / 84.5 (same) |
| GEOCS, iPad cool pair | 48.6 | 49.0 |
| GEOCS, iPad hot pair | 41.4 | 42.3 |
| GEOCS, iPad hot BAAB | 40.3 / 39.6 | **41.2 / 40.2** (+2%) |

- GEOCS: iPad RSX thread 86–93% busy vs 95–96%.
- The Mac at ~83 flips/s is no longer RSX-bound: GEOCS gives it nothing.

### Texture pool (11-11)

- Depth snapshots, depth colour copies and feedback copies were new
  Metal textures every time (~14 a frame). AGX texture init + dealloc
  was ~3.7% of the Mac RSX thread.
- 835e6a5: they now come from a small pool (64 textures, 192 MB,
  dropped after 120 frames unused). Only textures the GPU writes
  whole (full-screen resolve, full blit). Never colour targets or
  CPU-filled textures.
- A pooled texture is handed out again only after the last commit that
  used it is done on the GPU. (Taking one the GPU still reads is
  correct, but makes the new pass wait for the old frame: Mac -2%.)
- Mac check: ~7800 hits, 0–3 new per 600 frames; holds 5–7 textures
  (19–27 MB). Frames look right.
- `PS3RECOMP_METAL_TEX_POOL=0` off, `=2` logs hits. Off with ENC_THREAD.

| | off | on |
|---|---|---|
| Mac static scene | 77.1 / 75.3 | 76.7 / 76.7 (same) |
| iPad hot BAAB | 39.6 / 38.6 | **39.7 / 39.3** (+1%) |

- iPad total CPU 668 / 671% → 647 / 653%.
- This iPad was hotter than in the GEOCS test (39 vs 41 hot).

### Thread-local cuts (11-11)

- On Apple every `__thread` read is a call (`_tlv_get_addr`).
  It was 2.3% of iPad CPU (shown as libdyld in profiles).
- 4336c81: the SPU call fast path read 6 thread-locals per call.
  Now one block (`g_spu_hot_tls`: trampoline + IRQ count) = 2 reads.
  `spu_img_restore` is inline on Apple.
- Windows: unchanged (same names, same code).

| | old | new |
|---|---|---|
| Mac static scene, BAAB | 87.0 / 85.8 | **89.3 / 88.4** (+2.7%) |
| iPad, ABBA | 50.9 / 51.2 | 51.3 / 50.9 (same) |

### Short sleeps on Apple: modes tried (11-11)

- iPad hot ABCCBA (flips/s):
  - yield loop (default): 38.8 / 42.9
  - isb spin (`PS3_SHORT_SLEEP_SPIN=1`): 39.6 / 38.8
  - nanosleep (`PS3_SHORT_SLEEP_NANO=1`): 36.1 / 36.0 (−7%)
- WFE wait (tried, dropped): the thread never blocks, so the OS keeps
  a P core for it. game-main 51 → 96% busy, RSX thread 88 → 71%,
  46.3 / 48.7 vs 50.9 flips/s. Diff kept in
  `$WORK_DIR/runs/wfe-sleep-tried.diff`.
- Default stays the yield loop.

### SPURS kick on Apple (11-11)

- Windows wakes a parked SPURS kernel thread when new work comes
  (`ps3_spurs_kick`, a per-thread event). On Apple this was compiled
  out: an idle kernel only saw new work when its 250 µs nap ran out.
- Now: mutex + condition variable per kernel thread (iOS 16 has no
  `os_sync_wait_on_address`). Same protocol as Windows.
- Off by default on Apple: no gain on the iPad. `SPURS_KICK_APPLE=1` = on.
- iPad without the ReadyCountStore kick (`V221A_RCS=0`): 51.2 / 51.3.
- Mac: 12 threads registered, no hang.

| | off | on |
|---|---|---|
| Mac static scene, ABBA | 90.2 / 80.7 | 87.4 / 88.3 (noisy, load 15–20) |
| iPad, ABBA | 51.8 / 50.8 | 49.5 / 50.7 |

### Other findings

- memmove (2.2% of iPad CPU) is real SPU DMA (`mfc_do_transfer`).
- PGO builds have ~200k outlined code stubs (no-PGO: 577). On the
  iPad they are only 0.67% of CPU. Not worth chasing.

### Park long waits (11-11)

- The main thread polls job counters with `sys_timer_usleep(5)`
  (lr 0x00116824). `PS3_SHORT_SLEEP_STATS=1` logs how long those
  waits last (every 10 s). Most last 0.5–10 ms; ~70–80% of the
  main thread's time is waiting.
- Before: every sleep was a yield loop, so a long wait kept a core busy.
- Now (Apple default): a wait older than 200 µs sleeps for real in
  50 µs steps (`nanosleep`). Short waits keep the yield loop.
- `PS3_SHORT_SLEEP_PARK=<after_us>[:<sleep_us>]`, `=0` = off.
- Windows: unchanged (Apple-only block).

| | off | 200:50 | 100:20 |
|---|---|---|---|
| Mac static scene, ABBA | 82.9 / 81.4 | **83.0 / 84.5** | 78.2 / 80.9 (vs 200:50: 74.0 / 81.3, load 22–31) |
| Mac CPU | 747% / 729% | **701% / 694%** | |
| iPad, ABCCBA | 49.3 / 51.5 | 51.0 / 50.3 | 49.4 / 50.1 |
| iPad CPU | 657% / 671% | **642% / 636%** | 627% / 634% |

- Mac +2.3% flips/s, −6% CPU. iPad same flips/s, −3..5% CPU.
- Without park the iPad main thread had ~310 waits/10 s over 10 ms
  (3.4 s of every 10 s). With park those are gone.

### Tried, no gain

- Faster clock in the Win32 shim (`QueryPerformanceCounter`,
  `GetTickCount64`). macOS `clock_gettime(CLOCK_MONOTONIC)` is 17 ns
  vs 7 ns raw, and 11% of a Mac SPURS kernel thread. But on the iPad
  it is ~0.2%, and the raw clock is 10.5 s off the monotonic one, so
  mixing them would break timers. Dropped.

### Mac profile recipe

- `$WORK_DIR/tools/prof_mac_xc.sh <tag> <at> <dur>` → `prof.trace`.
- Export: `xcrun xctrace export --input prof.trace --xpath
  '/trace-toc/run/data/table[@schema="time-profile"]' > tp.xml`.
- Scripts in `$WORK_DIR/tools` (python3.12): `tsym.py tp.xml <thread>
  <binary>` (leaf + inclusive), `tcallee.py` (callees of a function),
  `tcaller2.py` (caller chains), `tline2.py` (by source line; needs
  DWARF from a `-g` build), `tleafbin.py` (leaf share by library;
  works on iPad traces too).
- iPad traces: keep a copy of the app binary from the profiled build.
  App frames are offsets; `atos` needs that exact binary.

### Next

- Mac RSX thread now (profile 11-11, after pool, inclusive):
  `sink_end` 69% (leaf 11%), Metal encode 18% (`drawIndexedPrimitives`
  6.5%, new render encoders 2.8%), FIFO walk leaf 9%,
  `rsx_dispatch_method` 5%, vertex fetch plan 5%.
- Main-thread wait: done (park, 05e2575). Spin, WFE, nanosleep and
  the SPURS kick were tried, see above.
- SPU jobs: 3 lanes + kernel 0/11 ≈ 1.8 cores of real job work
  (`jobmod_d466…`, `jobmod_153e…`). Lifted code stores and reloads
  many registers around every call. Fewer instructions = less power.

## 2026-11-10 — iOS: "too old" screen for chips before A14

### Result

- The iOS build uses `-mcpu=apple-a14` (PORT_IOS_CPU), but the app
  allows iOS 16, which still runs on A11–A13. Those devices could install
  it and crash. Info.plist can't ask for an A14.
- `ios/ios_shell.mm` now checks `hw.optional.arm.FEAT_FRINTTS` and
  `FEAT_FlagM2` (A14+ only) at start. If either is 0 it shows a
  "too old" screen (iPad: lists the first A14/M1 iPads; iPhone: "iPhone
  12 or newer") and never starts the game. A missing sysctl doesn't block.
- On only when PORT_IOS_CPU is apple-a14+ (`PORT_IOS_CPU_CHECK`, set on
  ios_shell.mm only, so the lifted files don't rebuild).
- Test switch: `INFAMOUS_CPU_TEST=old` fakes an old chip.
- README: real device list for iPad and iPhone.
- Scan of today's device binary (27.5 M instructions): no A14-only
  instructions (frint32/64, axflag, xaflag) yet; it does use LSE (925)
  and LDAPR (87), so A11 would crash for sure. The check keeps the line
  at A14 so a later build can't crash on A12/A13.

### Checks

| test | result |
|---|---|
| Simulator iPad, `INFAMOUS_CPU_TEST=old` | "too old" screen, game not started |
| Simulator iPad, normal | no cpu line, game starts |
| real iPad (M4), normal, 60 s | no cpu line, game starts, 6.4 GB free |

- Xcode trap: after a CMake change, the first `xcodebuild` regenerates
  the project mid-build and compiles with the old flags. Build twice (or
  re-run ios_configure.sh first).

---

## 2026-11-09 — Mac keyboard + mouse controls

### Result

A Mac player with no controller can play with keyboard and mouse.

- **Same pad as Windows:** `cellPad.c` now runs the Windows v150
  keyboard + mouse pad on macOS too (bindings, direct mouse look, map,
  menu keys, merge with a controller).
- **Input comes from the app:** `mac/mac_kbm.mm` uses AppKit local event
  monitors and reports keys, mouse, wheel and focus through
  `ps3recomp/libs/input/pad_host_kbm.h` (Windows key codes).
- **Pointer:** hidden and held while the game wants mouse look. Esc or
  Option frees it, a click grabs it again. Cmd shortcuts still work.
- **Controller wins:** a connected pad owns port 0, the keys merge in,
  the sticks follow the keyboard/mouse only while they move.
- **Windows is unchanged:** `unifdef` with `_WIN32` gives the same
  `cellPad.c` apart from one comment. iOS keeps its touch pad.
- Switches: `PAD_KBM=0` or `PAD_NO_KEYBOARD=1` = off.
  `INFAMOUS_KBM_TEST=1` = accept input posted to the process in the
  background and never hold the real pointer (for tests).
- Test tool: `$WORK_DIR/tools/rmouse <pid> move <dx> <dy>` or
  `click <button>` (postToPid, like `rkey`).

### Tests (build-mac, windowed, `INFAMOUS_KBM_TEST=1`, no controller)

| step | input | result |
|---|---|---|
| title | Enter | new game starts (cutscene) |
| tutorial "look at the helicopter" | mouse up | camera looks up, tutorial moves on |
| tutorial "look left" | mouse left | camera turns, tutorial moves on |
| gameplay | W held | Cole walks forward |
| gameplay | Esc | pause menu opens |
| pause menu | Down arrow, S, S | cursor moves down 3 items |
| pause menu | Backspace | menu closes |

- `[mousediag]` shows the mouse writes every frame (x about 1.2-1.6).
- Runs: `runs/20261109-102447-kbm`, `-103012-kbm2`, `-103507-kbm3`
  (frames deleted).
- At the first tutorial step the real right stick barely turns the
  camera either; that is the game, not the mouse.

### Open

- Not tested by me: real pointer hold, real key focus, and a real
  controller plugged in with keys (all need a person at the Mac).
- Mouse speed: NSEvent deltas are accelerated points, not raw counts.
  Tune with `PAD_MOUSE_SPEED` (default 450) or `PAD_MOUSE_SENS`.
  Raw input via GCMouse could follow.
- Key prompts still show controller icons (the keyboard icon set,
  `kbm_prompts`, is not built on Mac).
- The Mac release build (SDL3 pads): `cellPad.c` compiles with
  `PS3RECOMP_PAD_USE_SDL3`, but that build and the `.app` were not run.

---

## 2026-11-09 — End-user release: in-app disc import

### Result

A player downloads the app, picks their own disc, and plays. No dev
tools, no RPCS3, no terminal.

- **Importer** `infamous-recomp/import/disc_import.c` (Apple only):
  - reads a plain ISO, an encrypted ISO (+ `.dkey`/`.key` next to it, or
    asks for one), or a game folder;
  - checks the title id and the EBOOT.BIN hash, decrypts the SELF with
    the embedded metadata key, checks the EBOOT.elf hash;
  - clones files on the same APFS volume, copies otherwise;
  - checks free space first. It counts purgeable space too
    (`kCFURLVolumeAvailableCapacityForImportantUsageKey`): on the test
    iPad statfs said 6.5 GB free while iOS could make 142 GB free, so the
    old check would have refused every import there.
- The key header `import/eboot_key.h` is git-ignored. A build without it
  still runs; the importer then only takes a folder that has EBOOT.elf.
  `import/import_cli.c make-key` makes the header (needs the APP key).
- **Mac app** `mac/mac_app.mm` + `tools/make_mac_app.sh`:
  - data in `~/Library/Application Support/inFamous`
    (`INFAMOUS_DATA_DIR` overrides);
  - first-run import window when no game is found;
  - test switches `INFAMOUS_IMPORT_TEST=<src>`, `INFAMOUS_UI_SNAPSHOT=<dir>`.
- **Mac threading fix:** the Metal window code does `dispatch_sync` to
  the main queue, so in windowed mode the guest now runs on a 64 MB
  "game-main" thread and the main thread runs `[NSApp run]`
  (`port_mac_run_game`). Headless runs and Windows are unchanged.
- **macOS release build** `build-mac-release`: static deps for macOS 13
  (`$WORK_DIR/tools/build_mac_deps.sh` → `$WORK_DIR/deps/macosx`),
  `-DPS3RECOMP_MAC_SDL3=ON` (static SDL3). Links system libs only.
- **iPad import screen** `ios/ios_import.mm`: same flow, document picker,
  data in Documents (Files → On My iPad → inFamous).
  - test switches (launch env): `INFAMOUS_DATA_DIR=<dir>` keeps the game
    folder there instead of Documents (relative = inside Documents);
    `INFAMOUS_IMPORT_TEST=<src>` imports that file or folder without
    the picker (relative = inside Documents).
  - device test recipe: copy the ISO in with `devicectl device copy to
    ... --destination Documents/imptest-src/inFamous.iso`, then
    `ios_device.sh run 330 ipad-import INFAMOUS_DATA_DIR=imptest
    INFAMOUS_IMPORT_TEST=imptest-src/inFamous.iso GCM_FLIPCOUNT=1`.
- **Release script** `tools/release_apple.sh [ver] [out]`:
  - Mac zip (ad-hoc signed, not notarized);
  - .ipa: ad-hoc signed, keeps only the two kernel entitlements,
    no provisioning profile (it lists team and device ids);
  - stops if any bundle has game-like files or a big non-program file;
  - writes THIRD_PARTY_NOTICES.txt and SHA256SUMS.txt.
- README: download and install steps first, "Build it yourself" after.

### Checks

| test | result |
|---|---|
| Mac release zip, unzipped fresh, real ISO from the T7 | import ok, window open, 12450 flips at 290 s (timeout), no crash, audio real time |
| Mac dev app, windowed | 3520 flips at 74 s, audio real time |
| Simulator (iPad), folder import via the picker | imported to Documents, reached the title screen |
| **real iPad**, real ISO (copied into the app), test switch | import screen showed progress, import ok in about 30 s, booted from the new copy, title screen, 7450 flips at 305 s, no crash |
| iPad device build, game already in Documents | skips the import screen, boots |
| release bundles | no game files; Mac exe links system libs only, minos 13.0 |
| Mac release zip remade 12:43 with keyboard + mouse, game already imported | "keyboard + mouse on", window open, ran 3 h 23 min (see note), no crash, audio median real time (187.5 blocks/s) |

- Release files remade 2026-11-10 12:54 (keyboard + mouse, CPU check):
  mac.zip sha256 67b52ac3…, ipa 9fa57dc9….
- Note: that last run used `timeout 150` without `-k`. The port ignores
  SIGTERM, so it ran until killed at 16:09. Always use `timeout -k 10`.

- Real iPad run: `runs/20261109-101719-ipad-import`.
- An earlier iPad run (20261109-095824-import-build) showed the app
  switcher the whole time (the iPad was in use), so it has no picture.

### Open

- On the real iPad the Files picker step itself is untested (the test
  switch skips it; the Simulator test covered it). Picking from iCloud
  Drive or a USB drive needs the user's hands.
- Test copies on the iPad (about 15 GB): Documents/imptest and
  Documents/imptest-src. devicectl can't delete; use Files → On My iPad
  → inFamous.
- Free Apple ID sideloading: the two entitlements are untested. Without
  extended virtual addressing the app can't reserve 4 GB.
- No app icon. Bundle id is still `local.subs.infamous`.
- Not notarized (that uploads the app to Apple: ask the user first).
- Windows prebuilt with import: later.
- Legal points: see "Now".

---

## 2026-11-08 — RSX thread trim

### Result

Four cuts to the RSX FIFO thread (`frame_clock`), the frame-rate limit.
All are exact: each has a check switch that compares against the old
path, and every check said 0 differ.

| change | check | speed |
|---|---|---|
| decode micro-opts (index dedupe, table decode) | VTX_VERIFY: 39.5M draws, 0 differ | not split out |
| geometry memo (as D3D12 VCACHE) | GEO_VERIFY: 61M hits, 0 differ | Mac static +8%, iPad +5% (noisy) |
| constant blocks in one call | TC_VERIFY: 103M blocks, 0 differ | Mac +12% |
| memo hits drawn in place (no copy) | GEO_VERIFY: 33M hits, 0 differ | iPad -8% RSX CPU per frame, Mac neutral |
| pipeline key in one XXH3 pass | old vs new key: 107.6M draws, 297 keys, 0 differ | key work: Mac 13.5% → 7.7%, iPad 14.6% → 9.2% of the RSX thread |
| texture span kept per cache entry; texture units decoded once per draw | old vs new: 683M spans, 104M masks, 0 differ | iPad RSX busy 9.4 → 8.6 s per 10 s; profile pair 19.3 → 19.8 flips/s |

- **iPad now 20.2 and 21.5 flips/s** (two plain runs, plus 19.3 with
  the profiler on), RSX thread 86% on P cores. Before this round:
  16.2–16.6. Gameplay screenshots look right.

- Memo: 99% of ~8.7k draws a frame re-use converted geometry.
- Constant fast path, Mac static: 31.8, 31.9 (one noisy 27.4) vs
  28.3, 28.0, 28.4. On the iPad `rsx_process_method` fell from 11.6%
  to 3.8% of the RSX thread.
- In-place memo hits, iPad: memcpy fell from 10.1% to 5.4% of the RSX
  thread. Flips 14.6, 11.4, 11.3 vs 9.9, 10.8, 10.8.
- Mac static is now ~34–36 flips/s in quiet runs (was ~32).
- Texture span/mask cut, iPad self time: mip chain + layout 3.1% →
  under 0.5%; texture register decode 3.1% → 1.4%. Mac the same
  (mip chain + layout 3.2% → under 1%, decode 3.6% → 1.5%).

### iPad slow state

- From ~17:40 the iPad got slow again for every setting
  (18 → 11 flips/s). Thermal 0, no other app.
- The 18:51 profile shows why: the RSX thread ran **99% on E cores**
  (at midday it was ~75% on P cores). In that state the RSX thread is
  only 70% busy, so it is no longer the only limit.
- Not our code. Maybe night-time background work or a power mode.
  Speed numbers from that state are only good for on/off pairs.

### What changed

- `engine: reuse texture span and texture-unit masks per draw`.
  `eng_texture_slot` keeps each entry's span (it depends only on the
  identity fields plus levels). `sink_end` reads the 16 texture units
  once: the cube mask comes from `sink_bind_textures`, and the vertex
  texture mask is read once and passed to `eng_pipeline_get`.
  The draw engine runs only with the Metal back end.
- `engine: branch-free index dedupe, table-based vertex decode`.
- `engine: cross-frame geometry memo (as D3D12 VCACHE)`.
- `engine: constant-upload fast path with the draw engine (Apple)`.
  The engine took the per-word path because GCM_TC_FAST turned off
  with the engine on. On Apple, `constants_gen` now moves only when a
  value changes (more constant re-use). Windows keeps the old
  behaviour (`#if defined(__APPLE__)`).
- `metal: draw geometry memo hits straight from the memo ring`.
  The ring is a shared MTLBuffer. Before an overwrite the engine asks
  the back end: it waits for a submitted command buffer that still
  reads that spot, or skips the store when the open batch reads it. New optional
  back-end hooks; D3D12 does not set them.
- Switches (all in the port notes, "Engine speed"): `GEO_MEMO=0`,
  `GEO_MB`, `GEO_STATS`, `GEO_DIRECT=0`, `GCM_TC_ENG=0`, and the
  three `*_VERIFY` checks.

### What failed

- Plain flips A/B of the texture cut (old, new, new, old, 22:57–23:22):
  21.8, 17.7, 15.9, 14.5. The iPad slowed steadily whatever the build
  (thermal 0), so this says nothing either way. The profile pair above
  is the evidence.
- Mac flips during the texture-cut profile (38.9 vs 46.1) are not
  comparable: another task was running the Simulator and
  copying files (load ~12).
- iPad profile of the new key at 21:10: the iPad was locked. Re-run
  after the user unlocked it.
- Texture check (once per frame per bound texture, 1024 samples each):
  Mac ~470–590 a frame, ~1.7–2.0 ms; iPad ~300 a frame, ~2.4 ms
  (~4% of a frame). Only 0–3 change per frame. Eight-load batches and
  prefetch gave nothing on P cores and only noise on E cores, so no
  change. Moving the check to worker threads would make it earlier than
  the bind, which is not exact.
- The iPad A/B for the constant fast path is inconclusive: the iPad
  drifted slow during it (on 18.0, 15.2, 11.1; off 13.9, 14.9, 12.6).
- The Mac stays noisy (load ~18). One profile pair had every function
  ~15–20% slower in one run.
- A first version of the constant change also changed Windows. Caught
  before the commit and guarded.

### Next

- Top of the RSX thread now (iPad, self time): sink_end 8.7%, texture
  check 8.3%, FIFO parse 6.9%, XXH3 6.4% (mostly `eng_geo_vtx_hash`),
  memmove 5.4%. Find the D3D12 way first.
- `eng_bind_vs_constants_split` is 16% inclusive, but most of that is
  the Metal submit under `eng_stage_flush_point`, not the copy.
- Texture check: an exact cut needs to know a texture was not written
  since its last check. Page protection is out (file reads into guest
  memory would fail with EFAULT).

---

## 2026-11-08 — iPad speed: profiling

### Result

- **Gameplay 14.6 → 16.2–16.6 flips/s (+12%)** from two QoS changes
  on iOS: the RSX thread runs at user-interactive (+7%), and the SPURS
  lane workers run at utility (+3%), which leaves the P cores to RSX.
- The limit is the RSX FIFO thread (`frame_clock`): 97% busy.
- Same work as on the Mac: same top functions in the same shares
  (`sink_end` 16–18%, `dc_fetch_indexed_fast` 13–15%,
  `rsx_process_method` 8%). Each step just takes ~2x longer.
  - The iPad has 3 P cores. The RSX thread still runs ~25% of its
    time on E cores (the SPURS threads compete for P cores).
  - M4 P cores are slower than M5 Pro ones, with less cache.
- Thermal state stays 0 (nominal) in every run. No throttling.
- Build flags are fine: `-O2` in both. iOS used `-mcpu=apple-a7`
  (deployment target 16.0); now `apple-a14`, but that made no
  difference.

| change (iPad, gameplay) | flips/s |
|---|---|
| default QoS (`RSX_QOS=0`) | 14.6 |
| RSX at user-interactive | 15.7, 15.5, 15.8 |
| + `-mcpu=apple-a14` | 15.5, 15.5 |
| + `SPURS_LANE_SPIN_US=0` | 16.3 (noise: lean already turns this spin off) |
| + lane and kernel spin 0 | 15.4 |

Lane workers at utility QoS (`SPURS_LANE_QOS=ut`), runs in time order:

| iPad state | lanes at utility | old (default QoS) |
|---|---|---|
| quiet (14:17–14:54) | 16.6, 16.4, 16.2 | 15.8, 16.1, 15.7 |
| slow (14:55–15:25) | 14.3, 13.3, 12.9 | 13.2, 12.9 |

Utility never lost to a nearby old run. Now the iOS default.
In the slow runs even the pause menu (little SPU work) was slower
(16 vs 19), so that drop is the iPad, not the setting.

| Mac static scene | flips/s |
|---|---|
| default QoS | 32.1, 31.7, 32.1 |
| RSX at user-interactive | 30.9, 30.1 |

So the QoS raise is iOS-only (`PORT_IOS`).

### What changed

- `apple: raise RSX FIFO thread QoS on iOS (RSX_QOS)`.
- `ios: log thermal state and per-thread CPU use (IOS_CPU_LOG_SECS)`.
- `ios: build for A14/M1-class CPUs (PORT_IOS_CPU)`.
- `spurs: SPURS_LANE_QOS` switch (Apple only), then
  `spurs: lane workers at utility QoS by default on iOS`.
  macOS and Windows unchanged. `SPURS_LANE_QOS=0` = old behaviour.
- Tools (`$WORK_DIR/tools`): `ios_prof.sh` (iPad Time Profiler),
  `prof_mac_xc.sh` (Mac Time Profiler, same scene), `ab_mac.sh`.
  `ios_device.sh`: relaunch retry, log-pull retry, `JOBS=<n>`.
- Profile reader: `tprof_core.py` (scratchpad; per-thread P/E split
  and top functions).

### What failed

- xctrace over Wi-Fi fails. Over USB `--attach` still fails;
  `--all-processes` works. iPad traces hold fewer samples than the
  Mac's, but the shares are fine.
- One A/B run: the relaunch failed (CoreDevice 10004) and the tool
  read the old log. Another pulled an empty log. Both now retry.
- 12:14: the iPad dropped off USB (state "connecting") mid-run.
  Tests stopped. It may be in use by another task.
- From 14:55 the iPad got ~15% slower for every setting. Thermal 0,
  memory the same, no third-party app running (Settings was open).
  Cause unknown.

### Next

- When the iPad is quiet again: one plain run to confirm ~16.
- The rest of the gap is per-core speed. Next: cut RSX-thread work
  (`sink_end`, `dc_fetch_indexed_fast`, texture content hash). That
  helps every platform but is a bigger job.
- User: touch pad taps and a controller.

---

## 2026-11-08 — M9: real iPad

### Result

- **Runs on the iPad Pro 11 (M4)**, iPadOS 27.2, 9 CPUs, 7.5 GB RAM.
  - Title → START → blast cutscene → gameplay tutorial → START = pause
    menu → DOWN moves to "Commandes". 330 s, no crash (4 runs).
  - Full-screen landscape 2816x1940, game image 16:9 with black bars.
  - MetalFX on: 1280x720 → 2560x1440 (kept within 2x), the final blit
    does the rest.
  - Entitlements work: extended virtual addressing, increased memory
    limit (8183 MB available to the app at start).
  - Memory: lowest free during a run 6542 MB, so the app uses about
    1.6 GB. Plenty of headroom.
  - Touch pad: shown by default (no controller connected). Not yet
    tested with real taps. No real controller tested.
- **Speed: about 14–15 flips/s in gameplay** (flips/s 150–250 s,
  same script each run):

  | run | flips/s |
  |---|---|
  | lean on, MetalFX on (default) | 14.2 |
  | lean off | 14.1 |
  | MetalFX off | 15.0 |
  | lean on again | 14.5 |

  All within noise. Defaults stay (lean on, MetalFX on).

### What changed

- `ios: keep the view landscape on iPadOS 27` — the first iPad run drew
  the game and touch pad in portrait size (970x1408) on the landscape
  screen. Cause: SDL's view-frame code reads the old
  `statusBarOrientation`, which says portrait on iPadOS 27, and swaps
  the view on the first resume. Fix: `SDL_HINT_ORIENTATIONS` =
  landscape only. Checked on the iPad and the iPhone 17 Simulator.
- The shell logs `[ios-geo]` lines: screen, scene, window, view and
  drawable sizes at start, 1/3/10 s and on resize.
- `$WORK_DIR/tools/ios_device.sh run` takes screenshots
  (`SHOT_EVERY`, default 10 s) and copies the log to `stderr.log`.
- New `$WORK_DIR/tools/ios_ab.sh` for iPad A/B runs.

### What failed

- Signing: blocked on the Apple PLA until the user re-accepted it and
  signed in again with the developer account.
- First run: portrait layout (fixed above).

### Next

- Profile on the iPad to find why gameplay runs at ~15 flips/s.
- User: try the touch pad with real taps, and a controller.

---

## 2026-11-07 — M9 / M10: Simulator coverage, low-thread rule

### Result

- **iPad Pro 11 (M5) Simulator** (2420x1668): title → START → gameplay
  → pause menu → DOWN moves to "Commandes". 285 s, no crash, 28.1 flips/s.
  Touch pad on (`TOUCH_PAD=1`). On the 4:3 screen the pad sits over
  the edges of the game image (it is see-through).
- **iPhone 17 Simulator** (2622x1206): same script, same result.
  283 s, no crash, 28.8 flips/s.
- **Real iPad: still blocked** on the PLA (signing says
  `PLA Update available`). The iPad was free (unlocked, no apps running).
- **Low-thread (lean) mode:** on iOS by default, off on the Mac.
  - Windows turns it on when fewer than 17 CPUs are in the affinity mask.
    Apple has no affinity masks, so it counts online CPUs.
  - Mac A/B (15 CPUs, 2 pairs, 300 s, static scene, flips/s 150–290 s):

    | | run 1 | run 2 |
    |---|---|---|
    | lean off | 34.8 | 35.3 |
    | lean on | 35.2 | 33.6 |

    No gain, so the Mac keeps the old default (off).
  - iOS: on when fewer than 17 CPUs (iPhone 6, iPad 9–10).
    Not measured on a real device yet.
  - `V242A_NCPU=6` on the Mac applies the rule as if it had 6 CPUs
    (checked: `6 logical CPUs -> lean ON`).
  - `PS3_NCPU=<n>` now overrides the host CPU count off Windows too.

### What changed

- `cellSpurs.c`: Apple branch of the lean auto rule (commit a11eb72).
  Windows code is untouched.
- `ppu_loader.cpp`: non-Windows `ps3_host_ncpu` reads `PS3_NCPU`.
- Tools: `$WORK_DIR/tools/ab_lean.sh` (Mac lean A/B),
  `$WORK_DIR/tools/flips.sh <run> [from] [to]` (flips/s from `[flip]` lines),
  `$WORK_DIR/tools/ios_device.sh` (iPad steps).

### What failed

- First A/B: 0 flips, endless `GCM control spin`. Not a bug: the script
  left out `PS3RECOMP_METAL_HEADLESS=1`. Mac runs from a shell need it
  (and `PAD_FILE`), as the M5 entry says.

### Next

- User: accept the PLA. Then M9 on the iPad: memory headroom, the 4 GB
  reserve, MetalFX, lean on vs off, controller and touch.

---

## 2026-11-07 — M8: iOS Simulator

### Result

- **M8 done.** The app runs in the iPhone 18 Pro Simulator (iOS 27.2):
  - Title screen at 2622x1206, letterboxed to 16:9, French text.
  - `PAD_SCRIPT` START → blast cutscene → gameplay tutorial (~200 s)
    → START = pause menu → DOWN moves the cursor. 330 s, no crash.
  - About 28 flips/s in the Simulator (not real device speed).
- **Lifecycle:** going to the background parks the frame clock and drains
  the GPU (`[ios] background: frame clock parked after 755 ms, GPU idle`).
  Back in front it resumes and keeps rendering.
- **Touch pad** draws over the game (all 16 buttons, two sticks). Shown when
  no gamepad is connected (`TOUCH_PAD=1` forces it). The Simulator reports
  one gamepad, so by default it hides itself there.
  Not yet checked with real taps.
- Device build (`iphoneos`) compiles, links and signs (wildcard profile,
  no extra entitlements). The same app is for iPad and iPhone (family 1,2).

### What changed

- iOS deps built from source, static arm64, for both SDKs:
  `$WORK_DIR/deps/<sdk>` (SDL3, glslang, SPIRV-Tools, SPIRV-Cross) by
  `$WORK_DIR/tools/build_ios_deps.sh`.
- `$WORK_DIR/tools/ios_configure.sh [iphonesimulator|iphoneos] [cmake args]`
  → Xcode project in `build-ios-sim` or `build-ios`.
  Re-run it when a source file is added (ZERO_CHECK misses new files).
- `$WORK_DIR/tools/ios_sim_run.sh <secs> <tag> [VAR=val ...]`: installs,
  launches with the host game path, screenshots every 10 s.
- Toolkit (commits on `apple-port`):
  - `vm_region_64` on iOS (no `mach_vm_region` in the iOS SDK).
  - SDL3 audio and pad backends (iOS has no SDL2).
  - CMake iOS branch (UIKit, SDL3, host tools skipped).
  - Metal: app-owned `CAMetalLayer`, 16:9 letterbox, `wait_idle`.
  - Metal: **BC1–3 CPU decode** when the GPU can't sample BC. The Simulator
    GPU can't (`MTLPixelFormatBC3` invalid → crash). It asks the GPU at
    run time (`supportsBCTextureCompression`), so it stays off where BC works.
    `PS3RECOMP_METAL_BC_DECODE=0/1` forces it.
  - Metal: **MetalFX** spatial upscaling of the letterboxed image
    (perceptual, at most 2x per axis). Like Windows `UPSCALER`:
    `METALFX=1/0` wins, else `UPSCALER=off` turns it off, else on for iOS,
    off for macOS. The Simulator SDK has no MetalFX, so it is compiled out
    there. **Not yet run on hardware.**
- Game layer:
  - `ios/ios_shell.mm`: SDL3 owns the UIKit main thread, game main on a
    64 MB thread, lifecycle, `[ios-mem]` log every 5 s, logs to
    Documents/play.err.log on device.
  - `ios/touch_pad.mm`: on-screen controller.
  - Bundle has `launch.env` (from `[launch] env`), never game data.
    Device: game goes in Documents/BCES00220 (`UIFileSharingEnabled`).
  - `PORT_IOS_ENTITLEMENTS` (default ON): extended-virtual-addressing and
    increased-memory-limit. OFF = wildcard profile (works without the PLA).

### What failed

- iPad install: `0xe8008012 This provisioning profile cannot be installed
  on this device`. The wildcard profile only lists the iPhone. The iPad is
  already registered with the team (other profiles have it), but Xcode
  can't refresh the profile: `PLA Update available`. Blocked on the user.
- `os_proc_available_memory()` is 0 in the Simulator (no limit there).
- Lifecycle events arrive twice (SDL event watch). Harmless.

### Next

- User: accept the PLA. Then M9 on the iPad (check it is free first):
  memory headroom, whether the 4 GB reserve works, MetalFX, controller
  and touch.
- M10: iPhone Simulator runs (done for M8 on iPhone 18 Pro; add iPhone 17).

---

## 2026-11-07 — M7: speed

### Result

- **M7 done.** Steady 30+ flips/s on the Mac (was ~9.6).
  - The game is not capped at 30, so it runs a bit above.
  - Static tutorial scene (no input after START), 150–290 s:
    **32.9 flips/s**. 10-flip windows: lowest 29.7, p5 31.4, p50 33.0.
    0 of 460 below 29.
  - Movement soak (walk, jump, camera, R1, pause loop), 240–420 s:
    **34.1 flips/s**. Lowest 31.6, p5 33.1. 0 of 613 below 29.
- **Memory:** RSS 1.43 GB at 7 min (M6: ~1.5 GB). See "Memory" below.
- **Audio:** clean. 0% silent, 0 late blocks (M6: ~33% silent).
  The M5 "choppy audio" item is fixed by the speed.
- **Checks:**
  - `PS3RECOMP_ENGINE_VTX_VERIFY=1` run: 34.3 M index reads and
    34.9 M draws checked against the old path, 0 differences.
  - Frames look right by eye (title, gameplay) after each change.
  - Toolkit: builds, ctest 2/2, `test_shader_msl` 65/65,
    `ps3recomp_host --draw` OK.

### Where the time went

- Time Profiler (`xctrace`) on the static scene.
- The RSX thread is the limit: ~98% busy at 9.6 flips/s, and still at 33.
- Big costs at the start:
  - vertex and index fetch, one index at a time;
  - hashing every byte of every texture each frame;
  - pipeline lookup (long key, linear search);
  - clearing a 32 KB constant block per draw;
  - headless presents waited for the GPU every frame;
  - each commit copied the whole staging arena into a new buffer.
- SPU: `setjmp`/`longjmp` made a syscall each time (signal mask),
  ~35% of the SPURS kernel thread.

### What changed (static scene, flips/s after each step)

| Step | flips/s |
|---|---|
| Start (`base1`) | 9.6–10.2 |
| SPU: NEON `tbl` for shufb tables, `_setjmp`/`_longjmp` | 9.1–9.4 (frees CPU; RSX still the limit) |
| Engine: fp constants without the 32 KB clear; sampled texture checksum + set-associative texture lookup | 12.8–14.0 |
| Engine: fast vertex path. A/B on vs off: 16.3 vs 12.9 (+26%) | ~16.3 |
| XXH3 pipeline key + hashed lookup; element ranges scanned once; VS constants staged without a copy; Metal: pipelined headless presents, staging ring, no redundant encoder state | 25.3–26.4 |
| Bulk index read with one-pass dedupe | 28.8–30.5 |
| Per-type vertex loops | 31.0–32.4 |
| Fixed 64 MB staging slots (memory fix) | 31.0–31.2 |
| VCB_DEDUP: re-use the staged vertex constants when unchanged (as D3D12) | 32.2–33.4 |

### Memory

- Pipelined presents need a ring of 4 staging buffers (3 frames in flight).
- A gameplay frame stages 150–240 MB: 8 KB of vertex constants per draw
  (~13k draws), plus vertices and indices.
- First version: each slot grew to the largest frame. RSS 2.9 GB.
  Growing in smaller steps was worse (4.9 GB): freed Metal buffers stay
  resident.
- Now: slots are a fixed 64 MB, made once. A frame that fills ¾ of a slot
  commits what it has (no wait) and goes on in the next slot.
- VCB_DEDUP then cut what a frame stages, which won back the speed.

### New switches

- `PS3RECOMP_ENGINE_VTX_FAST=0`: old vertex and index path.
- `PS3RECOMP_ENGINE_VTX_VERIFY=1`: run both paths, log any difference.
- `PS3RECOMP_ENGINE_TEX_FULLHASH=1`: hash whole textures (old way).
- `PS3RECOMP_METAL_HEADLESS_SYNC=1`: wait for the GPU on every commit (old way).
- `PS3RECOMP_METAL_STAGE_MB=<n>`: staging slot size, 8–384 (default 64).
- `VCB_DEDUP=0`: stage the vertex constants for every draw (same name as D3D12).

### Tools added

- `$WORK_DIR/tools/prof.sh <secs> <tag> <at> <dur> [template] [VAR=val ...]`:
  static-scene run (START at 50 s, then no input). If `<at>` > 0, attaches
  `xctrace` at `<at>` s for `<dur>` s into `<run>/prof.trace`.
  - Export: `xcrun xctrace export --input prof.trace --xpath
    '/trace-toc/run/data/table[@schema="time-profile"]' > tp.xml`.
- Keep the Mac awake beside it: `nohup caffeinate -i -t <secs> &`.
  Early runs slept at 1% battery; now on AC.

### Runs

- Start: `20261107-185502-m7-base1`.
- Final static: `20261107-211501-m7-vcb1`.
- Final movement: `20261107-212032-m7-move4`.
- Index verify: `20261107-202722-m7-vverify2`.
- Memory: `20261107-203743-m7-move` (2.9 GB), `-205016-m7-move2` (4.9 GB),
  `-210032-m7-move3` (1.42 GB).

### Open items

- RSX thread still ~98% busy. Next hot spots: `sink_end` 16%,
  `dc_fetch_indexed_fast` 12%, `rsx_process_method` 8%, `memmove` 8%,
  texture hash 6%, Metal encode ~16% (profile before VCB_DEDUP).
- Ideas from D3D12: cross-frame vertex cache (VCACHE), vertex fetch on
  worker threads (VTX_ASYNC).
- iPhone: maybe 2 frames in flight instead of 3 (one less staging slot).
- Linker warning "reducing alignment of section `__DATA,__common`".
  Harmless; the toolkit build shows it too.
- `[STALL] late pickup` SPURS lines: same rate as M6 (~3–4 a minute). Not new.

### Next step

- M8: build for the iOS Simulator and boot to the menu.

---

## 2026-11-07 — M6: soak and TSAN

### Result

- **Soak 1 (31 min): pass.** No crash, no hang.
  - Script: boot, START, then a 20 s loop: walk, jump, camera, R1, pause
    menu open and close. Ran to the timeout (1910 s).
  - Flips never stalled: lowest 270 per 30 s. Last flip 1.5 s before the kill.
  - Gameplay speed ~9.6 flips/s (M7).
  - RSS 1.33 GB at 4 min, 1.47 GB at 29 min, peak 1.59 GB, 1.36 GB at the
    end. Grows ~5 MB/min, then drops back. Watch on iPad (M9).
  - Run: `$WORK_DIR/runs/20261107-173902-m6-soak1/` (`soak.tsv`).
- **Soak 2 (31 min, with the sync fixes): pass.** Same script, same result.
  - Lowest 270 flips per 30 s. Last flip 1.6 s before the kill.
  - ~9.5 flips/s in gameplay.
  - RSS 1.47 GB at 4 min, 1.56 GB at the end, peak 1.64 GB.
  - Run: `$WORK_DIR/runs/20261107-181123-m6-soak2/`.
- **TSAN on sync code: clean.** `sync_stress` (lv2 mutex, cond, semaphore,
  event queue, rwlock, plus the Win32 shim) under ThreadSanitizer.
  - Before: 26 data-race warnings (all tests still passed).
  - After: 0 warnings on 16 seeds. Normal `ctest` passes.

### What TSAN found

- No lost wakeups, no corruption. All were reads without the lock:
  - Mutex owner pre-checks (`sys_mutex_lock`/`trylock`/`unlock`,
    `sys_cond_wait`).
  - "Queue empty?" hint before `sys_event_queue_receive` waits.
  - Two log counters (`semaphore_post`, `PS3_EVQSTAT` init).
  - Test watchdogs reading `Interlocked*` counters.
- Fix: `LV2_PEEK`/`LV2_POKE`/`LV2_BUMP` in `lv2_syscall_table.h`. Relaxed
  atomics off Windows (same `ldr`/`str` on arm64); plain access on
  `_WIN32`, so Windows is unchanged. Harness uses `ReadAcquire`.
- Not covered: lwmutex and lwcond (not in `sync_stress`), and a TSAN
  build of the whole game (generated files are huge).

### Tools added

- `$WORK_DIR/tools/soak.sh <secs> <tag>`: soak run with the pad loop
  above; samples secs, RSS MB and flip count to `<run>/soak.tsv` every 30 s.
- TSAN build of the stress test (outside the repo):
  ```
  cmake -S ps3recomp/tests/sync_stress -B $WORK_DIR/build/sync_stress-tsan -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -ffp-contract=off" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
  ```
  Run `./sync_stress <seed>`; count `WARNING: ThreadSanitizer` in stderr.

### Next step

- M7: profile a gameplay run with `xctrace` (Time Profiler) and find
  where the ~100 ms per frame goes.
- Later: add lwmutex/lwcond to `sync_stress`; recheck RSS growth on iPad.

---

## 2026-11-07 — M5: lighting fixes, audio check

### Result

- **Audio plays.** SDL2 on CoreAudio, 48 kHz stereo float. A WAV dump of a
  228 s run has game sound. Choppy: ~half the blocks arrive late
  (concealed), 30–50% of blocks silent. Tied to low frame rate (~15 flips/s). Goes to M7.
- **3D colour: better, not done.** Ported three D3D12 fixes to the engine:
  - TEX_XF (bx2 expand of G-buffer normals; sRGB decode when the guest asks).
  - SUN_NDL_CLAMP (sun cascade N.L clamped).
  - Depth bounds (each sun cascade lights only its own depth slice).

| Scene | Mean RGB RPCS3 / port | HSL sat RPCS3 / port | RMSE |
|---|---|---|---|
| Title | 43,53,64 / 49,62,73 | 47 / 50 | 0.084 |
| Cutscene (Cole wakes) | 54,36,28 / 98,76,66 | 56 / 48 | 0.221 |
| Tutorial | 72,50,41 / 98,76,66 | 66 / 53 | 0.137 |

Run: `$WORK_DIR/runs/20261107-154709-m5-dbt1/keep/` (+ `cmp/`).

### What is still wrong

- Near lit surfaces are 1.3–2x too bright and less warm. Far fog and sky
  match (ratio ~0.92). Rock is grey in the port, brown in RPCS3.
- Ruled out (measured, not guessed):
  - sRGB output: packer reg 0x1FEC is 0 on every draw.
  - Texture gamma: no texture sets gamma bits (engine reads the same
    register as D3D12).
  - Composite tone curve: its c0 is 0, so it is off. Bloom and particles
    are small (means < 0.03).
  - Cascades stacking: each cascade lights a separate screen band.
  - Cube map stride: the cubes are 1 mip, 2048 bytes a face, pitch 0.
  - BC1 decode and channel remap: same code path as D3D12.
- Left: the sun shader's light terms (sun colour, cool back light, ambient
  cube, reflection cube, B8 ramp), or a decompiler gap shared with D3D12
  (H/R register aliasing is not modelled). No second reference to tell
  which: Windows can't run here.
- Missing from the engine: signed blend (D3D12 v166). Heat-haze smoke may
  show hard holes. Does not change overall brightness.

### Tools added

- Engine trace line: `ps=<hash>` (matches `fp_<hash>.hlsl` from
  `PS3RECOMP_METAL_SHADER_DUMP`), depth bounds, stencil, texture location,
  size, pitch, first bytes, and FP constants (up to 8).
- `AUDIO_WAV=<path>` (Apple only): float WAV of what goes to the device.
- Run recipe must include `PS3RECOMP_METAL_HEADLESS=1 PAD_FILE=...`, or
  the GPU stalls at boot.

### Next step

Pick one:
- Dump RPCS3's decompiled shaders (its "log shader programs" option) and
  diff the sun shader against ours. Finds a shared decompiler bug.
- Or get one Windows (D3D12) capture of the tutorial frame. Tells if the
  gap is Apple-only.
- Or park colour and go on to M6.

---

## 2026-11-07 — M5 prep: reference frames

### Result

Golden set captured in RPCS3 and compared with the port.

- **Golden set:** `$REF_DIR/rpcs3/` (10 frames, 1280x720, `SOURCES.txt`).
  Run: `$WORK_DIR/runs/20261107-142201-golden/`.
- **Port, same path, French:** `$WORK_DIR/runs/20261107-142726-golden-port-fr/keep/`
  (`cmp/` has the diff images).

| Scene | RMSE | Mean RGB RPCS3 / port | Saturation RPCS3 / port | Look |
|---|---|---|---|---|
| SCE presents | 0.095 | 4,3,4 / 3,2,3 | — | same; legal line differs (US vs EU title id) |
| Sucker Punch logo (warm) | 0.083 | 83,49,30 / 82,49,29 | 60 / 61 | same |
| Sucker Punch logo (cool) | 0.086 | 48,61,68 / 48,60,66 | 19 / 18 | same |
| Title | 0.144 | 37,46,55 / 58,68,74 | 17 / 15 | port brighter, flatter |
| Cutscene (Cole wakes) | 0.162 | 46,30,24 / 66,58,54 | 20 / 9 | port grey; jacket light instead of dark |
| First gameplay | 0.077 | 45,29,23 / 48,40,36 | 20 / 9 | port grey |
| Tutorial | 0.104 | 69,48,40 / 78,68,62 | 24 / 12 | port grey; Cole's clothes light |
| Pause menu | 0.134 | 147,146,145 / 156,155,154 | 4 / 5 | same text and layout; port adds Settings, Quit Game (by design) |

RMSE is high everywhere because animated scenes never line up in time.
Read the colour columns and the pictures, not RMSE alone.

### Findings

- **2D is right.** Movies, logos and menus match RPCS3.
- **3D scenes are washed out.** About half the saturation, brighter shadows,
  no warm grade. Cole's dark jacket comes out light grey. Looks like a
  missing or wrong post-process (colour grade / tone curve) or a texture
  channel problem. This is the first M5 task. Next: frame trace + pass
  dumps of one gameplay frame, find the grade pass, compare with D3D12.

### Setup found on the way

- **Firmware 4.93** from Sony (`deu01.ps3.update.playstation.net/.../PS3UPDAT.PUP`,
  206,197,916 bytes, MD5 `a2b60b6ac1d2e49e230144345616927c`, matches the URL).
  In `$WORK_DIR/firmware/`. `--installfw` fails with `--headless`; GUI mode works.
- **RPCS3 wrote to ~/Library since M1.** It ignores `RPCS3_CONFIG_DIR` /
  `RPCS3_CACHE_DIR`. Moved `~/Library/Application Support/rpcs3` and
  `~/Library/Caches/rpcs3` (both made by the M1 run) into
  `$WORK_DIR/rpcs3-home/Library/`. Now run with `HOME=$WORK_DIR/rpcs3-home`.
  Checked: nothing new in ~/Library.
- **Language:** the EU disc ships French, Italian, German, Spanish, Dutch
  (lists at `0x00720868`, read from `EBOOT.elf`). No English, so RPCS3 is
  always French. The port reports title id `BLES00000`, gets the US list,
  shows English. Use `PS3_LANG=french` on the port for comparisons.
- **Driving RPCS3** with no screen-recording permission: `tools/rkey`
  (CGEvent posted to the RPCS3 pid only), F12 screenshots, keyboard pad.
  New tools in `$WORK_DIR/tools`: `rkey`, `rshot.sh`, `rpcs3_run.sh`.
- RPCS3 config: fullscreen off, "Show capture hints" off (older shots have a
  "Screenshot saved" box top-left; mask `0,0 560,56`).
- The port ignores SIGINT and SIGTERM; stop it with KILL.
- Disk: was 94% full. Deleted all `frames/*.ppm` dumps in `$WORK_DIR/runs`
  (7.6 GB); logs and `keep/` PNGs stay. Now 64 GB free, runs = 168 MB.
  Delete a run's `frames/` once its key frames are in `keep/`.

---

## 2026-11-07 — M4: menu

### Result

M4 check passes. The menu is the **pause menu**: there is no save, so
START at the title starts a new game (no main menu with Continue).

| Check | Result |
|---|---|
| Title screen | lit and correct after the fixes below |
| START at title | new-game cinematic, then the first tutorial ("look up at the chopper") |
| Gameplay | renders; no dropped draws |
| START in gameplay | pause menu: Powers, Controls, Options, Saved Data, Settings, Quit Game |
| DOWN | highlight moves one row each press (Controls ... Settings) |
| CROSS on Settings | game calls the port hook (`[pausemenu] Settings`); see open items |
| CIRCLE | closes the menu, gameplay resumes |
| Crashes / drops | 0 in 420 s |

- All input was through `PAD_FILE`, no window.
- Engine unit test (`test_rsx_draw_engine`): 77 checks, all pass.

Logs: `$WORK_DIR/runs/20261107-140056-menu4/` (key frames in `keep/`),
title fix in `$WORK_DIR/runs/20261107-134801-trace7/`.

### What was wrong, and the fixes

Each fix copies what `rsx_d3d12_backend.c` does.

- **Dark 3D frames.** Two reads fell through to guest memory:
  - a depth buffer read as A8R8G8B8. Fix: a colour copy of depth and
    stencil, same byte order as D3D12 `zs_convert`.
  - a draw reading its own target. Fix: a copy per draw, like D3D12's
    feedback path.
  - Title screen mean went from 0.011 to 0.232.
- **Black gameplay frames.** The Metal engine dropped ~1,150 draws a frame
  past its 8,192 record cap. Fix: cap 32,768, and a full stream submits
  mid-frame instead of dropping.
- **Immediate-mode draws.** Vertices written with `SET_VERTEX_DATA*_M`
  were ignored. Now passed to the engine, as `rsx_commands.c` does for D3D12.
- **Shadows.** Shadow compare per texture unit, keyed like D3D12.

### What changed

- Commits on `apple-port`:
  - `metal: numbered frame dumps`
  - `engine: immediate-mode vertices`
  - `metal: submit mid-frame when the record stream is full`
  - `engine: read render targets as the D3D12 backend does, plus a frame trace`
  - `metal: depth colour copy and surface copy hooks`
- New switches (default on, as in D3D12): `FEEDBACK_COPY`, `ZCONV`.
- New debug: `PS3RECOMP_ENGINE_TRACE_AT=<frame>|<secs>s`,
  `PS3RECOMP_ENGINE_SURFACE_DUMP=<dir>`, `PS3RECOMP_METAL_FRAME_DUMP_EVERY`.
- Tools (outside the repo): `$WORK_DIR/tools/pad_at.sh` (timed presses),
  `$WORK_DIR/tools/press.sh` (one press, then save the next frame).
- the port notes "Run and observe" lists all of the above.
- No Windows code or generated code touched. New backend hooks are
  optional; D3D12 does not use the Metal engine.

### Open items

- **Settings and Quit Game do nothing on Mac.** They open a host overlay
  (`rsx_ui_overlay.c`) that is D3D12-only; the non-Windows build is a
  stub. Needs a Metal version. The README promises Settings in the menu.
- **No reference frames yet.** RPCS3 needs PS3 firmware. Checks so far are
  by eye only. Ask the user once (capture the golden set, or allow the
  firmware download).
- **Speed:** about 9 flips/s in gameplay with frame dumps on (M7).
- **Not verified:**
  - yellow frame right after START at the title (effect or bug?);
  - stencil channel in the colour copy (`.r`; try `.g` if it reads 0);
  - a SIGBUS seen once on the vtable path early in M4.
- `DIAGNOSTICS.md` generator misses `eng_env_flag` switches, and a regen
  adds ~575 unrelated knobs. Not regenerated.
- **Disk:** `$WORK_DIR/runs` is 5.8 GB (PPM dumps); the disk is 94% full.

### Next step

- M5: compare gameplay with references, check audio plays.
- Get the golden set first.

---

## 2026-11-07 — M3: boot

### Result

M3 check passes on the first try. No code change was needed.

| Check | Result |
|---|---|
| Entry | ELF entry OPD `0x00771B50` -> code `0x00010230`; dispatched |
| `main` | crt `func_00010348` calls `func_00193950(argc, argv, envp)`, then `exit()`. That is guest `main`. |
| In `main` | flip stack: `func_00010230` -> `func_00010348+0x190` -> `func_00193950+0x104` -> ... -> flip, on PPU tid 1 |
| Flips | 11,390 in 100 s (`GCM_FLIPCOUNT=1`) |
| Errors / crashes | 0 in the log |
| Repo after run | clean (no stray files) |

- Two headless runs (120 s, 100 s), same result.
- Metal: "headless on Apple M5 Pro (1280x720), guest shaders on".
- SPURS, SPU threads, audio, file reads and Bink all start.

Logs: `$WORK_DIR/runs/20261107-130159-boot1/`, `$WORK_DIR/runs/20261107-130458-boot2/`.

### What changed

- No code changes.
- **Run script:** `$WORK_DIR/tools/run_game.sh` (outside the repo). It
  loads `$WORK_DIR/launch_env.sh`, which holds the 173 `[launch] env`
  vars from `installer/infamous.toml`. Re-make that file if the toml changes.
- **Port notes:**
  - The `PS3_DEBUG` console only exists in the toolkit harness, not in
    `inFamousRecomp`.
  - Added the flip switches (`GCM_FLIPCOUNT`, `FRAME_DRAWS`, `GCM_FLIP_BT`).
  - Use `/usr/bin/grep` on the lifted files; the shell's ugrep skips them.

### Open items

- **Ctrl-C (SIGINT) doesn't stop the game.** `gtimeout` needs its
  `-k 10` kill (rc 137). Likely SDL's own SIGINT handler, which turns the
  signal into a quit event that nothing reads. Look at it before the M6 soak.
- **Frame rate:** about 15 flips/s late in the run, with ~5,900 draws and
  ~1.6 M verts per frame. Windows in-world is ~1,270 draws. Don't know
  yet what is on screen. Check it with a frame dump in M4.
- **Pad:** `connected=0` (no controller, headless). M4 uses `PAD_FILE`.
- **`__common` alignment warning** (from M2): nothing odd seen so far.

### Next step

- M4: frame dump (`PS3RECOMP_METAL_FRAME_DUMP`) to see what renders. Then
  reach the main menu and drive it with `PAD_FILE`.
- Needs reference frames for the menu in `$REF_DIR` (RPCS3, or ask the user).

---

## 2026-11-07 — M2: link for arm64

### Result

M2 check passes.

| Check | Result |
|---|---|
| `inFamousRecomp` | links: Mach-O 64-bit arm64, 128.7 MB |
| x86 flags in `build.ninja` | 0 |
| Toolkit rebuild + ctest | pass, 2 / 2 |
| `ps3recomp_host --draw` (headless) | pass |
| `test_win32_compat` | 341 passed, 0 failed |
| SPU float diff test (arm64 vs scalar) | 144 M cases, 0 differ |
| x86: AVX2 SPU code (cross-compiled) | byte-identical to before |
| x86: `PORT_CPU_COMPAT` copy step | still applies, no FMA in output |

Logs: `$WORK_DIR/runs/m2-20261107-124158/`.

### What changed

- **CMake: `PORT_TARGET_X86`.** Off when the target is not x86
  (arm64 Mac, iOS). Then `PORT_SPU_AVX2`, `PORT_PPU_AVX2` and
  `PORT_CPU_COMPAT` are forced off, and SPU files get `-O2` with no
  `-msse4.1`. Windows targets x86, so nothing changes there.
- **CMake: FP contraction.** Game glue (`main`, `stubs`, `trainer`) gets
  `-ffp-contract=off` on arm64, like the runtime and PPU code. SPU files
  keep clang's default (contract on), as the Windows AVX2 build does.
- **SPU float SIMD on arm64** (`spu_helpers.h`, `spu_xfk.h`). The SSE2 path
  now builds on arm64 through sse2neon. Fused ops use NEON `vfmaq` /
  `vfmsq`: one rounding, like x86 FMA. The diff test passed in default mode
  and in SPU float mode (truncate + FTZ/DAZ).
  - `spu_xfk.h` must never contain the text `__FMA__`. The
    `PORT_CPU_COMPAT` copy step keys on it.
- **`win32_compat.h`:** `TerminateProcess` (own process only) and
  `_putenv_s`. An empty value removes the variable, like MSVC.
  `msvc_compat.h` had its own `_putenv_s`; both now share one guard and the
  same rule, so a file can include both. Test added.
- **`stubs.cpp`, `infamous_lang.h`:** the `__declspec` lines are behind
  `_WIN32`. Off Windows they use the shim's `GetTickCount64` /
  `VirtualQuery` and a weak symbol.
- **`wwsplug/spu_recomp.c`** (generated, not edited): CMake force-includes
  `win32_compat.h` for it off Windows.
- **Non-Windows defaults** for 5 globals the portable code uses:
  `g_ui_fps_cap`, `g_uiov_paused_fn`, `g_pause_menu_built_ms`,
  `g_uiov_request` (`rsx_ui_overlay.c`) and `g_lodx_f5_off`
  (`rsx_d3d12_backend.c`). Same values as on Windows.
- **Port notes:** build command uses `-O2` (Windows is `/O2`; AppleClang
  Release would be `-O3`) and Python 3.12.

### Open items

- **ld warning:** `__DATA,__common` alignment cut from 32 KB to 16 KB.
  The M0 toolkit build shows it too, so it comes from the runtime. Find the
  symbol if anything odd shows up in M3.
- **`PPU_INLINE_LOADS`:** the unix lifted-PPU path doesn't define it;
  Windows does. Perf item for M7.
- **`test_spu_shufb`** says "built without SSE4.1, nothing to compare" on
  arm64. The SSE2 shufb path runs there; `test_spu_helpers` (63 / 63) and
  `test_spu_v131_shufb_xfq` (bad = 0) cover it.
- **Build cost:** the 5 big PPU files take about 85 s, peak about 3.5 GB
  RAM each.
- **Windows build not re-checked** (no Windows box). x86 SPU output was
  cross-checked as above; everything else is guarded.

### Next step

- M3: run `inFamousRecomp` against `$WORK_DIR/game/BCES00220` under
  `gtimeout`. Goal: guest reaches entry, then `main`, and the log shows
  flips.

---

## 2026-11-07 — M1: generate on Mac

### Result

M1 check passes.

| Check | Result |
|---|---|
| `EBOOT.elf` sha256 vs `eboot_sha256` | match |
| `install --no-build --regen -y` | exit 0, all 19 steps ran |
| Hash check vs `generated.lock` | 156 / 156 files at the final hash |
| Patches | 51 patched, 105 already right, 0 created, 0 removed |

Logs: `$WORK_DIR/runs/m1-install-20261107-123619/`, `$WORK_DIR/logs/m1-*.log`.

### Setup found on the way

- **Python:** `/usr/bin/python3` is 3.9. `doctor` flags it. Pass
  `--python /opt/homebrew/bin/python3.12`.
- **ISO:** `infamous-installer extract-iso` worked as is.
- **RPCS3:** Homebrew has no cask. Used the official build
  (v0.0.43-20570, github.com/RPCS3/rpcs3-binaries-mac-arm64), digest checked,
  unpacked into `$WORK_DIR/tools/rpcs3`. Only used for `--decrypt`.
  (Wrong at the time: RPCS3 ignores `RPCS3_CONFIG_DIR` / `RPCS3_CACHE_DIR`.
  Fixed during M5 prep, see that entry.)

### What changed

- **installer `apply_lock`: line endings.** The lock was made on Windows.
  Python text mode writes CRLF there and LF on macOS, so 33 generated files
  "differed" on Mac. Now: if a file's hash doesn't match the patch base, try
  the CRLF and LF versions; if one hashes to the base exactly, write that and
  go on. Hash-checked, so nothing else can slip through.
  Windows: its files already match the base, so this path never runs there.

### Open items

- **Optional step `kbm-icons-build` fails.** `tools/make_kbm_icons.py` reads
  `infamous-recomp/_xfer/bak_v189/font_icons_l0.bin`, a dev-only file that is
  not in the repo. A fresh Windows checkout fails the same way. The step is
  optional, so the keyboard/mouse icon set just stays off. Not a port issue.
- **Lock not re-made.** `generated.lock` still holds the Windows (CRLF) base
  hashes. That's fine with the fix above. Don't re-lock on Mac.

### Next step

- M2: configure `infamous-recomp` with CMake + Ninja for arm64, x86-only
  options off, and get `inFamousRecomp` to link.

---

## 2026-11-07 — M0: toolkit on Mac

### Result

All M0 checks pass on an M5 Pro (macOS arm64).

| Check | Result |
|---|---|
| `cmake --build build` | builds, no errors |
| `ctest` (sync_stress, fiber_switch) | 2/2 pass |
| `test_shader_msl` | 65 pass, 0 fail |
| `ps3recomp_boot_smoke build/smoke/smoke.elf` (headless) | PASS: 3 flips, 3 frames, 3 vblanks |
| `PS3RECOMP_METAL_HEADLESS=1 ps3recomp_host --draw` | PASS: clear colour and pixel OK |

Logs: `$WORK_DIR/runs/20261107-121418/`.

### Note

The port notes said ps3recomp already built on Mac arm64. It did not.
The fixes below were needed first.

### What changed

- **CMake:** `-msse4.1` only on x86 targets. Windows still gets it.
- **SIMD:** new `include/ps3emu/host_simd.h`.
  - x86: same intrinsics headers as before.
  - arm64: vendored sse2neon 1.9.1 (MIT), precise min/max, div, sqrt, dp.
  - arm64 `__rdtsc` = `CNTVCT_EL0` (24 MHz). Every rdtsc user is
    calibrated against QPC, so only diagnostics see it.
  - Files that included `<x86intrin.h>` / `<xmmintrin.h>` now go through it
    on arm64 only.
- **cellSysutil:** `__declspec(selectany)` → `__attribute__((weak))` off Windows.
- **cellPad:** PAD_FILE state moved out of the XInput-only block.
- **cellSpurs:** `s_w16p_mode` = 0 off Windows.
- **Boot scaffold (host + smoke):** off-Windows includes, a
  `PPU_INJECT_BASE_C` fallback, and inert stubs for symbols the game's
  loader normally owns.
- **rsx_d3d12_backend.c (non-Windows stub part):** `g_hz`,
  `ps3_hz_is_main`, `rsx_ww_bump`.
- **rsx_commands.c bug fix:** the `draw_arrays` / `draw_indexed` calls sat
  outside their `if` (misleading indent). Metal's register-file engine leaves
  the command backend NULL on purpose, so `--draw` crashed. Now braced.
  Windows: D3D12 always sets the backend, so no change.
- **sync_stress test fix:** stored ms-since-boot in a 32-bit LONG. After 24.8
  days of uptime it wraps and fires a false stall. Windows has the same bug.
  Now counts from test start.
- **test shims:** `spu_interp_set/clear_pending_event`.

### Open items

- **Windowed Metal from a worker thread.** Smoke without
  `PS3RECOMP_METAL_HEADLESS=1` hangs in Metal init: the vblank ticker thread
  tries to open a window. macOS needs windows on the main thread. Fix in M2/M3.
- **`rsx_ww_bump` is a no-op off Windows.** Fine now. If Metal caches guest
  textures, it must hook the same write-watch event first.
- **Windows build not re-checked here** (no Windows box). All changes are
  guarded or are plain bug fixes. Check on Windows when possible.

### Next step

- M1: run the installer pipeline on Mac, check hashes vs `generated.lock`,
  apply patches.
- Needs `GAME_DIR` from the user.
