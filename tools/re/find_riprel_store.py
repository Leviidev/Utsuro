import struct, sys
path = sys.argv[1]; target = int(sys.argv[2], 16)
d = open(path, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]; nsec = struct.unpack_from('<H', d, pe + 6)[0]; optsz = struct.unpack_from('<H', d, pe + 20)[0]
for i in range(nsec):
    o = pe + 24 + optsz + i * 40
    name = d[o:o+8].rstrip(b'\0').decode(); vsz, va, rsz, raw = struct.unpack_from('<IIII', d, o + 8)
    if name != '.text': continue
    text = d[raw:raw + rsz]
    hits = []
    for j in range(len(text) - 7):
        b0, b1, m = text[j], text[j+1], text[j+2]
        # REX.W (48/4C) 89 (store) or 8B (load), modrm mod=00 rm=101: rip-relative disp32
        if b0 in (0x48, 0x4c) and b1 in (0x89, 0x8b) and (m & 0xC7) == 0x05:
            disp = struct.unpack_from('<i', text, j + 3)[0]
            rva = va + j + 7 + disp
            if rva == target: hits.append((va + j, 'store' if b1 == 0x89 else 'load'))
    for rva, kind in hits: print(hex(rva), kind)
    print(len(hits), 'refs')
