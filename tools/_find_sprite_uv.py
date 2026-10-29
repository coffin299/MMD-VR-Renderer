import struct
import pathlib
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
import math

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
rdata = next(s for s in sections if s[0] == ".rdata")
raw0, raw1 = text[3], text[3] + text[4]


def va_to_off(va):
    rva = va - imgbase
    for name, va0, vs, raw, rsz in sections:
        if va0 <= rva < va0 + max(vs, rsz):
            return raw + (rva - va0)
    return None


def off_to_va_text(off):
    return imgbase + text[1] + (off - raw0)


md = Cs(CS_ARCH_X86, CS_MODE_64)

# Search .rdata for distinctive floats from sprite_overlay.cpp
targets = {
    "0.470705": 0.470705,
    "0.529299": 0.52929902,
    "0.23242199": 0.23242199,
    "130": 130.0,
    "115": 115.0,
}
rr0, rr1 = rdata[3], rdata[3] + rdata[4]
found = {k: [] for k in targets}
i = rr0
while i + 4 <= rr1:
    f = struct.unpack_from("<f", data, i)[0]
    for k, want in targets.items():
        if abs(f - want) < 1e-7:
            found[k].append(imgbase + rdata[1] + (i - rr0))
    i += 4
for k, v in found.items():
    print(k, len(v), [hex(x) for x in v[:5]])

# Find rip-relative loads of 0.470705
if found["0.470705"]:
    target = found["0.470705"][0]
    print("target VA", hex(target))
    refs = []
    i = raw0
    while i + 7 < raw1:
        # movss xmm, [rip+disp] = F3 0F 10 05 xx xx xx xx
        # or lea rax, [rip+disp]
        if data[i : i + 4] == b"\xF3\x0F\x10\x05":
            disp = struct.unpack_from("<i", data, i + 4)[0]
            next_ip = text[1] + (i + 8 - raw0)
            if imgbase + next_ip + disp == target:
                refs.append(off_to_va_text(i))
            i += 8
            continue
        i += 1
    print("movss refs", [hex(x) for x in refs])
    if refs:
        # find function start
        off = va_to_off(refs[0])
        prologue = None
        for back in range(1, 0x5000):
            o = off - back
            if data[o] == 0xC3 and data[o + 1] == 0xCC:
                prologue = o + 1
                while data[prologue] == 0xCC:
                    prologue += 1
                break
        print("PrepareFrameSpriteOverlay candidate", hex(off_to_va_text(prologue)) if prologue else None)
        if prologue:
            print("sig", data[prologue : prologue + 16].hex())
            for n, insn in enumerate(md.disasm(data[prologue : prologue + 0x100], off_to_va_text(prologue))):
                print(f"0x{insn.address:x}:\t{insn.mnemonic}\t{insn.op_str}")
                if n >= 40:
                    break
            # callers
            fn_rva = off_to_va_text(prologue) - imgbase
            callers = []
            i = raw0
            while i + 5 < raw1:
                if data[i] == 0xE8:
                    rel = struct.unpack_from("<i", data, i + 1)[0]
                    dest = text[1] + (i + 5 - raw0) + rel
                    if dest == fn_rva:
                        callers.append(off_to_va_text(i))
                i += 1
            print("callers", [hex(c) for c in callers[:10]], "n=", len(callers))
