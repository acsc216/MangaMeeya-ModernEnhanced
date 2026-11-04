#!/usr/bin/env python3
"""Apply the ZIP64 patches to arc.dll (MangaMeeya 7.4).

Usage:
    python apply_patch.py [input_arc.dll] [output_arc.dll]

Defaults: ../arc.dll -> ../arc.dll.patched

The input must be the original arc.dll (2006-07-19, 229376 bytes).
The script refuses to run on an already patched file.
"""
import os
import shutil
import struct
import sys

BASE = os.path.dirname(os.path.abspath(__file__))

# (cave_file_offset, cave_va, site_file_offset, site_va, blob_name, replaced_len, original_prefix)
PATCHES = [
    (0x29170, 0x10029170, 0x093C5, 0x100093C5, "zip64_extra.bin", 0x87,
     bytes.fromhex("66837C241400")),                    # cmp word [esp+0x14],0
    (0x292E0, 0x100292E0, 0x09BB5, 0x10009BB5, "zip64_eocd.bin", 7,
     bytes.fromhex("8B842496000000")),                  # mov eax,[esp+0x96]
    (0x29390, 0x10029390, 0x09DAE, 0x10009DAE, "zip64_eocd_comment.bin", 5,
     bytes.fromhex("0FB7442456")),                      # movzx eax,word [esp+0x56]
]

def main():
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(BASE, "..", "arc.dll")
    dst = sys.argv[2] if len(sys.argv) > 2 else os.path.join(BASE, "..", "arc.dll.patched")
    data = bytearray(open(src, "rb").read())
    if data[PATCHES[0][2]] == 0xE9:
        sys.exit("input already appears patched, refusing to run")

    for cave_off, cave_va, site_off, site_va, blob_name, old_len, orig in PATCHES:
        got = bytes(data[site_off:site_off + len(orig)])
        if got != orig:
            sys.exit("unexpected bytes at file offset 0x%X: %s (expected %s); wrong arc.dll version?"
                     % (site_off, got.hex(), orig.hex()))
        blob = open(os.path.join(BASE, blob_name), "rb").read()
        if len(blob) > 0x1000:
            sys.exit("%s too large" % blob_name)
        data[cave_off:cave_off + len(blob)] = blob
        rel = cave_va - (site_va + 5)
        data[site_off:site_off + 5] = b"\xE9" + struct.pack("<i", rel)
        for i in range(5, old_len):
            data[site_off + i] = 0x90
        print("patched site 0x%08X -> cave 0x%08X" % (site_va, cave_va))

    open(dst, "wb").write(data)
    print("wrote", dst)

if __name__ == "__main__":
    main()
