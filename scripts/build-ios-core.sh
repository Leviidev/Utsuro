#!/usr/bin/env bash
# Compiles the Utsuro runtime for iphoneos into build/ios/libutsuro-core.a (everything but the Mac main and window, which the
# app replaces). FEXCore comes from scripts/build-fex.sh ios.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FEXCORE_SRC="${FEXCORE_SRC:-$HOME/Documents/Coding/husk-fex/runtime/sources/fexcore-darwin}"
FEX_BUILD="${FEX_BUILD:-$ROOT/build/fex/ios}"
OUT="$ROOT/build/ios"
SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
MIN=17.4
mkdir -p "$OUT/obj"

INCLUDES=(
    -I"$ROOT/src" -I"$ROOT/src/engine"
    -I"$FEXCORE_SRC/FEXCore/include" -I"$FEXCORE_SRC/Source" -I"$FEXCORE_SRC/FEXHeaderUtils"
    -I"$FEXCORE_SRC/CodeEmitter" -I"$FEXCORE_SRC/External/fmt/include"
    -I"$FEXCORE_SRC/External/unordered_dense/include" -I"$FEXCORE_SRC/External/range-v3/include"
    -I"$FEXCORE_SRC/External/vixl/src" -I"$FEXCORE_SRC/External/xxhash" -I"$FEXCORE_SRC/External/tiny-json"
    -I"$FEX_BUILD/include" -I"$FEX_BUILD/generated"
)
COMMON=(-target arm64-apple-ios$MIN -isysroot "$SDK" -O2 -g -DARCHITECTURE_arm64=1 -DUTSURO_IOS=1 -Wall -Wno-unused-parameter
        -DMETA_NO_STD_FORWARD_DECLARATIONS -DUTSURO_DATA_DIR=\"$ROOT/data\" "${INCLUDES[@]}")

objs=(); pids=()
while IFS= read -r src; do
    rel="${src#$ROOT/src/}"
    case "$rel" in main.cpp|selftest.cpp|host/window.mm) continue ;; esac
    obj="$OUT/obj/$(echo "$rel" | tr '/' '_' | sed -E 's/\.(cpp|mm|c|S)$/.o/')"
    objs+=("$obj")
    flags=(-std=c++20); cc=clang++
    [[ "$src" == *.mm ]] && flags=(-std=c++20 -x objective-c++ -fobjc-arc)
    [[ "$src" == *.c ]] && { cc=clang; flags=(-std=gnu11); }
    [[ "$src" == *.S ]] && { cc=clang; flags=(-x assembler-with-cpp); }
    if [[ ! -f "$obj" || "$src" -nt "$obj" || -n "$(find "$ROOT/src" \( -name '*.h' -o -name '*.inc' \) -newer "$obj" -print -quit)" ]]; then
        "$cc" "${COMMON[@]}" "${flags[@]}" -c "$src" -o "$obj" &
        pids+=($!)
    fi
done < <(find "$ROOT/src" \( -name '*.cpp' -o -name '*.mm' -o -name '*.c' -o -name '*.S' \) | sort)
fail=0
for p in "${pids[@]:-}"; do [[ -n "$p" ]] && { wait "$p" || fail=1; }; done
[[ $fail == 0 ]] || { echo "compile failed" >&2; exit 1; }
rm -f "$OUT/libutsuro-core.a"
xcrun libtool -static -o "$OUT/libutsuro-core.a" "${objs[@]}" 2>/dev/null
echo "built $OUT/libutsuro-core.a"
