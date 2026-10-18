#!/usr/bin/env python3
"""xref.py <pe> <target rva>: finds code that references a global through a rip-relative disp32
(raw byte scan, so jump tables and desync cannot hide it). Prints the RVA of the displacement and the
bytes around it, with the function (by .pdata) it lies in."""
import sys, pefile, struct, bisect
pe = pefile.PE(sys.argv[1], fast_load=True)
tgt = int(sys.argv[2], 16)
pe.parse_data_directories([pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]])
d = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
raw = pe.get_data(d.VirtualAddress, d.Size)
starts = sorted(struct.unpack_from("<I", raw, o)[0] for o in range(0, len(raw) - 11, 12))
for sec in pe.sections:
    if not (sec.Characteristics & 0x20000000): continue
    data = sec.get_data(); va = sec.VirtualAddress
    for i in range(0, len(data) - 4):
        disp = struct.unpack_from("<i", data, i)[0]
        for extra in (0, 1, 4):   # immediate bytes after the displacement
            if va + i + 4 + extra + disp == tgt and i >= 2 and data[i - 1] & 0xC7 == 0x05:
                f = starts[bisect.bisect_right(starts, va + i) - 1]
                print(f"{va + i - 2:x}  in function {f:x}  bytes {data[i-3:i+4+extra].hex()}")
