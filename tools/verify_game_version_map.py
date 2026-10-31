"""Verify ERVR's RVA map between two local game executables.
Usage: python verify_game_version_map.py OLD_EXE NEW_EXE
Each exe is identified by its file version (2.6.2.0, 2.7.1.0 or 2.7.2.0), which picks its column of
kGameRvaMap in game_version.h. Every row's OLD site, as a signature with rel32 branch targets and
RIP-relative displacements masked, must match exactly once in NEW, at the NEW column's RVA.
Read-only. Requires capstone and pefile; never launches or modifies either file.
"""
import re
import sys
from pathlib import Path
import capstone as cs
from capstone.x86 import X86_OP_MEM, X86_REG_RIP
import pefile

COLUMNS = {'2.6.2.0': 0, '2.7.1.0': 1, '2.7.2.0': 2}
WALL_RECOVERY = 0x3b15c80   # a data flag, verified through the code that writes it

def load(path):
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_RESOURCE']])
    info = pe.VS_FIXEDFILEINFO[0]
    version = f'{info.FileVersionMS >> 16}.{info.FileVersionMS & 0xffff}.{info.FileVersionLS >> 16}.{info.FileVersionLS & 0xffff}'
    if version not in COLUMNS:
        sys.exit(f'{path}: version {version} has no column in kGameRvaMap')
    return pe, version

(old, old_version), (new, new_version) = (load(path) for path in sys.argv[1:3])
md = cs.Cs(cs.CS_ARCH_X86, cs.CS_MODE_64)
md.detail = True
section = next(s for s in new.sections if s.Name.startswith(b'.text'))
code = section.get_data()
header = (Path(__file__).resolve().parents[1] / 'src/ervr/game_version.h').read_text()
rows = [tuple(int(v, 16) for v in row) for row in re.findall(r'\{(0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+)\}', header)]
assert rows, 'kGameRvaMap not found in game_version.h'
mapping = {row[0]: (row[COLUMNS[old_version]], row[COLUMNS[new_version]]) for row in rows}
print(f'{old_version} -> {new_version}: {len(mapping)} rows')

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

for legacy, (a, b) in mapping.items():
    if legacy == WALL_RECOVERY:
        continue
    src = instructions(old, a)
    for count in (40, 25, 16, 10):
        hits = [m.start() + section.VirtualAddress for m in re.finditer(signature(src[:count]), code, re.S)]
        if len(hits) == 1:
            break
    assert hits == [b], f'RVA {legacy:x}: {old_version} {a:x} expected at {new_version} {b:x}, found {[hex(h) for h in hits]}'
    print(f'{a:08x} -> {b:08x}: unique match ({min(count, len(src))} instructions)')

old_section = next(s for s in old.sections if s.Name.startswith(b'.text'))
flag_old, flag_new = mapping[WALL_RECOVERY]
verified_flag = False
for match in re.finditer(b'\x88\x05....', old_section.get_data(), re.S):
    at = old_section.VirtualAddress + match.start()
    ins = instructions(old, at)
    i = ins[0]
    if i.address + i.size + i.operands[0].mem.disp != flag_old:
        continue
    hits = [m.start() + section.VirtualAddress for m in re.finditer(signature(ins[:12]), code, re.S)]
    if len(hits) == 1:
        j = instructions(new, hits[0])[0]
        assert j.address + j.size + j.operands[0].mem.disp == flag_new
        verified_flag = True
assert verified_flag, 'Could not verify camera wall-recovery data reference'
print(f'All {len(mapping)} code/data mappings verified. This does not replace in-game validation.')
