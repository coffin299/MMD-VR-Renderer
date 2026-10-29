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


def off_to_va(off):
    return imgbase + text[1] + (off - raw0)


md = Cs(CS_ARCH_X86, CS_MODE_64)
disp = struct.pack("<I", 0xA11E4)
hits = []
i = raw0
while True:
    j = data.find(disp, i, raw1)
    if j < 0:
        break
    hits.append(j)
    i = j + 1
print("0xa11e4 sites", len(hits))
for h in hits:
    # back up a bit to show insn
    o = max(raw0, h - 8)
    for insn in md.disasm(data[o : h + 8], off_to_va(o)):
        if insn.address <= off_to_va(h) < insn.address + insn.size or abs(insn.address - off_to_va(h)) < 0x10:
            print(f"  0x{insn.address:x}:\t{insn.mnemonic}\t{insn.op_str}")
    print("  ---")

# Verify signature bytes at 0x1400225c0 for version gate
sig_va = 0x1400225c0
sig_off = text[3] + ((sig_va - imgbase) - text[1])
sig = data[sig_off : sig_off + 16]
print("sig at RVA 0x225c0:", sig.hex())

# Resolve global ptr slot used by the fn
# mov rsi, [rip+0x122014] at 0x1400225dd
insn_va = 0x1400225dd
next_ip = 0x1400225e4
target = next_ip + 0x122014
print("global ptr slot VA", hex(target), "RVA", hex(target - imgbase))

# Also check if screen-capture path skips this - look at caller switch
# Disassemble more carefully around 0x1400dbc7d - find real function
print("\n=== switch/dispatch around caller ===")
# look for lea/table earlier
for start in range(0x1400dbb00, 0x1400dbc80, 0x10):
    pass
o = text[3] + ((0x1400dbb80 - imgbase) - text[1])
for insn in md.disasm(data[o : o + 0x150], 0x1400dbb80):
    print(f"0x{insn.address:x}:\t{insn.mnemonic}\t{insn.op_str}")
    if insn.address >= 0x1400dbc90:
        break
