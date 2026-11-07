# Apple port log

Read this first every session. Newest entry on top.

## Now

- Milestone: **M1 done**. Next: **M2** (link `inFamousRecomp` for arm64).
- Game: extracted copy in `$WORK_DIR/game/BCES00220` (has `EBOOT.elf`).
  The ISO on an external drive is only needed to re-extract.
  - BCES00220, inFamous, Europe. PARAM.SFO VERSION 01.01, APP_VER 01.00.
  - Decrypted image: no .dkey. EBOOT.BIN is a retail SELF; RPCS3 decrypts it.
- Generated sources are in place in the repo (git-ignored). Don't regenerate
  unless needed; `install ... --no-build -y` re-checks them.

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
