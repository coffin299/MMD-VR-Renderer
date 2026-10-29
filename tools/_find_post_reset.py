"""Locate MMD 9.32 x64 PostDeviceReset from recovered renderer offsets."""
import pathlib
import struct
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
    name = data[off:off + 8].split(b"\0", 1)[0].decode("ascii", "ignore")
    vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", data, off + 8)
    sections.append((name, va, vsize, rawptr, rawsize))
imgbase = struct.unpack_from("<Q", data, opt + 24)[0]
text = next(s for s in sections if s[0] == ".text")
raw0, raw1 = text[3], text[3] + text[4]


def off_to_va(off):
    return imgbase + text[1] + (off - raw0)


def va_to_off(va):
    return text[3] + (va - imgbase - text[1])


md = Cs(CS_ARCH_X86, CS_MODE_64)

# Verified MikuDanceStudio x64 renderer offsets.
offsets = {
    "device": 240064,
    "screenW": 240072,
    "screenH": 240076,
    "present": 240104,
    "captureSurface": 240168,
    "backbuffer": 240176,
    "depth": 240184,
    "hdr": 240200,
}
print({k: hex(v) for k, v in offsets.items()})

# Raw displacement hits for the fields released by PostDeviceReset.
for name, disp in offsets.items():
    needle = struct.pack("<I", disp)
    hits = []
    pos = raw0
    while True:
        pos = data.find(needle, pos, raw1)
        if pos < 0:
            break
        hits.append(off_to_va(pos))
        pos += 1
    print(name, len(hits), [hex(h) for h in hits[:30]])

# A candidate has references to captureSurface + backbuffer + depth within 0x300 bytes.
all_hits = {}
for name, disp in offsets.items():
    pos = raw0
    hs = []
    needle = struct.pack("<I", disp)
    while True:
        pos = data.find(needle, pos, raw1)
        if pos < 0:
            break
        hs.append(pos)
        pos += 1
    all_hits[name] = hs

candidates = []
for h in all_hits["captureSurface"]:
    neighborhood = range(h - 0x250, h + 0x250)
    names = [n for n, hs in all_hits.items() if any(x in neighborhood for x in hs)]
    if {"captureSurface", "backbuffer", "depth", "present", "device"}.issubset(names):
        candidates.append(h)
print("candidate raw offsets", [hex(x) for x in candidates])

seen = set()
for h in candidates:
    # Find nearest plausible function start (CC padding / previous RET).
    start = None
    for back in range(1, 0x3000):
        q = h - back
        if data[q] == 0xC3 and data[q + 1] == 0xCC:
            start = q + 1
            while data[start] == 0xCC:
                start += 1
            break
    if not start or start in seen:
        continue
    seen.add(start)
    start_va = off_to_va(start)
    print("\n=== candidate fn", hex(start_va), "sig", data[start:start + 16].hex(), "===")
    # Print instructions involving renderer offsets and virtual calls until ret.
    count = 0
    for insn in md.disasm(data[start:start + 0x2500], start_va):
        interesting = (
            any(hex(v) in insn.op_str for v in offsets.values())
            or insn.mnemonic == "call"
            or insn.mnemonic == "ret"
        )
        if interesting:
            print(f"0x{insn.address:x}:\t{insn.mnemonic}\t{insn.op_str}")
        count += 1
        if insn.mnemonic == "ret" or count > 2000:
            break

# Direct callers reveal GrowRenderTarget and the output/canvas dialog path.
post_reset_va = 0x1400A6160
post_reset_rva = post_reset_va - imgbase
print("A5EA0", data[va_to_off(0x1400A5EA0):va_to_off(0x1400A5EA0) + 16].hex(" "))
callers = []
i = raw0
while i + 5 < raw1:
    if data[i] == 0xE8:
        rel = struct.unpack_from("<i", data, i + 1)[0]
        dest = text[1] + (i + 5 - raw0) + rel
        if dest == post_reset_rva:
            callers.append(off_to_va(i))
    i += 1
print("\nPostDeviceReset direct callers", [hex(c) for c in callers])
for call in callers:
    o = max(raw0, va_to_off(call) - 0x100)
    print(f"\n=== caller context {hex(call)} ===")
    for insn in md.disasm(data[o:o + 0x400], off_to_va(o)):
        mark = " <<<" if insn.address == call else ""
        print(f"0x{insn.address:x}:\t{insn.mnemonic}\t{insn.op_str}{mark}")
        if insn.address > call + 0x250:
            break
