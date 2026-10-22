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
for arg in sys.argv[2:]:
    vt, slot = arg.split(':'); vt = int(vt, 16) - base; slot = int(slot, 0)
    p = struct.unpack_from('<Q', d, off(vt + slot * 8))[0]
    print(arg, '->', hex(p), 'rva', hex(p - base))
