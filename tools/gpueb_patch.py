#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
Unlock the 950 MHz GPU OPP on MT6895 (xaga) by patching the GPUEB firmware.

The GPUEB coprocessor firmware owns GPU frequency selection on this SoC. Its
GPU OPP table has six entries, with 950 MHz at index 0:

    [0] 950000  78750  78750  posdiv=2   <- never selected by the firmware
    [1] 880000  78750  75000  posdiv=2   <- highest reachable OPP
    [2] 800000  72500  75000  posdiv=2
    [3] 610000  65000  75000  posdiv=2
    [4] 430000  60000  75000  posdiv=2
    [5] 350000  57500  75000  posdiv=3

The selection loop in the firmware uses the bound `i > 0`, which excludes index
0. The AP-side copy of the same logic still carries the comment
"Fstack > 800Mhz: Fgpu = 880MHz (OPP[0])" -- it was written when the table only
had five entries and 880 MHz was index 0. When the 950 MHz entry was inserted
at index 0, the loop bound was never updated, so 950 MHz became unreachable.

The AP cannot work around this:
  - gpufreq_commit(TARGET_GPU, ...) returns 0 but the firmware ignores it
    (verified with tools/gtprobe.c; all indices 0..5 leave the clock unchanged)
  - every userspace write interface uses TARGET_DEFAULT, which
    gpufreq_validate_target() remaps to TARGET_STACK when dual-buck is on
  - the AP-side policy table in EB mode (platform_eb_fp) is an empty struct

So the table itself has to change. Rewriting entry [1] to 950 MHz makes the
highest OPP the firmware can reach the one we want.

Patch: 5 bytes, file size unchanged.

The OTA payload only carries 128 KiB for the gpueb partition; the partition
itself is 1 MiB and the stock image pads the tail with 0x00 (not 0xFF). Use
--pad-to to reproduce that, so the result differs from stock in exactly the
five patched bytes.

Usage:
    python3 gpueb_patch.py firmware/gpueb_stock.bin firmware/gpueb_950mhz.bin
    python3 gpueb_patch.py gpueb.img gpueb_950.img --pad-to 1048576
"""

import struct
import sys

# Start of the GPU OPP table inside the GPUEB firmware image.
GPU_OPP_TABLE = 0x15D60
OPP_ENTRY_SIZE = 24  # sizeof(struct gpufreq_opp_info)

# Field offsets inside one entry.
OFF_FREQ = 0
OFF_VOLT = 4
OFF_VSRAM = 8
OFF_POSDIV = 12

# The entry the firmware actually selects as its maximum.
TARGET_ENTRY = 1

NEW_FREQ = 950000  # kHz
NEW_VSRAM = 78750  # 0.1 mV units, matches the stock 950 MHz entry


def read_entry(buf, idx):
    base = GPU_OPP_TABLE + idx * OPP_ENTRY_SIZE
    freq, volt, vsram, posdiv = struct.unpack("<4I", buf[base:base + 16])
    return base, freq, volt, vsram, posdiv


def dump_table(buf, count=6):
    print("GPU OPP table:")
    for i in range(count):
        base, freq, volt, vsram, posdiv = read_entry(buf, i)
        mark = "  <-- patched" if (i == TARGET_ENTRY and freq == NEW_FREQ) else ""
        print(f"  [{i}] @0x{base:05x} freq={freq:>7} volt={volt:>6} "
              f"vsram={vsram:>6} posdiv={posdiv}{mark}")


def main():
    argv = sys.argv[1:]
    pad_to = None
    positional = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--pad-to":
            pad_to = int(argv[i + 1])
            i += 2
            continue
        if a.startswith("--pad-to="):
            pad_to = int(a.split("=", 1)[1])
            i += 1
            continue
        positional.append(a)
        i += 1

    if len(positional) != 2:
        print(__doc__)
        return 1

    src, dst = positional
    with open(src, "rb") as f:
        data = bytearray(f.read())

    base, freq, volt, vsram, posdiv = read_entry(data, TARGET_ENTRY)
    print(f"source: {src}")
    if freq == NEW_FREQ:
        print("entry[1] is already 950 MHz, nothing to do")
        return 0
    if posdiv != 2:
        print(f"unexpected posdiv {posdiv} at entry[1], refusing to patch")
        return 1

    dump_table(data)
    print()

    data[base + OFF_FREQ:base + OFF_FREQ + 4] = struct.pack("<I", NEW_FREQ)
    data[base + OFF_VSRAM:base + OFF_VSRAM + 4] = struct.pack("<I", NEW_VSRAM)

    out = bytes(data)
    if pad_to and len(out) < pad_to:
        # Pad with 0x00: that is what the stock partition looks like past the
        # payload. Padding with 0xFF produces a different image.
        out = out + b"\x00" * (pad_to - len(out))
        print(f"padded {len(data)} -> {len(out)} bytes with 0x00")

    with open(dst, "wb") as f:
        f.write(out)

    print(f"patched: {dst}")
    dump_table(data)
    print()
    print(f"entry[1]: {freq} -> {NEW_FREQ} kHz, vsram {vsram} -> {NEW_VSRAM}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
