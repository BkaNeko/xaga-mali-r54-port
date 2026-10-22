# xaga (Redmi Note 11T Pro / MT6895) Mali DDK r32p1 → r54p1 移植记录

## 1. 目标与结果

| 项目 | 原厂 | 移植后 |
|---|---|---|
| 内核驱动 | r32p1-01eac0 (UK 1.18) | **r54p1-11eac0 (UK 1.38)** |
| CSF 固件 | 0x1010000 | **0x1050000** |
| 用户态 GLES | r32p1 | **r54p1** |
| GPU 利用率统计 | 正常 | 正常 |
| DVFS | 正常 | 正常（5 档）|
| GPU 温控 | 正常 | 正常（cooling_device 已注册）|

所有改动均在 KernelSU / 镜像层面，未解锁 bootloader。

---

## 2. 最终架构

```
┌─────────────────────────────────────────────────────┐
│ userspace: libGLES_mali.so (r54) + meow (A16)       │  ← vendor.img
│            + libmtk_mali_user.so / 图形 HAL 库      │
├─────────────────────────────────────────────────────┤
│ kbase r54p1 (UK 1.38)                               │  ← vendor_dlkm.img
│   ├─ 无 devfreq（本 SoC 无 DT clock）               │
│   ├─ 内建 simple_ondemand 调频策略                  │
│   └─ 注册 thermal cooling device                    │
├─────────────────────────────────────────────────────┤
│ gpufreqv2 (厂商模块, 未改动)                        │
│ ged.ko (厂商模块, 未改动 — 其 KPI 输入链路本身失效) │
└─────────────────────────────────────────────────────┘
```

---

## 3. 踩过的坑（按发现顺序）

### 坑 1：CFI 未插桩 → 开机内核 panic
`olddefconfig` 把 `CONFIG_LTO_CLANG_FULL` / `CONFIG_CFI_CLANG` 丢掉了，
模块无 CFI 插桩，`modprobe` 时 `CFI failure (target: trace_event_raw_init)` 直接 panic。

**修复**：显式保持 LTO+CFI，并确保 `scripts/module.lds` 是 CFI 版本
（`generate_cfi_kallsyms.pl` 需要 `__cfi_jt_start`）。

### 坑 2：EROFS 只读 + bootloader 锁定
`/vendor_dlkm` 与 `/vendor` 都是 EROFS，无法在设备上改；
`fastboot getvar unlocked` → `no`。

**方案**：解包 OTA 的 `vendor.img` / `vendor_dlkm.img`，修改后用
`mkfs.erofs -zlz4hc -b4096` 重打包（保持原 UUID / 特性），再由用户自行刷入。

### 坑 3：eglInitialize 内部 bad_optional_access
反汇编定位到崩溃点在
`vendor::arm::egl::properties::<格式>_hal_format()` 一类访问器，
要求 44 个厂商属性：

```
ro.vendor.arm.egl.configs.<format>.hal_format
ro.vendor.arm.egl.configs.<format>.framebuffer_target
ro.vendor.arm.egl.configs.<format>.recordable
```

这些属性只存在于 A16 ROM 的 `/vendor/build.prop`，
A17/LineageOS 完全没有 → `std::optional` 为空 → abort。

**修复**：从 A16 ROM 提取 44 条属性追加进 `vendor/build.prop`。
（注意：属性名长达 59 字符，超过传统的 32 字符上限，但 Android 13+ 已放宽）

### 坑 4：KernelSU 覆盖文件的 SELinux 标签错误
KSU 挂载的文件标签是 `adb_data_file` / `shell_data_file`，
zygote/surfaceflinger 无权读取 → dlopen 被拒。

**修复**：所有新增/替换文件 `chcon u:object_r:same_process_hal_file:s0`
（配置类用 `vendor_configs_file`）。

### 坑 5：ThermalHAL 崩溃 → 开机卡动画
thermal 配置里有 `{"Name":"thermal-devfreq-0","Type":"GPU"}`，
但 r54 下该 cooling device 不存在 → ThermalHAL fatal →
system_server 在 `getThermalHalLocked` 的 `waitForService` 无限阻塞。

**修复**：在驱动里注册同名 cooling device（见补丁 P-5），
注册时机必须在 ThermalHAL 启动（~7s）之前，即 kbase probe（~4.3s）。

### 坑 6：GPU 利用率恒为 0
利用率来自 `kbase_pm_get_dvfs_metrics()` 的 busy/idle 累计，
而该采样 hrtimer 在 MTK DVFS 策略下被写成了空函数：

```c
#if IS_ENABLED(CONFIG_MALI_MTK_DVFS_POLICY)
void kbase_pm_metrics_start(struct kbase_device *kbdev) {}   // 空实现
```

**修复**：恢复真实实现（补丁 P-4）。

### 坑 7：mali_kbdev 恒为 NULL
`mtk_common_get_kbdev()` 返回的全局指针只在 `mtk_common_device_init()` 里赋值，
而该函数所在的初始化表项被 `#if IS_ENABLED(CONFIG_MALI_MTK_COMMON)` 包着，
我们的构建从未定义该宏。

后果：MTK 自己的 `mtk_common_ged_dvfs_commit()` 也因拿到 NULL 直接返回，
**即使 GED 想调频，提交也会被静默丢弃**。

**修复**：启用 `CONFIG_MALI_MTK_COMMON` + 补一个 `mtk_common_set_kbdev()` 双保险（P-1/P-2）。

### 坑 8：OPP 索引空间搞错
`gpufreq_get_working_table()` 返回 40 个频率档（含签核/老化细档），
而 `gpufreq_commit()` 只接受粗 OPP 索引。

**修复**：用 `gpufreq_get_opp_num(TARGET_GPU)`（=6）取档数，
再过滤掉芯片实际不跑的 950MHz，得到 5 档：880/800/610/430/350。

注：`TARGET_DEFAULT` 返回 40，必须用 **`TARGET_GPU`**。

---

## 4. 源码补丁清单

仓库：`/home/bkaneko/work/xaga_kernel_5.10/drivers/gpu/mediatek/mali-r54-test/gpu/arm/midgard/`

| # | 文件 | 改动 |
|---|---|---|
| P-1 | `Kbuild-mtk-custom-env` | 增加 `-DCONFIG_MALI_MTK_COMMON` |
| P-2 | `platform/mtk_platform_common/mtk_platform_common.c` | 新增 `mtk_common_set_kbdev()`、`mtk_common_get_current_rate_hz()` |
| P-3 | `csf/ipa_control/mali_kbase_csf_ipa_control.c` | `cur_gpu_rate` 为 0 时从 gpufreq 取值；顺带 set_kbdev + 启动策略 |
| P-4 | `backend/gpu/mali_kbase_pm_metrics.c` | MTK 策略分支下恢复 `kbase_pm_metrics_start/stop/is_active` 真实实现 |
| P-5 | `platform/mtk_platform_common/mtk_platform_dvfs.c` | ① `kbase_devfreq_set_core_mask` 空实现 ② simple_ondemand 调频策略 ③ thermal cooling device ④ 记录 GED 提交时间戳 |
| P-6 | `gpu/arm/midgard/Makefile`（mt6895 段） | 去掉 `CONFIG_MALI_DEVFREQ` / `MALI_MTK_DEVFREQ*`，保留 `MALI_MIDGARD_DVFS` |

### 调频策略关键参数

```c
#define MTK_GPU_MAX_OPP      6          /* 上限保护 */
#define MTK_GPU_FREQ_CAP_KHZ 880000     /* 屏蔽厂商表里的 950MHz */

/* simple_ondemand 语义 */
load = busy * 100 / (busy + idle);
load = load + (load >> 2);              /* +25% 余量 */
target_freq = max_freq * load / 100;
/* 降档需连续 2 次采样, 升档立即生效 */
```

---

## 5. 镜像构建流程

```bash
# 1. 解包
fsck.erofs --xattrs --preserve --extract=vendor      vendor.img
fsck.erofs --xattrs --preserve --extract=vendor_dlkm vendor_dlkm.img

# 2. 修改（保持 SELinux xattr）
#    - vendor_dlkm/lib/modules/{mali_kbase_mt6895,mali_mgm,mali_prot_alloc}.ko
#    - vendor/lib64/egl/mt6895/libGLES_mali.so
#    - vendor/lib64/egl/{libGLES_meow,libMEOW_*}.so
#    - vendor/lib64/{libgpud,libged,libmtk_mali_user,...}.so
#    - vendor/firmware/mali_csffw.bin
#    - vendor/etc/mali_platform.config
#    - vendor/build.prop                     (+44 条 EGL 属性)
#    - vendor/etc/thermal_info_config.json   (原厂, 含 GPU cooling device)

# 3. 重打包（与原镜像参数一致）
mkfs.erofs -zlz4hc,12 -b4096 -U <原UUID> vendor.img      vendor/
mkfs.erofs -zlz4hc,12 -b4096 -U <原UUID> vendor_dlkm.img vendor_dlkm/

# 4. 刷入
adb reboot fastboot
fastboot flash vendor      vendor.img
fastboot flash vendor_dlkm vendor_dlkm.img
fastboot reboot
```

模块编译：

```bash
cd /home/bkaneko/work/xaga_kernel_5.10
PATH=/home/bkaneko/work/android-clang12/bin:$PATH \
make ARCH=arm64 LLVM=1 LLVM_IAS=1 CROSS_COMPILE=aarch64-linux-gnu- \
  M=drivers/gpu/mediatek/mali-r54-test modules \
  MTK_PLATFORM=mt6895 MTK_PLATFORM_VERSION=mt6895
# 产物需 llvm-strip --strip-debug 后放入镜像
```

---

## 6. 验收命令

```bash
su -c 'dmesg | grep -iE "Kernel DDK|Loading Mali firmware|CSF firmware was initialized"'
# 期望: r54p1-11eac0 / firmware 0x1050000 / initialized successfully

su -c 'dumpsys SurfaceFlinger | grep "GLES: ARM"'
# 期望: v1.r54p1-11eac0...

su -c 'dmesg | grep mtk-gpufreq'
# 期望: skipping OPP 0 (950000 kHz) above cap
#       using 5 entries: 880000/800000/610000/430000/350000

su -c 'dmesg | grep -c "commit .* failed"'      # 期望 0
su -c 'cat /sys/class/thermal/cooling_device*/max_state'
su -c 'grep -o "GPU-OPP.*Freq: [0-9]*" /proc/gpufreqv2/gpufreq_status'
```

---

## 7. 已知限制

1. **MTK GED 原生调频链路不可用**
   `ged_kpi` 全 0、FastDVFS disabled，与 kbase 无关（是这台机器 GED 自身的问题）。
   实际调频由驱动内建的 simple_ondemand 策略负责。

2. **Mali 官方 devfreq 路径不可用**
   本 SoC 的 mali 节点无 DT clock，kbase 主动返回 `-ENODEV`
   （`Clock not available for devfreq`）。这是平台设计，非缺陷。

3. 用户态库来自同 SoC 的小米 13T (aristo) OS3.0 A16 ROM，
   GPU 部分按 SoC 打包因此通用，但若遇到异常可换回更接近机型的版本。

---

## 8. 回滚

```bash
fastboot flash vendor      vendor.img.orig
fastboot flash vendor_dlkm vendor_dlkm.img.orig
fastboot reboot
```
（KernelSU 路径方式回滚：`su -c "rm -rf /data/adb/modules/mali_r54 && reboot"`）

---

## 9. 关键数据备查

```
设备      : xaga / 小米红米 Note 11T Pro (OP5911)
SoC       : MT6895 (Mali-G610 MC6)
内核      : 5.10.269-g56de8342020a
系统      : Android 17 / LineageOS 24
kbase     : r54p1-11eac0, UK version 1.38, vermagic 精确匹配
OPP 表    : [1]880MHz [2]800MHz [3]610MHz [4]430MHz [5]350MHz
cooling   : thermal-devfreq-0, max_state=4
```

---

# 第二部分：GED 6.12 移植（配套 r54）

## 10. 移植动机

r54 kbase 与 GED 6.12 是配套设计。原机 GED 是 5.10 版本，虽能工作但与 r54 的内部接口语义存在偏差。
13T（同款 MT6895 + 6.12 内核）的官方模块可作为行为参照。

源码：`oppo-source/android_kernel_modules_oppo_mt6896`
路径：`kernel/kernel_device_modules-6.12/drivers/gpu/mediatek/ged`

## 11. 移植步骤

1. 复制源码到 `drivers/gpu/mediatek/ged612/`
2. Makefile 中 `DEVICE_MODULES_PATH` → `$(srctree)`，`GED_TOP` 指向新目录
3. 恢复被注释的 `obj-$(CONFIG_MTK_GPU_SUPPORT) += ged.o` 并加 `ged-objs := $(ged-y)`
4. 新建 `include/ged_compat_510.h`，通过 `-include` 强制包含
5. 逐轮修编译错误（7 → 17 → 22 → 38 → 21 → 13 → 2 → 0）

## 12. 兼容点清单（18 处）

| # | 问题 | 解决方案 |
|---|---|---|
| 1 | C99 for 循环声明 | `-std=gnu11` |
| 2 | `MAX` 宏重定义 | `-Wno-error=macro-redefined` |
| 3 | `mtk_gpu_utility.h` 路径 | 加 `-I drivers/gpu/mediatek/mt-plat/` |
| 4 | `ged_gpu_slc.h`/`ged_tracepoint.h` 找不到 | 修正 `GED_TOP` 指向 |
| 5 | `gpufreq_get_cur_out_freq`/`cur_avg_freq` | 用 `gpufreq_get_cur_freq` 等价 |
| 6 | `mtk_dvfs_workload_mode`/`mtk_get_dvfs_workload_mode` | 空实现返回 false |
| 7 | `LIMIT_POWERHAL` 枚举 | 映射到 `LIMIT_PBM` |
| 8 | `DVFS_FIX_OPP`/`DVFS_FIX_FREQ_VOLT` | 补 `BIT(2)`/`BIT(3)` |
| 9 | `__assign_str(name)` | 改 5.10 两参数形式 `(name, name)` |
| 10 | `MTK_GPU_DVFS_TYPE_IDLE`/`SKIPFALLBACK` | 归并到 `FALLBACK` |
| 11 | `MODULE_IMPORT_NS` | 5.10 无此机制，置空 |
| 12 | `struct v1_data` 不完整 | 5.10 定义在 .c 内，为 GED 建本地 `gpu_bm.h` |
| 13 | `gpufreq_set_mfgsys_config` | 空实现 |
| 14 | `FEAT_ENABLE`/`CONFIG_AXI_MON` | 补 1 / 32 |
| 15 | `ged_get_last_commit_*_fp` 等 5 个函数指针 | 本地 `static` 定义 |
| 16 | `gpufreq_get_cur_temperature`/`wrapper_ready`/`MTKGPUQoS_mode_ratio`/`set_gpu_pre_throttle*`/`mtk_ipi_send_compl_to_gpueb` | 新建 `src/ged_compat_510.c` 空实现 |
| 17 | `of_property_read_u32` 隐式声明 | 给 `ged_dcs.c`/`ged_dvfs.c`/`ged_kpi.c` 补 `<linux/of.h>` |
| 18 | `ged_dvfs_gpu_clock_switch_notify` 签名从 `bool` 改枚举 | 见下 |

## 13. 最关键的 ABI 兼容

6.12 把签名从 `bool` 改成枚举，会改变符号 CRC，导致 mali 模块拒绝加载：

```
5.10:  void ged_dvfs_gpu_clock_switch_notify(bool bSwitch)
6.12:  void ged_dvfs_gpu_clock_switch_notify(enum ged_gpu_power_state power_state)
```

解决方案：原实现改名 `_v612`，另加 5.10 ABI 包装并导出：

```c
void ged_dvfs_gpu_clock_switch_notify_v612(enum ged_gpu_power_state power_state)
{ /* 原逻辑 */ }
EXPORT_SYMBOL(ged_dvfs_gpu_clock_switch_notify_v612);

void ged_dvfs_gpu_clock_switch_notify(bool bSwitch)
{
    ged_dvfs_gpu_clock_switch_notify_v612(bSwitch ? GED_POWER_ON : GED_POWER_OFF);
}
EXPORT_SYMBOL(ged_dvfs_gpu_clock_switch_notify);
```

CRC 复核（mali 依赖的 5 个符号，全部一致）：
```
ged_dvfs_cal_gpu_utilization_ex_fp  ✓
ged_dvfs_get_last_commit_idx        ✓
ged_dvfs_gpu_clock_switch_notify    ✓
ged_dvfs_gpu_freq_commit_fp         ✓
ged_dvfs_set_gpu_core_mask_fp       ✓
```

## 14. 开机卡住问题（重要）

首次刷入卡在开机第一屏，日志：

```
[GPU/GED][ERROR]@ged_dispatch: Failed to region_num, it must be 22
```

原因：`GE_ALLOC` 命令的结构数量不一致

| 版本 | `GE_ALLOC_STRUCT_NUM` |
|---|---|
| 5.10 GED（设备 libged 按此发送）| 18 |
| 6.12 GED | 22 |

用户态只发 18 个 region，6.12 内核侧要求 22，直接拒绝，导致显示合成器无法启动。

**修复**：`src/ged_ge.h` 中改回 `#define GE_ALLOC_STRUCT_NUM 18`

完整的 ABI 核查结论：命令号一致（仅末尾多 17 号）、GE 相关结构体布局完全一致、`ged_type.h` 只有追加定义 —— **这是唯一的破坏性变更**。

## 15. GED 6.12 的运行效果

- 初始化正常：`ged_init`、`fdvfs init done`
- STACK 域由 GED 原生管理：空闲 5 档（815MHz），此前 5.10 下恒为 39 档（219MHz）
- GLES 填充率：**18 Gpix/s**（移植前 14.1，提升 28%）
- 遗留 14 条 ERROR 均为 DT 节点缺失（非致命）

## 16. DT 节点缺失的结论

GED 6.12 会查找 11 个 `mediatek,gpu_*` 节点，xaga 的 DT 全部没有。

核查过全部 xaga 固件（xaga_super / HyperOS v1.1 / stock / OS2.0.12）的 dtbo，**无一包含这些节点**。
反编译 13T 的完整 DTB（334KB，同款 MT6895 + 6.12）后确认，它也只有两个 GPU 节点：

- `mediatek,gpu_fdvfs`（且 `fdvfs-policy-support = 0`，策略未启用）
- `mediatek,gpueb`

DCS、memsys、SLС、async_ratio 属于 mt6991/mt6993 新平台特性，MT6895 从未定义。

**结论**：GED 在这台机器上运行于基础模式，这是固件层面限制，不是移植缺陷。
xaga 硬件存在 gpueb 分区（`/dev/block/by-name/gpueb_a`），理论上可补 DT 节点启用 EB 通道，
但该特性只改进升频延迟，不影响持续满载频率，对跑分无收益，风险大于收益。

## 17. 性能实测结论

| 阶段 | 3DMark Wild Life |
|---|---|
| 原厂 r32 | ~6000 |
| 大页 + core mask + 调频策略 | 5767 |
| 手动 STACK 跟踪（已撤销，负优化）| 5489 |
| GED 6.12 + 官方 STACK 管理 | **5739** |

5767 → 5739 为测量波动（±0.5%）。

**峰值性能由最高频率决定，而 GPU 早已工作于硬件最高档 880MHz、利用率 99~100%。**
后续所有改进（GED 配套、STACK 管理、大页、调频响应）作用于：
- 调频响应延迟
- 缓存带宽效率
- 低负载功耗

这些在 60 秒持续满载的跑分中被摊薄，不体现为分数提升，但对日常使用和游戏体验有实际价值。

剩余约 4% 差距来源于原厂闭源部分：
1. 热管理策略（同频率下更精细的降压提频）
2. MTK perf 框架的动态功耗配额（AOSP 系 ROM 缺失）
3. 测量波动

## 18. 最终交付物

```
/home/bkaneko/桌面/vendor_dlkm.img        30.05MB  (GED 6.12 + Mali r54)
/home/bkaneko/桌面/vendor.img            921.5MB   (r54 用户态 + EGL 配置)
/home/bkaneko/桌面/vendor_dlkm.img.orig    原厂备份
/home/bkaneko/桌面/vendor.img.orig         原厂备份
```

关键源码位置：
```
drivers/gpu/mediatek/mali-r54-test/           r54 kbase 移植树
drivers/gpu/mediatek/ged612/                  GED 6.12 移植树
drivers/gpu/mediatek/ged612/include/ged_compat_510.h   兼容层
drivers/gpu/mediatek/ged612/src/ged_compat_510.c       符号垫片
```

验证命令：
```bash
su -c 'dmesg | grep "Kernel DDK version"'          # r54p1-11eac0
su -c 'dmesg | grep -c region_num'                  # 0
su -c 'cat /sys/class/misc/mali0/device/idle_hysteresis_time'  # 10000
su -c 'grep -E "GPU-OPP|STACK-OPP" /proc/gpufreqv2/gpufreq_status'
```

---

# 第三部分：gpueb 6.12 backport

## 19. 为什么要动 gpueb

GED 的频率提交最终走 GPUEB 固件（双 buck 架构），GED 的 EB（Event Bridge）也要和固件通信。
原机 gpueb 是 5.10 精简版（只导出调试和内存辅助函数），6.12 版提供了完整的 IPI 封装。

## 20. 移植踩的 8 个坑

| # | 问题 | 现象 | 解法 |
|---|---|---|---|
| 1 | `CONFIG_MTK_TINYSYS_GPUEB_SUPPORT` 未启用 | 编译 0 个文件 | 改用 `CONFIG_MTK_GPU_SUPPORT`（5.10 写法）|
| 2 | **模块名与主文件同名冲突** | 只链主 .o，其他 .o 全丢 | 主文件改名 `gpueb_init.c` |
| 3 | `-Wno-unused-but-set-variable` | Clang 报 unknown warning | 换 `-Wno-unused-variable` |
| 4 | `pde_data()` vs `PDE_DATA()` | 5.10 用大写宏 | 改 .c + 2 个头文件 |
| 5 | GHPM 符号缺失 | `ghpm_wrapper_init` 等未定义 | 兼容层，`g_ghpm_support=0` |
| 6 | **`gpueb_dump_status` 签名变化** | **CRC 不匹配，gpufreq_wrapper 拒绝加载 → 卡开机** | 保留 5.10 ABI，6.12 实现改名 `_ex` |
| 7 | **三个调试 DT 资源强制要求** | `fail to get resource GPUEB_GPR_BASE` → probe 失败 | 改为可选（代码本就有判空）|
| 8 | **DT 属性名连字符 vs 下划线** | `ipi init fail`、`reserved mem init fail` | 20 处属性名改回下划线 |

## 21. 坑 8 是真正的拦路虎

6.12 把 DT 属性命名风格从下划线改成了连字符：

```
6.12 驱动读取          5.10 设备 DT 实际
mbox-count         →  mbox_count
mbox-size          →  mbox_size
slot-size          →  slot_size
ts-mbox            →  ts_mbox
send-table         →  send_table
recv-table         →  recv_table
send-name-table    →  send_name_table
recv-name-table    →  recv_name_table
gpueb-mem-table    →  gpueb_mem_table
```

全部取不到 = 全空值 = `gpueb_ipi_init` 从头失败 → IPI channel 未注册 → gpufreq 拿不到频率服务 → 卡开机。

## 22. 坑 6 的教训：CRC 才是 ABI 的真正契约

`lsmod` 显示模块"已加载"不等于"能用"。第一次替换 gpueb 时：

```
gpueb 102400 1          ← 自己加载了
（ged、mali 都没加载）   ← 依赖链断了
```

内核日志：

```
mtk_gpufreq_wrapper: disagrees about version of symbol gpueb_dump_status
mtk_gpufreq_wrapper: Unknown symbol gpueb_dump_status (err -22)
```

**教训**：换模块时必须做**全量 CRC 比对**，不能只查符号存在性。

```bash
# 正确做法
llvm-nm old.ko | grep '__crc_' | awk '{print $3,$1}' | sort > old_crc.txt
llvm-nm new.ko | grep '__crc_' | awk '{print $3,$1}' | sort > new_crc.txt
join -j1 old_crc.txt new_crc.txt | awk '$2!=$3{print "MISMATCH:", $1}'
```

## 23. 移植结果

```
name     = gpueb                      （保持原名，6.12 改叫 mtk_gpueb）
vermagic = 5.10.269-g56de8342020a     精确匹配
导出符号 = 73 个（原 56，新增 17）
CRC      = 全部旧符号与原版一致（全量比对通过）
```

验证：

```
[GPU/EB][INFO] GPUEB driver probe done
[GPU/FREQ][INFO] gpufreq wrapper driver probe done, dual_buck: true, gpueb: on
[GPU-OPP] Index: 1, Freq: 880000, FmeterFreq: 879937
Mali-G610 6 cores, GL_VERSION v1.r54p1-11eac0
填充率 18.2 / 16.7 / 15.3 / 16.7 Gpix/s（无回退）
```

## 24. 仍未解决的问题：GED 的 EB 初始化

gpueb 通了，但 GED 的 EB 仍卡在 DT 节点：

```
[GPU/GED][INFO] @ged_dvfs_system_init: Failed to find async_dvfs_node
[GPU/GED][ERROR] gpu_slc_sysram_init: Cannot find gpu slc dts node
[GPU/GED][ERROR] gpu_memsys_sysram_init: Cannot find gpu memsys dts node
```

`ged_eb.c` 的初始化要求两个节点：

```c
mtk_gpueb_dvfs_sysram_base_addr = _gpu_fastdvfs_of_ioremap("mediatek,gpu_fdvfs");
if (NULL) { GPUFDVFS_LOGE("can't find fdvfs sysram"); return; }

mtk_gpueb_base_addr = _gpu_fastdvfs_of_ioremap("mediatek,gpueb");
if (NULL) { GPUFDVFS_LOGE("can't find gpueb"); return; }
```

两个节点在 xaga 的 DT 里都不存在。13T 的完整 DTB 里有现成定义可抄（同款 MT6895），
但注意 13T 自己也设了 `fdvfs-policy-support = 0`，所以即使补上节点，能得到的可能
只是 `gpu-freq-notify`（频率变化通知固件），而非完整的固件内调频。

## 25. 调频架构的最终结论

经过多轮实测，最终确定：

```
kbase 内置 DVFS（唯一决策源）
    dvfs_callback (hrtimer, 20ms)
      → kbase_pm_get_dvfs_action (算利用率)
        → kbase_platform_dvfs_event (官方平台钩子，我们实现的)
          → mtk_dvfs_apply (选档 + 提交)
            → gpufreq_commit(TARGET_DEFAULT)
              → 双 buck 映射 → GPUEB 固件 → GPU 域 + STACK 域
```

**GED 的调频引擎默认关闭**（`gpu_dvfs_enable=0`），原因：

- 它的策略需要 MTK composer 喂的 KPI 帧数据，本机没有
- 强制启用后它算出的目标恒为最低档，却仍周期性提交
- 导致空闲时 GPU 误跳到 880MHz（实测 10 次采样有 1-2 次）

**验证过的空转表现**（关闭我们的策略、只留 GED）：

```
GPU 利用率 100% 持续负载下，24 次采样全部锁死在 OPP 5（350MHz）
```

## 26. 关键数据备查（新增）

```
GPUEB 固件状态: /proc/gpueb/gpueb_status
  GPUEB_CFGREG_WDT_KICK = 0x000ef63e   （看门狗在踢，固件运行中）

GPUEB DT 节点资源（我们设备）:
  gpueb_base mbox0_base mbox0_set mbox0_clr mbox0_send mbox0_recv
  缺失（3 个）: gpueb_gpr_base gpueb_cfgreg_base mfg0_pwr_con
  —— 只被 gpueb_debug.c 使用，且代码有判空，故改为可选

模块加载顺序（依赖链）:
  gpueb → mtk_gpufreq_wrapper → ged → mali_kbase_mt6895
```

## 27. 最终镜像构成（vendor_dlkm.img）

```
lib/modules/gpueb.ko                  184KB  gpueb 6.12 backport
lib/modules/ged.ko                   2.0MB   GED 6.12 backport（EB IPI 已修复）
lib/modules/mali_kbase_mt6895.ko     2.5MB   Mali r54 + kbase 内置 DVFS
其余模块                                      原厂
```

vendor.img（921.5MB）：
```
lib64/egl/mt6895/libGLES_mali.so     51.7MB  r54 用户态
etc/permissions/android.hardware.vulkan.version.xml        Vulkan 1.3
etc/permissions/android.software.vulkan.deqp.level.xml     2025-03-01
```
