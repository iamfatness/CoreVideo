#!/bin/bash
# Assemble the loadable macOS OBS plugin bundle.
#
# The repo's install() rules produce the Windows layout (obs-plugins/64bit +
# data/obs-plugins/...), which macOS OBS cannot load: it wants a .plugin bundle
# whose Contents/MacOS holds the module. This script builds that bundle, and is
# the single place that knows the macOS-specific runtime fixups. Assembling it
# by hand is how the Qt double-load and missing-TLS-backend problems kept
# reappearing.
#
# Usage:
#   scripts/make-macos-bundle.sh --build-dir build [options]
#
#   --build-dir DIR   CMake build directory (required)
#   --out DIR         Where to write the .plugin (default: <build-dir>)
#   --qt-prefix DIR   Qt prefix supplying the TLS backends (default: auto)
#   --obs-app PATH    OBS.app to resolve Qt against (default: /Applications/OBS.app)
#   --zoom-sdk DIR    Zoom macOS SDK runtime (default: $ZOOM_SDK_DIR, then
#                     ~/Developer/zoom-sdk-macos)
#   --link-sdk        Symlink the SDK into the engine app instead of copying it.
#                     Fast for local iteration; NOT distributable.
#   --install         Also copy into ~/Library/Application Support/obs-studio/plugins
#   --sign IDENTITY   codesign identity (default: "-", ad-hoc)
#
# Ad-hoc signing is fine for local runs. A distributable build needs a Developer
# ID identity plus notarization, which this script does not do.

set -euo pipefail

BUNDLE_NAME="obs-zoom-plugin.plugin"
BUILD_DIR=""
OUT_DIR=""
QT_PREFIX=""
OBS_APP="/Applications/OBS.app"
ZOOM_SDK="${ZOOM_SDK_DIR:-$HOME/Developer/zoom-sdk-macos}"
LINK_SDK=0
DO_INSTALL=0
SIGN_ID="-"

while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --out)       OUT_DIR="$2";   shift 2 ;;
        --qt-prefix) QT_PREFIX="$2"; shift 2 ;;
        --obs-app)   OBS_APP="$2";   shift 2 ;;
        --zoom-sdk)  ZOOM_SDK="$2";  shift 2 ;;
        --sign)      SIGN_ID="$2";   shift 2 ;;
        --link-sdk)  LINK_SDK=1;     shift ;;
        --install)   DO_INSTALL=1;   shift ;;
        -h|--help)   sed -n '2,31p' "$0"; exit 0 ;;
        *) echo "error: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

[ -n "$BUILD_DIR" ] || { echo "error: --build-dir is required" >&2; exit 2; }
[ -d "$BUILD_DIR" ] || { echo "error: build dir '$BUILD_DIR' does not exist" >&2; exit 2; }
OUT_DIR="${OUT_DIR:-$BUILD_DIR}"
SRC_DIR="$(cd "$(dirname "$0")/.." && pwd)"

MODULE="$BUILD_DIR/obs-zoom-plugin.so"
[ -f "$MODULE" ] || { echo "error: $MODULE not found; build first" >&2; exit 1; }

OBS_FRAMEWORKS="$OBS_APP/Contents/Frameworks"
[ -d "$OBS_FRAMEWORKS" ] || {
    echo "error: $OBS_FRAMEWORKS not found; pass --obs-app" >&2; exit 1; }

# Qt prefix: only needed for the TLS backends. Derive it from the module's own
# rpath when not given, since that is the Qt this build linked against.
if [ -z "$QT_PREFIX" ]; then
    for rp in $(otool -l "$MODULE" | awk '/LC_RPATH/{f=1} f&&/path /{print $2; f=0}'); do
        if [ -d "$rp/../plugins/tls" ]; then QT_PREFIX="$(cd "$rp/.." && pwd)"; break; fi
    done
fi

BUNDLE="$OUT_DIR/$BUNDLE_NAME"
rm -rf "$BUNDLE"
mkdir -p "$BUNDLE/Contents/MacOS" "$BUNDLE/Contents/Resources"

VERSION="$(sed -n 's/^#define[[:space:]]*OBS_ZOOM_PLUGIN_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' \
    "$BUILD_DIR/obs-zoom-version.h" 2>/dev/null | head -1)"
VERSION="${VERSION:-0.0.0}"

cat > "$BUNDLE/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key><string>en</string>
    <key>CFBundleExecutable</key><string>obs-zoom-plugin</string>
    <key>CFBundleIdentifier</key><string>us.iamfatness.corevideo.obs-zoom-plugin</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>CFBundleName</key><string>obs-zoom-plugin</string>
    <key>CFBundlePackageType</key><string>BNDL</string>
    <key>CFBundleShortVersionString</key><string>$VERSION</string>
    <key>CFBundleVersion</key><string>$VERSION</string>
    <key>LSMinimumSystemVersion</key><string>12.0</string>
</dict>
</plist>
PLIST

cp "$MODULE" "$BUNDLE/Contents/MacOS/obs-zoom-plugin"
[ -d "$SRC_DIR/data/locale" ] && cp -R "$SRC_DIR/data/locale" "$BUNDLE/Contents/Resources/"

# The engine and the OAuth helper must sit BESIDE the module: both are resolved
# relative to the loaded module (dladdr) at runtime.
#
# The engine ships as an .app, not a loose executable. That is a hard runtime
# requirement, not tidiness: the macOS Zoom SDK loads its runtime bundles
# (ssb_sdk, zNet, zPTUIEx, ...) through the MAIN BUNDLE's Frameworks directory.
# Run the engine as a bare binary and initSDKWithParams still reports success
# while sdkAuth then fails synchronously with no delegate callback and no
# explanation. See engine/src/main-macos.mm.
ENGINE_APP="$BUNDLE/Contents/MacOS/ZoomObsEngine.app"
if [ -f "$BUILD_DIR/ZoomObsEngine" ]; then
    mkdir -p "$ENGINE_APP/Contents/MacOS"
    cp "$BUILD_DIR/ZoomObsEngine" "$ENGINE_APP/Contents/MacOS/ZoomObsEngine"
    cat > "$ENGINE_APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key><string>en</string>
    <key>CFBundleExecutable</key><string>ZoomObsEngine</string>
    <key>CFBundleIdentifier</key><string>us.iamfatness.corevideo.ZoomObsEngine</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>CFBundleName</key><string>ZoomObsEngine</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$VERSION</string>
    <key>CFBundleVersion</key><string>$VERSION</string>
    <key>LSMinimumSystemVersion</key><string>12.0</string>
    <key>NSCameraUsageDescription</key><string>CoreVideo captures Zoom meeting video for OBS.</string>
    <key>NSMicrophoneUsageDescription</key><string>CoreVideo captures Zoom meeting audio for OBS.</string>
</dict>
</plist>
PLIST

    if [ -d "$ZOOM_SDK/ZoomSDK.framework" ]; then
        if [ "$LINK_SDK" -eq 1 ]; then
            ln -s "$ZOOM_SDK" "$ENGINE_APP/Contents/Frameworks"
            echo "note: engine SDK is a symlink to $ZOOM_SDK (dev only, not distributable)"
        else
            ditto "$ZOOM_SDK" "$ENGINE_APP/Contents/Frameworks"
        fi
    else
        echo "warning: no ZoomSDK.framework under '$ZOOM_SDK'; the engine will" >&2
        echo "         report sdk_runtime_missing and cannot authenticate." >&2
        echo "         Pass --zoom-sdk DIR or set ZOOM_SDK_DIR." >&2
    fi

    # The build tree binary carries an absolute rpath to the developer's SDK
    # checkout. Leave it in place and the shipped engine silently prefers that
    # machine-specific path over its own bundled copy, so the bundle would be
    # untested on every machine but this one.
    if [ ! -L "$ENGINE_APP/Contents/Frameworks" ]; then
        while :; do
            rp="$(otool -l "$ENGINE_APP/Contents/MacOS/ZoomObsEngine" \
                  | awk '/LC_RPATH/{f=1} f&&/path /{print $2; exit}')"
            [ -n "$rp" ] || break
            install_name_tool -delete_rpath "$rp" \
                "$ENGINE_APP/Contents/MacOS/ZoomObsEngine" 2>/dev/null || break
        done
        install_name_tool -add_rpath "@executable_path/../Frameworks" \
            "$ENGINE_APP/Contents/MacOS/ZoomObsEngine"
    fi
fi
[ -d "$BUILD_DIR/CoreVideoOAuthCallback.app" ] && \
    ditto "$BUILD_DIR/CoreVideoOAuthCallback.app" \
          "$BUNDLE/Contents/MacOS/CoreVideoOAuthCallback.app"

# Qt TLS backends. OBS.app bundles Qt but ships no TLS backend at all, so
# without these every https request fails with "TLS initialization failed".
# plugin-main.cpp adds Contents/MacOS/plugins to the Qt library paths.
if [ -n "$QT_PREFIX" ] && [ -d "$QT_PREFIX/plugins/tls" ]; then
    mkdir -p "$BUNDLE/Contents/MacOS/plugins/tls"
    for lib in libqsecuretransportbackend.dylib libqcertonlybackend.dylib; do
        if [ -f "$QT_PREFIX/plugins/tls/$lib" ]; then
            cp "$QT_PREFIX/plugins/tls/$lib" "$BUNDLE/Contents/MacOS/plugins/tls/"
        else
            echo "warning: Qt TLS backend $lib not found; https will fail at runtime" >&2
        fi
    done
else
    echo "warning: no Qt plugins/tls dir found; https will fail at runtime" >&2
fi

# Repoint every Qt-linking binary at OBS's own frameworks. Left alone they point
# at the build machine's Qt, which would load a SECOND Qt runtime into a process
# that already has OBS's -- duplicate QApplication/meta-object state, i.e. a
# crash. Replace each existing rpath rather than adding one, so the build Qt is
# not merely deprioritised but unreachable.
#
# Strip every existing rpath and add exactly one. Replacing entries in place is
# not deterministic when a binary carries several Qt rpaths -- the load-command
# list shifts under you and you end up with duplicates. Both targets here (the
# module and the Qt TLS backends) use rpath solely to find Qt, so removing all of
# them is safe; do not reuse this on a binary that resolves anything else that
# way.
repoint_rpaths() {
    local target="$1" rp
    while :; do
        rp="$(otool -l "$target" | awk '/LC_RPATH/{f=1} f&&/path /{print $2; exit}')"
        [ -n "$rp" ] || break
        install_name_tool -delete_rpath "$rp" "$target" 2>/dev/null || break
    done
    install_name_tool -add_rpath "$OBS_FRAMEWORKS" "$target"
}

repoint_rpaths "$BUNDLE/Contents/MacOS/obs-zoom-plugin"
for f in "$BUNDLE/Contents/MacOS/plugins/tls/"*.dylib; do
    [ -f "$f" ] && repoint_rpaths "$f"
done

# Sign nested code BEFORE the enclosing bundle: signing the outer bundle seals
# its contents, so signing anything inside afterwards invalidates that seal.
for f in "$BUNDLE/Contents/MacOS/plugins/tls/"*.dylib; do
    [ -f "$f" ] && codesign --force --sign "$SIGN_ID" "$f"
done
[ -d "$ENGINE_APP" ] && \
    codesign --force --sign "$SIGN_ID" "$ENGINE_APP"
[ -d "$BUNDLE/Contents/MacOS/CoreVideoOAuthCallback.app" ] && \
    codesign --force --sign "$SIGN_ID" "$BUNDLE/Contents/MacOS/CoreVideoOAuthCallback.app"
codesign --force --sign "$SIGN_ID" "$BUNDLE"
# A symlinked SDK is deliberately unsealable, so --strict would fail on exactly
# the builds we know are dev-only. Verify the real thing; say so for the other.
if [ "$LINK_SDK" -eq 1 ]; then
    echo "note: skipping codesign --verify --strict (--link-sdk bundle is not sealable)"
else
    codesign --verify --strict "$BUNDLE"
fi

echo "built: $BUNDLE (version $VERSION)"

if [ "$DO_INSTALL" -eq 1 ]; then
    DEST="$HOME/Library/Application Support/obs-studio/plugins"
    if pgrep -x OBS >/dev/null 2>&1; then
        echo "warning: OBS is running; it will not load the new build until restarted" >&2
    fi
    mkdir -p "$DEST"
    rm -rf "${DEST:?}/$BUNDLE_NAME"
    ditto "$BUNDLE" "$DEST/$BUNDLE_NAME"
    echo "installed: $DEST/$BUNDLE_NAME"
fi
