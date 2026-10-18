#!/usr/bin/env python3
"""rt_class_at.py <pe> <rva>: for the function containing the RVA, prints the wide strings its lea
instructions reference and the IID-looking immediates it builds (what it asks RoGetActivationFactory for)."""
import sys, pefile, struct, bisect, re
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
pe = pefile.PE(sys.argv[1], fast_load=True)
rva = int(sys.argv[2], 16)
pe.parse_data_directories([pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]])
d = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
raw = pe.get_data(d.VirtualAddress, d.Size)
starts = sorted(struct.unpack_from("<I", raw, o)[0] for o in range(0, len(raw) - 11, 12))
i = bisect.bisect_right(starts, rva) - 1
start = starts[i]
end = starts[i + 1] if i + 1 < len(starts) else start + 0x200
data = pe.get_data(start, end - start)
base = pe.OPTIONAL_HEADER.ImageBase
md = Cs(CS_ARCH_X86, CS_MODE_64)
dwords = []
for ins in md.disasm(data, base + start):
    m = re.match(r"\[rip ([+-]) (0x[0-9a-f]+)\]", ins.op_str.split(", ")[-1]) if ins.mnemonic == "lea" else None
    if m:
        t = ins.address + ins.size + (int(m.group(2), 16) if m.group(1) == "+" else -int(m.group(2), 16)) - base
        try:
            s = pe.get_data(t, 160).decode("utf-16le", errors="ignore").split("\x00")[0]
            if len(s) > 3 and all(32 <= ord(ch) < 127 for ch in s): print(f"{ins.address-base:x}: string {s}")
        except Exception: pass
    if ins.mnemonic == "mov" and ins.op_str.endswith(tuple("0123456789abcdef")) and "dword ptr" in ins.op_str:
        v = int(ins.op_str.split(", ")[-1], 16)
        if v > 0xffff: dwords.append(v)
if len(dwords) >= 4:
    for k in range(0, len(dwords) - 3, 4):
        a, b, c, e = dwords[k:k+4]
        print("iid %08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X" % (a, b & 0xffff, b >> 16, c & 0xff, (c >> 8) & 0xff, (c >> 16) & 0xff, c >> 24, e & 0xff, (e >> 8) & 0xff, (e >> 16) & 0xff, e >> 24))
