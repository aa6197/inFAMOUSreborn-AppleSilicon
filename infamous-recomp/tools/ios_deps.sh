#!/bin/zsh
# Build the static libraries the iPad/iPhone app links:
# SDL3, glslang, SPIRV-Tools, SPIRV-Cross (arm64, static).
# Gets the sources with git the first time.
#
#   ios_deps.sh [iphoneos|iphonesimulator]      (default iphoneos)
#
# DEPS_DIR (env): where sources and libraries go. Default ~/inFamous/deps.
# PYTHON   (env): Python 3.10 or newer. Default Homebrew python3.12.
# Out: $DEPS_DIR/<sdk>  (lib/, include/, lib/cmake/, lib/pkgconfig/)
set -eu
SDK=${1:-iphoneos}
case $SDK in iphoneos|iphonesimulator) ;; *) echo "unknown sdk $SDK"; exit 2 ;; esac
D=${DEPS_DIR:-$HOME/inFamous/deps}
PY=${PYTHON:-/opt/homebrew/bin/python3.12}
SRC=$D/src
P=$D/$SDK
B=$D/build-$SDK
mkdir -p $SRC $P $B

KV=vulkan-sdk-1.4.363.0
GL=16.6.0
SV=3.4.18

fetch() {   # fetch <folder> <github repo> <tag>
    [[ -d $SRC/$1 ]] && return
    echo "=== get $2 $3"
    git -c advice.detachedHead=false clone --quiet --depth 1 --branch $3 \
        https://github.com/$2.git $SRC/$1
}
fetch SPIRV-Headers-$KV KhronosGroup/SPIRV-Headers $KV
fetch SPIRV-Tools-$KV   KhronosGroup/SPIRV-Tools   $KV
fetch SPIRV-Cross-$KV   KhronosGroup/SPIRV-Cross   $KV
fetch glslang-$GL       KhronosGroup/glslang       $GL
fetch SDL3-$SV          libsdl-org/SDL             release-$SV

COMMON=(-G Ninja -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=$SDK -DCMAKE_OSX_ARCHITECTURES=arm64
  -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0
  -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_PREFIX_PATH=$P -DCMAKE_FIND_ROOT_PATH=$P
  -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY
  -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER
  -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON
  -DPython3_EXECUTABLE=$PY)
export PKG_CONFIG_LIBDIR=$P/lib/pkgconfig

lib() {   # lib <name> <source folder> [cmake args...]
    local n=$1 s=$2; shift 2
    echo "=== $n ($SDK)"
    rm -f $B/$n.log
    if ! { cmake -S $SRC/$s -B $B/$n $COMMON "$@" &&
           cmake --build $B/$n --target install --parallel 6; } >> $B/$n.log 2>&1; then
        echo "failed: see $B/$n.log"
        exit 1
    fi
}

lib spirv-headers SPIRV-Headers-$KV -DSPIRV_HEADERS_ENABLE_TESTS=OFF
lib spirv-tools SPIRV-Tools-$KV \
  -DSPIRV-Headers_SOURCE_DIR=$SRC/SPIRV-Headers-$KV \
  -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON -DSPIRV_WERROR=OFF \
  -DSPIRV_TOOLS_BUILD_STATIC=ON
lib glslang glslang-$GL \
  -DALLOW_EXTERNAL_SPIRV_TOOLS=ON -DENABLE_OPT=ON -DENABLE_HLSL=ON \
  -DGLSLANG_TESTS=OFF -DGLSLANG_ENABLE_INSTALL=ON -DENABLE_GLSLANG_BINARIES=OFF \
  -DENABLE_SPVREMAPPER=OFF
lib spirv-cross SPIRV-Cross-$KV \
  -DSPIRV_CROSS_STATIC=ON -DSPIRV_CROSS_SHARED=OFF -DSPIRV_CROSS_CLI=OFF \
  -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_ENABLE_C_API=ON \
  -DSPIRV_CROSS_ENABLE_MSL=ON -DSPIRV_CROSS_ENABLE_GLSL=ON -DSPIRV_CROSS_ENABLE_HLSL=ON \
  -DSPIRV_CROSS_ENABLE_REFLECT=ON -DSPIRV_CROSS_ENABLE_CPP=ON -DSPIRV_CROSS_ENABLE_UTIL=ON
lib sdl3 SDL3-$SV \
  -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF

echo "=== done: $P"
