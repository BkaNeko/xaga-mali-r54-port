#!/usr/bin/env python3
"""从底包 vendor_dlkm.img 生成 qqcandy 用的 r54 版镜像。

用法（需要 root，会 loop 挂载）：
    sudo python3 build_dlkm_image.py --stock-vendor-dlkm /path/vendor_dlkm.img \
                                     --out ./vendor_dlkm_new.img

做的事：
  1. 挂载底包镜像，整树复制（保留权限/xattr）
  2. 删 met*.ko（编译残留，无任何引用，腾约 1.3MB）
  3. 用 blobs/vendor_dlkm/ 的 r54 模块替换 kbase / mgm / prot_alloc
     —— ged.ko / gpueb.ko 不动，底包那份已经导出了 r54 kbase 需要的全部 ged_dvfs_* 符号
  4. 所有文件打 u:object_r:vendor_file:s0
  5. mke2fs 整树重建（分区上限大小，inode 比例放大以腾空间）
"""
import argparse, os, shutil, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
QROOT = HERE.parent
BLOCKSIZE = 4096
LABEL = b"u:object_r:vendor_file:s0"
REPLACE = ["mali_kbase_mt6895.ko", "mali_mgm_mt6895.ko", "mali_prot_alloc_mt6895.ko"]


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stock-vendor-dlkm", required=True)
    ap.add_argument("--out", default="vendor_dlkm_new.img")
    ap.add_argument("--metadir", default=str(QROOT / "blobs" / "vendor_dlkm"))
    a = ap.parse_args()

    if os.geteuid() != 0:
        sys.exit("需要 root（要 loop 挂载）")

    stock = Path(a.stock_vendor_dlkm).resolve()
    out = Path(a.out).resolve()
    size = stock.stat().st_size
    blocks = size // BLOCKSIZE
    print(f"底包: {stock}  {size} B = {blocks} 块")

    work = Path(tempfile.mkdtemp(prefix="dlkm."))
    tree = work / "tree"
    mnt = work / "mnt"
    tree.mkdir(); mnt.mkdir()

    # 1) 整树复制
    r = run(["mount", "-o", "loop,ro", str(stock), str(mnt)])
    if r.returncode: sys.exit(f"挂载失败: {r.stderr}")
    try:
        r = run(["cp", "-a", "--preserve=all", f"{mnt}/.", f"{tree}/"])
        if r.returncode: sys.exit(f"复制失败: {r.stderr}")
    finally:
        run(["umount", str(mnt)])
    print(f"  复制出 {sum(1 for _ in tree.rglob('*') if _.is_file())} 个文件")

    moddir = tree / "lib/modules"

    # 2) 删 met*.ko
    freed = 0
    for f in sorted(moddir.glob("met*.ko")):
        freed += f.stat().st_size
        f.unlink()
    print(f"  删除 met*.ko: {freed} B")
    if not freed:
        print("  [警告] 没找到 met*.ko，底包结构可能不同")

    # 3) 替换 r54 模块
    for name in REPLACE:
        src = Path(a.metadir) / name
        if not src.is_file():
            sys.exit(f"缺 {src}")
        dst = moddir / dst_name_fix(name)
        shutil.copyfile(src, dst)
        os.chmod(dst, 0o644)
        print(f"  {dst.name}: {dst.stat().st_size} B")
    for keep in ("ged.ko", "gpueb.ko"):
        p = moddir / keep
        print(f"  保持原厂 {keep}: {p.stat().st_size if p.exists() else '缺失'}")

    # 4) 标签
    n = 0
    for root, _, files in os.walk(tree):
        for x in files:
            p = Path(root) / x
            if p.is_symlink():
                continue
            try:
                os.setxattr(p, "security.selinux", LABEL); n += 1
            except OSError as e:
                print(f"  [警告] 设标签失败 {p}: {e}")
    print(f"  标签: {n} 个文件")

    # 5) 重建
    if out.exists(): out.unlink()
    cmd = ["mke2fs", "-t", "ext4", "-b", str(BLOCKSIZE), "-I", "256", "-i", "131072",
           "-O", "^has_journal,^metadata_csum,^64bit,^resize_inode",
           "-E", "root_owner=0:0", "-d", str(tree), str(out), str(blocks)]
    r = run(cmd)
    if r.returncode:
        sys.exit(f"mke2fs 失败:\n{r.stdout}\n{r.stderr}")
    info = run(["tune2fs", "-l", str(out)]).stdout
    keep = [l.strip() for l in info.splitlines()
            if l.split(":")[0].strip() in ("Block count", "Free blocks", "Inode count", "Free inodes")]
    print(f"  生成 {out}  {out.stat().st_size} B")
    for l in keep:
        print(f"    {l}")
    shutil.rmtree(work, ignore_errors=True)
    print("\n下一步：make_recovery_zip.py --image", out.name)


def dst_name_fix(name):
    """blobs 里的文件名已经是 _mt6895 后缀，直接返回。"""
    return name


if __name__ == "__main__":
    main()
