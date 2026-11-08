# Apple port log

Read this first every session. Newest entry on top.

## Now

- Milestone: **M9 done on the real iPad** (iPad Pro 11 M4, iPadOS 27.2):
  gameplay + pause menu, 330 s, no crash. M10 (iPhone) is done in the
  Simulator only (iPhone 17, iPhone 18 Pro). **Never use the physical iPhone.**
- Signing works with my team (the PLA block is gone).
- The iPad may be busy with another task: run `ios_device.sh check` first.
- iPad speed: **15.5–15.8 flips/s** in gameplay (was 14.6), after
  raising the RSX thread's QoS. Profiled: see the entry below.
  Next step: A/B `SPURS_LANE_QOS=ut` on the iPad (built, not measured:
  the iPad dropped off USB mid-run).
- Still needs the user: real taps on the touch pad, and a real controller.
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

---

## 2026-11-08 — iPad speed: profiling

### Result

- **Gameplay 14.6 → 15.5–15.8 flips/s (+7%)** from one change: the RSX
  thread now runs at user-interactive QoS on iOS.
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

| Mac static scene | flips/s |
|---|---|
| default QoS | 32.1, 31.7, 32.1 |
| RSX at user-interactive | 30.9, 30.1 |

So the QoS raise is iOS-only (`PORT_IOS`).

### What changed

- `apple: raise RSX FIFO thread QoS on iOS (RSX_QOS)`.
- `ios: log thermal state and per-thread CPU use (IOS_CPU_LOG_SECS)`.
- `ios: build for A14/M1-class CPUs (PORT_IOS_CPU)`.
- `spurs: SPURS_LANE_QOS` opt-in switch (Apple only, unset = old
  behaviour). Built, not measured yet.
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

### Next

- When the iPad is free: A/B `SPURS_LANE_QOS=ut` vs default, 2 pairs.
- If no gain, the gap is per-core speed. Then cut RSX-thread work
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
