"""tp.py <dll> <rva> [<rva>...]  -> prints UTSURO_TP specs with a safe displaced length (whole
instructions, >= 5 bytes, none rip-relative or branching)."""
import sys, pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
path = sys.argv[1]
pe = pefile.PE(path, fast_load=True)
img = pe.get_memory_mapped_image()
md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True
name = path.split('/')[-1].lower()
out = []
for r in sys.argv[2:]:
    rva = int(r, 16)
    n = 0
    ok = True
    for ins in md.disasm(img[rva:rva+32], rva):
        if any(g in (7, 1, 2, 3) for g in ins.groups):  # jump/call/ret/int
            ok = False; break
        if 'rip' in ins.op_str:
            ok = False; break
        n += len(ins.bytes)
        if n >= 5: break
    out.append(f"{name}+{rva:x}:{n}" if ok and n >= 5 else f"# {name}+{rva:x} unsafe")
print(','.join(out))
