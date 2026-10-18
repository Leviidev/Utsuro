"""Finds every reference to KUSER_SHARED_DATA (0x7ffe0000..0x7ffe0fff) in x64 PE images and
writes a patch table the runtime applies when it maps the image.

usage: kusd_scan.py <out dir> <pe> [<pe> ...]
Each output file is named <image>_<timestamp>_<sizeofimage>.txt, one record per line:
  A <rva> <len>                abs [disp32] operand, no base/index: becomes rip-relative
  B <rva> <len> <reg>          mov reg, imm (address as a value): becomes a jump to a stub
  C <rva> <len>                indexed operand [idx*scale+disp32]: needs a hand stub (reported)
  E <rva> <len>                mov qword ptr [rsp], imm32: stub stores the full 64-bit address
  F <rva> <len> <reg>          add reg64, imm32: stub adds the full 64-bit address
  D <rva> <len>                anything else (reported)
"""
import os, sys, struct
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_OP_MEM, CS_OP_IMM, CS_OP_REG
from capstone.x86 import X86_REG_INVALID, X86_REG_RSP

LO, HI = 0x7FFE0000, 0x7FFE1000

def scan(path):
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories()
    img = pe.get_memory_mapped_image()
    base = pe.OPTIONAL_HEADER.ImageBase
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    records = []
    ex = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
    starts = []
    if ex.VirtualAddress and ex.Size:
        for i in range(ex.Size // 12):
            b, e, u = struct.unpack_from('<III', img, ex.VirtualAddress + 12 * i)
            starts.append((b, e))
    # Cover whole executable sections too, to reach code without unwind info.
    ranges = list(starts)
    # Leaf functions have no unwind data: sweep the gaps between the described ranges too.
    for sec in pe.sections:
        if not (sec.Characteristics & 0x20000000):
            continue
        sb, se = sec.VirtualAddress, sec.VirtualAddress + max(sec.Misc_VirtualSize, sec.SizeOfRawData)
        cur = sb
        for (b, e) in sorted(x for x in starts if sb <= x[0] < se):
            if b > cur:
                ranges.append((cur, b))
            cur = max(cur, e)
        if cur < se:
            ranges.append((cur, se))
    seen = set()
    for (b, e) in ranges:
        off = b
        while off < e:
            progressed = False
            for ins in md.disasm(img[off:e], off):
                progressed = True
                off = ins.address + len(ins.bytes)
                if ins.address in seen:
                    continue
                seen.add(ins.address)
                hit = None
                for op in ins.operands:
                    if op.type == CS_OP_MEM:
                        if LO <= (op.mem.disp & 0xFFFFFFFF) < HI and op.mem.base == X86_REG_INVALID:
                            hit = ('A', None) if op.mem.index == X86_REG_INVALID else ('C', None)
                    elif op.type == CS_OP_IMM and LO <= (op.imm & 0xFFFFFFFF) < HI and (op.imm >> 32) in (0, -1):
                        if ins.mnemonic == 'mov' and ins.operands[0].type == CS_OP_REG:
                            hit = ('B', ins.reg_name(ins.operands[0].reg))
                        elif ins.mnemonic == 'mov' and ins.operands[0].type == CS_OP_MEM and ins.operands[0].mem.base == X86_REG_RSP and ins.operands[0].mem.index == 0 and ins.operands[0].mem.disp == 0 and ins.operands[0].size == 8:
                            hit = ('E', None)   # mov qword ptr [rsp], imm32
                        elif ins.mnemonic == 'add' and ins.operands[0].type == CS_OP_REG and ins.operands[0].size == 8:
                            hit = ('F', ins.reg_name(ins.operands[0].reg))
                        else:
                            hit = ('D', None)
                if hit:
                    records.append((hit[0], ins.address, len(ins.bytes), hit[1], ins.mnemonic + ' ' + ins.op_str))
            if not progressed:
                off += 1
            elif off < e and False:
                pass
        continue
    for (b, e) in []:
        for ins in md.disasm(img[b:e], b):
            if ins.address in seen:
                continue
            seen.add(ins.address)
            hit = None
            for op in ins.operands:
                if op.type == CS_OP_MEM:
                    if LO <= (op.mem.disp & 0xFFFFFFFF) < HI and op.mem.base == X86_REG_INVALID:
                        if op.mem.index == X86_REG_INVALID:
                            hit = ('A', None)
                        else:
                            hit = ('C', None)
                elif op.type == CS_OP_IMM and LO <= (op.imm & 0xFFFFFFFF) < HI and (op.imm >> 32) in (0, -1):
                    if ins.mnemonic == 'mov' and ins.operands[0].type == CS_OP_REG:
                        hit = ('B', ins.reg_name(ins.operands[0].reg))
                    else:
                        hit = ('D', None)
            if hit:
                records.append((hit[0], ins.address, len(ins.bytes), hit[1], ins.mnemonic + ' ' + ins.op_str))
    return pe, sorted(records, key=lambda r: r[1])

def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for path in sys.argv[2:]:
        pe, recs = scan(path)
        if not recs:
            continue
        name = os.path.basename(path).lower()
        fn = f"{name}_{pe.FILE_HEADER.TimeDateStamp:08x}_{pe.OPTIONAL_HEADER.SizeOfImage:x}.txt"
        with open(os.path.join(out, fn), 'w') as f:
            for kind, rva, ln, reg, text in recs:
                f.write(f"{kind} {rva:x} {ln}" + (f" {reg}" if reg else "") + f"  ; {text}\n")
        counts = {}
        for r in recs: counts[r[0]] = counts.get(r[0], 0) + 1
        print(name, counts)

main()
