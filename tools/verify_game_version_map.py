"""Verify the 2.6.2 -> 2.7.1 RVA map against two local game executables.
Usage: python verify_game_version_map.py OLD_EXE NEW_EXE
Read-only. Requires capstone and pefile; never launches or modifies either file.
"""
import re
import sys
from pathlib import Path
import capstone as cs
from capstone.x86 import X86_OP_MEM, X86_REG_RIP
import pefile

old, new = (pefile.PE(path) for path in sys.argv[1:3])
md = cs.Cs(cs.CS_ARCH_X86, cs.CS_MODE_64)
md.detail = True
section = next(s for s in new.sections if s.Name.startswith(b'.text'))
code = section.get_data()
header = (Path(__file__).resolve().parents[1] / 'src/ervr/game_version.h').read_text()
mapping = {int(a, 16): int(b, 16) for a, b in re.findall(r'case (0x[0-9a-f]+): return (0x[0-9a-f]+);', header)}

def instructions(pe, rva):
    return list(md.disasm(pe.get_data(rva, 384), rva))

def signature(ins):
    pattern = []
    for i in ins:
        masked = set()
        if i.group(cs.CS_GRP_CALL) or i.group(cs.CS_GRP_JUMP):
            masked.update(range(i.imm_offset, i.imm_offset + i.imm_size))
        if any(o.type == X86_OP_MEM and o.mem.base == X86_REG_RIP for o in i.operands):
            masked.update(range(i.disp_offset, i.disp_offset + i.disp_size))
        pattern += [b'.' if j in masked else re.escape(bytes([b])) for j, b in enumerate(i.bytes)]
    return b''.join(pattern)

for a, b in mapping.items():
    if a == 0x3b15c80:  # verified below by code referencing the data flag
        continue
    src = instructions(old, a)
    for count in (40, 25, 16, 10):
        hits = [m.start() + section.VirtualAddress for m in re.finditer(signature(src[:count]), code, re.S)]
        if len(hits) == 1:
            break
    assert hits == [b], f'RVA {a:x}: expected {b:x}, found {hits}'
    print(f'{a:08x} -> {b:08x}: unique match ({min(count, len(src))} instructions)')

old_section = next(s for s in old.sections if s.Name.startswith(b'.text'))
verified_flag = False
for match in re.finditer(b'\x88\x05....', old_section.get_data(), re.S):
    at = old_section.VirtualAddress + match.start()
    ins = instructions(old, at)
    i = ins[0]
    if i.address + i.size + i.operands[0].mem.disp != 0x3b15c80:
        continue
    hits = [m.start() + section.VirtualAddress for m in re.finditer(signature(ins[:12]), code, re.S)]
    if len(hits) == 1:
        j = instructions(new, hits[0])[0]
        assert j.address + j.size + j.operands[0].mem.disp == mapping[0x3b15c80]
        verified_flag = True
assert verified_flag, 'Could not verify camera wall-recovery data reference'
print(f'All {len(mapping)} code/data mappings verified. This does not replace in-game validation.')
