#!/usr/bin/env python3
"""Add a static import of mmunicode.dll!MMUInit to MangaMeeya.exe.

Rather than patching the entry point, a new section (`.mmuimp`) is appended
and the Import Directory is repointed at a copy of the original import
descriptor array plus one descriptor for mmunicode.dll.  The Windows loader
then loads the shim before the program starts, and its DllMain patches the
ANSI file APIs.

No existing byte of code, the entry point, the PE headers (other than
NumberOfSections / SizeOfImage / the Import Directory entry) or the existing
sections are modified.

Usage:
    python patch_mangameeya_unicode.py [input_exe] [output_exe]

Defaults: ../MangaMeeya.exe -> ../MangaMeeya.exe.unicode
"""
import os
import struct
import sys

BASE = os.path.dirname(os.path.abspath(__file__))
DLL_NAME = "mmunicode.dll"
EXPORT = "MMUInit"
SECTION_NAME = b".mmuimp\0"


def align(v, a):
    return (v + a - 1) & ~(a - 1)


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(BASE, "..", "MangaMeeya.exe")
    dst = sys.argv[2] if len(sys.argv) > 2 else os.path.join(BASE, "..", "MangaMeeya.exe.unicode")

    data = bytearray(open(src, "rb").read())
    if len(data) < 0x40:
        sys.exit("not a PE file")

    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        sys.exit("not a PE file")
    num_sec = struct.unpack_from("<H", data, e_lfanew + 6)[0]
    opt_size = struct.unpack_from("<H", data, e_lfanew + 20)[0]
    opt = e_lfanew + 24
    if struct.unpack_from("<H", data, opt)[0] != 0x10B:
        sys.exit("not a PE32 (x86) file")

    section_align = struct.unpack_from("<I", data, opt + 32)[0]
    file_align = struct.unpack_from("<I", data, opt + 36)[0]
    size_of_image = struct.unpack_from("<I", data, opt + 56)[0]
    sec_table = opt + opt_size

    sections = []
    for i in range(num_sec):
        o = sec_table + i * 40
        name = data[o:o + 8].rstrip(b"\0")
        vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", data, o + 8)
        sections.append(dict(name=name, vsize=vsize, vaddr=vaddr,
                             rsize=rsize, raddr=raddr))
        if name == SECTION_NAME.rstrip(b"\0"):
            sys.exit("already patched (found %s section)" % SECTION_NAME.rstrip(b"\0").decode())

    def rva_to_off(rva):
        for s in sections:
            if s["vaddr"] <= rva < s["vaddr"] + max(s["vsize"], s["rsize"]):
                return s["raddr"] + (rva - s["vaddr"])
        raise ValueError("RVA %#x not mapped" % rva)

    imp_dir_off = opt + 96 + 1 * 8
    imp_rva, _ = struct.unpack_from("<II", data, imp_dir_off)
    imp_off = rva_to_off(imp_rva)

    n = 0
    while struct.unpack_from("<IIIII", data, imp_off + n * 20) != (0, 0, 0, 0, 0):
        n += 1

    new_va = align(size_of_image, section_align)
    new_raw = align(len(data), file_align)
    new_size = align(0x200, file_align)
    if new_size < file_align:
        new_size = file_align

    # Layout in the new section.
    desc_rva = new_va
    desc_size = (n + 2) * 20
    thunk_rva = desc_rva + desc_size
    iat_rva = thunk_rva + 8
    hint_rva = thunk_rva + 16
    name_rva = hint_rva + 2 + len(EXPORT) + 1
    dll_rva = name_rva
    if dll_rva + len(DLL_NAME) + 1 > new_va + new_size:
        sys.exit("internal error: new section too small")

    sec = bytearray(new_size)
    sec[0:n * 20] = data[imp_off:imp_off + n * 20]
    struct.pack_into("<IIIII", sec, n * 20, thunk_rva, 0, 0, dll_rva, iat_rva)

    struct.pack_into("<II", sec, thunk_rva - new_va, hint_rva, 0)
    struct.pack_into("<II", sec, iat_rva - new_va, hint_rva, 0)
    struct.pack_into("<H", sec, hint_rva - new_va, 0)
    sec[hint_rva - new_va + 2:hint_rva - new_va + 2 + len(EXPORT)] = EXPORT.encode("ascii")
    sec[dll_rva - new_va:dll_rva - new_va + len(DLL_NAME)] = DLL_NAME.encode("ascii")

    if new_raw > len(data):
        data.extend(b"\0" * (new_raw - len(data)))
    data.extend(sec)

    hdr = bytearray(40)
    hdr[0:8] = SECTION_NAME
    struct.pack_into("<IIII", hdr, 8, new_size, new_va, new_size, new_raw)
    struct.pack_into("<HHI", hdr, 32, 0, 0, 0xC0000040)  # RW initialized data
    data[sec_table + num_sec * 40:sec_table + (num_sec + 1) * 40] = hdr

    struct.pack_into("<H", data, e_lfanew + 6, num_sec + 1)
    struct.pack_into("<I", data, opt + 56, new_va + new_size)  # SizeOfImage
    struct.pack_into("<II", data, imp_dir_off, desc_rva, desc_size)

    open(dst, "wb").write(data)
    print("import directory -> %#x (%d descriptors + terminator)" % (desc_rva, n + 1))
    print("new section %s at RVA %#x, %d import(s) total" %
          (SECTION_NAME.rstrip(b"\0").decode(), new_va, n + 1))
    print("wrote", dst)


if __name__ == "__main__":
    main()
