import struct, sys
path = sys.argv[1]; disp = int(sys.argv[2], 16)
d = open(path, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]; nsec = struct.unpack_from('<H', d, pe + 6)[0]; optsz = struct.unpack_from('<H', d, pe + 20)[0]
needle = struct.pack('<I', disp)
for i in range(nsec):
    o = pe + 24 + optsz + i * 40
    name = d[o:o+8].rstrip(b'\0').decode(); vsz, va, rsz, raw = struct.unpack_from('<IIII', d, o + 8)
    if name != '.text': continue
    t = d[raw:raw + rsz]; j = t.find(needle)
    while j != -1:
        # byte stores: 88 /r with mod=10 (disp32) -> modrm at j-1, opcode at j-2 (or REX before); C6 /0 disp32 imm8
        op, m = t[j-2], t[j-1]
        if (op == 0x88 and (m >> 6) == 2) or (op == 0xC6 and (m >> 6) == 2 and ((m >> 3) & 7) == 0):
            print(hex(va + j - 2), 'store', hex(op))
        j = t.find(needle, j + 1)
