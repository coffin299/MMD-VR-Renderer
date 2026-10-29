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
text = next(s for s in sections if s[0] == ".text")
raw0, raw1 = text[3], text[3] + text[4]
md = Cs(CS_ARCH_X86, CS_MODE_64)


def off_to_va(off):
    return imgbase + text[1] + (off - raw0)


# Global used by Local gizmo at 0x1400225dd: rip+0x122014 -> 0x1401445f8
slot = 0x1401445f8
# Find all rip-relative loads of this slot
refs = []
i = raw0
while i + 7 < raw1:
    if data[i] in (0x48, 0x4C) and data[i + 1] == 0x8B and (data[i + 2] & 0xC7) == 0x05:
        disp = struct.unpack_from("<i", data, i + 3)[0]
        next_rva = text[1] + (i + 7 - raw0)
        if imgbase + next_rva + disp == slot:
            refs.append(off_to_va(i))
        i += 7
        continue
    i += 1
print("loads of app global", len(refs), [hex(x) for x in refs[:15]])

# At PrepareFrameSprite / frame fn 0x1400274f0, rcx is app - does caller pass from same global?
print("\n=== caller 0x140025b00 context ===")
o = text[3] + ((0x140025ac0 - imgbase) - text[1])
for insn in md.disasm(data[o : o + 0x80], 0x140025ac0):
    print(f"0x{insn.address:x}:\t{insn.mnemonic}\t{insn.op_str}")
    if insn.address >= 0x140025b20:
        break

# Inside frame fn after mov r12, rcx - later uses r12+0x368?
print("\n=== 0x368 uses near strip UV 0x140028aff ===")
o = text[3] + ((0x140028a80 - imgbase) - text[1])
for insn in md.disasm(data[o : o + 0x120], 0x140028a80):
    if "0x368" in insn.op_str or "0x4302" in insn.op_str or insn.address in range(0x140028af0, 0x140028b40):
        print(f"0x{insn.address:x}:\t{insn.mnemonic}\t{insn.op_str}")
