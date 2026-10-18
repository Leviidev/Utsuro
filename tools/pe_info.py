import sys, pefile
for p in sys.argv[1:]:
    pe = pefile.PE(p, fast_load=True); pe.parse_data_directories()
    o = pe.OPTIONAL_HEADER
    rel = o.DATA_DIRECTORY[5]
    print(p.split('/')[-1], hex(o.ImageBase), 'size', hex(o.SizeOfImage), 'dllchar', hex(o.DllCharacteristics), 'reloc', hex(rel.Size), 'stack', hex(o.SizeOfStackReserve), hex(o.SizeOfStackCommit), 'heap', hex(o.SizeOfHeapReserve), hex(o.SizeOfHeapCommit), 'os', o.MajorOperatingSystemVersion, o.MinorOperatingSystemVersion, 'sub', o.MajorSubsystemVersion, o.MinorSubsystemVersion, 'subsys', o.Subsystem)
