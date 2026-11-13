#!/bin/zsh
# Make the Apple release files. They hold the program only: no game files.
# The player picks their own disc image or game folder on first start.
#
#   release_apple.sh [version] [out dir]
#
# In:   infamous-recomp/build-mac-release   macOS release build (static deps, see README)
#       infamous-recomp/build-ios           iPadOS/iOS device build (Release-iphoneos);
#                                           IOS_BUILD=<dir> takes another one (e.g. build-ios-ss)
# Out:  inFamous-<ver>-mac.zip      inFamous.app (ad-hoc signed, not notarized)
#       inFamous-<ver>-ios.ipa      ad-hoc signed; keeps the two kernel entitlements.
#                                   SideStore, AltStore or Sideloadly re-sign it.
#       THIRD_PARTY_NOTICES.txt     licences of the code linked into both apps
#       SHA256SUMS.txt
# A missing build is skipped with a note. Every bundle is checked for game
# files before it is packed; one hit stops the script.
#
# out dir:  default ~/inFamous/dist/<ver>
# DEPS_DIR: as for tools/ios_deps.sh, default ~/inFamous/deps.
# DEPS_SRC: the dependency sources (licence files), default $DEPS_DIR/src.
set -eu
HERE=${0:A:h}
ROOT=${HERE:h}                  # infamous-recomp/
REPO=${ROOT:h}
VER=${1:-0.1}
OUT=${2:-$HOME/inFamous/dist/$VER}
DEPS_SRC=${DEPS_SRC:-${DEPS_DIR:-$HOME/inFamous/deps}/src}
STAGE=$OUT/.stage
rm -rf $STAGE
mkdir -p $OUT $STAGE

# --- game file check ----------------------------------------------------------
# Names that only come from a PS3 disc or from this game, plus anything big
# that is not the program itself.
check_bundle() {
    local b=$1 exe=$2 bad
    bad=$(find $b -type f | grep -i -E \
        '(EBOOT|PARAM\.SFO|PS3_DISC\.SFB|\.elf$|\.self$|\.sprx$|\.psarc$|\.pkg$|\.iso$|\.dkey$|\.bik$|\.at3$|\.edat$|\.sdat$|\.pam$|ui_font\.bin$|kbm_prompts\.bin$|/USRDIR/|/PS3_GAME/)' || true)
    bad+=$(find $b -type f -size +20M ! -path "*/$exe" || true)
    if [[ -n $bad ]]; then
        echo "STOP: possible game files in $b:"
        echo $bad
        exit 1
    fi
    echo "  checked: no game files in ${b:t} ($(find $b -type f | wc -l | tr -d ' ') files)"
}

made=()

# --- macOS ----------------------------------------------------------------------
if [[ -x $ROOT/build-mac-release/inFamousRecomp ]]; then
    echo "== macOS"
    $ROOT/tools/make_mac_app.sh $ROOT/build-mac-release $STAGE/mac $VER
    check_bundle $STAGE/mac/inFamous.app inFamous
    rm -f $OUT/inFamous-$VER-mac.zip
    ditto -c -k --norsrc --keepParent $STAGE/mac/inFamous.app $OUT/inFamous-$VER-mac.zip
    made+=(inFamous-$VER-mac.zip)
else
    echo "== macOS: no build-mac-release, skipped"
fi

# --- iPadOS / iOS -----------------------------------------------------------------
IOS_APP=${IOS_BUILD:-$ROOT/build-ios}/Release-iphoneos/inFamousRecomp.app
if [[ -d $IOS_APP ]]; then
    echo "== iPadOS / iOS (${IOS_APP:h:h:t})"
    mkdir -p $STAGE/ios/Payload
    ditto $IOS_APP $STAGE/ios/Payload/inFamousRecomp.app
    A=$STAGE/ios/Payload/inFamousRecomp.app
    # The developer's profile lists their team and device IDs: never ship it.
    rm -rf $A/embedded.mobileprovision $A/_CodeSignature
    cat > $STAGE/ios/ent.plist <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>com.apple.developer.kernel.extended-virtual-addressing</key>
	<true/>
	<key>com.apple.developer.kernel.increased-memory-limit</key>
	<true/>
</dict>
</plist>
EOF
    codesign --force --sign - --timestamp=none --entitlements $STAGE/ios/ent.plist $A
    check_bundle $A inFamousRecomp
    rm -f $OUT/inFamous-$VER-ios.ipa
    (cd $STAGE/ios && zip -qr -X $OUT/inFamous-$VER-ios.ipa Payload)
    made+=(inFamous-$VER-ios.ipa)
else
    echo "== iPadOS / iOS: no build-ios device build, skipped"
fi

# --- licences ---------------------------------------------------------------------
N=$OUT/THIRD_PARTY_NOTICES.txt
{
    echo "inFamous (unofficial port) $VER - third-party notices"
    echo "No game files are included. The apps need your own copy of the game."
    echo
    notice() {
        echo "================================================================"
        echo "$1"
        echo "================================================================"
        cat $2
        echo
    }
    notice "ps3recomp (MIT)" $REPO/ps3recomp/LICENSE
    notice "sse2neon (MIT)" $REPO/ps3recomp/third_party/sse2neon/LICENSE
    notice "xxHash (BSD 2-Clause)" $REPO/ps3recomp/libs/video/xxhash_LICENSE.txt
    notice "at3_standalone ATRAC3 decoder (LGPL 2.1)" $REPO/ps3recomp/third_party/at3_standalone/COPYING.LGPLv2.1
    notice "SDL 3 (zlib)" $DEPS_SRC/SDL3-*/LICENSE.txt
    notice "glslang" $DEPS_SRC/glslang-*/LICENSE.txt
    notice "SPIRV-Tools (Apache 2.0)" $DEPS_SRC/SPIRV-Tools-*/LICENSE
    notice "SPIRV-Cross (Apache 2.0)" $DEPS_SRC/SPIRV-Cross-*/LICENSE
    notice "SPIRV-Headers (MIT)" $DEPS_SRC/SPIRV-Headers-*/LICENSE
} > $N
made+=(THIRD_PARTY_NOTICES.txt)

rm -rf $STAGE
(cd $OUT && shasum -a 256 ${made:#THIRD_PARTY_NOTICES.txt} > SHA256SUMS.txt)
echo "== done: $OUT"
ls -la $OUT
