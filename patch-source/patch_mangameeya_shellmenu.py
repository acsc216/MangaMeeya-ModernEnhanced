#!/usr/bin/env python3
"""Make MangaMeeya's Explorer context-menu registration work per-user.

The built-in "文件关联设置" writes the shell verb to HKEY_CLASSES_ROOT.
A non-elevated process on Windows 7+ cannot write there (and the app's
manifest disables UAC virtualization), so registration fails silently.

This patch replaces the three registration helpers with implementations
that write HKCU\\Software\\Classes instead, which needs no administrator
rights and is honoured by Explorer:

  Folder                     -> HKCU\\Software\\Classes\\Folder\\shell\\<verb>
  .zip/.rar/.../.jpg/...     -> HKCU\\Software\\Classes\\SystemFileAssociations\\<ext>\\shell\\<verb>

The fourth list entry (displayed as "....") is left untouched.

Usage:
    python patch_mangameeya_shellmenu.py [input_exe] [output_exe]

Defaults: ../MangaMeeya.exe -> ../MangaMeeya.exe.shellmenu
"""
import os
import struct
import sys

BASE = os.path.dirname(os.path.abspath(__file__))
BLOB = os.path.join(BASE, "shellmenu", "shellmenu.bin")

IMAGE_BASE = 0x400000
CAVE_RVA = 0xE4700
# Offsets of check_entry / set_entry / del_entry inside shellmenu.bin.
CHECK_OFF = 0x006D
SET_OFF = 0xF8
DEL_OFF = 0x18C

CHECK_RVA = 0x58D50
SET_RVA = 0x58E60
DEL_RVA = 0x58FD0

ORIG = {
    CHECK_RVA: bytes.fromhex("8b44240483"),
    SET_RVA: bytes.fromhex("83ec145657"),
    DEL_RVA: bytes.fromhex("518b442408"),
}


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(BASE, "..", "MangaMeeya.exe")
    dst = sys.argv[2] if len(sys.argv) > 2 else os.path.join(BASE, "..", "MangaMeeya.exe.shellmenu")

    data = bytearray(open(src, "rb").read())
    blob = open(BLOB, "rb").read()

    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        sys.exit("not a PE file")
    opt = e_lfanew + 24
    if struct.unpack_from("<H", data, opt)[0] != 0x10B:
        sys.exit("not PE32")
    if struct.unpack_from("<I", data, opt + 28)[0] != IMAGE_BASE:
        sys.exit("unexpected image base (this patch is for 0x400000)")

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

    entries = [
        (CHECK_RVA, CAVE_RVA + CHECK_OFF),
        (SET_RVA, CAVE_RVA + SET_OFF),
        (DEL_RVA, CAVE_RVA + DEL_OFF),
    ]
    for rva, target in entries:
        off = rva_to_off(rva)
        rel = (IMAGE_BASE + target) - (IMAGE_BASE + rva + 5)
        if data[off] == 0xE9 and struct.unpack_from("<i", data, off + 1)[0] == rel:
            sys.exit("already patched")
        if bytes(data[off:off + 5]) != ORIG[rva]:
            sys.exit("unexpected bytes at RVA %#x: %s" % (rva, bytes(data[off:off + 5]).hex()))

    cave_off = rva_to_off(CAVE_RVA)
    if any(data[cave_off:cave_off + len(blob)]):
        sys.exit("cave at RVA %#x is not empty" % CAVE_RVA)

    data[cave_off:cave_off + len(blob)] = blob
    for rva, target in entries:
        off = rva_to_off(rva)
        rel = (IMAGE_BASE + target) - (IMAGE_BASE + rva + 5)
        data[off:off + 5] = b"\xE9" + struct.pack("<i", rel)

    open(dst, "wb").write(data)
    print("cave at RVA %#x (%d bytes)" % (CAVE_RVA, len(blob)))
    for rva, target in entries:
        print("  jmp at RVA %#x -> %#x" % (rva, IMAGE_BASE + target))
    print("wrote", dst)


if __name__ == "__main__":
    main()
