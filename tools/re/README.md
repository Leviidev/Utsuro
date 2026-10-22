Small static helpers for console executables (PE, x64). Run with python3; no packages needed.

- `find_calls.py <exe> <rva-hex>`: every `call rel32` to a function.
- `find_riprel_store.py <exe> <rva-hex>`: rip-relative loads/stores of a global (`mov [rip+x], reg`).
- `find_disp_store.py <exe> <disp-hex>`: byte stores `[reg+disp]`, to find who sets a member flag.
- `vtable_slot.py <exe> <vtable-va>:<slot> ...`: the function in a vtable slot.
- `rtti_name.py <exe> <vtable-va> ...`: the MSVC RTTI class name and bases behind a vtable.
- `hive_key.py <SOFTWARE hive> <CLSID>`: a COM class's registration (InprocServer32 etc.) in the console's registry.
