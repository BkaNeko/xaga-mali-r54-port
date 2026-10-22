# xaga-mali-r54-port

把 MediaTek **Mali-G610 (MT6895)** 的 GPU 内核驱动从原厂 **r32p1** 升级到 **r54p1**，
并配套移植 **GED 6.12** 的移植记录与补丁集。

目标设备：Redmi Note 11T Pro / POCO X4 GT（代号 **xaga**，MT6895，Mali-G610 MC6），
运行 **Android 17 / LineageOS 24**，内核 **5.10.269**。

> 这是一个**移植记录仓库**，不是可直接刷入的成品固件。
> 仓库里放的是补丁、改动后的源文件和验证工具；镜像本身请自行构建。

---

## 为什么要做这件事

ARM Mali 的 CSF 驱动栈要求三件东西版本严格配对：

```
libGLES_mali.so   (userspace)  ─┐
mali_kbase*.ko    (kernel)     ─┼─  必须同为 r54p1 / UK version 1.38
mali_csffw.bin    (firmware)   ─┘
```

原厂 r32p1 已经停止更新。升级到 r54p1 能拿到：
更新的着色器编译器、更完整的 Vulkan 特性、以及后续 Mesa/PanVK 工作的对照基准。

## 结果

| 项目 | 状态 |
|---|---|
| 内核 DDK | `r54p1-11eac0`，UK version 1.38，vermagic 精确匹配 |
| 固件 | 加载并初始化成功 |
| GLES | `v1.r54p1-11eac0` |
| Vulkan | API 1.3.305 正常 |
| 填充率 | 14.1 → **18.0 Gpix/s** |
| 3DMark Wild Life | 原厂 ~6000 → **5739**（96%）|
| 系统 | 正常启动，无致命错误 |

性能为何没有超过原厂，见 [docs/PORTING.zh-CN.md](docs/PORTING.zh-CN.md) 第 17 节：
峰值性能由最高频率决定，而 GPU 早已工作在硬件最高档 880 MHz。后续改进作用于
调频响应、缓存带宽和低负载功耗，在持续满载的跑分里不体现为分数。

---

## 仓库结构

```
patches/
  ged-6.12-to-5.10.patch          GED 6.12 → Linux 5.10 的完整补丁（598 行，12 个文件）
  mali/
    01-kbuild-mtk-custom-env.diff  Mali 编译配置变更
    02-Makefile.diff               Mali 平台配置变更
    backend/gpu/mali_kbase_pm_ca.c           devfreq 关闭时的 core mask 真实实现
    backend/gpu/mali_kbase_pm_metrics.c      恢复被 MTK 打桩的利用率统计
    platform/mtk_platform_common/mtk_platform_dvfs.c   DVFS 策略与温控
    platform/mtk_platform_common/mtk_platform_common.c  MTK 公共层接线
    csf/ipa_control/mali_kbase_csf_ipa_control.c        IPA 频率来源
docs/
  PORTING.zh-CN.md                完整移植文档（410 行，含 18 个坑与实测数据）
tools/
  glbench.c                       GLES2 填充率 / 提交开销测量
  vkprobe.c                       Vulkan 实例与物理设备探测
  gedboot.c                       向 GED 投递 vsync 事件（激活原生 DVFS）
  payload_extract.py              从 OTA payload.bin 提取分区
```

---

## 快速开始

详细步骤见 [docs/PORTING.zh-CN.md](docs/PORTING.zh-CN.md)。要点：

**1. 打 GED 补丁**

```bash
git clone <oppo-source/android_kernel_modules_oppo_mt6896>
cd kernel_device_modules-6.12/drivers/gpu/mediatek
cp -r ged /path/to/kernel/drivers/gpu/mediatek/ged
cd /path/to/kernel/drivers/gpu/mediatek/ged
git apply -p1 /path/to/patches/ged-6.12-to-5.10.patch
```

**2. 编译**

```bash
make -j$(nproc) ARCH=arm64 LLVM=1 LLVM_IAS=1 \
     M=drivers/gpu/mediatek/ged modules \
     MTK_PLATFORM=mt6895 MTK_PLATFORM_VERSION=mt6895
```

**3. 验证 ABI（关键）**

换掉 GED 之前必须核对 mali 依赖的符号 CRC，不一致会导致 mali 拒绝加载：

```bash
for s in ged_dvfs_cal_gpu_utilization_ex_fp ged_dvfs_get_last_commit_idx \
         ged_dvfs_gpu_clock_switch_notify ged_dvfs_gpu_freq_commit_fp \
         ged_dvfs_set_gpu_core_mask_fp; do
    echo -n "$s "
    llvm-nm new_ged.ko | grep "__crc_$s\$" | awk '{print $1}'
done
```

---

## 三个最容易踩的坑

### 1. `GE_ALLOC_STRUCT_NUM` 必须改回 18

6.12 的值是 22，而设备上的 `libged.so` 按 18 发送。不改会卡开机第一屏：

```
[GPU/GED][ERROR]@ged_dispatch: Failed to region_num, it must be 22
```

### 2. `ged_dvfs_gpu_clock_switch_notify` 签名变了

6.12 把 `bool` 改成了枚举，符号 CRC 随之改变。需要保留 5.10 ABI 的包装函数，
否则 mali 加载失败。见补丁 `src/ged_notify_sw_vsync.c`。

### 3. 关掉 `CONFIG_MALI_DEVFREQ` 后 core mask 会变成空函数

MTK 平台没有 DT clock，devfreq 建不起来。但 `kbase_devfreq_set_core_mask()`
定义在 `#ifdef CONFIG_MALI_DEVFREQ` 里，关掉就没有实现。必须补一个真实实现，
否则着色器核心数完全不受控。

完整的 18 个坑见文档第 12 节。

---

## 已知限制

**GED 只能跑在基础模式。** 它想要 11 个 `mediatek,gpu_*` DT 节点，xaga 一个都没有。
核查过全部 xaga 固件（xaga_super / HyperOS / stock / OS2.0.12）的 dtbo，
并用同款 MT6895+6.12 的 13T 完整 DTB 对照过：连 13T 也只有
`mediatek,gpu_fdvfs`（且策略未启用）和 `mediatek,gpueb` 两个。

DCS、memsys、SLC、async_ratio 是 mt6991/mt6993 新平台的特性，MT6895 从未定义过。
这是固件层限制，不是移植缺陷。

**原生 GED DVFS 需要外部驱动。** GED 的调频完全由 userspace 每帧投递的
vsync 事件驱动，AOSP 系 ROM 没有这个组件，所以 `gpu_loading` 恒为 0。
`tools/gedboot.c` 可以手动投递一次事件来激活它，但要做成长期生效需要
一个周期注入的守护进程。

---

## 引用

- 上游 GED 源码：[oppo-source/android_kernel_modules_oppo_mt6896](https://github.com/oppo-source/android_kernel_modules_oppo_mt6896)
- 设备参考数据：13T（aristo）OS3.0 A16 的 `mali_kbase_mt6895_a16w.ko` 作为 r54 行为对照

## 许可

补丁部分按 **GPL-2.0** 发布（派生自 MediaTek 以 GPL-2.0 发布的 GED 源码）。
工具部分为 **MIT**。详见 [LICENSES.md](LICENSES.md)。

## 免责声明

修改内核驱动与分区存在变砖风险。本项目仅作技术记录，刷机前请务必备份原厂镜像，
并确认自己了解回滚方式。作者不对任何设备损坏负责。
