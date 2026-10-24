# xaga（Redmi Note 11T Pro / MT6895）Mali r54 移植指南

把 Mali DDK r54p1 移植到 MT6895 的 Android 17 / LineageOS 24 上。
本文只讲怎么移植。

---

## 1. 版本配对（必须严格一致）

| 组件 | 版本 |
|---|---|
| 用户态 `libGLES_mali.so` | r54p1-11eac0 |
| 内核 `mali_kbase_mt6895.ko` | r54p1 |
| CSF 固件 `mali_csffw.bin` | UK 1.38 |
| Vulkan 驱动 | 1.3.305 |

三者不匹配的表现：`dlopen` 报 `cannot locate symbol`，或 `eglInitialize` 崩溃。

---

## 2. 准备

```bash
# 解 OTA
unzip lineage-*-xaga-signed.zip payload.bin
python3 tools/payload_extract.py payload.bin vendor
python3 tools/payload_extract.py payload.bin vendor_dlkm
python3 tools/payload_extract.py payload.bin gpueb

# 解成目录（必须保留 xattr，否则 SELinux 标签全丢）
mkdir -p vendor vendor_dlkm
sudo mount -o loop,ro vendor.img      vendor
sudo mount -o loop,ro vendor_dlkm.img vendor_dlkm
sudo cp -a --preserve=all vendor/.      work_vendor/
sudo cp -a --preserve=all vendor_dlkm/. work_vendor_dlkm/
```

**每次都必须从原版镜像重新解包**，不要在上一次改过的产物上叠补丁。

---

## 3. `vendor.img` 改动

### 3.1 替换用户态库（10 个）

从 r54 包里拷，权限 `0644`：

```
lib64/egl/mt6895/libGLES_mali.so     51,726,104   r54 驱动
lib64/egl/libGLES_meow.so             2,165,008   MEOW 加载器
lib64/egl/libMEOW_gift.so               859,392
lib64/egl/libMEOW_trace.so              592,392
lib64/egl/libMEOW_qt.so                 201,872
lib64/egl/libMEOW_data.so               202,360
lib64/egl/libMEOW_mfrc_ext.so           731,304
lib64/libged.so                         101,944   GED 用户接口
lib64/libgpud.so                        462,976   GPU debug（r54 需要新符号）
firmware/mali_csffw.bin                 282,624   UK 1.38
```

> `libgpud.so` 不能省。r54 的 `libGLES_mali.so` 依赖
> `gpudVkCreateAccelerationStructureKHR`，原厂 `libgpud.so`（108,808 B）没有这个符号。

### 3.2 新增 ROM 里缺失的依赖（4 个）

```
lib64/libmtk_mali_user.so                        52,128   libGLES_mali 直接 NEEDED
lib64/android.hardware.graphics.common-V6-ndk.so 31,512   libgpud 直接 NEEDED
lib64/mt6895/arm.mali.platform-V2-ndk.so         85,344
lib64/mt6895/libarm_mali_config_sysprops.so      52,048
```

用 `readelf -d` 核对 r54 库的 NEEDED，逐个确认在新 ROM 里存在。

### 3.3 44 条 EGL 属性（关键，漏了就崩）

追加到 `build.prop`，完整内容见 `files/vendor-build.prop.egl-r54`：

```
ro.vendor.arm.egl.configs.r8_g8_b8_a8_32bit_fixed.hal_format=0x1
ro.vendor.arm.egl.configs.r8_g8_b8_a8_32bit_fixed.recordable=true
ro.vendor.arm.egl.configs.r8_g8_b8_a8_32bit_fixed.framebuffer_target=true
... 共 44 条
```

**r54 驱动靠这些属性构建 EGL config 表。** 缺失时 `eglInitialize` 内部取空
`std::optional`，abort 信息是 `bad_optional_access was thrown in -fno-exceptions mode`，
SurfaceFlinger 无限重启。

不需要改 `vendor_property_contexts`：已有规则 `ro.vendor.arm.` →
`vendor_mtk_gpu_prop` 覆盖这些属性。

### 3.4 `mali_platform.config`

```bash
cp files/mali_platform.config work_vendor/etc/mali_platform.config
```

内容 2 行：

```
PLATFORM_AGT_FREQUENCY_KHZ=13000
OSU_PROTECTED_MEMORY_HEAP_NAME=mtk_svp_page-uncached
```

两个原厂 ROM 里都没有这个文件，但 `vendor_file_contexts` 里**已经有**它的规则：

```
/vendor/etc/mali_platform.config u:object_r:same_process_hal_file:s0
```

sepolicy 预期它存在，直接补上。

### 3.5 Vulkan 声明

```
etc/permissions/android.hardware.vulkan.version.xml       version=4206592    (1.3.0)
etc/permissions/android.software.vulkan.deqp.level.xml    version=132711169  (2025-03-01)
```

原版是 1.1（4198400），改数值即可，保留原文件结构。

### 3.6 模块加载兜底（建议）

`etc/init.insmod.mt6895.cfg` 加上按依赖顺序的 `insmod|` 行，内容见
`files/init.insmod.mt6895.cfg`。

这个 ROM 用 MTK 的 `init.insmod.sh` 加载 `vendor_dlkm` 模块：

```rc
# /vendor/etc/init/hw/init.mtkgki.rc
service insmod_sh /vendor/bin/init.insmod.sh /vendor/etc/init.insmod.${ro.hardware}.cfg
on early-init
    start insmod_sh
```

cfg 里原本只有 `modprobe|*`（走 `modules.dep` 依赖解析）。若解析失败，
GPU 链会整体不加载，所以再补一层显式 `insmod`：

```
modprobe|*
insmod|/vendor_dlkm/lib/modules/mtk-mbox.ko
insmod|/vendor_dlkm/lib/modules/mtk_rpmsg_mbox.ko
...
insmod|/vendor_dlkm/lib/modules/mali_kbase_mt6895.ko
setprop|vendor.all.modules.ready
```

依赖顺序：

```
mtk-mbox mtk_rpmsg_mbox mtk_tinysys_ipi tinysys-scmi mtk_qos sspm_v3
pmic_lbat_service mtk_battery_oc_throttling mtk_low_battery_throttling
mtk_dynamic_loading_throttling mtk_bp_thl mtk_mdpm mtk-dvfsrc-start
mtk_pbm mtk_gpu_hal mtk_gpu_qos mtk_gpufreq_wrapper mtk_gpufreq_mt6895
gpueb ged mali_mgm mali_prot_alloc mali_kbase_mt6895
```

### 3.7 SELinux 标签

**最容易翻车的一步。** 库和 HAL 必须是：

- `vendor/lib64/egl/*` → `u:object_r:same_process_hal_file:s0`
- `firmware/mali_csffw.bin` → `u:object_r:same_process_hal_file:s0`（不是 vendor_file）
- `etc/*.xml`、`etc/init*.cfg` → `u:object_r:vendor_configs_file:s0`

改完用原版镜像做权威来源逐个对齐：

```python
import os
for root, _, files in os.walk(NEW):
    for f in files:
        p = os.path.join(root, f)
        if os.path.islink(p):
            continue
        rel = os.path.relpath(p, NEW)
        src = os.path.join(ORIG, rel)
        want = (os.getxattr(src, "security.selinux") if os.path.exists(src)
                else b"u:object_r:same_process_hal_file:s0")
        os.setxattr(p, "security.selinux", want)
```

---

## 4. `vendor_dlkm.img` 改动

替换 **5 个**模块（缺一不可）：

| 模块 | 原厂 | r54 |
|---|---|---|
| `mali_kbase_mt6895.ko` | 1,670,712 | 2,568,256 |
| `mali_mgm.ko` | 29,360 | 28,704 |
| `mali_prot_alloc.ko` | 24,400 | 24,240 |
| `ged.ko` | 431,688 | 2,028,912 |
| `gpueb.ko` | 111,736 | 183,752 |

其余 423 个文件保持原样。标签全部 `u:object_r:vendor_file:s0`，
`etc/` 下 4 个文件是 `vendor_configs_file`（共 424 + 4）。

---

## 5. gpueb 950MHz（可选）

> 完整前因后果（固件表结构、AP 三条路为什么堵死、数学佐证、调度侧连带修的三个问题、
> 实际收益）见 [950MHZ.zh-CN.md](950MHZ.zh-CN.md)。

GPUEB 是 RISC-V 协处理器，GPU 频率由它决定。其 GPU OPP 表在偏移 `0x15d60`，
6 项 × 24 字节，950MHz 在 index 0，但固件选档循环边界是 `i > 0`，
**index 0 永远选不到**，实际最高只到 index 1（880MHz）。

950 MHz 用 **787.5 mV**（表里 78750，单位 0.1 mV），和原厂 880 档同压。

把 index 1 改成 950MHz 即可解锁：

```bash
python3 tools/gpueb_patch.py gpueb.img gpueb_950.img --pad-to 1048576
# 等价于：0x15d78 起 880000 -> 950000，0x15d80 起 75000 -> 78750
```

补齐到分区大小 1MB 时 **必须用 `0x00` 填充**（原厂就是这样），不要用 `0xFF`：

```python
padded = patched + b"\x00" * (1048576 - len(patched))
```

验证：与原厂 1MB 镜像应**只差 5 个字节**。

---

## 6. 重新打包

```bash
sudo mkfs.erofs -zlz4hc,12 -b4096 -U <UUID> vendor_new.img       work_vendor
sudo mkfs.erofs -zlz4hc,12 -b4096 -U <UUID> vendor_dlkm_new.img  work_vendor_dlkm
```

### UUID 的坑

`mkfs.erofs -U` **按字面字符串写入 superblock**，不做字节序转换。
原镜像的 UUID 要用 raw 字节读出，直接当字符串传：

```python
f = open("vendor.img", "rb"); f.seek(1024); d = f.read(64)
h = d[48:64].hex()          # raw 字节
print(f"{h[0:8]}-{h[8:12]}-{h[12:16]}-{h[16:20]}-{h[20:32]}")
```

`file` 命令显示的 UUID 是字节序反转后的，**不能直接用**。

---

## 7. 刷入

```bash
fastboot getvar current-slot                       # 先确认槽位！
fastboot flash vendor      vendor_new.img
fastboot flash vendor_dlkm vendor_dlkm_new.img
fastboot flash gpueb_<slot> gpueb_950.img          # current-slot 是 b 就刷 gpueb_b
fastboot reboot
```

**槽位会变。** 开机失败时 Android 的 boot control 会自动切换到另一个槽，
所以每次刷之前都要重新确认 `current-slot`，否则会刷到不再使用的分区上
（表现为「刷了但没生效」）。

如果 `fastboot flash gpueb_*` 不生效，可以在 root shell 里直接写：

```bash
adb push gpueb_950.img /data/local/tmp/
adb shell 'su -c "dd if=/data/local/tmp/gpueb_950.img of=/dev/block/by-name/gpueb_b bs=1M; sync"'
```

---

## 8. 验收

```bash
su -c 'getprop sys.boot_completed'                              # 1
su -c 'getprop ro.vendor.arm.egl.configs.nv12.hal_format'       # 0x200
su -c 'cat /sys/class/misc/mali0/device/gpuinfo'                # Mali-G610 6 cores
su -c 'dumpsys SurfaceFlinger | grep -o "v1.r54p1[a-z0-9-]*"'   # v1.r54p1-11eac0
su -c 'pm list features | grep vulkan.version'                  # 4206592
su -c 'sed -n 2p /proc/gpufreqv2/gpu_signed_opp_table'          # 950000（打过补丁）
su -c 'lsmod | grep -E "mali_kbase|mali_mgm|mali_prot_alloc|ged|gpueb"'
```

满载验频（`FmeterFreq` 是硬件频率计，不是软件上报值）：

```bash
su -c 'cd /data/local/tmp && (for i in $(seq 1 30); do ./glbench 1280 720 3000 >/dev/null 2>&1; done) &'
sleep 4
su -c 'grep -E "GPU-OPP|GPU-REALOPP" /proc/gpufreqv2/gpufreq_status'
# 期望 FmeterFreq ≈ 950015
```

---

## 9. 症状对照

| 症状 | 原因 |
|---|---|
| `dlopen failed: ... not found` | 缺依赖库，或 SELinux 标签不是 `same_process_hal_file` |
| `cannot locate symbol "gpudVk..."` | `libgpud.so` 没换成 r54 版 |
| `bad_optional_access` / `no suitable EGLConfig` | 缺 44 条 `ro.vendor.arm.egl.configs.*` |
| GPU 模块全部 MISSING | `vendor_dlkm` 标签丢成 `unlabeled`，或 `modules.load` 依赖解析失败 |
| 刷了镜像但没变化 | 槽位变了，刷到了非活动分区 |
| 整机卡开机画面 | 内核模块与用户态版本不配对（r54 用户态 + r32 内核等） |
| GPU 最高只到 880MHz | gpueb 固件没打 950 补丁 |

---

## 10. 内核侧

模块源码改动见 `patches/`：

```
patches/mali/                        Mali kbase 平台层（DVFS / IPA / pm_metrics）
patches/ged-6.12-to-5.10.patch       GED 6.12 回移到 5.10
patches/gpueb-6.12-to-5.10.patch     gpueb 6.12 回移到 5.10
```

编译：

```bash
export PATH=/path/to/android-clang/bin:$PATH
make -j8 ARCH=arm64 LLVM=1 LLVM_IAS=1 CROSS_COMPILE=aarch64-linux-gnu- \
  M=drivers/gpu/mediatek/mali-r54-test modules \
  MTK_PLATFORM=mt6895 MTK_PLATFORM_VERSION=mt6895
llvm-strip --strip-debug <每个>.ko      # 放进镜像前必须做，体积减半
```

调频链路：

```
kbase hrtimer (20ms)
  → kbase_pm_get_dvfs_action
    → kbase_platform_dvfs_event
      → mtk_dvfs_apply                    (阈值分档 + 升降频防抖)
        → gpufreq_commit(TARGET_DEFAULT, stack_idx)
          → GPUEB 固件（自行推导 GPU 频率）
```

`gpufreq_commit` 的第二参数走 `TARGET_DEFAULT`，dual-buck 下被
`gpufreq_validate_target()` 重映射成 `TARGET_STACK`。STACK 表和 GPU 表长度不同
（40 vs 6），**必须按频率映射索引**，不能把 GPU 索引直接当 STACK 索引传。
