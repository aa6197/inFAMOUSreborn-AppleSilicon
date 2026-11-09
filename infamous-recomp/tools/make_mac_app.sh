#!/bin/zsh
# Package a macOS build of the Apple port as inFamous.app.
#
#   make_mac_app.sh <build dir> <out dir> [version]
#
# The app holds the program, Info.plist and launch.env (the launcher
# environment from installer/infamous.toml). No game files: the player picks
# their own disc image or game folder on first start (mac/mac_app.mm).
#
# The program must link only system libraries (a release build:
# -DPS3RECOMP_MAC_SDL3=ON against static deps, see README). DEV_APP=1 skips
# that check for a local test of a Homebrew-linked build; such an app runs
# only on this Mac.
set -eu
HERE=${0:A:h}
ROOT=${HERE:h}                  # infamous-recomp/
REPO=${ROOT:h}
B=${1:?build dir}
OUT=${2:?out dir}
VER=${3:-0.1}
PY=${PYTHON:-/opt/homebrew/bin/python3.12}
EXE=$B/inFamousRecomp
APP=$OUT/inFamous.app

[[ -x $EXE ]] || { echo "no program at $EXE"; exit 1; }

# Only system libraries (/System, /usr/lib) may be linked.
other=$(otool -L $EXE | sed 1d | awk '{print $1}' | grep -v -E '^(/System/Library/|/usr/lib/)' || true)
if [[ -n $other && ${DEV_APP:-0} != 1 ]]; then
    echo "the program links libraries that are not part of macOS:"
    echo "$other"
    echo "build it as a release build (see README) or set DEV_APP=1 for a local test"
    exit 1
fi
MINOS=$(vtool -show-build $EXE | awk '/minos/ {print $2; exit}')

rm -rf $APP
mkdir -p $APP/Contents/MacOS $APP/Contents/Resources
cp $EXE $APP/Contents/MacOS/inFamous
sed -e "s/@VERSION@/$VER/g" -e "s/@MINOS@/$MINOS/g" $ROOT/mac/Info.plist > $APP/Contents/Info.plist
printf 'APPL????' > $APP/Contents/PkgInfo
$PY $ROOT/ios/gen_launch_env.py $REPO/installer/infamous.toml $APP/Contents/Resources/launch.env
[[ -f $ROOT/mac/AppIcon.icns ]] && cp $ROOT/mac/AppIcon.icns $APP/Contents/Resources/ && \
    plutil -insert CFBundleIconFile -string AppIcon $APP/Contents/Info.plist

# Ad-hoc signature: Apple silicon will not run unsigned code. Not notarized,
# so the first start needs right-click > Open (see README).
xattr -cr $APP   # stray Finder/quarantine attributes break the signature check
codesign --force --sign - --timestamp=none $APP
codesign --verify --strict $APP
echo "$APP  (macOS $MINOS+, version $VER)"
