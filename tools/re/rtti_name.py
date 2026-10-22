import struct, sys
path = sys.argv[1]; d = open(path, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]; nsec = struct.unpack_from('<H', d, pe + 6)[0]; optsz = struct.unpack_from('<H', d, pe + 20)[0]
base = struct.unpack_from('<Q', d, pe + 24 + 24)[0]
secs = []
for i in range(nsec):
    o = pe + 24 + optsz + i * 40
    vsz, va, rsz, raw = struct.unpack_from('<IIII', d, o + 8); secs.append((va, vsz, raw))
def off(rva):
    for va, vsz, raw in secs:
        if va <= rva < va + vsz: return raw + rva - va
def name_of(vt_va):
    col = struct.unpack_from('<Q', d, off(vt_va - base - 8))[0] - base
    sig, offs, cdoff, td_rva, chd_rva = struct.unpack_from('<IIIII', d, off(col))[:5]
    td = off(td_rva) + 16
    nm = d[td:d.index(b'\0', td)].decode()
    # base classes
    chd = off(chd_rva); n = struct.unpack_from('<I', d, chd + 8)[0]; arr = off(struct.unpack_from('<I', d, chd + 12)[0])
    bases = []
    for i in range(n):
        bcd = off(struct.unpack_from('<I', d, arr + 4 * i)[0]); t = off(struct.unpack_from('<I', d, bcd)[0]) + 16
        bases.append(d[t:d.index(b'\0', t)].decode())
    return nm, bases
for a in sys.argv[2:]:
    print(a, name_of(int(a, 16)))
