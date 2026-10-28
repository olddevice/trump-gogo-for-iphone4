#!/bin/bash
#
# build.sh — full build pipeline for the jailbreak iPhone 4 TrumpGoGo app.
#
#   1. run host-side world-simulation unit tests (native clang, no SDK)
#   2. bake the runtime assets (sprite sheet + font -> raw premultiplied .bin)
#   3. render a few frames on the host so the scene can be eyeballed locally
#   4. cross-compile C + Objective-C for armv7 with the iPhoneOS 10.3 SDK
#   5. fake-sign with ldid (jailbroken devices with AppSync accept this)
#   5. assemble TrumpGoGo.app, package TrumpGoGo.ipa and
#      TrumpGoGo_x.y.z_iphoneos-arm.deb
#
# Requirements: Xcode Command Line Tools (clang) and the SDK in ./toolchain
# (auto-downloaded on first run). No Homebrew, no Theos, no Xcode project.
# Baking assets needs Pillow; it is skipped when the .bin files already exist.
#
set -euo pipefail

# Pin the core file tools to the real macOS binaries: some launchers put
# wrapper shims ahead of them on PATH, and those shims can fail inside
# restricted shells. Harmless in a normal Terminal.
for _t in mkdir cp rm mv tar ar zip chmod ln; do
    for _d in /bin /usr/bin; do
        if [ -x "$_d/$_t" ]; then eval "$_t() { \"$_d/$_t\" \"\$@\"; }"; break; fi
    done
done
unset _t _d

ROOT="$(cd "$(dirname "$0")" && pwd)"
SRC="$ROOT/src"
SDK="$ROOT/toolchain/iPhoneOS10.3.sdk"
BUILD="$ROOT/build"
APP_NAME="TrumpGoGo"
MINOS="7.0"
BUNDLE_ID="com.claw.trumpgogo"

log()  { printf '\033[1;32m==> %s\033[0m\n' "$*"; }
fail() { printf '\033[1;31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

mkdir -p "$BUILD"

# --------------------------------------------------------------------------
# 0. host-side unit tests — validate the ported world before touching iOS
# --------------------------------------------------------------------------
log "Running world unit tests on host"
clang -Wall -Wextra -O1 "$SRC/trump_game.c" "$SRC/test_game.c" \
      -o "$BUILD/game_test" -lm
"$BUILD/game_test"

# --------------------------------------------------------------------------
# 0b. runtime assets: the renderer wants raw premultiplied pixels and an
#     8-bit coverage font, both baked once here so the device never has to
#     decode a PNG (and the host harness reads the very same bytes).
# --------------------------------------------------------------------------
if [ ! -f "$ROOT/assets/atlas.bin" ] || [ ! -f "$ROOT/assets/font.bin" ]; then
    log "Baking assets (atlas.bin, font.bin)"
    PY=""
    for cand in python3 /Users/apple/.workbuddy/binaries/python/envs/default/bin/python; do
        if command -v "$cand" >/dev/null 2>&1 && \
           "$cand" -c "import PIL" >/dev/null 2>&1; then PY="$cand"; break; fi
    done
    [ -n "$PY" ] || fail "Pillow not found — pip install pillow, or drop atlas.bin/font.bin in assets/"
    "$PY" "$ROOT/tools/gen_assets.py"
fi

# --------------------------------------------------------------------------
# 1. host render check — the scene is pure C, so these are the phone's frames.
#    Frames land in build/host/N.bmp; errors are fatal because a broken scene
#    is not worth packaging.
# --------------------------------------------------------------------------
log "Rendering reference frames on host"
mkdir -p "$BUILD/host"
clang -Wall -Wextra -O2 -I"$SRC" "$ROOT/tools/hostshot.c" \
      "$SRC/gogo_scene.c" "$SRC/tgraster.c" "$SRC/trump_game.c" \
      -o "$BUILD/hostshot" -lm
"$BUILD/hostshot" "$ROOT/assets" "$BUILD/host" 0

# --------------------------------------------------------------------------
# 2. toolchain
# --------------------------------------------------------------------------
log "Checking toolchain"
if [ ! -d "$SDK" ]; then
    log "SDK missing — downloading iPhoneOS10.3.sdk from theos/sdks (~160 MB)"
    TARBALL="$BUILD/sdks.tar.gz"
    curl -fL --retry 3 --retry-delay 2 \
         "https://codeload.github.com/theos/sdks/tar.gz/refs/heads/master" \
         -o "$TARBALL"
    mkdir -p "$ROOT/toolchain"
    tar -xzf "$TARBALL" --include 'sdks-master/iPhoneOS10.3.sdk/*' -C "$ROOT/toolchain"
    mv "$ROOT/toolchain/sdks-master/iPhoneOS10.3.sdk" "$ROOT/toolchain/"
    rmdir "$ROOT/toolchain/sdks-master"
    rm -f "$TARBALL"
fi
[ -f "$SDK/usr/lib/libobjc.tbd" ] || fail "SDK at $SDK looks incomplete"

command -v clang >/dev/null || fail "clang not found — install Xcode Command Line Tools"

LDID_BIN="$ROOT/toolchain/ldid"
if [ -x "$LDID_BIN" ]; then
    :
elif command -v ldid >/dev/null; then
    LDID_BIN="$(command -v ldid)"
else
    fail "ldid not found (expected at $ROOT/toolchain/ldid)"
fi

# icons are generated from the sprite atlas (needs python3 + Pillow)
if [ ! -f "$ROOT/assets/icon@2x.png" ] && [ -x "$ROOT/make_icons.py" ]; then
    log "Generating launcher icons"
    "$ROOT/make_icons.py" || log "icon generation skipped"
fi

# --------------------------------------------------------------------------
# 2. cross-compile for armv7 (iPhone 4 = A4, Cortex-A8)
# --------------------------------------------------------------------------
log "Compiling armv7 objects"
COMMON=(-arch armv7
        -isysroot "$SDK"
        -miphoneos-version-min="$MINOS"
        -O2 -fno-objc-arc -fvisibility=hidden   # MRC: avoids libarclite
        # -O2, not -Os: at -Os clang refuses to inline the pixel blend and the
        # resulting out-of-line call per pixel is ~100x slower on an A4
        -Wall)
rm -f "$BUILD"/*.o
for f in "$SRC"/*.c "$SRC"/*.m; do
    case "$(basename "$f")" in
        test_game.c) continue ;;   # host-only
    esac
    obj="$BUILD/$(basename "${f%.*}").o"
    clang "${COMMON[@]}" -c "$f" -o "$obj"
done

log "Linking $APP_NAME (armv7)"
clang "${COMMON[@]}" \
      -o "$BUILD/$APP_NAME" \
      "$BUILD"/*.o \
      -lobjc \
      -framework UIKit \
      -framework Foundation \
      -framework CoreGraphics \
      -framework QuartzCore \
      -framework CoreMotion \
      -framework AVFoundation \
      -framework AudioToolbox

file "$BUILD/$APP_NAME" | grep -q "arm_v7" || fail "binary is not armv7?!"

# --------------------------------------------------------------------------
# 2b. ios_launch helper: opens the app through LaunchServices, which is the
#     only way to get it activated from a shell (see tools/ios_launch.m)
# --------------------------------------------------------------------------
log "Building ios_launch helper"
clang "${COMMON[@]}" -o "$BUILD/ios_launch" "$ROOT/tools/ios_launch.m" \
      -lobjc -framework Foundation
file "$BUILD/ios_launch" | grep -q "arm_v7" || fail "ios_launch is not armv7?!"

# --------------------------------------------------------------------------
# 2c. bench_dev: on-device frame cost + screenshots. Pure C, no UIKit and no
#     window server, so it runs over SSH even though a shell-launched UIKit
#     app never gets activated on this device.
# --------------------------------------------------------------------------
log "Building bench_dev probe"
clang "${COMMON[@]}" -I"$SRC" -o "$BUILD/bench_dev" "$ROOT/tools/bench_dev.c" \
      "$SRC/gogo_scene.c" "$SRC/tgraster.c" "$SRC/trump_game.c" -lm
file "$BUILD/bench_dev" | grep -q "arm_v7" || fail "bench_dev is not armv7?!"
"$LDID_BIN" -S "$BUILD/bench_dev"

# --------------------------------------------------------------------------
# 3. fake-sign (jailbroken devices + AppSync accept pseudo-signatures)
# --------------------------------------------------------------------------
log "Pseudo-signing with ldid ($LDID_BIN)"
"$LDID_BIN" -S "$BUILD/$APP_NAME"

# --------------------------------------------------------------------------
# 4. assemble .app bundle
# --------------------------------------------------------------------------
log "Assembling $APP_NAME.app"
APPDIR="$BUILD/$APP_NAME.app"
rm -rf "$APPDIR" "$BUILD/Payload" "$BUILD/$APP_NAME.ipa"
mkdir -p "$APPDIR"
cp "$BUILD/$APP_NAME"           "$APPDIR/"
cp "$ROOT/packaging/Info.plist" "$APPDIR/Info.plist"
cp "$ROOT/assets/atlas.bin"     "$APPDIR/"
cp "$ROOT/assets/font.bin"      "$APPDIR/"
cp "$ROOT/assets/coin.wav"      "$APPDIR/"
cp "$ROOT/assets/icon.png"      "$APPDIR/"
cp "$ROOT/assets/icon@2x.png"   "$APPDIR/"
chmod 755 "$APPDIR/$APP_NAME"

# --------------------------------------------------------------------------
# 5. IPA (Payload/<App>.app, zipped)
# --------------------------------------------------------------------------
log "Building $APP_NAME.ipa"
cd "$BUILD"
mkdir -p Payload
cp -R "$APP_NAME.app" Payload/
zip -qr "$APP_NAME.ipa" Payload
rm -rf Payload

# --------------------------------------------------------------------------
# 6. Debian package for Cydia (Architecture: iphoneos-arm)
# --------------------------------------------------------------------------
log "Building .deb"
VERSION="$(awk -F': ' '/^Version:/{print $2; exit}' "$ROOT/packaging/control")"
DEB="$BUILD/${APP_NAME}_${VERSION}_iphoneos-arm.deb"
STAGE="$BUILD/debroot"
rm -rf "$STAGE" "$DEB"
mkdir -p "$STAGE/DEBIAN" "$STAGE/Applications"

cp "$ROOT/packaging/control" "$STAGE/DEBIAN/control"
cp "$ROOT/packaging/postinst" "$STAGE/DEBIAN/postinst"
chmod 755 "$STAGE/DEBIAN/postinst"
cp -R "$APP_NAME.app" "$STAGE/Applications/"

build_deb_manual() {
    # pure macOS tools (bsdtar + ar); produces a perfectly valid .deb
    local tmp="$BUILD/.debtmp"
    rm -rf "$tmp" && mkdir -p "$tmp"
    # control.tar.gz holds control/postinst at ITS ROOT (no DEBIAN/ wrapper) --
    # otherwise dpkg fails with "unable to open /var/lib/dpkg/tmp.ci//DEBIAN".
    ( cd "$STAGE/DEBIAN" && COPYFILE_DISABLE=1 tar czf "$tmp/control.tar.gz" \
        --uname root --gname wheel . )
    ( cd "$STAGE" && COPYFILE_DISABLE=1 tar czf "$tmp/data.tar.gz" \
        --uname root --gname wheel ./Applications )
    printf '2.0\n' > "$tmp/debian-binary"
    ( cd "$tmp" && rm -f "$DEB" && ar -r "$DEB" debian-binary control.tar.gz data.tar.gz )
    rm -rf "$tmp"
}

if command -v dpkg-deb >/dev/null; then
    dpkg-deb --root-owner-group -Zgzip -b "$STAGE" "$DEB"
else
    log "dpkg-deb not found — building deb with ar/tar fallback"
    build_deb_manual
fi
rm -rf "$STAGE"

# --------------------------------------------------------------------------
# done
# --------------------------------------------------------------------------
log "Build complete"
ls -lh "$BUILD/$APP_NAME.ipa" "$DEB"
echo
echo "  IPA : $BUILD/$APP_NAME.ipa   (usable with AppSync / installers)"
echo "  DEB : $DEB   (Cydia / dpkg -i)"
echo "  Install on device:  ./push.sh"
