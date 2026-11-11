# Apple port log

Read this first every session. Newest entry on top.

## Now

- **End-user release (new):** prebuilt Mac app and iPad .ipa with no game
  files. The player picks their own disc image or game folder at first
  start; the app unpacks it, decrypts EBOOT.BIN and checks it. See entry
  "End-user release: in-app disc import". Make the files with
  `infamous-recomp/tools/release_apple.sh` → `$WORK_DIR/dist/<ver>`.
- **Legal points for the user's lawyer** (not decided by me): the
  prebuilt binary holds code translated from the game; it embeds the
  EBOOT metadata key (`import/eboot_key.h`, git-ignored, never commit);
  at3_standalone (LGPL 2.1) is linked statically; the repo has no
  top-level licence.
- Milestone: **M9 done on the real iPad** (iPad Pro 11 M4, iPadOS 27.2):
  gameplay + pause menu, 330 s, no crash. M10 (iPhone) is done in the
  Simulator only (iPhone 17, iPhone 18 Pro). **Never use the physical iPhone.**
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
  already a switch; the default stays on until the user picks.
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
