# qqcandy 移植步骤

只写「怎么做」。为什么这么做见 `PITFALLS.zh-CN.md`。

前提：底包（vendor + vendor_dlkm + gpueb 镜像）、r54 包（13T 的 13 个用户态库 + 5 个内核模块
+ `mali_csffw.bin`）、目标机已 root（KernelSU + 元模块）。

---

## 0. 先确认底包和目标机是同一台

```bash
adb shell 'getprop ro.vendor.build.fingerprint'
```

再比对 GPU 栈文件尺寸（骗不了人）：

```bash
adb shell 'stat -c "%s %n" /vendor/lib64/egl/mt6895/libGLES_mali.so \
  /vendor_dlkm/lib/modules/mali_kbase_mt6895.ko'
```

和底包镜像里的同名文件尺寸一致才继续。

---

## 1. 用户态：装 KSU 模块

### 1.1 生成模块

```bash
bash scripts/build_ksu_module.sh          # 输出 r54_mali.zip
```

脚本做的事：把仓库根 `blobs/vendor/` 的 10 个 r54 库 +
本目录 `blobs/vendor_extra/` 的 5 个补充文件，按下面的布局放进模块：

```
r54_mali/
├── module.prop
├── system.prop                     ← files/system.prop
└── system/vendor/
    ├── lib64/egl/mt6895/libGLES_mali.so
    ├── lib64/egl/libGLES_meow.so
    ├── lib64/egl/libMEOW_{gift,trace,qt,data,mfrc_ext}.so
    ├── lib64/libged.so             ← blobs/vendor_extra/libged.so（已改 atrace 符号）
    ├── lib64/libgpud.so
    ├── lib64/libmtk_mali_user.so
    ├── lib64/libc++.so             ← 新增
    ├── lib64/android.hardware.graphics.common-V6-ndk.so
    ├── lib64/android.hardware.graphics.allocator-V2-ndk.so
    ├── lib64/android.hardware.common-V2-ndk.so
    ├── lib64/mt6895/{arm.mali.platform-V2-ndk,libarm_mali_config_sysprops,
    │                 libarm_gralloc_properties_sysprop,libarm_egl_properties_sysprop,
    │                 arm.graphics-V5-ndk}.so
    ├── firmware/mali_csffw.bin
    ├── firmware/mali_csffw_reload.bin
    ├── etc/mali_platform.config
    └── etc/permissions/{android.hardware.vulkan.version,
                         android.software.vulkan.deqp.level}.xml
```

### 1.2 设 SELinux 标签

`same_process_hal_file` —— `egl/` 下全部、`libged.so`、`libgpud.so`、
`libmtk_mali_user.so`、`libc++.so`、三个 `android.hardware.*-ndk.so`、
`mt6895/` 下全部、`firmware/mali_csffw.bin`。

`vendor_file` —— `firmware/mali_csffw_reload.bin`。

`vendor_configs_file` —— `etc/` 下全部。

```bash
find .../system/vendor/lib64 -type f -exec chcon u:object_r:same_process_hal_file:s0 {} \;
chcon u:object_r:vendor_configs_file:s0 .../etc/mali_platform.config .../etc/permissions/*.xml
```

不设也能挂上，但 app 进程加载会被拒（现象是 `library "xxx" not found`）。

### 1.3 装进 KernelSU

```bash
# KernelSU Manager → 模块 → 从本地安装 → 选 r54_mali.zip
# 或手工：
unzip r54_mali.zip -d /data/adb/modules/r54_mali
```

需要元模块 `magic_mount_rs`（提供 Magic Mount 挂载）。它只用默认分区列表，
`/data/adb/magic_mount/config.toml` 保持 `partitions = []` 即可 —— 内核模块不走 overlay。

重启后先验证用户态：

```bash
adb shell 'dumpsys SurfaceFlinger | grep -m1 "^GLES:"'
# 期望 v1.r54p1-11eac0；此时 kernel 侧还是 1.18，日志会有
# "file /dev/mali0 is different version (user 1.38, kernel 1.18)"，属正常
```

---

## 2. 内核态：构建并刷 vendor_dlkm

### 2.1 生成镜像

```bash
python3 scripts/build_dlkm_image.py \
  --stock-vendor-dlkm /path/to/底包/vendor_dlkm.img \
  --out ./vendor_dlkm_new.img
```

脚本做的事：

1. 挂载底包 `vendor_dlkm.img`，整树复制出来
2. 删掉 `met*.ko`（`modules.list` 里没有、任何 init rc 也没 insmod、全 vendor 无引用，
   属于编译残留，腾出 ~1.3 MB）
3. 用 `blobs/vendor_dlkm/` 的 3 个 r54 模块替换同名文件，**`ged.ko` / `gpueb.ko` 不动**
4. 全部文件打 `u:object_r:vendor_file:s0`
5. `mke2fs -t ext4 -b 4096 -I 256 -i 131072 -O ^has_journal,^metadata_csum,^64bit,^resize_inode
   -d <tree> <out> 14258`

> 第 5 步的两个参数是被空间逼出来的：底包 fs 只有 14013 块且 100% 满（free 42 块、inode 剩 15），
> 而 r54 的 5 个模块比原厂大 2.35 MB。就地改写会因碎片分配不出 2.5 MB 的 kbase 而 ENOSPC；
> 整树重建时默认 inode 比例（1/16KB → 3564 个 inode，约 900KB）又吃掉全部余量，
> 所以要 `-i 131072` 把 inode 数压到 ~450。
> 分区上限 = 底包镜像尺寸 58400768 B = 14258 块。

### 2.2 打包成 recovery 刷机包

```bash
python3 scripts/make_recovery_zip.py --image ./vendor_dlkm_new.img --out ./r54_dlkm.zip
```

### 2.3 刷

`fastboot` 不行（见 PITFALLS 第 1 条），必须走 recovery：

```bash
adb push r54_dlkm.zip /sdcard/
adb shell
mount /data                                    # OrangeFox 默认不挂
twrp install /sdcard/r54_dlkm.zip
```

`update-binary` 就一条 dd：

```
dd if=vendor_dlkm.img of=/dev/block/by-name/super bs=4096 seek=<sector> conv=notrunc
```

`<sector>` 从当前 super 元数据里取：

```bash
# recovery 里（需要 lpdump）
lpdump /dev/block/by-name/super | grep -A4 vendor_dlkm_a
#   Name: vendor_dlkm_a
#   Group: main_a
#   Extents:
#     0 .. 114063 linear super 17084416     ← 这个数 × 512 / 4096 = seek 值
```

**只写数据、不动元数据、不动 vbmeta**（AVB 已经是关的：`verifiedbootstate=orange`）。

### 2.4 验证

```bash
# recovery 里，把区域抽出来挂载检查
dd if=/dev/block/by-name/super of=/tmp/vd.img bs=4096 skip=<seek> count=14258
mount -t ext4 -o loop,ro /tmp/vd.img /mnt
ls -l /mnt/lib/modules/mali_kbase_mt6895.ko     # 2,571,656
ls -l /mnt/lib/modules/ged.ko                   # 487,920（原厂）
```

重启后：

```bash
adb shell 'dmesg | grep -i "mali.*version"'
# 不再有 "user 1.38, kernel 1.18"
adb shell 'dumpsys SurfaceFlinger | grep -m1 "^GLES:"'
```

---

## 3. 回滚

| 部件 | 回滚方式 |
|---|---|
| 用户态 | KernelSU Manager 里禁用/删除 `r54_mali` 模块，重启 |
| vendor_dlkm | 重刷底包的 `vendor_dlkm.img`（同样走 `twrp install`，或整包重刷 ROM） |
| gpueb | `fastboot flash gpueb_a blobs/gpueb_stock_restore.img` |

---

## 4. 不要做的事

- 不要用 `adb push` 写块设备（会把符号链接换成普通文件写进 tmpfs）
- 不要在 fastbootd 里 `fastboot flash vendor_dlkm`（该 fastbootd 打不开这个分区）
- 不要改 gpueb 固件（RSA 签名，见 PITFALLS 第 6 条）
- 不要把属性写进 overlay 的 `vendor/build.prop`（读得太早，用 `system.prop`）
