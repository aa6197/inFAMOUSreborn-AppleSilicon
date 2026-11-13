#!/bin/zsh
# Build the iPad/iPhone app. With a device id it also installs it there.
# First run tools/ios_deps.sh, and make the game code with the installer
# ("install ... --no-build", see README).
#
#   ios_build.sh <team id> [device id]
#
# team id:   your Apple Developer team id (developer.apple.com → Account →
#            Membership details). Sign in to Xcode with that account first.
# device id: from "xcrun devicectl list devices". The device must be
#            plugged in or on the same network, and unlocked.
# BUNDLE_ID (env): app id. Default local.<your user name>.infamous
# DEPS_DIR  (env): same as for ios_deps.sh. Default ~/inFamous/deps
# BUILD_DIR (env): Default infamous-recomp/build-ios
# PYTHON    (env): Default Homebrew python3.12
# JOBS      (env): compile jobs. Each needs several GB of RAM. Default 4
# GAME      (env): your game folder. Default ~/inFamous/BCES00220. When it has
#                  PS3_GAME/USRDIR/EBOOT.elf, the app gets save states (the
#                  in-game menu's Save state / Load state; the file is only read)
# SAVE_STATES (env): 0 = build without save states
#
# Out: <build dir>/Release-iphoneos/inFamousRecomp.app
#      To make an .ipa for SideStore / AltStore, run tools/release_apple.sh after.
set -eu
if (( $# < 1 )); then
    echo "usage: ios_build.sh <team id> [device id]"
    exit 2
fi
TEAM=$1
DEV=${2:-}
HERE=${0:A:h}
ROOT=${HERE:h}                  # infamous-recomp/
REPO=${ROOT:h}
D=${DEPS_DIR:-$HOME/inFamous/deps}
P=$D/iphoneos
BD=${BUILD_DIR:-$ROOT/build-ios}
PY=${PYTHON:-/opt/homebrew/bin/python3.12}
BID=${BUNDLE_ID:-local.$(id -un | tr -cd 'a-zA-Z0-9').infamous}
APP=$BD/Release-iphoneos/inFamousRecomp.app
LOG=$BD/ios-build.log
ELF=${GAME:-$HOME/inFamous/BCES00220}/PS3_GAME/USRDIR/EBOOT.elf
ss=(-DPORT_SAVE_STATES=OFF)
if [[ ${SAVE_STATES:-1} != 0 && -f $ELF ]]; then ss=(-DPORT_SAVE_STATES=ON -DPORT_SAVE_STATES_ELF=$ELF); fi

[[ -d $P/lib ]] || { echo "no libraries in $P: run tools/ios_deps.sh first"; exit 1; }

echo "=== configure ($BID, team $TEAM, ${ss[1]#-DPORT_})"
mkdir -p $BD
export PKG_CONFIG_LIBDIR=$P/lib/pkgconfig
if ! cmake -S $ROOT -B $BD -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 \
  -DCMAKE_PREFIX_PATH=$P -DCMAKE_FIND_ROOT_PATH=$P \
  -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
  -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
  -DCMAKE_CONFIGURATION_TYPES=Release \
  -DPORT_PPU_REGCACHE=ON \
  -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" \
  -DCMAKE_OBJC_FLAGS_RELEASE="-O2 -DNDEBUG" -DCMAKE_OBJCXX_FLAGS_RELEASE="-O2 -DNDEBUG" \
  -DPS3RECOMP_DIR=$REPO/ps3recomp -DPython3_EXECUTABLE=$PY \
  ${ss[@]} \
  -DPORT_TEAM_ID=$TEAM -DPORT_BUNDLE_ID=$BID -DPORT_IOS_ENTITLEMENTS=ON > $BD/ios-configure.log 2>&1; then
    tail -20 $BD/ios-configure.log
    echo "configure failed: see $BD/ios-configure.log"
    exit 1
fi

echo "=== build (this takes a long time the first time; log: $LOG)"
if [[ -n $DEV ]]; then dest=(-destination id=$DEV -allowProvisioningDeviceRegistration)
else dest=(-destination generic/platform=iOS); fi
cd $ROOT
if ! xcodebuild -project $BD/inFamousRecomp.xcodeproj -scheme inFamousRecomp -configuration Release \
    ${dest[@]} -jobs ${JOBS:-4} -allowProvisioningUpdates build > $LOG 2>&1; then
    grep -E 'error:' $LOG | head -10
    echo "build failed: see $LOG"
    exit 1
fi
echo "built: $APP"

if [[ -n $DEV ]]; then
    echo "=== install on $DEV"
    xcrun devicectl device install app --device $DEV $APP
fi
