#!/usr/bin/env bash
# Builds husk-fex's FEXCore (the branch with the hosted-JIT hooks) for one target:
#   build-fex.sh ios        -> build/fex/ios        (iphoneos, the phone)
#   build-fex.sh macphone   -> build/fex/macphone   (macOS with FEX_MAC_PHONE_PATH: the phone's JIT path on the Mac)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FEX_SRC="${HUSK_FEX_DIR:-$HOME/Documents/Coding/husk-fex}/runtime/sources/fexcore-darwin"
TARGET="${1:?usage: build-fex.sh ios|macphone}"
B="$ROOT/build/fex/$TARGET"
COMMON=(-G "Unix Makefiles" -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_SYSTEM_PROCESSOR=arm64 -DCMAKE_BUILD_TYPE=Release
    -DBUILD_FEXCORE_ONLY=ON -DBUILD_FEXCORE_SMOKE=OFF -DBUILD_FEXCONFIG=OFF -DBUILD_TESTING=OFF -DBUILD_THUNKS=OFF
    -DENABLE_LTO=OFF -DENABLE_CCACHE=ON -DENABLE_ASSERTIONS=OFF -DENABLE_JEMALLOC_GLIBC_ALLOC=OFF
    -DENABLE_OFFLINE_TELEMETRY=OFF -DENABLE_FEX_ALLOCATOR=OFF -DTUNE_ARCH=generic -DTUNE_CPU=none)
case "$TARGET" in
ios) EXTRA=(-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_DEPLOYMENT_TARGET=17.4
        "-DCMAKE_CXX_FLAGS=-DMETA_NO_STD_FORWARD_DECLARATIONS") ;;
macphone) EXTRA=(-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 "-DCMAKE_CXX_FLAGS=-DFEX_MAC_PHONE_PATH=1 -DMETA_NO_STD_FORWARD_DECLARATIONS"
        -DCMAKE_C_FLAGS=-DFEX_MAC_PHONE_PATH=1) ;;
*) echo "unknown target $TARGET" >&2; exit 2 ;;
esac
[ -f "$B/CMakeCache.txt" ] || cmake -S "$FEX_SRC" -B "$B" "${COMMON[@]}" "${EXTRA[@]}"
make -C "$B" -j"$(sysctl -n hw.ncpu)" FEXCore Common CommonTools JemallocLibs
# One flat directory of archives for the link line.
LIBS="$ROOT/build/fex/$TARGET-libs"; rm -rf "$LIBS"; mkdir -p "$LIBS"
find "$B" -name 'lib*.a' -exec cp {} "$LIBS/" \;
# macOS finds xxhash on the system instead of building its own; the archive is the same code.
[ -f "$LIBS/libxxhash.a" ] || cp "${FATHOM:-$HOME/Documents/Coding/fathom}/build/fexcore-macos-libs/libxxhash.a" "$LIBS/"
echo "built $LIBS"
