import struct, sys
path = sys.argv[1]; target = int(sys.argv[2], 16)
d = open(path, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]; nsec = struct.unpack_from('<H', d, pe + 6)[0]; optsz = struct.unpack_from('<H', d, pe + 20)[0]
for i in range(nsec):
    o = pe + 24 + optsz + i * 40
    name = d[o:o+8].rstrip(b'\0').decode(); vsz, va, rsz, raw = struct.unpack_from('<IIII', d, o + 8)
    if name != '.text': continue
    t = d[raw:raw + rsz]
    j = t.find(b'\xe8')
    while j != -1 and j < len(t) - 5:
        if va + j + 5 + struct.unpack_from('<i', t, j + 1)[0] == target: print(hex(va + j))
        j = t.find(b'\xe8', j + 1)
