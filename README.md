# xaga-mali-r54-port

把 MediaTek **Mali-G610 (MT6895)** 的 GPU 内核驱动从原厂 **r32p1** 升级到 **r54p1**，
并配套把 **GED** 和 **gpueb** 从 6.12 源码 backport 到 Linux 5.10 的完整记录。

目标设备：Redmi Note 11T Pro / POCO X4 GT（代号 **xaga**，MT6895，Mali-G610 MC6），
运行 **Android 17 / LineageOS 24**，内核 **5.10.269**。

> 这是**移植记录仓库**，不是可直接刷入的固件。仓库里放的是补丁、改动后的源文件和验证工具。

---

## 为什么做这件事

ARM Mali 的 CSF 驱动栈要求三件东西版本严格配对：

```
libGLES_mali.so   (userspace)  ─┐
mali_kbase*.ko    (kernel)     ─┼─  必须同为 r54p1 / UK version 1.38
mali_csffw.bin    (firmware)   ─┘
```

原厂 r32p1 已停止更新。升级到 r54p1 能拿到更新的着色器编译器、更完整的 Vulkan 特性，
以及后续 Mesa/PanVK 工作的对照基准。

配套的 GED（GPU 电源/调频框架）和 gpueb（GPU Event Bridge 固件接口）也必须同步升级，
否则内核模块之间的符号 CRC 与 DT 约定会对不上。

## 结果

| 项目 | 状态 |
|---|---|
| Mali kbase | `r54p1-11eac0`，UK 1.38，vermagic 精确匹配 |
| GED | 6.12 backport，probe 正常，ABI 与原版一致 |
| gpueb | 6.12 backport，probe 正常，导出符号 56 → 73 |
| GLES | `v1.r54p1-11eac0` |
| Vulkan | 驱动 1.3.305，特性声明已修正为 1.3 |
| 填充率 | 14.1 → **18.2~18.7 Gpix/s** |
| GPU 最高频率 | 880 → **950 MHz**（固件补丁，FmeterFreq 950015 实测）|
| 3DMark Wild Life | 原厂 ~6000 → 5730（880 MHz）→ **5814**（950 MHz）|
| 系统 | 正常启动，无致命错误 |

---

## 仓库结构

```
blobs/
  vendor/                          Mali r54 用户态库（13 个，51.7MB 主库）
  vendor_dlkm/                     r54 / GED / GPUEB 内核模块（5 个）
  README.md                        完整清单与 md5
files/
  vendor-build.prop.egl-r54        44 条 ro.vendor.arm.egl.configs.* 属性
  mali_platform.config             驱动平台配置（sepolicy 已预期其存在）
  init.insmod.mt6895.cfg           模块加载清单（含 GPU 链显式 insmod）
  android.hardware.vulkan.version.xml
  android.software.vulkan.deqp.level.xml
firmware/
  mali_csffw.bin                  Mali CSF 固件 UK 1.38（ARM 专有）
  gpueb_stock.bin                 GPUEB 原厂固件（GPU 最高 880 MHz）
  gpueb_950mhz.bin                GPUEB 改 5 字节（GPU 最高 950 MHz）
  README.md                       固件说明与刷写/恢复步骤
patches/
  ged-6.12-to-5.10.patch          GED 6.12 → 5.10（478 行）
  gpueb-6.12-to-5.10.patch        gpueb 6.12 → 5.10（1042 行）
  mali/
    01-kbuild-mtk-custom-env.diff  Mali 编译开关
    02-Makefile.diff               Mali 平台配置
    backend/gpu/mali_kbase_pm_ca.c            core mask 真实实现
    backend/gpu/mali_kbase_pm_metrics.c       DVFS 钩子接线
    platform/mtk_platform_common/mtk_platform_dvfs.c   DVFS 策略
    platform/mtk_platform_common/mtk_platform_common.c MTK 公共层
    csf/ipa_control/mali_kbase_csf_ipa_control.c       IPA 频率来源
docs/
  PORTING.zh-CN.md                完整移植文档（576 行，27 节）
  FLASHABLE-SUPER.zh-CN.md        把它打成可刷入的 super.img（真实 ROM 实测，含 9 个坑与 r54 专有开关）
tools/
  gpueb_patch.py                  生成 950 MHz 的 GPUEB 固件（5 字节补丁）
  gtprobe.c                       gpufreq 域/OPP 提交探针内核模块
  glbench.c                       GLES2 填充率 / 提交开销测量
  vkprobe.c                       Vulkan 实例与物理设备探测
  gedboot.c                       单次 GED vsync 注入
  gedvsyncd.c                     GED vsync 注入守护进程（自适应频率）
  payload_extract.py              从 OTA payload.bin 提取分区
```

---

## 三个组件各自的关键坑

### Mali kbase

**1. 关掉 `CONFIG_MALI_DEVFREQ` 后 core mask 会变成空函数**

MTK 平台没有 DT clock，devfreq 建不起来。但 `kbase_devfreq_set_core_mask()`
定义在 `#ifdef CONFIG_MALI_DEVFREQ` 里，关掉就没有实现。必须补真实实现，
否则着色器核心数完全不受控。

**2. 2MB 大页支持**

官方模块没有 `"Large page support was disabled at compile-time!"` 这条日志，
说明原厂启用了 `CONFIG_LARGE_PAGE_SUPPORT`。我们构建时漏了，导致 tiler heap
频繁回收（实测 dmesg 里大量 `Tiler heap reclaim scan freed pages`）。

**3. MTK 把 ARM 的标准 DVFS 钩子编译掉了**

```c
void kbase_pm_get_dvfs_action(struct kbase_device *kbdev)
{
#if !IS_ENABLED(CONFIG_MALI_MTK_COMMON)
    ...
    kbase_platform_dvfs_event(kbdev, utilisation);   // ← 被整段屏蔽
#endif
}
```

MTK 的打算是让 GED 做 DVFS。但 GED 在本机不工作（见下），结果两边都没人管。
解法：接回这条链路，实现 `kbase_platform_dvfs_event`。

### GED

**`GE_ALLOC_STRUCT_NUM` 必须改回 18**

6.12 是 22，而设备上的 `libged.so` 按 18 发送。不改会卡开机第一屏：

```
[GPU/GED][ERROR]@ged_dispatch: Failed to region_num, it must be 22
```

**签名变更必须保持 CRC**

```c
5.10:  void ged_dvfs_gpu_clock_switch_notify(bool bSwitch)
6.12:  void ged_dvfs_gpu_clock_switch_notify(enum ged_gpu_power_state power_state)
```

6.12 实现改名 `_v612`，另加 5.10 ABI 包装并导出。

**GED 的调频引擎在本机不可用**

它的策略需要 MTK composer 上报的 KPI 帧数据。AOSP 系 ROM 没有这个组件，
策略停留在未初始化状态。强制切到负载基模式后实测：

```
GPU 利用率 100% 持续负载下，24 次采样全部锁死在 OPP 5（350MHz）
```

而强制启用反而有害——它会周期性提交，导致空闲时 GPU 误跳到 880MHz。
所以默认关闭（`gpu_dvfs_enable=0`）。

### gpueb

**模块名与主文件同名**

6.12 的模块叫 `mtk_gpueb`，主文件 `gpueb.c` 不冲突。我们要保持模块名 `gpueb`
（`ged`/`mtk_gpufreq_wrapper`/`fhctl` 都按这个名字建依赖），所以主文件必须改名
`gpueb_init.c`，否则 kbuild 只链主 .o，其余全丢。

**DT 属性命名风格变了（最隐蔽的一个）**

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

全部取不到 = `gpueb_ipi_init` 失败 = IPI channel 未注册 = gpufreq 拿不到频率服务 = 卡开机。

**三个调试 DT 资源不该强制要求**

`gpueb_gpr_base`、`gpueb_cfgreg_base`、`mfg0_pwr_con` 只被 `gpueb_debug.c` 使用，
且代码里本来就有判空。6.12 却让 probe 硬失败，改为可选即可。

---

## 换内核模块时必做的 CRC 校验

`lsmod` 显示"已加载"不代表能用。第一次替换 gpueb 时模块装进去了，但依赖链断了：

```
gpueb 102400 1          ← 自己加载了
（ged、mali 都没加载）   ← 连锁失败
```

```
mtk_gpufreq_wrapper: disagrees about version of symbol gpueb_dump_status
mtk_gpufreq_wrapper: Unknown symbol gpueb_dump_status (err -22)
```

**必须做全量 CRC 比对**，不能只查符号存在性：

```bash
llvm-nm old.ko | grep '__crc_' | awk '{print $3,$1}' | sort > old.txt
llvm-nm new.ko | grep '__crc_' | awk '{print $3,$1}' | sort > new.txt
join -j1 old.txt new.txt | awk '$2!=$3{print "MISMATCH:", $1}'
```

---

## 调频架构

```
kbase 内置 DVFS（唯一决策源）
    dvfs_callback (hrtimer, 20ms)
      → kbase_pm_get_dvfs_action (算利用率)
        → kbase_platform_dvfs_event (官方平台钩子)
          → 选档 + 提交
            → gpufreq_commit(TARGET_DEFAULT, gpu_idx)
              → GPUEB 固件 → 自动联动 STACK（Fgpu >= 1.1 * Fstack）
```

策略参数（实测调优）：

```
升频：连续 2 次采样超阈值（约 40ms 确认）
降频：保持 200ms + 连续 1 秒低负载才降一档
阈值：>=70% → 880MHz | >=50% → 800MHz | >=30% → 610MHz | >=15% → 430MHz | 其余 350MHz
```

### 调频踩过的五个假象

每个 bug 都表现为"返回成功但什么也没发生"，光看返回值发现不了。

| # | 问题 | 症状 |
|---|---|---|
| 1 | `time_busy` 是 256ns 单位，被当 ns 除 | util 恒假（差 256×） |
| 2 | `cur_gpu_rate` 只在 commit 成功时更新 | util 虚高 86% |
| 3 | **索引坐标系错位**（GPU 原始 1-5 vs 表 0-4）| `cur` 永远读 5 |
| 4 | **`TARGET_GPU` 提交在 gpueb 模式下无效** | `ret=0` 但频率不变 |
| 5 | 打印 `mtk_opp_freq[5]` 但只有 5 档 | 启动日志垃圾数据 |

详见 [
docs/PORTING.zh-CN.md              移植步骤（只讲怎么做）
docs/FLASHABLE-SUPER.zh-CN.md      整包移植：怎么产出能直接 fastboot flash 的 super.img

### gpueb 模式下 OPP 表来自固件

```c
// gpufreq_get_freq_by_idx
if (g_gpueb_support) {
    if (!gpufreq_ipi_to_gpueb(send_msg))
        freq = g_recv_msg.u.freq;    // ← 固件返回，本地静态表不参与
    goto done;
}
```

**判断档数必须读 `gpufreq_status` 的 `WorkingOPPNum`**，不要数
`/proc/gpufreqv2/gpu_working_opp_table` 的行数——那个接口会打印一行越界数据。

### 950MHz：固件补丁解锁

固件表里有 6 档（`WorkingOPPNum: 6`），最高 950MHz 在 index 0，但实测永远只到
`Index 1`（880MHz）。

根因是选档循环边界写成 `i > 0`，index 0 被永久排除 —— 这段逻辑写的时候表只有
5 档、880MHz 在 index 0（注释里还留着 `"Fgpu = 880MHz (OPP[0])"`），后来 950MHz
插到 index 0，边界没跟着改。

AP 侧三条路都堵死（均有实测依据）：

1. `gpufreq_commit(TARGET_GPU, idx)` 返回 0 但固件完全忽略（`tools/gtprobe.c` 验证）
2. 所有用户空间写接口用 `TARGET_DEFAULT`，dual-buck 下被重映射到 `TARGET_STACK`
3. EB 模式下 AP 策略表是空结构体 `platform_eb_fp = {}`

修复方式是改固件里的 OPP 表：把 entry[1] 从 880000 改成 950000（5 个字节）。
GPUEB 是 RISC-V (RV33)，固件未加密、无 hash 校验头。

```bash
python3 tools/gpueb_patch.py firmware/gpueb_stock.bin firmware/gpueb_950mhz.bin
fastboot flash gpueb_b firmware/gpueb_950mhz.bin
```

验证（`FmeterFreq` 是硬件频率计读数，不是软件上报值）：

```
[GPU-OPP]     Index: 1, Freq: 950000
[GPU-REALOPP] FmeterFreq: 950015
```

持续满载 60 秒无 fault、无复位，温度 46-54°C。详见 [firmware/README.md](firmware/README.md)。

**实际收益**：3DMark Wild Life 5730 → 5814（+1.5%），而 GPU 核心频率是 +8%。
中间差掉的 6.5% 说明这个负载下瓶颈不在核心频率——填充率、内存带宽或散热先行触顶。
核心频率上去了但分数没同比例涨，这类情况很常见，记录在这里以免高估收益。

## 已知限制

**GED 只跑在基础模式。** 它想要 11 个 `mediatek,gpu_*` DT 节点，xaga 一个都没有。
核查过全部 xaga 固件（xaga_super / HyperOS / stock / OS2.0.12）的 dtbo，
并用同款 MT6895+6.12 的 13T 完整 DTB 对照过：连 13T 也只有
`mediatek,gpu_fdvfs`（且 `fdvfs-policy-support = 0`）和 `mediatek,gpueb` 两个。

DCS、memsys、SLC、async_ratio 是 mt6991/mt6993 新平台特性，MT6895 从未定义过。

**GED 的 EB 通道仍不可用。** `ged_eb.c` 初始化要求 `mediatek,gpu_fdvfs` 与
`mediatek,gpueb` 两个节点映射 sysram/寄存器窗口，缺任一即 return。
即使补上节点，13T 自己也没开 fdvfs 策略，能得到的可能只是频率变化通知。

**GHPM 未移植。** 它只有 mt6991/mt6993 实现，MT6895 不需要。

**原生 GED DVFS 需要帧数据。** 依赖 MTK composer 的 KPI 上报，AOSP 系 ROM 无此组件。

---

## 引用

- 上游源码：[oppo-source/android_kernel_modules_oppo_mt6896](https://github.com/oppo-source/android_kernel_modules_oppo_mt6896)
- 行为对照：13T（aristo）OS3.0 的 `mali_kbase_mt6895_a16w.ko` / `mtk_gpueb.ko`

## 许可

补丁部分 **GPL-2.0**（派生自 MediaTek 的 GED/gpueb 源码与 ARM Mali 驱动）。
工具部分 **MIT**。详见 [LICENSES.md](LICENSES.md)。

## 免责声明

修改内核驱动与分区存在变砖风险。刷机前请备份原厂镜像并确认回滚方式。
作者不对任何设备损坏负责。
