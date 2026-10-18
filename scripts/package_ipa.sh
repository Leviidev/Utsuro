#!/bin/bash
# Builds Utsuro for iPhone and packages an IPA (or, with --tipa, a TrollStore .tipa), validating the bundle before shipping it.
#
#   scripts/package_ipa.sh            -> ~/Desktop/Utsuro.ipa
#   scripts/package_ipa.sh --tipa     -> ~/Desktop/Utsuro.tipa
#
# No signing team is needed: AltStore / SideStore re-sign at install with the user's own profile. The IPA is signed ad hoc with
# Utsuro.entitlements embedded anyway, because TrollStore keeps the entitlements a binary already carries; signed this way, the same
# file gets the memory and get-task-allow entitlements under TrollStore, and a sideloader simply replaces the signature.
# (The .tipa drops the entitlements TrollStore refuses.)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DD="${DD:-/tmp/utsuro_ipa}"
TROLLSTORE=false
OUT=""
for arg in "$@"; do
    case "$arg" in
        --trollstore|--tipa) TROLLSTORE=true ;;
        *) OUT="${OUT:-$arg}" ;;
    esac
done
[[ "$OUT" == *.tipa ]] && TROLLSTORE=true
[ -n "$OUT" ] || OUT="$HOME/Desktop/Utsuro.$([ "$TROLLSTORE" = true ] && echo tipa || echo ipa)"
mkdir -p "$DD"

echo "==> FEXCore (husk-fex) for iphoneos"
"$ROOT/scripts/build-fex.sh" ios > "$DD/fex.log" 2>&1 || { tail -20 "$DD/fex.log" >&2; exit 1; }
echo "==> Utsuro runtime for iphoneos"
"$ROOT/scripts/build-ios-core.sh" 2>&1 | grep -E "error|built |failed" || true
[ "$ROOT/build/ios/libutsuro-core.a" -nt "$ROOT/src/engine/engine.cpp" ] || { echo "runtime build failed" >&2; exit 1; }

echo "==> regenerating the project from project.yml"
(cd "$ROOT/ios" && xcodegen generate --quiet)

echo "==> building"
xcodebuild -project "$ROOT/ios/Utsuro.xcodeproj" -scheme Utsuro -sdk iphoneos -configuration Release -derivedDataPath "$DD" \
    CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY="" \
    build 2>&1 | tee "$DD/build.log" | grep -E "error:|BUILD (SUCCEEDED|FAILED)" || true
if ! grep -q "BUILD SUCCEEDED" "$DD/build.log"; then
    echo "build failed; refusing to package a stale app" >&2
    grep -E "error:" "$DD/build.log" | head -10 >&2
    exit 1
fi

APP="$DD/Build/Products/Release-iphoneos/Utsuro.app"
[ -d "$APP" ] || { echo "no app bundle at $APP" >&2; exit 1; }

# Stamp the build's identity into the bundle so its logs can name themselves.
COMMIT="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
git -C "$ROOT" diff --quiet 2>/dev/null || COMMIT="$COMMIT-dirty"
plutil -replace UtsuroBuildCommit -string "$COMMIT" "$APP/Info.plist"
plutil -replace UtsuroBuildDate -string "$(date -u '+%Y-%m-%d %H:%M UTC')" "$APP/Info.plist"
echo "==> stamped build $COMMIT"

echo "==> validating bundle"
PLIST="$APP/Info.plist"
rc=0
for key in CFBundleIdentifier CFBundleExecutable CFBundleName CFBundlePackageType CFBundleVersion CFBundleShortVersionString \
           MinimumOSVersion UIDeviceFamily; do
    val="$(/usr/libexec/PlistBuddy -c "Print :$key" "$PLIST" 2>/dev/null || true)"
    if [ -z "$val" ]; then echo "  MISSING  $key   <-- the app will not install" >&2; rc=1
    else printf "  ok       %-28s %s\n" "$key" "$(echo "$val" | head -1)"; fi
done
EXE="$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$PLIST" 2>/dev/null || true)"
[ -n "$EXE" ] && [ -f "$APP/$EXE" ] || { echo "  MISSING  executable '$EXE'" >&2; rc=1; }
for f in utsuro-jit.js data/kusd/combase.dll_53eb190c_dc000.txt \
         PlugIns/UtsuroJITHelper.appex/UtsuroJITHelper Frameworks/StikJIT.framework/StikJIT Frameworks/StikJIT.framework/Info.plist; do
    if [ ! -f "$APP/$f" ]; then echo "  MISSING  $f" >&2; rc=1
    else printf "  ok       %-28s %s\n" "$(basename "$f")" "$(du -h "$APP/$f" | cut -f1)"; fi
done
# Every symbol the app uses from the JIT and runtime C side must be in the binary.
nm "$APP/$EXE" > "$DD/symbols.txt"
for sym in _utsuro_ios_run _utsuro_ios_prewarm_jit _utsuro_ios_set_pad _husk_ios_jit_install_trap_handler _husk_jit_arena_open; do
    grep -q " T $sym\$" "$DD/symbols.txt" || { echo "  MISSING  symbol $sym" >&2; rc=1; }
done
[ $rc -eq 0 ] || { echo "==> bundle is not installable; refusing to package" >&2; exit 1; }

echo "==> packaging"
STAGE="$(mktemp -d "$DD/stage.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/Payload"
cp -R "$APP" "$STAGE/Payload/"

ENT="$STAGE/entitlements.plist"
cp "$ROOT/ios/Utsuro/Utsuro.entitlements" "$ENT"
BANNED_KEYS=("com.apple.private.cs.debugger" "dynamic-codesigning" "com.apple.private.skip-library-validation")
if [ "$TROLLSTORE" = true ]; then
    for key in "${BANNED_KEYS[@]}"; do plutil -remove "$key" "$ENT" 2>/dev/null || true; done
fi
SAPP="$STAGE/Payload/$(basename "$APP")"
sign() { codesign --force --sign - --timestamp=none "$@"; }
for fw in "$SAPP"/Frameworks/*.framework; do [ -d "$fw" ] && sign "$fw"; done
for ex in "$SAPP"/PlugIns/*.appex; do [ -d "$ex" ] && sign "$ex"; done
sign --entitlements "$ENT" "$SAPP"
echo "==> entitlements in the signed app:"
EMBEDDED_ENTS=$(codesign -d --entitlements - "$SAPP" 2>&1 || true)
for need in get-task-allow increased-memory-limit extended-virtual-addressing; do
    grep -q "$need" <<< "$EMBEDDED_ENTS" || { echo "==> entitlement $need did not embed; refusing to package" >&2; exit 1; }
done
if [ "$TROLLSTORE" != true ]; then
    grep -q dynamic-codesigning <<< "$EMBEDDED_ENTS" || { echo "==> dynamic-codesigning did not embed; refusing to package" >&2; exit 1; }
else
    for key in "${BANNED_KEYS[@]}"; do
        ! grep -q "$key" <<< "$EMBEDDED_ENTS" || { echo "==> banned entitlement '$key' in the trollstore package" >&2; exit 1; }
    done
fi
TMP_OUT="$STAGE/package.tmp"
( cd "$STAGE" && zip -qry "$TMP_OUT" Payload )
mv -f "$TMP_OUT" "$OUT"
rm -rf "$STAGE"
echo "==> $OUT  ($(du -h "$OUT" | cut -f1))"
