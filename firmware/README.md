# 固件

这个目录放的是设备上实际使用的两份 GPU 相关固件，以及解锁 950 MHz 所用的补丁文件。

| 文件 | 大小 | 用途 |
|---|---|---|
| `mali_csffw.bin` | 282,624 B | Mali CSF 固件，UK 1.38，配合 r54p1 用户态驱动 |
| `gpueb_stock.bin` | 1,048,576 B | GPUEB 协处理器固件（原厂），GPU 最高 880 MHz |
| `gpueb_950mhz.bin` | 1,048,576 B | 同上，改 5 字节后 GPU 最高 950 MHz |

## mali_csffw.bin

ARM Mali CSF 固件。Mali 驱动栈要求三者版本严格配对：

```
libGLES_mali.so   r54p1 / UK 1.38
mali_kbase*.ko    r54p1
mali_csffw.bin    UK 1.38
```

设备上的位置：`/vendor/firmware/mali_csffw.bin`

拉取方式：

```bash
adb shell 'su -c "cp /vendor/firmware/mali_csffw.bin /data/local/tmp/ && chmod 644 /data/local/tmp/mali_csffw.bin"'
adb pull /data/local/tmp/mali_csffw.bin
```

> **许可提示**：这是 ARM 的专有固件，不是开源代码。放在这里仅作为移植工作的对照与完整性记录，
> 不适用本仓库的 GPL-2.0 许可。分发前请自行确认 ARM 的授权条款。

## gpueb_stock.bin / gpueb_950mhz.bin

GPUEB 是 MT6895 上的 **RISC-V (RV33)** 协处理器，GPU 的频率、电压、电源域全部由它控制。
AP 只能通过 IPI 发请求，固件决定实际频率。

固件头：

```
0x00: 88 16 88 58      magic
0x04: 2c ef 01 00      size
0x08: "tinysys-gpueb-RV33_A"
```

**未加密，固件头里没有任何 hash 或签名字段。** 实际数据只有约 96 KB（0x0–0x17000），
其余是 0xFF 填充。

### 为什么要改它

固件里的 GPU OPP 表在偏移 `0x15d60`，6 项，每项 24 字节：

```
[0] 950000  78750  78750  posdiv=2   <- 固件永远不选这一档
[1] 880000  78750  75000  posdiv=2   <- 固件能选到的最高档
[2] 800000  72500  75000  posdiv=2
[3] 610000  65000  75000  posdiv=2
[4] 430000  60000  75000  posdiv=2
[5] 350000  57500  75000  posdiv=3
```

选档循环的边界写成了 `i > 0`，index 0 被永久排除。AP 侧源码里同一个函数还留着痕迹：

```c
/*
 * Fstack <= 800MHz: find Fgpu >= Fstack*1.1
 * Fstack > 800Mhz: Fgpu = 880MHz (OPP[0])     <- 880 曾是 index 0
 */
for (i = min_oppidx; i > 0; i--) {             <- 边界没随表更新
```

这是「旧 5 档表 + 新 6 档表」错位一格造成的 off-by-one。

### AP 侧为什么绕不过去

三条路都堵死了，都有实测依据：

1. **`gpufreq_commit(TARGET_GPU, idx)` 无效**
   `tools/gtprobe.c` 实测：`idx=0..5` 全部返回 `ret=0`，GPU 频率一动不动。
   固件只认真 `TARGET_STACK`。

2. **所有用户空间写接口都走 `TARGET_DEFAULT`**
   而 `gpufreq_validate_target()` 在 dual-buck 下把它重映射成 `TARGET_STACK`：

   ```c
   if (*target == TARGET_DEFAULT) {
       if (g_dual_buck) *target = TARGET_STACK;
       else             *target = TARGET_GPU;
   }
   ```

   所以 `/proc/gpufreqv2/*` 里的 `fix_target_opp_index`、`fix_custom_freq_volt`、
   `set_limit` 全部作用在 STACK 域。写 `950000` 会报 `fail to fix STACK freq: 950000 (-1)`。

3. **EB 模式下 AP 侧策略是空壳**

   ```c
   static struct gpuppm_platform_fp platform_eb_fp = {};
   ```

   一行都不执行，全交给固件。

### 补丁内容

把 entry[1] 从 880 MHz 改成 950 MHz（vsram 同步改成库存 950 档的 78750）：

```
0x15d78: 80 6d 0d 00  ->  f0 7e 0e 00    (880000 -> 950000)
0x15d80: f8 24 00 00  ->  9e 33 00 00    (75000 -> 78750)
```

共 5 个字节，文件大小不变。用脚本生成：

```bash
python3 tools/gpueb_patch.py firmware/gpueb_stock.bin firmware/gpueb_950mhz.bin
```

### 刷入

当前 slot 是 `_b` 的话：

```bash
fastboot flash gpueb_b firmware/gpueb_950mhz.bin
```

> A/B 两个 gpueb 分区在出厂时内容完全相同（md5 都是 `07dd0afd...`），
> 可以互为备份。刷之前务必确认两个分区都备份过。

### 恢复

```bash
fastboot flash gpueb_b firmware/gpueb_stock.bin
```

### 验证

```bash
# 表里 entry[1] 应该变成 950000
adb shell 'su -c "cat /proc/gpufreqv2/gpu_signed_opp_table"'

# 满载时看硬件频率计（FmeterFreq 是硬件读数，不是软件上报值）
adb shell 'su -c "cat /proc/gpufreqv2/gpufreq_status" | grep -E \"GPU-OPP|GPU-REALOPP\"'
```

实测结果：

```
[GPU-OPP]     Index: 1, Freq: 950000
[GPU-REALOPP] FmeterFreq: 950015        <- 硬件实测 950 MHz
```

持续满载 60 秒无 fault、无复位，温度 46-54°C。

### 风险

| 情况 | 后果 | 恢复 |
|---|---|---|
| bootloader 校验固件 | GPUEB 不启动，GPU 全死 | 刷回 `gpueb_stock.bin` |
| 950 MHz 在本机不稳 | 满载崩溃/花屏 | 刷回 `gpueb_stock.bin` |
| 正常 | GPU 满载 950 MHz | — |

本机实测验证：固件头无 hash，改动后 bootloader 正常加载，说明没有签名校验。
但不同机型/固件版本不保证如此，刷之前请自行备份。
