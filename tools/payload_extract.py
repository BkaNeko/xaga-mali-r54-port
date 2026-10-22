#!/usr/bin/env python3
"""Extract a partition from an Android OTA payload.bin.

Only handles the operation types that matter for pulling boot/vendor_boot/dtbo
out of a stock ROM: REPLACE (0), REPLACE_BZ (1), REPLACE_XZ (8) and ZERO (6).

Usage:
    ./payload_extract.py /path/to/payload.bin dtbo
    ./payload_extract.py /path/to/payload.bin --list
"""
import bz2
import lzma
import struct
import sys


def read_varint(data, i):
    result = shift = 0
    while True:
        byte = data[i]
        i += 1
        result |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return result, i
        shift += 7


def iter_fields(data):
    i = 0
    while i < len(data):
        key, i = read_varint(data, i)
        field, wire = key >> 3, key & 7
        if wire == 0:
            value, i = read_varint(data, i)
            yield field, wire, value
        elif wire == 2:
            length, i = read_varint(data, i)
            yield field, wire, data[i:i + length]
            i += length
        elif wire == 1:
            yield field, wire, data[i:i + 8]
            i += 8
        elif wire == 5:
            yield field, wire, data[i:i + 4]
            i += 4
        else:
            return


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1

    path, want = sys.argv[1], sys.argv[2]
    fh = open(path, 'rb')
    header = fh.read(24)
    if header[:4] != b'CrAU':
        print('not a payload.bin (bad magic)')
        return 1

    manifest_size, = struct.unpack('>Q', header[12:20])
    sig_size, = struct.unpack('>I', header[20:24])
    manifest = fh.read(manifest_size)
    data_start = 24 + manifest_size + sig_size

    for field, wire, value in iter_fields(manifest):
        if field != 13 or wire != 2:      # repeated PartitionUpdate
            continue

        name = None
        ops = []
        for pf, pw, pv in iter_fields(value):
            if pf == 1 and pw == 2:
                name = pv.decode('utf-8', 'replace')
            elif pf == 8 and pw == 2:     # repeated InstallOperation
                op_type = offset = length = None
                for of, ow, ov in iter_fields(pv):
                    if of == 1 and ow == 0:
                        op_type = ov
                    elif of == 2 and ow == 0:
                        offset = ov
                    elif of == 3 and ow == 0:
                        length = ov
                ops.append((op_type, offset, length))

        if not name:
            continue
        if want == '--list':
            print(f'{name} ({len(ops)} ops)')
            continue
        if want not in name:
            continue

        print(f'partition {name}: {len(ops)} ops')
        out = open(f'{name}.img', 'wb')
        total = 0
        for op_type, offset, length in ops:
            if op_type == 6:              # ZERO
                continue
            if op_type not in (0, 1, 8):
                print(f'  unsupported op type {op_type}, skipped')
                continue
            fh.seek(data_start + offset)
            blob = fh.read(length)
            if op_type == 1:
                blob = bz2.decompress(blob)
            elif op_type == 8:
                blob = lzma.decompress(blob)
            out.write(blob)
            total += len(blob)
        out.close()
        print(f'  wrote {total} bytes -> {name}.img')
        return 0

    if want != '--list':
        print(f'partition matching {want!r} not found')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
