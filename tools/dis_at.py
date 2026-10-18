#!/usr/bin/env python3
"""dis_at.py <pe> <rva> [after]: disassembles from the start of the function (by .pdata) containing
the RVA to `after` bytes past it, marking the RVA."""
import sys, pefile, struct, bisect
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
pe = pefile.PE(sys.argv[1], fast_load=True)
rva = int(sys.argv[2], 16)
after = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x40
pe.parse_data_directories([pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]])
d = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
raw = pe.get_data(d.VirtualAddress, d.Size)
starts = [struct.unpack_from("<I", raw, o)[0] for o in range(0, len(raw) - 11, 12)]
i = bisect.bisect_right(starts, rva) - 1
start = starts[i] if i >= 0 else rva - 0x40
data = pe.get_data(start, rva + after - start)
base = pe.OPTIONAL_HEADER.ImageBase
md = Cs(CS_ARCH_X86, CS_MODE_64)
for ins in md.disasm(data, base + start):
    mark = "=>" if ins.address - base == rva else "  "
    print(f"{mark} {ins.address - base:x}: {ins.bytes.hex():24s} {ins.mnemonic} {ins.op_str}")
