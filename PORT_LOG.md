# Apple port log

Read this first every session. Newest entry on top.

## Now

- Milestone: **M2 done**. Next: **M3** (boot: guest reaches entry, then `main`; log shows flips).
- Build dir: `infamous-recomp/build-mac` (Release, `-O2`, arm64). Command is in the port notes.
- Game: extracted copy in `$WORK_DIR/game/BCES00220` (has `EBOOT.elf`).
  The ISO on an external drive is only needed to re-extract.
  - BCES00220, inFamous, Europe. PARAM.SFO VERSION 01.01, APP_VER 01.00.
  - Decrypted image: no .dkey. EBOOT.BIN is a retail SELF; RPCS3 decrypts it.
- Generated sources are in place in the repo (git-ignored). Don't regenerate
  unless needed; `install ... --no-build -y` re-checks them.

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
  `RPCS3_CONFIG_DIR` / `RPCS3_CACHE_DIR` keep it out of ~/Library.

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
