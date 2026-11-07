# Apple port log

Read this first every session. Newest entry on top.

## Now

- Milestone: **M4 done**. M5 references captured (`$REF_DIR/rpcs3/`). Next: **M5** (gameplay renders close to references; audio plays).
- Run: `$WORK_DIR/tools/run_game.sh <secs> <tag> [VAR=val ...]` (see the port notes, "Run and observe").
- Build dir: `infamous-recomp/build-mac` (Release, `-O2`, arm64). Command is in the port notes.
- Game: extracted copy in `$WORK_DIR/game/BCES00220` (has `EBOOT.elf`).
  The ISO on an external drive is only needed to re-extract.
  - BCES00220, inFamous, Europe. PARAM.SFO VERSION 01.01, APP_VER 01.00.
  - Decrypted image: no .dkey. EBOOT.BIN is a retail SELF; RPCS3 decrypts it.
- Generated sources are in place in the repo (git-ignored). Don't regenerate
  unless needed; `install ... --no-build -y` re-checks them.

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
- Disk: 94% full (58 GB free). `$WORK_DIR/runs` is about 6 GB of frame dumps.

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
