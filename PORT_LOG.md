# Apple port log

Read this first every session. Newest entry on top.

## Now

- Milestone: **M0 done**. Next: **M1** (installer on Mac).
- Blocked on: game dump location (`GAME_DIR`). Ask the user.

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
