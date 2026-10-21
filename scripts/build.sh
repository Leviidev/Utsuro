#!/usr/bin/env bash
# Builds the Utsuro runtime as a native macOS program (build/utsuro-run).
# FEXCore is not vendored: it is husk-fex's, built by scripts/build-fex.sh macphone. That branch has the hosted-JIT hooks, and
# the macphone build runs the phone's dual-mapped JIT on the Mac, so this binary exercises the same path the iOS app ships.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FEXCORE_SRC="${FEXCORE_SRC:-$HOME/Documents/Coding/husk-fex/runtime/sources/fexcore-darwin}"
FEX_BUILD="${FEX_BUILD:-$ROOT/build/fex/macphone}"
LIBS_DIR="${LIBS_DIR:-$ROOT/build/fex/macphone-libs}"
OUT="$ROOT/build"
mkdir -p "$OUT/obj"

INCLUDES=(
    -I"$ROOT/src" -I"$ROOT/src/engine"
    -I"$FEXCORE_SRC/FEXCore/include" -I"$FEXCORE_SRC/Source" -I"$FEXCORE_SRC/FEXHeaderUtils"
    -I"$FEXCORE_SRC/CodeEmitter" -I"$FEXCORE_SRC/External/fmt/include"
    -I"$FEXCORE_SRC/External/unordered_dense/include" -I"$FEXCORE_SRC/External/range-v3/include"
    -I"$FEXCORE_SRC/External/vixl/src" -I"$FEXCORE_SRC/External/xxhash" -I"$FEXCORE_SRC/External/tiny-json"
    -I"$FEX_BUILD/include" -I"$FEX_BUILD/generated"
)

objs=()
pids=()
while IFS= read -r src; do
    obj="$OUT/obj/$(echo "${src#$ROOT/src/}" | tr '/' '_' | sed -E 's/\.(cpp|mm|c|S)$/.o/')"
    lang=()
    cc=clang++
    [[ "$src" == *.mm ]] && lang=(-x objective-c++ -fobjc-arc)
    [[ "$src" == *.c ]] && { cc=clang; lang=(-std=gnu11); }
    [[ "$src" == *.S ]] && { cc=clang; lang=(-x assembler-with-cpp); }
    objs+=("$obj")
    if [[ ! -f "$obj" || "$src" -nt "$obj" || -n "$(find "$ROOT/src" \( -name '*.h' -o -name '*.inc' \) -newer "$obj" -print -quit)" ]]; then
        while (( $(jobs -rp | wc -l) >= ${JOBS:-6} )); do sleep 0.2; done   # a full rebuild would otherwise start every compile at once
        $cc -target arm64-apple-macos27.0 $([[ "$cc" == clang++ ]] && echo -std=c++20) -O2 -g -DARCHITECTURE_arm64=1 -DFEX_MAC_PHONE_PATH=1 \
            -Wall -Wno-unused-parameter -DMETA_NO_STD_FORWARD_DECLARATIONS -DUTSURO_DATA_DIR=\"$ROOT/data\" \
            "${INCLUDES[@]}" ${lang[@]+"${lang[@]}"} -c "$src" -o "$obj" &
        pids+=($!)
    fi
done < <(find "$ROOT/src" \( -name '*.cpp' -o -name '*.mm' -o -name '*.c' -o -name '*.S' \) | sort)
fail=0
for p in "${pids[@]:-}"; do [[ -n "$p" ]] && { wait "$p" || fail=1; }; done
[[ $fail == 0 ]] || { echo "compile failed" >&2; exit 1; }

clang++ -target arm64-apple-macos27.0 -o "$OUT/utsuro-run" "${objs[@]}" \
    -Wl,-search_paths_first "$LIBS_DIR"/*.a "$LIBS_DIR"/*.a \
    -Wl,-w -framework CoreFoundation -framework Foundation -framework Metal -framework QuartzCore -framework AppKit -framework CoreGraphics -framework ImageIO -framework GameController -framework AVFoundation -framework AudioToolbox -framework UniformTypeIdentifiers -lc++ -lpthread -lz

cat > "$OUT/jit.entitlements" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>com.apple.security.cs.allow-jit</key><true/>
</dict></plist>
PLIST
codesign --force --sign - --entitlements "$OUT/jit.entitlements" "$OUT/utsuro-run"
echo "built $OUT/utsuro-run"
