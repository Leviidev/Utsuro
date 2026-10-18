#!/usr/bin/env bash
# Builds the Utsuro runtime as a native macOS program (build/utsuro-run).
# FEXCore is not vendored: it uses Fathom's macOS FEXCore build (see fathom/scripts/build-fexcore-macos.sh).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FATHOM="${FATHOM:-$HOME/Documents/Coding/fathom}"
FEXCORE_SRC="${FEXCORE_SRC:-$HOME/Documents/Coding/fathom-fexcore/runtime/sources/fexcore-darwin}"
FEX_BUILD="${FEX_BUILD:-$FATHOM/build/fexcore-macos}"
LIBS_DIR="${LIBS_DIR:-$FATHOM/build/fexcore-macos-libs}"
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
    obj="$OUT/obj/$(echo "${src#$ROOT/src/}" | tr '/' '_' | sed 's/\.cpp$/.o/')"
    objs+=("$obj")
    if [[ ! -f "$obj" || "$src" -nt "$obj" || -n "$(find "$ROOT/src" -name '*.h' -newer "$obj" -print -quit)" ]]; then
        clang++ -target arm64-apple-macos27.0 -std=c++20 -O1 -g -DARCHITECTURE_arm64=1 \
            -Wall -Wno-unused-parameter -DMETA_NO_STD_FORWARD_DECLARATIONS \
            "${INCLUDES[@]}" -c "$src" -o "$obj" &
        pids+=($!)
    fi
done < <(find "$ROOT/src" -name '*.cpp' | sort)
fail=0
for p in "${pids[@]:-}"; do [[ -n "$p" ]] && { wait "$p" || fail=1; }; done
[[ $fail == 0 ]] || { echo "compile failed" >&2; exit 1; }

clang++ -target arm64-apple-macos27.0 -o "$OUT/utsuro-run" "${objs[@]}" \
    -Wl,-search_paths_first "$LIBS_DIR"/*.a "$LIBS_DIR"/*.a \
    -Wl,-w -framework CoreFoundation -framework Foundation -lc++ -lpthread

cat > "$OUT/jit.entitlements" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>com.apple.security.cs.allow-jit</key><true/>
</dict></plist>
PLIST
codesign --force --sign - --entitlements "$OUT/jit.entitlements" "$OUT/utsuro-run"
echo "built $OUT/utsuro-run"
