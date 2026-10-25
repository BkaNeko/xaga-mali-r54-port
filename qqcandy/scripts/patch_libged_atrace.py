#!/usr/bin/env python3
"""把 r54 libged.so 里新版 libcutils 的 atrace 符号改指到老版本（大小不变）。

r54 的 libged.so 引用 atrace_async_for_track_begin_body / _end_body，
底包 libcutils 只有 atrace_begin_body / atrace_end_body。
只改 .dynsym 里两个 UND 符号的 st_name，指向 .dynstr 里已存在的短名字，
文件大小完全不变（实测改 4 字节）。

用法: python3 patch_libged_atrace.py 原始libged.so 输出.so
"""
import struct, sys
from pathlib import Path

PAIRS = [("atrace_async_for_track_begin_body", "atrace_begin_body"),
         ("atrace_async_for_track_end_body",   "atrace_end_body")]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    data = bytearray(Path(sys.argv[1]).read_bytes())
    e_shoff, = struct.unpack_from("<Q", data, 0x28)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x3a)
    secs = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        name, typ, flags, addr, offset, size, link, info, align, entsize = \
            struct.unpack_from("<IIQQQQIIQQ", data, off)
        secs.append(dict(name=name, type=typ, offset=offset, size=size,
                         link=link, entsize=entsize))
    dynsym = next(s for s in secs if s["type"] == 11)      # SHT_DYNSYM
    dynstr = secs[dynsym["link"]]
    strtab = data[dynstr["offset"]:dynstr["offset"] + dynstr["size"]]

    def sname(idx):
        end = strtab.index(b"\0", idx)
        return strtab[idx:end].decode()

    def find_str(s):
        needle = s.encode() + b"\0"
        i = strtab.find(needle)
        if i < 0:
            raise SystemExit(f".dynstr 里没有 {s}")
        return i

    n = 0
    for i in range(dynsym["size"] // dynsym["entsize"]):
        off = dynsym["offset"] + i * dynsym["entsize"]
        st_name, = struct.unpack_from("<I", data, off)
        cur = sname(st_name)
        for old, new in PAIRS:
            if cur == old:
                struct.pack_into("<I", data, off, find_str(new))
                print(f"  {old} -> {new}  (st_name {st_name:#x} -> {find_str(new):#x})")
                n += 1
    if not n:
        print("  没找到需要改的符号（可能已经是老名字）")
    Path(sys.argv[2]).write_bytes(bytes(data))
    print(f"  写出 {sys.argv[2]}  ({len(data)} 字节，与原文件同大小)")


if __name__ == "__main__":
    main()
