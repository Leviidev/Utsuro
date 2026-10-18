#!/usr/bin/env python
"""Summarize a PE's imports: peimports.py <file.exe> [-v]  (run via tools/venv/bin/python -I)"""
import sys
import pefile

path = sys.argv[1]
verbose = "-v" in sys.argv
pe = pefile.PE(path, fast_load=True)
pe.parse_data_directories()
print(f"{path}: machine={hex(pe.FILE_HEADER.Machine)} subsystem={pe.OPTIONAL_HEADER.Subsystem} "
      f"entry={hex(pe.OPTIONAL_HEADER.AddressOfEntryPoint)} base={hex(pe.OPTIONAL_HEADER.ImageBase)} "
      f"size={hex(pe.OPTIONAL_HEADER.SizeOfImage)}")
for d in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
    names = [i.name.decode() if i.name else f"#{i.ordinal}" for i in d.imports]
    print(f"{d.dll.decode():40s} {len(names)}")
    if verbose:
        for n in names:
            print("    ", n)
for d in getattr(pe, "DIRECTORY_ENTRY_DELAY_IMPORT", []):
    print(f"[delay] {d.dll.decode():32s} {len(d.imports)}")
