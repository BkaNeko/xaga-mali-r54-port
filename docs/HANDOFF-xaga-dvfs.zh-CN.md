# xaga GPU 调频 —— 交接文档

写给接手的人/AI。目标不是复述过程，是让你**不重复踩坑、直接接着干**。

---

## 0. 一句话现状

xaga（Redmi K30 Ultra 改名机，MT6895，内核 `5.10.257-android12-9-Kirara-26Y10R01-nemo`，r54p1 Mali 驱动已正常工作）的 GPU 频率**由 gpueb 固件自带的 governor 决定**，AP 侧驱动改不动。当前配置是「AP 策略关闭、交回固件」，行为正确：空闲 350/219，满载 950，停载回落。

**未解决**：日常使用中只看得到 350 和 950 两档，中间档（880/800/610/430）从不出现。

---

## 1. 已经验证过的事实（不要再重测）

### 1.1 频率不是 AP 策略在控制

关掉整个 AP 策略后行为完全不变：

```bash
echo 0 > /sys/module/mali_kbase_mt6895/parameters/mtk_policy_enable
```

| 状态 | 实测 |
|---|---|
| 空闲 | GPU-OPP 5 / 350000，STACK-OPP 39 / 219000 |
| 满载（4 路 1280x720） | idx=1 real=950015 ×11 秒 |
| 停载后 | 回落 350 / 219 |

**结论：gpueb 固件内部有自己的 DVFS governor。** AP 的 `gpufreq_commit()` 只是建议，随时被覆盖。

### 1.2 TARGET_GPU 提交是空操作

干净状态（`DVFSState=0x0002`）实测：

```
GDBG commit: 5->0 GPU gpu_idx=0 ... ret=0    ← 连续请求 950
满载实测: idx=5  real=349984                  ← 实际还是 350
```

`ret=0` 但档位不动。**xaga 固件只认 STACK 域提交。**

### 1.3 880 和 950 互斥

把固件 `entry[1]` 从 950 改回原厂的 880（6 个不同值），stack 拉到顶 852：

```
表: 950 / 880 / 800 / 610 / 430 / 350
满载: idx=1  real=879937  stack=852000    ← 只有 880，上不去 950
```

改回 950/950 表后满载是 `idx=1 / 950015`。

**根因**：固件 `__gpufreq_get_hw_constraint_fgpu()` 的反推循环是
`for (i = min_oppidx; i > 0; i--)`，**永远返回不了 index 0**。
所以 6 条表项里 index 0 是死槽，**可用只有 index 1~5 共 5 格**。

### 1.4 用限频器压中间档也不行

`/sys/class/thermal/cooling_device5`（thermal-devfreq-0，max_state=4）：

| ceiling | 期望 | 实测满载 |
|---|---|---|
| 2 (610) | 610 | **350** |
| 3 (430) | 430 | **350** |

固件被限频后**直接塌到 floor**，不会停在 ceiling。

### 1.5 fix 接口做不了自建调度

`fix_target_oppidx` 确实能锁住档位，但有三个硬伤：

1. **每次调用强制 POWER_ON**：`__gpufreq_fix_target_oppidx_stack()` 入口就是
   `power_control(POWER_ON) ... power_control(POWER_OFF)`，40ms 一拍等于反复拉起下电的 GPU。
2. **`DVFS_DEBUG_KEEP` 毒死下电钩子**：fix 后 `g_dvfs_state=0x6`，而普通 commit 的 key 是
   `DVFS_FREE(0)`，`0x6 & ~0 = 0x6 ≠ 0` → 被状态门丢弃，残留值停在 950。
3. **仍不爬坡**：固件 governor 同一拍已顶满，策略读到的 `cur` 就是 0，无档可升。

实测表现：空闲 `2/0`（残留 800）、满载直跳 950、停载残留 `1/0`。

---

## 2. 下一步的唯一线索：GED 的 FDVFS

刚刚查到、**尚未验证**的线索，这是目前最有希望的突破口。

### 2.1 发现

固件里有 FDVFS 相关代码：

```
gpu_fdvfs_ipi_handler
[FDVFS] %s @ %d. unknown cmd(%d)!!!
middeware/gpu_fastdvfs/mt6895/src/fast_dvfs_ipi.c     ← 只有 corot 固件有这行字符串
```

而全设备扫描，**只有 `ged.ko` 里含 FDVFS 字样**（55 条），其他模块全为 0：

| 模块 | FDVFS 相关字符串 |
|---|---|
| ged.ko | **55** |
| mtk_gpufreq_wrapper / mtk_gpufreq_mt6895 / mtk_gpu_hal / mtk_gpu_qos / gpueb | 0 |

### 2.2 GED 里抓到的关键字符串

```
mediatek,gpu_fdvfs
mediatek,gpu_fdvfs_swrgo
IPI_ID_FAST_DVFS_EVENT
IPI_ID_FAST_DVFS
[GPU/FDVFS][ERROR] fail to get fast dvfs IPI channel id (ENOENT)
[GPU/FDVFS][ERROR] fail to get FDVFS EVENT IPI channel id (ENOENT)
[GPU/FDVFS][ERROR] can't find fdvfs sysram
[GPU/GED][ERROR] GPUFDVFS_IPI_SET_LOADING_SELECT fail
[GPU/FDVFS][INFO] (%d), cmd: %d, mtk_ipi_send_compl, ret: %d
[GPU/GED][ERROR] fail to read fdvfs-policy-support (%d)
[GPU/GED][INFO] @%s: fdvfs init done
```

### 2.3 假设

**GED 通过 FDVFS IPI 把 GPU loading / boost 请求送给固件，固件据此决定升到最高档。**

如果是这样，掐掉这条通道（或改掉它发的内容），固件就会退化成「听 AP commit 的」——也就是 corot 的行为，那时自建调度才成立。

这也解释了为什么关掉 AP 策略后固件仍然会升档：**发信号的是 GED，不是我的策略**。

### 2.4 建议的验证顺序

1. **先看 GED 参数有没有现成开关**（最省事）
   ```bash
   ls /sys/module/ged/parameters/
   # 已确认存在：gpu_dvfs_enable / ged_boost_enable / boost_gpu_enable /
   #            ged_smart_boost / gpu_cust_boost_freq / gpu_cust_upbound_freq / gpu_idle ...
   ```
   逐个试：`echo 0 > /sys/module/ged/parameters/<x>`，然后压满载看还上不上 950。
   **如果某个开关一关就固定在 350，说明找到阀门了。**

2. **查设备树里的 fdvfs 节点**
   ```bash
   ls /proc/device-tree/ | grep -i fdvfs
   cat /proc/device-tree/mediatek,gpu_fdvfs*/... 
   ```
   看有没有 `mediatek,gpu_fdvfs` 和 `mediatek,gpu_fdvfs_swrgo` 属性，以及是否有 disable 手段。

3. **看 GED 的 FDVFS 初始化日志**
   ```bash
   dmesg | grep -iE "fdvfs|FDVFS"
   ```
   当前实测**没有输出**，说明要么没走到，要么日志级别不够。可以调
   `/sys/module/ged/parameters/ged_log_perf_trace_enable` 或 `gpu_opp_logs_enable` 再看。

4. **反汇编 ged.ko 找 FDVFS 发送点**
   ```bash
   llvm-objdump -d --no-show-raw-insn /path/ged.ko | grep -B5 -A20 "fdvfs"
   ```
   关键词：`GPUFDVFS_IPI_SET_LOADING_SELECT`、`IPI_ID_FAST_DVFS_EVENT` 的调用点。

5. **对照 corot**
   corot 固件里有 `middleware/gpu_fastdvfs/mt6895/src/fast_dvfs_ipi.c`，**xaga 固件没有**。
   这说明两台机器的 FDVFS 实现层不同——corot 那层会把 `CMD_COMMIT` 直接应用，
   xaga 没有这层。**这是 1.1 现象的真正解释**，值得优先确认。

---

## 3. 如果 FDVFS 这条路也走不通

那就只剩改固件本身：

- 固件是 **RV33 RISC-V**，payload 未压缩（`0x200` 起是 `6f 00 c0 0a` = `jal x0, ...`，熵 5.5）
- 之前尝试反汇编，`0x200` 处不是代码起点（前面有段表），**需要先定位代码段基址**
- xaga 的 gpueb **不验签**（950 补丁能刷进去，说明签名没被校验），所以改固件是可行的
- 但要改的是 governor 决策逻辑，属于重写固件，工作量大

**优先级判断**：FDVFS 线索 >> 改固件。

---

## 4. 设备与文件现状

### 4.1 设备

```
xaga，adb 序列号 XG4HDUI79DAYAQBA
内核 5.10.257-android12-9-Kirara-26Y10R01-nemo
GLES: ARM, Mali-G610 MC6, OpenGL ES 3.2 v1.r54p1-11eac0...
```

当前状态（正常可用）：

```
mtk_policy_enable = 0
GPU-OPP Index 5 / 350000      STACK-OPP Index 39 / 219000
DVFSState 0x0002
```

### 4.2 关键镜像/模块

```
/home/bkaneko/桌面/qqcandy_r54/final/
  mali_kbase_nopolicy_verified.ko      当前装的模块（策略关闭）
  vendor_dlkm_nopolicy_verified.img    对应的 vendor_dlkm 镜像
  gpueb_950.img                        当前固件（表 950/950/800/610/430/350）
  gpueb_880_restore.img                880 还原版（会丢 950，仅实验用）
```

### 4.3 源码

```
/home/bkaneko/work/xaga_kernel_5.10/drivers/gpu/mediatek/mali-r54-test/
  gpu/arm/midgard/platform/mtk_platform_common/mtk_platform_dvfs.c   ← 主要改动文件
  gpu/arm/midgard/platform/mtk_platform_common/mtk_platform_common.c ← 下电钩子
```

### 4.4 完整技术文档

https://github.com/BkaNeko/xaga-mali-r54-port/blob/master/docs/DVFS-FIX.zh-CN.md

附录 A~G，包含：状态机推导、打包踩坑、实机验证、TARGET_GPU/880 互斥、固件自主 governor 实证、自建调度失败。

---

## 5. 两个必读的避坑点

### 5.1 打包必须走流水线，不能手工 mkfs.erofs

手工 `mkfs.erofs` 出来的 vendor_dlkm 内容全对也**起不来**（模块加载级联缺席 → surfaceflinger 无限重启 → 卡一）。

正确流程：

```bash
cd /home/bkaneko/new-port
cp <模块.ko> payload/xaga_r54/vendor_dlkm/lib/modules/mali_kbase_mt6895.ko
sudo python3 -u -m mtkport.patches.xaga_r54 out/dna -o out/dna --no-backup
sudo python3 build.py rebuild --parts vendor_dlkm
sudo python3 build.py avb-sign --parts vendor_dlkm
# 产物 out/repacked/vendor_dlkm.img，22,953,984 B，尾部有 AVBf footer

adb push out/repacked/vendor_dlkm.img /data/local/tmp/
adb shell 'su -c "dd if=/data/local/tmp/vendor_dlkm.img \
  of=/dev/block/by-name/super bs=512 seek=12130304 conv=fsync; sync"'
```

关键：**必须有 AVB footer**（`avb-sign` 那步），手工打的镜像没有，这就是卡一的原因。

### 5.2 xaga 刷完 super 后首次启动可能超过 2 分钟

不要 90 秒没起来就判死刑。我因为这个误判过一次「卡开机」，白折腾一轮。

---

## 6. 测试脚本（设备上直接跑）

```bash
# 压满载看频率（关键：real > 0 才说明 GPU 真的在跑）
/data/local/tmp/glbench 1280 720 100000 &

# 采样
cat /proc/gpufreqv2/gpufreq_status | grep -E "GPU-OPP|GPU-REALOPP|STACK-OPP|DVFSState"
```

`glbench` 已推到 `/data/local/tmp/glbench`。源码在
`/home/bkaneko/projects/xaga-mali-r54-port/tools/glbench.c`，编译：

```bash
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android21-cpp \
  -O2 -o glbench glbench.c -lEGL -lGLESv2 -lm
```

**判读要点**：
- `GPU-REALOPP FmeterFreq: 0` = GPU 已下电，此时 `GPU-OPP Index` 是**残留值不是实际频率**
- `DVFSState` 含 `0x0002` = POWEROFF，含 `0x0004` = DEBUG_KEEP（调过 fix 的痕迹）
