"""Static check of ERVR's game signatures against an eldenring.exe (no game launch needed).
usage: python tools/verify_signatures.py "C:\...\ELDEN RING\Game\eldenring.exe"
"""
import re, struct, sys
exe = sys.argv[1] if len(sys.argv) > 1 else r"C:\Program Files (x86)\Steam\steamapps\common\ELDEN RING\Game\eldenring.exe"
data = open(exe, 'rb').read()
e_lfanew = struct.unpack_from('<I', data, 0x3c)[0]
nsec = struct.unpack_from('<H', data, e_lfanew + 6)[0]
optsz = struct.unpack_from('<H', data, e_lfanew + 20)[0]
secs = []
off = e_lfanew + 24 + optsz
for i in range(nsec):
    vsize, va, rsize, rptr = struct.unpack_from('<IIII', data, off + 8)
    secs.append((va, vsize, rptr, rsize)); off += 40
def rva2off(rva):
    for va, vs, rp, rs in secs:
        if va <= rva < va + max(vs, rs): return rp + (rva - va)
def off2rva(o):
    for va, vs, rp, rs in secs:
        if rp <= o < rp + rs: return va + (o - rp)
def aob(pat):
    rx = b''.join(b'.' if p in ('?', '??') else re.escape(bytes([int(p, 16)])) for p in pat.split())
    return [m.start() for m in re.finditer(rx, data, re.S)]
checks = {
    "WorldChrMan": "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 0F 48 39 88",
    "FieldArea": "48 8B 3D ?? ?? ?? ?? 49 8B D8 48 8B F2 4C 8B F1 48 85 FF",
    "CSMenuMan (MenuNav)": "48 8B 0D ?? ?? ?? ?? 48 8B 53 08 48 8B 92 D8 00 00 00 48 83 C4 20 5B",
    "GetDmyPos call": "48 8d 44 24 40 48 89 44 24 28 41 8b 00 89 44 24 40 4c 8d 44 24 40 49 8b ca e8",
}
for name, pat in checks.items():
    hits = aob(pat); print(f"{name}: {len(hits)} hit(s) at", [hex(off2rva(h)) for h in hits[:3]])
o = rva2off(0x3bc5e0); print("camera step @0x3bc5e0:", data[o:o + 12].hex(' '), "(expect 48 89 5c 24 08 57 48 83 ec 30 f3 0f)")
for n in (b".?AVNoUpdateInterface@CS@@\0", b".?AVPlayerIns@CS@@\0"):
    i = data.find(n); print(n.decode().strip('\0'), "rva", hex(off2rva(i)) if i >= 0 else "MISSING")
