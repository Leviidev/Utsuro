import sys, pefile, struct
pe = pefile.PE(sys.argv[1], fast_load=True); pe.parse_data_directories()
img = pe.get_memory_mapped_image()
out = []
for s in pe.DIRECTORY_ENTRY_EXPORT.symbols:
    if not s.name: continue
    n = s.name.decode()
    if not n.startswith('Nt'): continue
    b = img[s.address:s.address+11]
    if b[0:3] == b'\x4c\x8b\xd1' and b[3] == 0xb8 and b[8:10] == b'\x0f\x05':
        out.append((struct.unpack('<I', b[4:8])[0], n))
    else:
        out.append((-1, n))
out.sort()
for nr, n in out: print(nr, n)
