#!/usr/bin/env python3
"""Enable IMAGE_FILE_LARGE_ADDRESS_AWARE in MangaMeeya.exe.

The flag lives in the PE *FileHeader.Characteristics* (bit 0x20),
NOT in DllCharacteristics.

Usage:
    python patch_mangameeya_laa.py [input_exe] [output_exe]

Defaults: ../MangaMeeya.exe -> ../MangaMeeya.exe.laa
"""
import os
import struct
import sys

BASE = os.path.dirname(os.path.abspath(__file__))

def main():
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(BASE, "..", "MangaMeeya.exe")
    dst = sys.argv[2] if len(sys.argv) > 2 else os.path.join(BASE, "..", "MangaMeeya.exe.laa")
    data = bytearray(open(src, "rb").read())
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        sys.exit("not a PE file")
    char_off = e_lfanew + 22  # FileHeader.Characteristics
    old = struct.unpack_from("<H", data, char_off)[0]
    if old & 0x20:
        sys.exit("already LARGE_ADDRESS_AWARE")
    struct.pack_into("<H", data, char_off, old | 0x20)
    open(dst, "wb").write(data)
    print("FileHeader.Characteristics: 0x%04X -> 0x%04X" % (old, old | 0x20))
    print("wrote", dst)

if __name__ == "__main__":
    main()
