#!/usr/bin/env python3
"""find_string_refs.py <exe> <file-offset-hex>...: code that takes the address of the data at a file offset (lea reg, [rip+x])."""
import struct, sys
d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]; nsec = struct.unpack_from('<H', d, pe + 6)[0]; optsz = struct.unpack_from('<H', d, pe + 20)[0]
secs = []
for i in range(nsec):
    o = pe + 24 + optsz + i * 40
    name = d[o:o+8].rstrip(b'\0').decode(); vsz, va, rsz, raw = struct.unpack_from('<IIII', d, o + 8); secs.append((name, va, vsz, raw, rsz))
def rva_of(off):
    for name, va, vsz, raw, rsz in secs:
        if raw <= off < raw + rsz: return va + off - raw
targets = {rva_of(int(a, 16)): a for a in sys.argv[2:]}
for name, va, vsz, raw, rsz in secs:
    if name != '.text': continue
    t = d[raw:raw + rsz]
    for j in range(len(t) - 7):
        if t[j] in (0x48, 0x4c) and t[j+1] == 0x8d and (t[j+2] & 0xC7) == 0x05:
            r = va + j + 7 + struct.unpack_from('<i', t, j + 3)[0]
            if r in targets: print(targets[r], '->', hex(va + j))
