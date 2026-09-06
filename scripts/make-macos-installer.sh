#!/bin/bash
# Build the macOS .pkg installer for CoreVideo, from an already-built .plugin
# bundle (scripts/make-macos-bundle.sh produces that bundle; this script does
# not build or rebuild it).
#
# Usage:
#   scripts/make-macos-installer.sh --bundle PATH --version X.Y.Z [options]
#
#   --bundle PATH     The signed, notarized .plugin bundle to package
#                      (required). Not modified; read-only input.
#   --version X.Y.Z    Package version (required). Must match the bundle's own
#                      CFBundleShortVersionString -- see the mismatch check
#                      below for why that is a hard error, not a warning.
#   --out DIR          Where to write the .pkg (default: directory containing
#                      --bundle)
#   --sign IDENTITY    productbuild/pkgbuild signing identity. Omit for an
#                      unsigned local-test pkg (installable and runnable
#                      through `installer`, just not distributable or
#                      notarizable). A DISTRIBUTABLE build needs a
#                      "Developer ID Installer: ..." identity -- note that
#                      this is a DIFFERENT certificate from the
#                      "Developer ID Application: ..." one the bundle itself
#                      was signed with; `security find-identity -v` lists
#                      both kinds side by side and it is easy to paste the
#                      wrong one.
#   --notarize PROF    After signing, submit the .pkg to Apple with notarytool
#                      using this stored credential profile, then staple the
#                      ticket to the .pkg itself. Requires --sign; refuses
#                      without it (see below).
#
# THE INSTALL LOCATION IS PER-USER BY DESIGN, NOT A FLAG.
#
# The product owner chose ~/Library/Application Support/obs-studio/plugins
# specifically to avoid the admin-password prompt a system-wide /Library
# install would force. A .pkg's payload is normally written by a root-owned
# installer process, so "just write to $HOME" is not available the ordinary
# way -- $HOME inside a postinstall script IS ROOT'S HOME, and that is a
# classic way to silently install into /var/root instead of the real user's
# home. The mechanism that actually gets root's installer process to write
# into an unprivileged user's home is a Distribution package that declares:
#     <domains enable_currentUserHome="true" enable_localSystem="false"/>
# combined with a pkgbuild --install-location that is RELATIVE (no leading
# slash) -- an absolute path bypasses domain resolution entirely and pins the
# write to whatever that absolute path names. Both pieces below
# (INSTALL_LOCATION and the <domains> line in the generated distribution.xml)
# are load-bearing together; changing one without the other breaks this
# silently, so neither is exposed as a flag. Verified empirically on this
# machine (not merely assumed -- Apple has tightened per-user installs over
# the years): `installer -pkg out.pkg -target CurrentUserHomeDirectory` runs
# with NO password prompt, "Installing at base path" reports the real user's
# home, the payload lands owned by that user (not root), and
# `pkgutil --volume ~ --pkgs` shows the receipt registered against that
# user's home volume -- `pkgutil --pkgs` (no --volume) shows nothing, because
# a per-user install never touches the system-wide receipt database at all.

set -euo pipefail

IDENTIFIER="us.iamfatness.corevideo.obs-zoom-plugin"
INSTALL_LOCATION="Library/Application Support/obs-studio/plugins"
BUNDLE=""
OUT_DIR=""
VERSION=""
SIGN_ID=""
NOTARIZE_PROFILE=""

while [ $# -gt 0 ]; do
    case "$1" in
        --bundle)    BUNDLE="$2";   shift 2 ;;
        --out)       OUT_DIR="$2";  shift 2 ;;
        --version)   VERSION="$2";  shift 2 ;;
        --sign)      SIGN_ID="$2";  shift 2 ;;
        --notarize)  NOTARIZE_PROFILE="$2"; shift 2 ;;
        -h|--help)   sed -n '2,54p' "$0"; exit 0 ;;
        *) echo "error: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

# Validated HERE, before anything is built: this combination can only fail
# (Apple's notary service rejects an unsigned .pkg outright), and a 613 MB
# bundle takes long enough to package and upload that finding this out
# afterwards wastes minutes to say something knowable at argument time. Same
# pattern as make-macos-bundle.sh's --notarize/--sign check.
if [ -n "$NOTARIZE_PROFILE" ]; then
    [ -n "$SIGN_ID" ] || {
        echo "error: --notarize needs a --sign identity; Apple rejects an" >&2
        echo "       unsigned installer package." >&2; exit 2; }
fi

# Not fatal -- codesign/productbuild will refuse the wrong certificate TYPE on
# their own -- but this exact mistake is one line away: this project's two
# Developer ID identities differ only in the word "Application" vs
# "Installer", and only the latter is valid here.
if [ -n "$SIGN_ID" ]; then
    case "$SIGN_ID" in
        *Installer*) ;;
        *) echo "warning: --sign identity '$SIGN_ID' does not look like a" >&2
           echo "         \"Developer ID Installer\" certificate; pkgbuild" >&2
           echo "         and productbuild need the Installer identity, not" >&2
           echo "         the Application one used to sign the bundle." >&2 ;;
    esac
fi

[ -n "$BUNDLE" ] || { echo "error: --bundle is required" >&2; exit 2; }
[ -n "$VERSION" ] || { echo "error: --version is required" >&2; exit 2; }

# ── Validate the input bundle BEFORE doing any packaging work ──────────────
#
# An installer that cheerfully packages a broken bundle is worse than no
# installer: the person running it sees "install successful" and only finds
# out the plugin can't join a meeting (or doesn't load at all) much later,
# with no signal pointing back at "the .pkg was built from a bad bundle."
# Two bugs of exactly that shape (an engine-less bundle, a bundle missing its
# effects) were fixed in make-macos-bundle.sh the same day this script was
# written, both silent until someone noticed the symptom downstream. This
# script is not in the business of re-deriving every one of that script's
# checks, but the three failure modes explicitly in scope here -- missing,
# unsigned, engine-less -- are checked up front and hard.
[ -d "$BUNDLE" ] || {
    echo "error: bundle '$BUNDLE' does not exist" >&2; exit 1; }

BUNDLE_NAME="$(basename "$BUNDLE")"
INFO_PLIST="$BUNDLE/Contents/Info.plist"
[ -f "$INFO_PLIST" ] || {
    echo "error: '$BUNDLE' has no Contents/Info.plist; not a bundle" >&2
    exit 1; }

BUNDLE_EXEC="$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" \
    "$INFO_PLIST" 2>/dev/null)"
[ -n "$BUNDLE_EXEC" ] && [ -f "$BUNDLE/Contents/MacOS/$BUNDLE_EXEC" ] || {
    echo "error: '$BUNDLE' has no Contents/MacOS/\$CFBundleExecutable;" >&2
    echo "       the module binary itself is missing" >&2; exit 1; }

# codesign --verify --strict, not just `codesign -d`: the latter happily
# describes a signature that no longer matches the bundle's actual contents
# (e.g. touched after signing). --strict catches a broken seal. It does NOT,
# however, distinguish a Developer ID signature from an ad-hoc signature, so
# the distributable path performs a separate Authority check immediately
# below. pkgbuild itself will accept an ad-hoc-signed component.
codesign --verify --strict "$BUNDLE" 2>&1 || {
    echo "error: '$BUNDLE' failed codesign --verify --strict; it is" >&2
    echo "       unsigned or its signature no longer" >&2
    echo "       matches its contents. Build with make-macos-bundle.sh's" >&2
    echo "       --sign \"Developer ID Application: ...\" first." >&2
    exit 1; }

# A signed/notarized installer must contain Developer ID Application-signed
# code. `codesign --verify --strict` alone also succeeds for the default ad-hoc
# signature produced by make-macos-bundle.sh, which made it possible to sign
# the outer .pkg while silently leaving Gatekeeper-rejected code inside it.
# Keep accepting ad-hoc bundles for the explicitly unsigned local-test path.
if [ -n "$SIGN_ID" ] || [ -n "$NOTARIZE_PROFILE" ]; then
    BUNDLE_SIGNATURE="$(codesign -dvv "$BUNDLE" 2>&1)"
    printf '%s\n' "$BUNDLE_SIGNATURE" | \
        grep -q '^Authority=Developer ID Application:' || {
        echo "error: '$BUNDLE' is not signed with a Developer ID Application" >&2
        echo "       certificate. A valid/ad-hoc code seal is not sufficient" >&2
        echo "       for distribution through a signed installer." >&2
        printf '%s\n' "$BUNDLE_SIGNATURE" | \
            grep -E '^(Authority|TeamIdentifier|Signature)=' >&2 || true
        echo "       Rebuild it with make-macos-bundle.sh --sign" >&2
        echo "       \"Developer ID Application: ...\"." >&2
        exit 1
    }
fi

# The engine is not optional -- see make-macos-bundle.sh's own hard stop for
# the identical check and the live incident that motivated it (a bundle
# without ZoomObsEngine.app loads into OBS and LOOKS fine; it just can't join
# a meeting). This script trusts that stop for "was the bundle built right"
# and re-checks it anyway for "is the SPECIFIC bundle passed to --bundle,
# right now, the one that has it" -- the two scripts run at different times,
# often days apart, and nothing stops someone pointing --bundle at a stale or
# hand-edited copy.
ENGINE_APP="$BUNDLE/Contents/MacOS/ZoomObsEngine.app"
[ -d "$ENGINE_APP" ] && [ -f "$ENGINE_APP/Contents/MacOS/ZoomObsEngine" ] || {
    echo "error: '$BUNDLE' has no Contents/MacOS/ZoomObsEngine.app with its" >&2
    echo "       own executable; this bundle cannot join a meeting and must" >&2
    echo "       not be packaged. Rebuild with make-macos-bundle.sh (no" >&2
    echo "       --no-engine)." >&2
    exit 1; }

# The version stamped into the .pkg has to match what is actually inside it.
# pkgutil, Software Update-style upgrade detection, and a support ticket that
# quotes "Installer said 0.1.45 but Help > About says 0.1.44" all key off
# this number, and a --version typo here is invisible until someone hits one
# of those. Comparing against the bundle's own baked-in version turns that
# into a build-time error instead of a support ticket.
BUNDLE_VERSION="$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" \
    "$INFO_PLIST" 2>/dev/null)"
if [ -n "$BUNDLE_VERSION" ] && [ "$BUNDLE_VERSION" != "$VERSION" ]; then
    echo "error: --version '$VERSION' does not match the bundle's own" >&2
    echo "       CFBundleShortVersionString '$BUNDLE_VERSION'" >&2
    exit 2
fi

OUT_DIR="${OUT_DIR:-$(cd "$(dirname "$BUNDLE")" && pwd)}"
mkdir -p "$OUT_DIR"
OUT_PKG="$OUT_DIR/CoreVideo-Setup-$VERSION.pkg"

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

COMPONENT_PKG="$WORKDIR/component.pkg"

# --component, not --root: the bundle already IS the single item to install
# (it carries its own Info.plist, which is what makes pkgbuild recognize it
# as a bundle-style component rather than a loose directory tree), so there is
# no need to stage a synthetic root just to get the same payload path.
# --install-location is RELATIVE (see the top-of-file comment for why that
# specific property is what makes the per-user domain resolution below work
# at all) and matches nothing on this machine's absolute filesystem until
# productbuild's <domains> directive resolves it against a target volume.
COMPONENT_SIGN_ARGS=()
if [ -n "$SIGN_ID" ]; then
    COMPONENT_SIGN_ARGS=(--sign "$SIGN_ID")
fi
pkgbuild --component "$BUNDLE" \
    --install-location "$INSTALL_LOCATION" \
    --identifier "$IDENTIFIER" \
    --version "$VERSION" \
    ${COMPONENT_SIGN_ARGS[@]+"${COMPONENT_SIGN_ARGS[@]}"} \
    "$COMPONENT_PKG"

# The Distribution wrapper is what actually carries the per-user domain
# declaration -- a bare component .pkg (what pkgbuild alone produces) has no
# way to express "resolve this against the current user's home, not root's."
# `customize="never"` keeps this a silent double-click install with no
# choices screen, matching the Windows installer's one-click experience this
# script exists to match. `enable_anywhere="false"` is deliberate, not
# redundant with `enable_localSystem="false"`: "anywhere" is a THIRD domain
# (an external volume picker) distinct from local-system, and leaving it on
# offers a volume-choice UI this installer has no reason to show.
DIST_XML="$WORKDIR/distribution.xml"
cat > "$DIST_XML" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="1">
    <title>CoreVideo for OBS</title>
    <options customize="never" require-scripts="false" rootVolumeOnly="false"/>
    <domains enable_currentUserHome="true" enable_localSystem="false" enable_anywhere="false"/>
    <choices-outline>
        <line choice="default">
            <line choice="$IDENTIFIER"/>
        </line>
    </choices-outline>
    <choice id="default"/>
    <choice id="$IDENTIFIER" visible="false">
        <pkg-ref id="$IDENTIFIER"/>
    </choice>
    <pkg-ref id="$IDENTIFIER" version="$VERSION" onConclusion="none">component.pkg</pkg-ref>
</installer-gui-script>
XML

DIST_SIGN_ARGS=()
if [ -n "$SIGN_ID" ]; then
    DIST_SIGN_ARGS=(--sign "$SIGN_ID")
fi
productbuild --distribution "$DIST_XML" --package-path "$WORKDIR" \
    ${DIST_SIGN_ARGS[@]+"${DIST_SIGN_ARGS[@]}"} \
    "$OUT_PKG"

# Confirm the artifact users will double-click, not merely the intermediate
# component package. This also prints the certificate chain into the build log
# so a report of "macOS says it isn't signed" has actionable evidence.
if [ -n "$SIGN_ID" ]; then
    PKG_SIGNATURE="$(pkgutil --check-signature "$OUT_PKG" 2>&1)" || {
        printf '%s\n' "$PKG_SIGNATURE" >&2
        echo "error: final installer package signature validation failed" >&2
        exit 1
    }
    printf '%s\n' "$PKG_SIGNATURE"
    printf '%s\n' "$PKG_SIGNATURE" | \
        grep -q 'Developer ID Installer:' || {
        echo "error: final package is not signed with a Developer ID Installer" >&2
        echo "       certificate; do not distribute it." >&2
        exit 1
    }
fi

echo "built: $OUT_PKG (version $VERSION)"

# ── Notarization ──────────────────────────────────────────────────────────
#
# Unlike a .plugin bundle, a .pkg is directly submittable and directly
# staplable -- no zip-for-submission dance, because notarytool accepts a bare
# .pkg and the staple target is the same file either way.
if [ -n "$NOTARIZE_PROFILE" ]; then
    echo "submitting to Apple (613 MB — expect several minutes)…"
    xcrun notarytool submit "$OUT_PKG" \
        --keychain-profile "$NOTARIZE_PROFILE" --wait

    xcrun stapler staple "$OUT_PKG"
    xcrun stapler validate "$OUT_PKG"
    # The question a tester's Mac actually asks before running the
    # installer. `-t install` is the assessment type for a .pkg, matching
    # make-macos-bundle.sh's own `-t install` use for the bundle it produces.
    spctl --assess -vv -t install "$OUT_PKG"
    echo "notarized and stapled: $OUT_PKG"
fi
