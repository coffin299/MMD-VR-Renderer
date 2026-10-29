import struct
import pathlib
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

p = pathlib.Path(r"C:\Users\coffi\Desktop\MMMD\MikuMikuDance.exe")
data = p.read_bytes()
e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
opt = e_lfanew + 24
size_opt = struct.unpack_from("<H", data, e_lfanew + 20)[0]
num = struct.unpack_from("<H", data, e_lfanew + 6)[0]
sec = opt + size_opt
sections = []
for i in range(num):
    off = sec + i * 40
    name = data[off : off + 8].split(b"\0", 1)[0].decode("ascii", "ignore")
    vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", data, off + 8)
    sections.append((name, va, vsize, rawptr, rawsize))
imgbase = struct.unpack_from("<Q", data, opt + 24)[0]


def rva_to_off(rva):
    for name, va, vsize, raw, rsz in sections:
        if va <= rva < va + max(vsize, rsz):
            return raw + (rva - va)
    return None


md = Cs(CS_ARCH_X86, CS_MODE_64)
start = 0x14000FB41
end = 0x14000FC40
off_s = rva_to_off(start - imgbase)
off_e = rva_to_off(end - imgbase)
print("=== HUD function head ===")
for insn in md.disasm(data[off_s:off_e], start):
    print(f"0x{insn.address:x}:\t{insn.mnemonic}\t{insn.op_str}")

# Find callers of HUD function start via call rel32
hud_rva = 0xFB41
text = next(s for s in sections if s[0] == ".text")
raw0, raw1 = text[3], text[3] + text[4]
callers = []
i = raw0
while i + 5 <= raw1:
    if data[i] == 0xE8:
        rel = struct.unpack_from("<i", data, i + 1)[0]
        # next rip rva
        next_rva = (i - raw0) + text[1] + 5
        target = (next_rva + rel) & 0xFFFFFFFF
        # sign extend for comparison with hud_rva in 32-bit space of module
        if target == hud_rva:
            callers.append((i - raw0) + text[1])
    i += 1
print("callers of HUD fn:", [hex(imgbase + c) for c in callers[:20]], "count", len(callers))

# Also look for cmp/test/jz shortly before HUD call sites
for c in callers[:5]:
    a = imgbase + c - 0x40
    o = rva_to_off(a - imgbase)
    print(f"\n--- context before call {hex(imgbase+c)} ---")
    for insn in md.disasm(data[o : o + 0x50], a):
        if insn.address > imgbase + c + 5:
            break
        print(f"0x{insn.address:x}:\t{insn.mnemonic}\t{insn.op_str}")
