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
    for name, va, vsize, raw, rsz in sections:
        if raw <= off < raw + rsz:
            return va + (off - raw), name
    return None, None


# Find Japanese / ASCII markers for manipulation UI
needles = [
    "カメラ操作".encode("cp932"),
    "bone manipulation",
    b"global",
]
# For each UI string near Recording/global, already known.
# Search LEA rip refs to 'X\0' 'Y\0' 'Z\0' near each other - gizmo labels

# Find all lea rip to short strings "X","Y","Z"
md = Cs(CS_ARCH_X86, CS_MODE_64)
# Build map of short string RVAs
string_rvas = {}
# scan .rdata
rdata = next(s for s in sections if s[0] == ".rdata")
rr0, rr1 = rdata[3], rdata[3] + rdata[4]
i = rr0
while i + 2 < rr1:
    if data[i] in (ord("X"), ord("Y"), ord("Z")) and data[i + 1] == 0:
        # prefer standalone, not mid-word: prev is 0 or non-alnum
        if i == rr0 or data[i - 1] < 0x20 or data[i - 1] > 0x7E:
            rva = rdata[1] + (i - rr0)
            string_rvas[imgbase + rva] = chr(data[i])
    i += 1
print("standalone X/Y/Z strings", len(string_rvas))

# rip-relative leas targeting those
xyz_leas = []
i = raw0
while i + 7 < raw1:
    if data[i] in (0x48, 0x4C) and data[i + 1] == 0x8D and (data[i + 2] & 0xC7) == 0x05:
        disp = struct.unpack_from("<i", data, i + 3)[0]
        next_rva = text[1] + (i + 7 - raw0)
        target = imgbase + next_rva + disp
        if target in string_rvas:
            xyz_leas.append((i, imgbase + text[1] + (i - raw0), string_rvas[target], target))
        i += 7
        continue
    i += 1
print("lea to X/Y/Z", len(xyz_leas))
# Cluster by address proximity
xyz_leas.sort(key=lambda t: t[0])
clusters = []
cur = []
for item in xyz_leas:
    if not cur or item[0] - cur[-1][0] < 0x100:
        cur.append(item)
    else:
        if len({c[2] for c in cur}) >= 2:
            clusters.append(cur)
        cur = [item]
if cur and len({c[2] for c in cur}) >= 2:
    clusters.append(cur)
print("clusters with multiple axes", len(clusters))
for c in clusters[:20]:
    labels = "".join(x[2] for x in c)
    print(hex(c[0][1]), labels, [(x[2], hex(x[1])) for x in c])
    # show a few insns before first
    o = max(raw0, c[0][0] - 0x30)
    for insn in md.disasm(data[o : c[-1][0] + 0x40], imgbase + text[1] + (o - raw0)):
        print(f"  0x{insn.address:x}:\t{insn.mnemonic}\t{insn.op_str}")
        if insn.address > c[-1][1] + 0x20:
            break
    print()
