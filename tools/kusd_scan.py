"""Finds every reference to KUSER_SHARED_DATA (0x7ffe0000..0x7ffe0fff) in x64 PE images and
writes a patch table the runtime applies when it maps the image.

usage: kusd_scan.py <out dir> <pe> [<pe> ...]
Each output file is named <image>_<timestamp>_<sizeofimage>.txt, one record per line:
  A <rva> <len>                abs [disp32] operand, no base/index: becomes rip-relative
  B <rva> <len> <reg>          mov reg, imm (address as a value): becomes a jump to a stub
  C <rva> <len>                indexed operand [idx*scale+disp32] that H could not encode (reported)
  H <rva> <len> <base,idx,scale,off,newbytes>  [base+idx*scale+disp32]: the loader builds a stub that computes the address in r11 and runs `newbytes` (the instruction re-encoded as [r11])
  E <rva> <len>                mov qword ptr [rsp], imm32: stub stores the full 64-bit address
  F <rva> <len> <reg>          add reg64, imm32: stub adds the full 64-bit address
  G <rva> <len> <pos>          32-bit lea/mov/add on a register holding a relocated address: set REX.W at byte <pos>
  D <rva> <len>                anything else (reported)
"""
import os, sys, struct, re
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_OP_MEM, CS_OP_IMM, CS_OP_REG
from capstone.x86 import X86_REG_INVALID, X86_REG_RSP, X86_REG_RIP

LO, HI = 0x7FFE0000, 0x7FFE1000

_Q = {'eax': 'rax', 'ecx': 'rcx', 'edx': 'rdx', 'ebx': 'rbx', 'esp': 'rsp', 'ebp': 'rbp', 'esi': 'rsi', 'edi': 'rdi'}

def norm(name):
    if name in _Q: return _Q[name]
    if re.match(r'^r\d+d$', name): return name[:-1]
    return name

def rex_pos(code):
    for i, b in enumerate(code[:5]):
        if 0x40 <= b <= 0x4F: return i
        if b not in (0x66, 0x67, 0xF2, 0xF3, 0x2E, 0x3E, 0x26, 0x36, 0x64, 0x65): return None
    return None


PREFIXES = (0x66, 0x67, 0xF2, 0xF3, 0x2E, 0x3E, 0x26, 0x36, 0x64, 0x65)

def h_form(ins, op):
    """The instruction re-encoded with its [base+index*scale+disp32] operand as [r11], or None."""
    b = bytes(ins.bytes)
    if b[0] in (0xC4, 0xC5, 0x62) or ins.disp_size != 4 or ins.modrm_offset == 0: return None
    if 0x67 in b[:4]: return None
    m = op.mem
    base = ins.reg_name(m.base) if m.base != X86_REG_INVALID else None
    index = ins.reg_name(m.index) if m.index != X86_REG_INVALID else None
    for r in (base, index):
        if r is not None and (r.startswith('r11') or r in ('rsp', 'esp') or r[0] == 'e' and r not in ('eax', 'ecx', 'edx', 'ebx', 'ebp', 'esi', 'edi')): return None
    if base is not None and not base.startswith('r'): return None
    for o in ins.operands:
        if o.type == CS_OP_REG and ins.reg_name(o.reg).startswith('r11'): return None
    i = 0
    while i < len(b) and b[i] in PREFIXES: i += 1
    rexpos = i if i < len(b) and 0x40 <= b[i] <= 0x4F else None
    mo, do = ins.modrm_offset, ins.disp_offset
    start = i + (1 if rexpos is not None else 0)
    rex = ((b[rexpos] | 0x01) & ~0x02) if rexpos is not None else 0x41
    new = b[:i] + bytes([rex]) + b[start:mo] + bytes([(b[mo] & 0x38) | 0x03]) + b[do + 4:]
    return (base or '-', index or '-', m.scale, (m.disp & 0xFFFFFFFF) - LO, new.hex())

def derived(md, img, b_records, covered):
    """Instructions after a `mov r32, <kusd>` that keep the address in 32 bits, which a relocated
    (64-bit) address would truncate: lea r32,[R+d], mov r32,R32, add/sub r32,imm."""
    out = []
    for kind, rva, ln, reg, _ in b_records:
        if kind != 'B': continue
        holding = {norm(reg)}
        off = rva + ln
        for _step in range(24):
            ins = next(md.disasm(img[off:off + 15], off), None)
            if ins is None: break
            off += len(ins.bytes)
            if ins.mnemonic in ('ret', 'jmp', 'call', 'int3'): break
            ops = ins.operands
            if not ops or ops[0].type != CS_OP_REG: continue
            dst = norm(ins.reg_name(ops[0].reg))
            promote = False
            if ins.mnemonic == 'lea' and ops[0].size == 4 and ops[1].type == CS_OP_MEM:
                m = ops[1].mem
                if m.base != X86_REG_INVALID and norm(ins.reg_name(m.base)) in holding and m.index == X86_REG_INVALID: promote = True
            elif ins.mnemonic == 'mov' and ops[0].size == 4 and len(ops) > 1 and ops[1].type == CS_OP_REG and norm(ins.reg_name(ops[1].reg)) in holding:
                promote = True
            elif ins.mnemonic in ('add', 'sub') and ops[0].size == 4 and dst in holding and len(ops) > 1 and ops[1].type == CS_OP_IMM:
                promote = True
            if promote:
                pos = rex_pos(bytes(ins.bytes))
                if pos is not None and ins.address not in covered:
                    out.append(('G', ins.address, len(ins.bytes), str(pos), ins.mnemonic + ' ' + ins.op_str))
                    covered.add(ins.address)
                holding.add(dst)
            elif dst in holding:
                holding.discard(dst)
    return out

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
                        if LO <= (op.mem.disp & 0xFFFFFFFF) < HI and op.mem.base != X86_REG_RIP:
                            if op.mem.base == X86_REG_INVALID and op.mem.index == X86_REG_INVALID:
                                hit = ('A', None)
                            else:
                                h = h_form(ins, op)
                                hit = ('H', "%s,%s,%d,%x,%s" % h) if h is not None else ('C', None)
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
    records += derived(md, img, [r for r in records if r[0] == 'B'], {r[1] for r in records})
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
