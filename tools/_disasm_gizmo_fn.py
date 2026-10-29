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
    rva = va - imgbase
    return text[3] + (rva - text[1])


def off_to_va(off):
    return imgbase + text[1] + (off - raw0)


md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

# Walk back from 0x1400236ba to find function prologue
target = 0x1400236ba
off = va_to_off(target)
# scan backwards for common prologue: push rbp / sub rsp / mov [rsp+..]
prologue = None
for back in range(0x20, 0x800, 1):
    o = off - back
    # look for: 40 55 (push rbp) or 48 89 5c 24 (mov [rsp+..],rbx) after int3 padding
    # or CC CC then function start
    if data[o] == 0xCC and data[o + 1] != 0xCC:
        # next byte starts func
        cand = o + 1
        # verify looks like prologue
        if data[cand] in (0x40, 0x41, 0x48, 0x4C, 0x55, 0x56, 0x57, 0x53):
            prologue = cand
            break
    if data[o : o + 2] == b"\x40\x55":  # push rbp with REX
        prologue = o
        break
    if data[o] == 0x55 and data[o + 1] in (0x48, 0x53, 0x56, 0x57, 0x41):
        prologue = o
        break

print("prologue candidate", hex(off_to_va(prologue)) if prologue else None)

# Also try walking back to previous ret/int3
if prologue is None:
    for back in range(1, 0x1000):
        o = off - back
        if data[o] == 0xC3 and data[o + 1] == 0xCC:
            prologue = o + 1
            while data[prologue] == 0xCC:
                prologue += 1
            break
    print("alt prologue", hex(off_to_va(prologue)) if prologue else None)

fn = off_to_va(prologue) if prologue else 0x140022000
print("disasm from", hex(fn))
o = va_to_off(fn)
count = 0
calls = []
for insn in md.disasm(data[o : o + 0x1200], fn):
    mark = ""
    if insn.mnemonic == "call":
        calls.append(insn.address)
        mark = "  <<<"
    if insn.mnemonic == "ret":
        print(f"0x{insn.address:x}:\t{insn.mnemonic}\t{insn.op_str}{mark}")
        break
    # print interesting parts: early flags, draws, and around XYZ
    if (
        count < 80
        or abs(insn.address - 0x1400236ba) < 0x120
        or abs(insn.address - 0x1400248f6) < 0x80
        or insn.mnemonic == "call"
        or (insn.mnemonic == "cmp" and "0xa1" in insn.op_str)
        or (insn.mnemonic == "test" and "byte" in insn.op_str)
        or "0xa1" in insn.op_str
    ):
        print(f"0x{insn.address:x}:\t{insn.mnemonic}\t{insn.op_str}{mark}")
    count += 1
    if count > 900:
        print("...truncated...")
        break

print("\ncall sites count", len(calls))

# Find callers of this function
fn_rva = fn - imgbase
pattern = struct.pack("<i", 0)  # fill later
callers = []
i = raw0
while i + 5 < raw1:
    if data[i] == 0xE8:
        rel = struct.unpack_from("<i", data, i + 1)[0]
        dest = text[1] + (i + 5 - raw0) + rel
        if dest == fn_rva:
            callers.append(off_to_va(i))
    i += 1
print("direct callers:", [hex(c) for c in callers[:20]], "count", len(callers))

# Also find callers of second XYZ block function if different - walk back from 0x1400248f6
target2 = 0x1400248f6
off2 = va_to_off(target2)
prologue2 = None
for back in range(1, 0x1500):
    o = off2 - back
    if data[o] == 0xC3 and data[o + 1] == 0xCC:
        prologue2 = o + 1
        while data[prologue2] == 0xCC:
            prologue2 += 1
        break
print("fn2 prologue", hex(off_to_va(prologue2)) if prologue2 else None, "same?", prologue2 == prologue)
