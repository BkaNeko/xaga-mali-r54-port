#!/usr/bin/env python3
"""把 vendor_dlkm 镜像包成 recovery 刷机包。

为什么不用 fastboot：
  OrangeFox 的 fastbootd 打不开 vendor_dlkm_a / super
  （getvar partition-size 报 Could not open partition，写入报 No such file or directory），
  而 adb/su/KernelSU 写受保护分区会被 baseband_guard 拒绝。
  只有 recovery 的 update-binary 跑在可信域里。

用法：
    python3 make_recovery_zip.py --image vendor_dlkm_new.img \
                                 --seek 2135552 \
                                 --out r54_dlkm.zip

--seek 是 super 分区里 vendor_dlkm_a 的起始位置，单位 = 4096 字节块。
取值方法（recovery 里）：
    lpdump /dev/block/by-name/super | grep -A4 vendor_dlkm_a
      Extents:
        0 .. 114063 linear super 17084416      ← 起始扇区
    seek = 17084416 * 512 / 4096 = 2135552
本机（qqcandy）默认就是 2135552。
"""
import argparse, os, zipfile
from pathlib import Path

UPDATE_BINARY = """#!/sbin/sh
OUTFD=$2
ZIP="$3"
ui_print() { echo "ui_print $1" >&$OUTFD; echo "ui_print" >&$OUTFD; }

ui_print "=========================================="
ui_print " Mali r54 : 写入 vendor_dlkm"
ui_print "=========================================="

WORK=/tmp/qcd; rm -rf $WORK; mkdir -p $WORK || exit 1
cd $WORK
unzip -o "$ZIP" vendor_dlkm.img >/dev/null 2>&1
[ -f vendor_dlkm.img ] || { ui_print "!! 解压失败"; exit 1; }
ui_print "镜像 $(wc -c < vendor_dlkm.img) 字节"

DEV=/dev/block/by-name/super
[ -b "$DEV" ] || DEV=/dev/block/sdc79
[ -b "$DEV" ] || { ui_print "!! 找不到 super 分区"; exit 1; }

ui_print "写入 super (bs=4096 seek=__SEEK__) ..."
dd if=vendor_dlkm.img of=$DEV bs=4096 seek=__SEEK__ conv=notrunc 2>&1 | tail -1
sync
ui_print "完成，请重启"
exit 0
"""

UPDATER_SCRIPT = """#FLASHAFTERUPDATEV2
#
# Dummy file; update-binary is a shell script (DO NOT CHANGE)
#
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--seek", type=int, default=2135552,
                    help="super 内起始块号（4096 字节为单位），本机默认 2135552")
    ap.add_argument("--out", default="r54_dlkm.zip")
    a = ap.parse_args()

    img = Path(a.image).resolve()
    out = Path(a.out).resolve()
    if not img.is_file():
        raise SystemExit(f"找不到 {img}")

    with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED) as z:
        z.writestr("META-INF/com/google/android/updater-script", UPDATER_SCRIPT)
        info = zipfile.ZipInfo("META-INF/com/google/android/update-binary")
        info.external_attr = (0o755 << 16) | 0o100000   # 可执行
        z.writestr(info, UPDATE_BINARY.replace("__SEEK__", str(a.seek)))
        z.write(img, "vendor_dlkm.img")

    print(f"生成 {out}  ({out.stat().st_size} 字节, seek={a.seek})")
    print("刷写（recovery 里）：")
    print(f"  adb push {out.name} /sdcard/")
    print("  adb shell")
    print("  mount /data                       # OrangeFox 默认不挂载")
    print(f"  twrp install /sdcard/{out.name}")


if __name__ == "__main__":
    main()
