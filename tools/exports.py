import sys, pefile
pe = pefile.PE(sys.argv[1], fast_load=True); pe.parse_data_directories()
for s in pe.DIRECTORY_ENTRY_EXPORT.symbols:
    print(f"{s.address:08x} {s.name.decode() if s.name else '#'+str(s.ordinal)}")
