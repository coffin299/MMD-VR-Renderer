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


def off_to_rva(off):
    return text[1] + (off - raw0)


# Look for D3DCOLOR immediates typical of axis gizmos within 64 bytes of each other
colors = {
    "red": [0xFFFF0000, 0xFFC00000, 0xFFE00000, 0xFFFF2020, 0xFFFF4040, 0xFFED145B],
    "green": [0xFF00FF00, 0xFF00C000, 0xFF00E000, 0xFF20FF20, 0xFF40FF40, 0xFF00FF40],
    "blue": [0xFF0000FF, 0xFF0000C0, 0xFF0000E0, 0xFF2020FF, 0xFF4040FF, 0xFF0080FF],
}

# Collect all positions of these immediates as imm32 in .text (mov reg, imm32 = B8+rd / C7 ...)
hits = {k: [] for k in colors}
i = raw0
while i + 5 <= raw1:
    # mov r32, imm32
    if 0xB8 <= data[i] <= 0xBF:
        imm = struct.unpack_from("<I", data, i + 1)[0]
        for k, vals in colors.items():
            if imm in vals:
                hits[k].append(i)
    # mov dword ptr [...], imm32 : C7 ...
    if data[i] in (0xC7,) and i + 6 < raw1:
        # skip for now; too many forms
        pass
    i += 1

print({k: len(v) for k, v in hits.items()})
# Find clusters where red/green/blue appear within 0x200 bytes
reds = hits["red"]
greens = set(hits["green"])
blues = set(hits["blue"])
clusters = []
for r in reds:
    g = next((x for x in hits["green"] if abs(x - r) < 0x300), None)
    b = next((x for x in hits["blue"] if abs(x - r) < 0x300), None)
    if g and b:
        clusters.append((min(r, g, b), r, g, b))
# unique by start
uniq = {}
for c in clusters:
    uniq[c[0] // 0x40] = c
print("clusters", len(uniq))
md = Cs(CS_ARCH_X86, CS_MODE_64)
for start, r, g, b in list(uniq.values())[:15]:
    addr = imgbase + off_to_rva(start)
    print(f"\ncluster file {hex(start)} VA {hex(addr)} r={hex(r)} g={hex(g)} b={hex(b)}")
    o = max(raw0, start - 0x20)
    for insn in md.disasm(data[o : start + 0x80], imgbase + off_to_rva(o)):
        print(f"  0x{insn.address:x}:\t{insn.mnemonic}\t{insn.op_str}")
        if insn.address > imgbase + off_to_rva(start) + 0x60:
            break
