sse2neon: Intel SSE intrinsics on Arm NEON (MIT, see LICENSE).

ps3recomp: vendored from https://github.com/DLTcollab/sse2neon v1.9.1,
unmodified. Used only on arm64 hosts, through include/ps3emu/host_simd.h,
which turns on its precise min/max, div, sqrt and dot-product modes so that
float results match x86 rather than NEON's faster approximations.
