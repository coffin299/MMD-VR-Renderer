"""Map PrepareFrameSpriteOverlay / playbackActive using MMD 9.32 x64."""
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


def va_to_off(va):
    return text[3] + ((va - imgbase) - text[1])


def off_to_va(off):
    return imgbase + text[1] + (off - raw0)


md = Cs(CS_ARCH_X86, CS_MODE_64)

# Find float constants unique to local XYZ strip:
# 130.0f = 0x43020000, 115.0f = 0x42E60000, 0.470705 ≈ look for lea of "local" already known
# Better: search for immediate 0x43020000 (130.f) near cmp byte [reg+0x368]
disp = struct.pack("<I", 0x368)
hits = []
i = raw0
while True:
    j = data.find(disp, i, raw1)
    if j < 0:
        break
    hits.append(j)
    i = j + 1
print("0x368 imm sites", len(hits))

# Look for cmp byte ptr [reg+0x368], 0  patterns near 130.0f use
f130 = struct.pack("<I", 0x43020000)  # 130.0f
f115 = struct.pack("<I", 0x42E60000)  # 115.0f
sites130 = []
i = raw0
while True:
    j = data.find(f130, i, raw1)
    if j < 0:
        break
    sites130.append(off_to_va(j))
    i = j + 1
print("130.0f sites", [hex(x) for x in sites130[:20]], "n=", len(sites130))

# For each 130.f, show nearby code looking for playback checks
for va in sites130[:8]:
    o = va_to_off(va) - 0x40
    print(f"\n=== around 130.f @ {hex(va)} ===")
    for insn in md.disasm(data[o : o + 0xA0], off_to_va(o)):
        mark = ""
        if "0x368" in insn.op_str or "0x43020000" in insn.op_str or insn.mnemonic == "call":
            mark = " <<<"
        print(f"0x{insn.address:x}:\t{insn.mnemonic}\t{insn.op_str}{mark}")
        if insn.address > va + 0x40:
            break

# Global app pointer used by gizmo: RVA 0x1445f8 from earlier
print("\nglobal slot RVA 0x1445f8 content is runtime-only")
# Find lea/mov of rip-relative that loads that slot in sprite-like code
# Search for functions that reference both 0x368 and DrawPrimitive-ish

# Find prologue near first 130.f site by walking back
if sites130:
    target = sites130[0]
    off = va_to_off(target)
    prologue = None
    for back in range(1, 0x4000):
        o = off - back
        if data[o] == 0xC3 and data[o + 1] == 0xCC:
            prologue = o + 1
            while data[prologue] == 0xCC:
                prologue += 1
            break
    print("fn containing first 130.f:", hex(off_to_va(prologue)) if prologue else None)
    if prologue:
        sig = data[prologue : prologue + 16]
        print("sig:", sig.hex())
        # first 40 insns
        for n, insn in enumerate(md.disasm(data[prologue : prologue + 0x80], off_to_va(prologue))):
            print(f"0x{insn.address:x}:\t{insn.mnemonic}\t{insn.op_str}")
            if n >= 25:
                break
