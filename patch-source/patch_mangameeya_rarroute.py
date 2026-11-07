#!/usr/bin/env python3
"""Route .rar/.cbr through the Susie archive plug-in instead of arc.dll.

MangaMeeya's loader table contains a NUL-padded list of archive extensions:

    .cbr\\0\\0\\0\\0.rar\\0\\0\\0\\0.cbl\\0\\0\\0\\0.lzh\\0\\0\\0\\0...

When an extension matches this table, MangaMeeya opens the file with the
built-in arc.dll and never offers it to Susie archive plug-ins.  The bundled
arc.dll only understands the old RAR 1.5-3.x format (RAR4); RAR5 archives
fail.  This patch changes the two RAR entries to sentinels that match no real
file, so .rar/.cbr files fall through to SusiePlugin (rar.spi), while
.zip/.cbz/.lzh/.cbl are untouched.

The patch changes 6 bytes in .rdata and is independent of the other
MangaMeeya.exe patches.

Usage:
    python patch_mangameeya_rarroute.py [input_exe] [output_exe]

Defaults: ../MangaMeeya.exe -> ../MangaMeeya.exe.rarroute
"""
import os
import struct
import sys

BASE = os.path.dirname(os.path.abspath(__file__))

# The loader's extension table in MangaMeeya 7.4 (Chinese).  The surrounding
# NUL padding makes this sequence unique; the file-dialog strings use
# "*.rar;*.cbr" and do not match.
ORIG = b".cbr\0\0\0\0.rar\0\0\0\0"
PATCHED = b".cbX\0\0\0\0.raX\0\0\0\0"


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(BASE, "..", "MangaMeeya.exe")
    dst = sys.argv[2] if len(sys.argv) > 2 else os.path.join(BASE, "..", "MangaMeeya.exe.rarroute")
    data = bytearray(open(src, "rb").read())

    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        sys.exit("not a PE file")

    if data.count(PATCHED) == 1 and data.count(ORIG) == 0:
        sys.exit("already patched")
    if data.count(ORIG) != 1:
        sys.exit("expected exactly one loader-table entry, found %d" % data.count(ORIG))

    off = data.find(ORIG)
    data[off:off + len(ORIG)] = PATCHED
    open(dst, "wb").write(data)
    print("loader table at file offset %#x: %s -> %s" % (off, ORIG, PATCHED))
    print("wrote", dst)


if __name__ == "__main__":
    main()
