# qqcandy 移植的六个坑

按遇到顺序。每条都写清楚**现象 / 判断方法 / 结论**，避免下次再花一遍时间。

---

## 1. 任何非 recovery 进程都写不了 super

**现象**

```
$ adb shell su -c 'dd if=/dev/zero of=/dev/block/by-name/super bs=4096 count=1 conv=notrunc'
dd: /dev/block/sdc79: write error: Operation not permitted
```

`dmesg` 里的真话：

```
baseband_guard: pid 720 has been marked as untrusted process due to its selinux domain
baseband_guard: deny write to protected partition dev=259:63 path=/dev/block/sdc79 comm=dd
```

**判断**

- `baseband_guard` 不是模块（`/sys/module/` 里没有），是 OPPO 编进内核的安全驱动，
  配套的用户态模块叫 `oplus_security_guard`
- 它**按 SELinux 域**判断进程可信度，不是按 SELinux 策略本身
- 所以 `setenforce 0` 没用（试过，仍是 EPERM）
- 已验证被拒的域：`u:r:su:s0`（recovery 里 adb root）、`u:r:ksu:s0`（系统内 KernelSU）、`u:r:shell:s0`

**另一个坑**

OrangeFox 的 fastbootd **打不开** `vendor_dlkm_a` 和 `super`：

```
$ fastboot getvar partition-size:vendor_dlkm_a
FAILED (remote: 'Could not open partition')
$ fastboot flash vendor_dlkm vendor_dlkm.img
Resizing 'vendor_dlkm_a'  OKAY
Sending  'vendor_dlkm_a'  OKAY
Writing  'vendor_dlkm_a'  FAILED (remote: 'No such file or directory')
```

同一个 fastbootd 却能正常写 `vendor_a`。所以不是权限问题，是它对这两个分区的 dm 创建有 bug。

**结论**

只有 recovery 自己的刷写流程跑在可信域里（官方 ROM 包就是这么写 super 的）。
最省事的是 OrangeFox 的 CLI：

```bash
twrp install /sdcard/xxx.zip
```

`update-binary` 里可以正常 `dd` 到 `/dev/block/by-name/super`。

**附带注意**：OrangeFox 刚启动时 `/data` 默认**没挂载**，`/sdcard/xxx.zip` 会报
`Unable to locate zip file`。先 `mount /data`。

**不要**用 `adb push` 直接写块设备：

```
adb push img /dev/block/by-name/super
→ remote write failed: No space left on device
```

它会把符号链接换成普通文件写进 tmpfs，白等半天还没写入。

---

## 2. MTK 在 early-init 就加载 GPU 模块，元模块赶不上

**现象**

KSU 元模块明明把 `/vendor_dlkm/lib/modules/*.ko` 覆盖成 r54 的了，
但驱动版本还是原厂：

```
W mali: ==>[WARN] file /dev/mali0 is different version (user 1.38, kernel 1.18)
```

**判断**

```
$ grep -nB8 insmod_sh /vendor/etc/init/hw/init.mtkgki.rc
service insmod_sh /vendor/bin/init.insmod.sh /vendor/etc/init.insmod.${ro.hardware}.cfg
    class main
on early-init          ← 这里是关键
```

`insmod_sh` 挂在 `on early-init`（`class main`，但由 early-init 触发）。
而 KernelSU 元模块的 `metamount.sh` 要等到 **post-fs-data** 才跑。

`early-init` → `post-fs-data` 中间隔着整个 fs 挂载阶段，差得太远。

**结论**

内核模块**不可能** systemless，必须写真分区。
用户态库不受影响（GPU 设备是 SurfaceFlinger 启动时才 open 的，那时 overlay 早就生效了）。

---

## 3. r54 的 ged.ko 在这台内核上装不了，但原厂 ged 够用

**现象**

写入 r54 全套（含 r54 ged.ko）后，kbase 加载失败：

```
mali_kbase_mt6895: Unknown symbol ged_dvfs_get_last_commit_idx (err -2)
mali_kbase_mt6895: Unknown symbol ged_dvfs_cal_gpu_utilization_ex_fp (err -2)
mali_kbase_mt6895: Unknown symbol ged_dvfs_gpu_freq_commit_fp (err -2)
mali_kbase_mt6895: Unknown symbol ged_dvfs_set_gpu_core_mask_fp (err -2)
mali_kbase_mt6895: Unknown symbol ged_dvfs_gpu_clock_switch_notify (err -2)
```

手工 insmod ged.ko 才看到真正的根因：

```
ged: disagrees about version of symbol mtk_dma_buf_set_name
ged: Unknown symbol mtk_dma_buf_set_name (err -22)
ged: Unknown symbol MTKGPUQoS_setup (err -2)
ged: Unknown symbol MTKGPUQoS_mode (err -2)
ged: Unknown symbol sspm_reserve_mem_get_phys (err -2)
ged: Unknown symbol sspm_reserve_mem_get_virt (err -2)
ged: Unknown symbol sspm_reserve_mem_get_size (err -2)
```

r54 的 ged.ko 是按更新的 MTK 内核编的（要 `MTKGPUQoS_*`、`sspm_reserve_mem_*`），
这台 5.10.236-Furruka 内核没有。

**判断**

原厂 ged.ko 导出的 `ged_dvfs_*` 符号**恰好覆盖** r54 kbase 需要的全部 5 个：

```
$ nm -g --defined-only ged_stock.ko | grep -E 'ged_dvfs_(get_last_commit_idx|cal_gpu_utilization_ex_fp|gpu_freq_commit_fp|set_gpu_core_mask_fp|gpu_clock_switch_notify)$'
ged_dvfs_cal_gpu_utilization_ex_fp
ged_dvfs_get_last_commit_idx
ged_dvfs_gpu_clock_switch_notify
ged_dvfs_gpu_freq_commit_fp
ged_dvfs_set_gpu_core_mask_fp      ← 全部命中
```

**结论**

`vendor_dlkm` 里只换 **kbase + mgm + prot_alloc** 三个，
`ged.ko` / `gpueb.ko` 保持原厂。这样 kbase 拿得到符号，ged 也不会有未定义符号。

---

## 4. 底包 libc++ 太老

**现象**

```
E vndksupport: Could not load /vendor/lib64/egl/libGLES_meow.so from sphal namespace:
  dlopen failed: cannot locate symbol "_ZNSt3__122__libcpp_verbose_abortEPKcz"
  referenced by "/vendor/lib64/egl/libGLES_meow.so"
E libEGL  : load_driver(/vendor/lib64/egl/libGLES_meow.so): unknown
```

**判断**

r54 的库是用新版 NDK 编的，需要 `__libcpp_verbose_abort` 等新符号，
而 sphal 命名空间解析到的是底包 VNDK 的老 libc++。

**结论**

把 r54 包里的 `libc++.so` 放进 `/vendor/lib64/`（sphal 命名空间会优先命中 /vendor）。
`blobs/vendor_extra/libc++.so` 就是这份。

同一轮还要补 `libgpud.so` 的两个新依赖 —— 它们**虽然 /system/lib64 里有，
但 vendor 命名空间看不到**：

```
dlopen failed: library "android.hardware.graphics.common-V6-ndk.so" not found:
  needed by /vendor/lib64/libgpud.so in namespace (default)
```

所以 `android.hardware.graphics.{common-V6,allocator-V2}-ndk.so` 和
`android.hardware.common-V2-ndk.so` 也要一并放进 `/vendor/lib64/`。

---

## 5. libged / libgpud 的 SELinux 标签要是 same_process_hal_file

**现象**

上面都补齐了，还是：

```
dlopen failed: library "libged.so" not found:
  needed by /vendor/lib64/egl/mt6895/libGLES_mali.so in namespace sphal
```

文件明明存在，`ls -la` 看得到，ELF 头也正常。

**判断**

bionic 的 linker 对任何 `open()` 失败（包括 `EACCES`）都统一报
`library "xxx" not found`。所以「not found」不等于「文件不存在」。

拿非 root 的 shell 用户读一下就能区分：

```
$ adb shell 'dd if=/vendor/lib64/libged.so bs=4 count=1'
dd: /vendor/lib64/libged.so: Permission denied      ← 标签问题
```

**结论**

用 `same_process_hal_file`：

```
chcon u:object_r:same_process_hal_file:s0 libged.so libgpud.so
```

> 注意：这台机器原厂的 `libged.so` / `libgpud.so` 标签是 `vendor_file`。
> 但 overlay 之后同一份文件被 app 进程通过 sphal 命名空间加载时，
> `vendor_file` 会被拒。改成 `same_process_hal_file` 后整条链路才通。
> `egl/` 下的库和 `firmware/mali_csffw.bin` 原厂就是 `same_process_hal_file`。

---

## 6. gpueb 固件带 RSA 签名 —— 950 MHz 在这台机器上做不到

**背景**

xaga 上 950 MHz 的做法是改 gpueb 固件里 GPU OPP 表 entry[1]（880→950 kHz），
因为固件选档循环从 index 1 开始，index 0 的 950 MHz 永远选不到。
xaga 的固件没有签名，改 5 个字节没问题。

**qqcandy 不一样**

镜像尾部有第二个段：

```
0x67230: 88 16 88 58 | c5 06 00 00 | "cert1" | ... 78 56 34 12 | ff ff ff ff | 89 16 89 58
         magic1       | size 0x6c5  |
```

`cert1` 段里是 DER 编码的 RSA-2048 公钥和签名：

```
@0x67500: 30 82 01 0a 02 82 01 01 00 ...   ← SEQUENCE / INTEGER / 256 字节模数
@0x67638: 30 82 01 0a 02 82 01 01 00 ...   ← 第二份
```

**结论**

payload 一个字节都不能动。改了以后签名校验失败 → GPUEB（RISC-V RV33 协处理器）起不来 →
整机卡在开机 logo，USB 都不枚举。

**回滚**

`blobs/gpueb_stock_restore.img`（430,080 B，补齐到 1 MB 后 md5 `4f5ba918...`，
与分区原始内容完全一致）。物理分区，bootloader 里 `fastboot flash gpueb_a` 可以写。

**如果一定要 950**

只剩 AP 侧驱动自己管 DVFS 这条路（AP 的 OPP 表是驱动里的，不受签名保护），
但 EB 模式下所有频率接口都会短路到 gpueb，要先关掉 gpueb support —— 这条路没验证通。

---

## 附：属性为什么不能走 build.prop

Android 的 properties 在 `early-init` 阶段就由 init 读完了，
KernelSU 元模块的 overlay 到 `post-fs-data` 才生效 —— 那时 `ro.vendor.arm.egl.configs.*`
早就被判定为「不存在」了。

必须用模块的 `system.prop`：KernelSU 在 `post-fs-data` 用 resetprop 直接改属性区，
`ro.*` 属性只要原来不存在就能加。
