import sys, pefile
from capstone import *
from capstone.x86 import *
pe=pefile.PE('/tmp/rlx/rl.exe',fast_load=True)
targets=set(int(x,16) for x in sys.argv[1:])
md=Cs(CS_ARCH_X86,CS_MODE_64); md.detail=True; md.skipdata=True
for s in pe.sections:
    if not s.Characteristics & 0x20000000: continue
    data=s.get_data(); base=s.VirtualAddress
    for ins in md.disasm(data, base):
        if ins.id==0: continue
        for op in ins.operands:
            if op.type==X86_OP_MEM and op.mem.base==X86_REG_RIP:
                t=ins.address+ins.size+op.mem.disp
                if t in targets: print(f"{ins.address:x}: {ins.mnemonic} {ins.op_str}")
