import struct, sys, re
path, clsid = sys.argv[1], sys.argv[2]
d = open(path, 'rb').read()
def cell(off): return 0x1000 + off + 4
def nk_at(p):
    assert d[p:p+2] == b'nk', d[p:p+2]
    nlen = struct.unpack_from('<H', d, p + 0x48)[0]; flags = struct.unpack_from('<H', d, p + 2)[0]
    name = d[p + 0x4C:p + 0x4C + nlen]
    name = name.decode('latin1') if flags & 0x20 else name.decode('utf-16-le')
    return dict(name=name, nsub=struct.unpack_from('<I', d, p + 0x14)[0], sub=struct.unpack_from('<I', d, p + 0x1C)[0],
                nval=struct.unpack_from('<I', d, p + 0x24)[0], val=struct.unpack_from('<I', d, p + 0x28)[0])
def subkeys(nk):
    out = []
    def walk(off):
        p = cell(off); sig = d[p:p+2]; n = struct.unpack_from('<H', d, p + 2)[0]
        if sig in (b'lf', b'lh'): out.extend(struct.unpack_from('<I', d, p + 4 + 8 * i)[0] for i in range(n))
        elif sig == b'li': out.extend(struct.unpack_from('<I', d, p + 4 + 4 * i)[0] for i in range(n))
        elif sig == b'ri': [walk(struct.unpack_from('<I', d, p + 4 + 4 * i)[0]) for i in range(n)]
    if nk['nsub']: walk(nk['sub'])
    return [(o, nk_at(cell(o))) for o in out]
def values(nk):
    res = []
    if not nk['nval']: return res
    lst = cell(nk['val'])
    for i in range(nk['nval']):
        vp = cell(struct.unpack_from('<I', d, lst + 4 * i)[0])
        nlen, size, doff, typ, flags = struct.unpack_from('<HIIIH', d, vp + 2)
        name = d[vp + 0x14:vp + 0x14 + nlen].decode('latin1') or '(default)'
        if size & 0x80000000: data = struct.pack('<I', doff)[:size & 0x7fffffff]
        else: data = d[cell(doff):cell(doff) + size]
        if typ in (1, 2): data = data.decode('utf-16-le', 'ignore').rstrip('\0')
        res.append((name, typ, data))
    return res
def dump(nk, depth=0):
    print('  ' * depth + '[' + nk['name'] + ']')
    for n, t, v in values(nk): print('  ' * depth + f'  {n} = {v!r}'[:200])
    for o, sk in subkeys(nk): dump(sk, depth + 1)
for m in re.finditer(re.escape(('{' + clsid + '}').encode('latin1')), d, re.I):
    p = m.start() - 0x4C
    if d[p:p+2] == b'nk': dump(nk_at(p))
