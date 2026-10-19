#!/bin/bash
# newos.sh <game dir>: generates the data/kusd tables a dump's console OS build needs (skipped when its ntdll already has one).
set -e
cd "$(dirname "$0")/.."
G="$1"
SYS="$G/EmbeddedXvd/Windows/System32"
NT=$(tools/venv/bin/python - "$SYS/ntdll.dll" 2>/dev/null <<'P' || /tmp/pyv/bin/python - "$SYS/ntdll.dll" <<'Q'
import sys, pefile
pe = pefile.PE(sys.argv[1], fast_load=True)
print("%08x_%x" % (pe.FILE_HEADER.TimeDateStamp, pe.OPTIONAL_HEADER.SizeOfImage))
P
import sys, pefile
pe = pefile.PE(sys.argv[1], fast_load=True)
print("%08x_%x" % (pe.FILE_HEADER.TimeDateStamp, pe.OPTIONAL_HEADER.SizeOfImage))
Q
)
if ls data/kusd/ntdll.dll_$NT.txt >/dev/null 2>&1; then echo "OS build $NT already has tables"; exit 0; fi
OUT=$(mktemp -d)
PY=tools/venv/bin/python; [ -x $PY ] || PY=/tmp/pyv/bin/python
$PY tools/kusd_scan.py "$OUT" "$SYS"/*.dll "$SYS"/*.exe "$G"/Mount/*.exe > /dev/null 2>&1
for f in "$OUT"/*; do [ -e "data/kusd/$(basename "$f")" ] || cp "$f" data/kusd/; done
echo "OS build $NT: tables written ($(ls "$OUT" | wc -l) images)"
rm -rf "$OUT"
