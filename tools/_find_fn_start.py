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

# Find function start for 0x1400236ba: walk back looking for CC padding then start
targets = [0x1400236ba, 0x1400248f6, 0x140022789, 0x14000fb41, 0x1400111cf]
for t in targets:
    off = va_to_off(t)
    found = None
    for back in range(0, 0x3000):
        o = off - back
        # int3 then non-int3
        if data[o] == 0xCC:
            # skip run of CCs
            s = o + 1
            while s < raw1 and data[s] == 0xCC:
                s += 1
            if s < off and s > o:
                # check if this is immediately after a ret (C3) or before our target
                prev = o
                while prev > raw0 and data[prev] == 0xCC:
                    prev -= 1
                if data[prev] in (0xC3, 0xC2) or True:
                    found = s
                    break
    print(f"target {hex(t)} -> start {hex(off_to_va(found)) if found else None} dist {off-found if found else None}")

# Better: use UNDNAME-less: scan for call E8 to a helper near DrawPrimitive, find enclosing
# Look at bytes just before first colored XYZ use of a15dc in this region
print("\n--- scan for function prologues near 0x140022500..0x140023800 ---")
start = va_to_off(0x140022400)
end = va_to_off(0x140023800)
i = start
while i < end:
    # push rbp (40 55) or sub rsp, imm (48 83 EC / 48 81 EC)
    if data[i : i + 2] == b"\x40\x55" or data[i : i + 3] == b"\x48\x83\xec" or data[i : i + 3] == b"\x48\x81\xec":
        # require preceding CC or C3
        if i > raw0 and data[i - 1] in (0xCC, 0xC3):
            print("prologue at", hex(off_to_va(i)))
    i += 1

# Disassemble from known HUD mid that had GetRenderTarget - find who calls Draw for icons
# Look for DrawPrimitiveUP / DrawIndexedPrimitive near XYZ text at 0x1400236ba
print("\n--- calls within +/- 0x200 of 0x1400236ba ---")
o = va_to_off(0x1400234ba)
for insn in md.disasm(data[o : o + 0x400], 0x1400234ba):
    if insn.mnemonic in ("call", "jmp", "je", "jne", "cmp", "test") or "a15" in insn.op_str or "Draw" in insn.op_str:
        print(f"0x{insn.address:x}:\t{insn.mnemonic}\t{insn.op_str}")

# What string is at the lea for X at 0x1400236ba?
def resolve_lea(va):
    off = va_to_off(va)
    # lea rdx, [rip+disp] = 48 8D 15 xx xx xx xx
    assert data[off] == 0x48 and data[off + 1] == 0x8D
    disp = struct.unpack_from("<i", data, off + 3)[0]
    target = va + 7 + disp
    # find which section
    rva = target - imgbase
    for name, sva, vsize, raw, rsz in sections:
        if sva <= rva < sva + max(vsize, rsz):
            foff = raw + (rva - sva)
            s = data[foff : foff + 16]
            return target, s.split(b"\0")[0]
    return target, None

for va in [0x1400236ba, 0x14002370c, 0x140023756]:
    print("lea", hex(va), resolve_lea(va))

# Check flag 0xa1b90 references - likely "show info" or similar
print("\n--- xref count to offset a1b90 style cmp byte ---")
# just count cmp byte ptr [reg+0xa1b90]
pat = b"\x80\xbe"  # cmp byte ptr [rsi+disp32], ib with 32-bit disp - not unique
# search for 90 1b 0a 00 as displacement
disp = struct.pack("<I", 0xA1B90)
idxs = []
i = raw0
while True:
    j = data.find(disp, i, raw1)
    if j < 0:
        break
    idxs.append(off_to_va(j))
    i = j + 1
print("0xa1b90 imm occurrences in .text:", len(idxs), [hex(x) for x in idxs[:15]])
