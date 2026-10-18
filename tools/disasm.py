import sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
data = open(sys.argv[1], 'rb').read()
base = int(sys.argv[2], 16)
md = Cs(CS_ARCH_X86, CS_MODE_64)
for i in md.disasm(data, base):
    print(f"{i.address:x}: {i.bytes.hex():24s} {i.mnemonic} {i.op_str}")
