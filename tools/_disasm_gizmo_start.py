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
raw0 = text[3]


def va_to_off(va):
    return text[3] + ((va - imgbase) - text[1])


def off_to_va(off):
    return imgbase + text[1] + (off - raw0)


md = Cs(CS_ARCH_X86, CS_MODE_64)
fn = 0x1400225c0
print("=== function", hex(fn), "===")
# show first 120 instructions and note early exits
o = va_to_off(fn)
n = 0
for insn in md.disasm(data[o : o + 0x400], fn):
    print(f"0x{insn.address:x}:\t{insn.mnemonic}\t{insn.op_str}")
    n += 1
    if n >= 100:
        break

# find end (ret) and size
print("\n=== find ret ===")
for insn in md.disasm(data[o : o + 0x3000], fn):
    if insn.mnemonic == "ret":
        print("ret at", hex(insn.address), "size", hex(insn.address - fn))
        break

# callers of 0x1400225c0
fn_rva = fn - imgbase
callers = []
i = raw0
raw1 = text[3] + text[4]
while i + 5 < raw1:
    if data[i] == 0xE8:
        rel = struct.unpack_from("<i", data, i + 1)[0]
        dest = text[1] + (i + 5 - raw0) + rel
        if dest == fn_rva:
            callers.append(off_to_va(i))
    i += 1
print("callers of draw-gizmo fn:", [hex(c) for c in callers], "n=", len(callers))

# Also check 0x140025bf0 which was called near icon draws
fn2 = 0x140025bf0
fn2_rva = fn2 - imgbase
callers2 = []
i = raw0
while i + 5 < raw1:
    if data[i] == 0xE8:
        rel = struct.unpack_from("<i", data, i + 1)[0]
        dest = text[1] + (i + 5 - raw0) + rel
        if dest == fn2_rva:
            callers2.append(off_to_va(i))
    i += 1
print("callers of 0x140025bf0:", [hex(c) for c in callers2[:20]], "n=", len(callers2))

# Disasm 0x140025bf0 briefly
print("\n=== 0x140025bf0 ===")
o2 = va_to_off(fn2)
n = 0
for insn in md.disasm(data[o2 : o2 + 0x200], fn2):
    print(f"0x{insn.address:x}:\t{insn.mnemonic}\t{insn.op_str}")
    n += 1
    if insn.mnemonic == "ret" or n >= 60:
        break

# Show context around each caller of fn
for c in callers[:5]:
    print(f"\n=== caller context {hex(c)} ===")
    oc = va_to_off(c) - 0x40
    for insn in md.disasm(data[oc : oc + 0x80], off_to_va(oc)):
        mark = " <<<" if insn.address == c else ""
        print(f"0x{insn.address:x}:\t{insn.mnemonic}\t{insn.op_str}{mark}")
        if insn.address > c + 0x20:
            break
