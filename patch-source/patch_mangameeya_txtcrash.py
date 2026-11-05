#!/usr/bin/env python3
"""Fix the text-page scanner overrun in MangaMeeya.exe.

sub_453040 renders text pages (txt entries).  Its line scanner reads the byte
at buffer[index] *before* testing whether index has reached the buffer length,
and the test uses equality.  When the last byte is a DBCS lead byte the index
is advanced by two and the equality test is skipped; the scanner then keeps
reading past the allocation until it hits an unmapped page.  Whether this
crashes depends on heap placement, the text bytes and prefetch settings
(e.g. PrepageForwardNum=2 on a zip whose pages are .txt files).

The patch inserts a bounds check in front of the read:

    if (index >= length) { index = length; goto end_of_text; }
    byte = buffer[index];
    ...

The new code lives in the zero padding at the end of .text (RVA 0xE4600) and
the original instruction at RVA 0x4531E0 is replaced by a 5-byte jmp.

Usage:
    python patch_mangameeya_txtcrash.py [input_exe] [output_exe]

Defaults: ../MangaMeeya.exe -> ../MangaMeeya.exe.txtfix
"""
import os
import struct
import sys

BASE = os.path.dirname(os.path.abspath(__file__))

PATCH_RVA = 0x531E0
CAVE_RVA = 0xE4600
IMAGE_BASE = 0x400000

# Byte sequences we expect to find in the original binary.
ORIG_BYTES = bytes.fromhex("8b4c2414")          # mov ecx, [esp+0x14]
CAVE = bytes.fromhex(
    "8b742410"          # mov esi, [esp+0x10]        ; this
    "8b86340d0000"      # mov eax, [esi+0xD34]       ; length
    "39c3"              # cmp ebx, eax
    "7207"              # jb  short below
    "89c3"              # mov ebx, eax               ; clamp index
    "e9e2eef6ff"        # jmp 0x4534F7
    "8b4c2414"          # below: mov ecx, [esp+0x14]
    "8a040b"            # mov al, [ebx+ecx]
    "3c0d"              # cmp al, 0Dh
    "0f84cfeef6ff"      # je 0x4534F3
    "3c0a"              # cmp al, 0Ah
    "0f84c7eef6ff"      # je 0x4534F3
    "e9c6ebf6ff"        # jmp 0x4531F7
)


def align(v, a):
    return (v + a - 1) & ~(a - 1)


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(BASE, "..", "MangaMeeya.exe")
    dst = sys.argv[2] if len(sys.argv) > 2 else os.path.join(BASE, "..", "MangaMeeya.exe.txtfix")
    data = bytearray(open(src, "rb").read())

    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        sys.exit("not a PE file")
    opt = e_lfanew + 24
    if struct.unpack_from("<H", data, opt)[0] != 0x10B:
        sys.exit("not PE32")
    base = struct.unpack_from("<I", data, opt + 28)[0]
    if base != IMAGE_BASE:
        sys.exit("unexpected image base %#x (this patch is for 0x400000)" % base)

    num_sec = struct.unpack_from("<H", data, e_lfanew + 6)[0]
    opt_size = struct.unpack_from("<H", data, e_lfanew + 20)[0]
    sec_table = opt + opt_size
    sections = []
    for i in range(num_sec):
        o = sec_table + i * 40
        vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", data, o + 8)
        sections.append((vaddr, vsize, raddr, rsize))

    def rva_to_off(rva):
        for vaddr, vsize, raddr, rsize in sections:
            if vaddr <= rva < vaddr + max(vsize, rsize):
                return raddr + (rva - vaddr)
        raise ValueError("RVA %#x not mapped" % rva)

    patch_off = rva_to_off(PATCH_RVA)
    cave_off = rva_to_off(CAVE_RVA)

    jmp_target = IMAGE_BASE + CAVE_RVA
    rel = jmp_target - (IMAGE_BASE + PATCH_RVA + 5)
    if not (-0x80000000 <= rel < 0x80000000):
        sys.exit("jump out of range")

    if data[patch_off] == 0xE9 and struct.unpack_from("<i", data, patch_off + 1)[0] == rel:
        sys.exit("already patched")
    if bytes(data[patch_off:patch_off + 4]) != ORIG_BYTES:
        sys.exit("unexpected bytes at RVA %#x: %s" %
                 (PATCH_RVA, bytes(data[patch_off:patch_off + 4]).hex()))

    need = len(CAVE) + 16
    cave = data[cave_off:cave_off + need]
    if any(cave):
        sys.exit("cave at RVA %#x is not empty" % CAVE_RVA)

    data[cave_off:cave_off + len(CAVE)] = CAVE
    data[patch_off:patch_off + 5] = b"\xE9" + struct.pack("<i", rel)

    open(dst, "wb").write(data)
    print("cave at RVA %#x (%d bytes), jmp at RVA %#x -> %#x" %
          (CAVE_RVA, len(CAVE), PATCH_RVA, jmp_target))
    print("wrote", dst)


if __name__ == "__main__":
    main()
