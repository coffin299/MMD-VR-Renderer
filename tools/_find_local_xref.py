import struct
import pathlib

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


def off_to_rva(off):
    for name, va, vsize, raw, rsz in sections:
        if raw <= off < raw + rsz:
            return va + (off - raw), name
    return None, None


def rva_to_off(rva):
    for name, va, vsize, raw, rsz in sections:
        if va <= rva < va + max(vsize, rsz):
            return raw + (rva - va), name
    return None, None


idx = data.find(b"global")
# Prefer the UI block near Recording
while idx >= 0:
    window = data[idx : idx + 40]
    if b"local" in window and b"Recording" in data[idx : idx + 80]:
        break
    idx = data.find(b"global", idx + 1)
print("UI global at", hex(idx))
local_idx = data.find(b"local", idx)
targets = {
    "global": imgbase + off_to_rva(idx)[0],
    "local": imgbase + off_to_rva(local_idx)[0],
}
print({k: hex(v) for k, v in targets.items()})

text = next(s for s in sections if s[0] == ".text")
raw0, raw1 = text[3], text[3] + text[4]
hits = []
i = raw0
while i + 7 < raw1:
    b0 = data[i]
    b1 = data[i + 1]
    # REX.W lea r64,[rip+disp32] : 48/4C 8D ModRM(mod=00 rm=101)
    if b0 in (0x48, 0x4C) and b1 == 0x8D:
        modrm = data[i + 2]
        if (modrm & 0xC7) == 0x05:
            disp = struct.unpack_from("<i", data, i + 3)[0]
            next_rva = off_to_rva(i + 7)[0]
            abs_target = imgbase + next_rva + disp
            for label, want in targets.items():
                if abs_target == want:
                    hits.append((label, i, off_to_rva(i)[0], abs_target))
            i += 7
            continue
    i += 1

print("rip-relative lea hits", len(hits))
for h in hits:
    print(h[0], "file", hex(h[1]), "rva", hex(h[2]), "target", hex(h[3]))
    # dump surrounding bytes for context
    start = max(raw0, h[1] - 32)
    end = min(raw1, h[1] + 48)
    print(" ", data[start:end].hex())
