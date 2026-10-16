#!/usr/bin/env python3
"""Build a tiny symless Win32 PE used for the in-guest JIT A/B benchmark.

The emulator's JIT only kicks in for 32-bit flat protected-mode code
(!code16 && CR0.PE && flat CS/SS), so the workload has to run as a real
Win32 image loaded by Windows 95.  There is no mingw toolchain on this
machine, so the code section is encoded by hand and wrapped in a minimal
PE32 header here.

The workload is a register-only, deterministic loop (the same shape the
host-side tests/bench_jit.c uses) that never exits, which lets the
emulator's own [PERF] MIPS counter measure steady-state in-guest
throughput for the same binary with the JIT on and off.

Usage: mk_guest_bench.py <output.exe>
"""
import struct
import sys

# --- code section -----------------------------------------------------
#   mov eax, 0
#   mov ecx, 0x7fffffff
#   mov edx, 1
# loop@0x0f:
#   add eax, ecx
#   sub ecx, edx
#   cmp ecx, 0
#   jnz loop
#   mov ecx, 0x7fffffff     ; restart the countdown, so the loop never ends
#   jmp loop
CODE = bytes([
    0xB8, 0x00, 0x00, 0x00, 0x00,       # 00: mov eax, 0
    0xB9, 0xFF, 0xFF, 0xFF, 0x7F,       # 05: mov ecx, 0x7fffffff
    0xBA, 0x01, 0x00, 0x00, 0x00,       # 0a: mov edx, 1
    0x01, 0xC8,                         # 0f: add eax, ecx
    0x29, 0xD1,                         # 11: sub ecx, edx
    0x83, 0xF9, 0x00,                   # 13: cmp ecx, 0
    0x75, 0xF7,                         # 16: jnz 0x0f
    0xB9, 0xFF, 0xFF, 0xFF, 0x7F,       # 18: mov ecx, 0x7fffffff
    0xEB, 0xF0,                         # 1d: jmp 0x0f
])
# A single absolute dword after the code so the image carries one real
# relocation and can be loaded at any base if 0x400000 is taken.  It is
# never executed (the preceding jmp loops away from it).
DUMMY_OFF = len(CODE)
CODE += struct.pack('<I', 0x00400000)

FILE_ALIGN = 0x200
SECT_ALIGN = 0x1000
IMAGE_BASE = 0x400000
TEXT_RVA = 0x1000
RELOC_RVA = 0x2000


def build():
    text_raw = CODE + b'\x00' * (FILE_ALIGN - len(CODE) % FILE_ALIGN)

    # .reloc: one HIGHLOW fixup for the dummy dword in .text
    reloc = struct.pack('<II', TEXT_RVA, 12)
    reloc += struct.pack('<HH', (3 << 12) | DUMMY_OFF, 0)
    reloc_raw = reloc + b'\x00' * (FILE_ALIGN - len(reloc))

    dos = bytearray(64)
    dos[0:2] = b'MZ'
    struct.pack_into('<H', dos, 0x02, 0x90)   # e_cblp
    struct.pack_into('<H', dos, 0x04, 0x03)   # e_cp
    struct.pack_into('<H', dos, 0x08, 0x04)   # e_cparhdr
    struct.pack_into('<H', dos, 0x18, 0x40)   # e_lfarlc
    struct.pack_into('<I', dos, 0x3C, 0x40)   # e_lfanew -> PE header at 0x40

    coff = struct.pack('<HHIIIHH',
                       0x014C,          # IMAGE_FILE_MACHINE_I386
                       2,               # NumberOfSections
                       0, 0, 0,         # TimeDateStamp / symbols
                       0xE0,            # SizeOfOptionalHeader
                       0x0102)          # EXECUTABLE_IMAGE | 32BIT_MACHINE

    opt = struct.pack('<HBB', 0x010B, 0, 0)          # PE32 magic
    opt += struct.pack('<I', len(text_raw))          # SizeOfCode
    opt += struct.pack('<I', len(reloc_raw))         # SizeOfInitializedData
    opt += struct.pack('<I', 0)                      # SizeOfUninitializedData
    opt += struct.pack('<I', TEXT_RVA)               # AddressOfEntryPoint
    opt += struct.pack('<I', TEXT_RVA)               # BaseOfCode
    opt += struct.pack('<I', RELOC_RVA)              # BaseOfData
    opt += struct.pack('<I', IMAGE_BASE)
    opt += struct.pack('<I', SECT_ALIGN)
    opt += struct.pack('<I', FILE_ALIGN)
    opt += struct.pack('<HH', 4, 0)                  # OS version
    opt += struct.pack('<HH', 0, 0)                  # image version
    opt += struct.pack('<HH', 4, 0)                  # subsystem version
    opt += struct.pack('<I', 0)                      # Win32VersionValue
    opt += struct.pack('<I', 0x3000)                 # SizeOfImage
    opt += struct.pack('<I', FILE_ALIGN)             # SizeOfHeaders
    opt += struct.pack('<I', 0)                      # CheckSum
    opt += struct.pack('<H', 2)                      # GUI subsystem
    opt += struct.pack('<H', 0)                      # DllCharacteristics
    opt += struct.pack('<IIII', 0x100000, 0x1000, 0x100000, 0x1000)
    opt += struct.pack('<I', 0)                      # LoaderFlags
    opt += struct.pack('<I', 16)                     # NumberOfRvaAndSizes
    opt += b'\x00' * (16 * 8)                        # data directories
    assert len(opt) == 0xE0, len(opt)

    def section(name, vaddr, raw, chars):
        return struct.pack('<8sIIIIIIHHI', name, len(raw), vaddr,
                           len(raw), 0, 0, 0, 0, 0, chars)

    headers = bytes(dos) + b'PE\x00\x00' + coff + opt
    headers += section(b'.text', TEXT_RVA, text_raw, 0x70000020)
    headers += section(b'.reloc', RELOC_RVA, reloc_raw, 0x42000040)
    headers += b'\x00' * (FILE_ALIGN - len(headers) % FILE_ALIGN)

    text_ptr = len(headers)
    reloc_ptr = text_ptr + len(text_raw)
    # patch PointerToRawData for both sections
    h = bytearray(headers)
    struct.pack_into('<I', h, len(dos) + 4 + 20 + 0xE0 + 20, text_ptr)
    struct.pack_into('<I', h, len(dos) + 4 + 20 + 0xE0 + 40 + 20, reloc_ptr)

    return bytes(h) + text_raw + reloc_raw


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else 'guest_bench.exe'
    data = build()
    with open(out, 'wb') as f:
        f.write(data)
    print("wrote %s (%d bytes, entry RVA 0x%x)" % (out, len(data), TEXT_RVA))


if __name__ == '__main__':
    main()
