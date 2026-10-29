# Mali r54 DVFS 中间档位失效 —— 修复说明

适用：`mali-r54-test` 内核模块（`mali_kbase_mt6895.ko`）里的 MTK AP 侧 DVFS 策略。
已在 **corot(SM8650/HyperOS)** 与 **xaga(MT6895/Android17, 5.10.257-Kirara)** 上复现同一根因。

---

## 1. 现象

| 设备 | 观察到的档位 | 应该有的档位 |
|---|---|---|
| xaga | 只有 **950** 和 **350** | 950 / 800 / 610 / 430 / 350 |
| corot | 只有 **800** 和 **350** | 800 / 610 / 430 / 350 |

日志里能看到策略一直在请求最高的 index 0，但实际频率不动/塌到同一个值。
corot 上还有个附带症状：**满载之后频率再也不回落**。

---

## 2. 背景：MTK gpufreq v2 的双域模型

- 有两套 OPP 表：**GPU 域**（目标频率）和 **STACK 域**（总线/内存侧）。
- 硬件约束：`Fgpu >= 1.1 × Fstack`（驱动里写作 `gpu*10 >= stack*11`）。
- `g_dual_buck = true` 时，`gpufreq_validate_target()` 把 `TARGET_DEFAULT` 重映射成 **`TARGET_STACK`**。
- EB 模式（`g_gpueb_support = 1`，两台设备都是）下，`gpufreq_commit()` 全部走 IPI 发给 gpueb 固件，
  **AP 侧的 OPP 表只用于查询**。
- 提交 STACK 档之后，GPU 档由反推函数决定（`gpufreq_mt6895.c`）：

```c
/* Fstack <= 800MHz: find Fgpu >= Fstack*1.1 */
for (i = min_oppidx; i > 0; i--) {                       /* 注意 i > 0 */
    target_freq = working_table[i].freq * GPUFREQ_BASE_COEF;      /* freq*10 */
    if (target_freq >= fstack * GPUFREQ_CONSTRAINT_COEF)          /* fstack*11 */
        break;                                            /* 取最慢的满足档 */
}
```

即：**从最慢档往上找第一个满足 `freq*10 >= fstack*11` 的 GPU 档**。

---

## 3. 根因

`mtk_platform_dvfs.c` 里的 `mtk_stack_idx_for_gpu()` 用「**频率最接近**」把 GPU 档映射到 STACK 档。
但反推是**约束关系**，不是近似关系，于是高档全部塌到同一个结果：

**xaga**（STACK 最高 852MHz）

| 请求 GPU | 最近 STACK | 反推 `gpu*10 >= stack*11` | 实际得到 |
|---|---|---|---|
| 950 | 852 | 9500 ≥ 9372 ✓ | **950** |
| 800 | ≈803 | 8000 < 8833 ✗ → 跳到 950 | **950** ← 塌了 |
| 610 | ≈610 | 6100 < 6710 ✗ → 跳到 950 | **950** ← 塌了 |
| 430 | ≈430 | → 跳到 950 | **950** ← 塌了 |

**corot**（STACK 最高 695MHz）

| 请求 GPU | 最近 STACK | 反推 | 实际得到 |
|---|---|---|---|
| 950 | 695（最高档） | 9500 ≥ 7645 ✓ → 但反推循环 `i>0` 够不到 index0，取最慢满足者 | **800** |
| 880 | 695 | 同上 | **800** |
| 800 | 695 | 8000 ≥ 7645 ✓ | **800** |
| 610 | 616 | 6100 < 6776 ✗ → 跳到 800 | **800** ← 塌了 |

两台设备都退化成「最高档 + 最低档」两档在跳。

---

## 4. 修法

### 4.1 主修复：STACK 映射改成约束公式

文件：`gpu/arm/midgard/platform/mtk_platform_common/mtk_platform_dvfs.c`
函数：`mtk_stack_idx_for_gpu()`

**改成**（选「最高的、仍满足 `stack*11 <= gpu*10` 的 STACK 档」，
`mtk_stack_freq[]` 下标 0 是最高频，所以第一个满足的就是最高可用档）：

```c
static int mtk_stack_idx_for_gpu(int gpu_idx)
{
	int gpu_freq, i;

	if (mtk_stack_last < 0 || gpu_idx < 0 || gpu_idx > mtk_opp_last)
		return gpu_idx;

	gpu_freq = mtk_opp_freq[gpu_idx];

	/* 与内核 __gpufreq_get_hw_constraint_fstack() 同一套规则：
	 * 在满足 Fgpu >= 1.1 * Fstack 的前提下选最高的 STACK 档。 */
	for (i = 0; i <= mtk_stack_last; i++) {
		if (mtk_stack_freq[i] * 11 <= gpu_freq * 10)
			return i;
	}
	return mtk_stack_last;
}
```

修完后的映射（xaga 实算）：

| GPU | STACK（选中的） | 反推 | 结果 |
|---|---|---|---|
| 950 | 852 | 9500 ≥ 9372 ✓ 取最慢满足档 | **950** |
| 800 | ≈725 | 8000 ≥ 7975 ✓ | **800** |
| 610 | ≈550 | 6100 ≥ 6050 ✓ | **610** |
| 430 | ≈390 | 4300 ≥ 4290 ✓ | **430** |
| 350 | ≈318 | 3500 ≥ 3498 ✓ | **350** |

中间档位就出来了。

### 4.2 附带修复 A：利用率计数器冻结保护

文件同上，函数 `mtk_common_cal_gpu_utilization_ex()`。

`time_busy[]` / `time_idle[]` 来自 `kbase_pm_get_dvfs_metrics()`。r54 kbase 配老 `ged.ko` 时
会出现计数器不再更新，比值就被永久钉在最后一次的值上（实测一直报 `util=100`），
**表现为满载之后频率再也不降**。

在 `delta_time` 计算处插入：

```c
	delta_time = diff->time_busy[0] + diff->time_idle[0];

	{
		static u64 last_busy, last_idle;
		static bool have_last;

		if (have_last && diff->time_busy[0] == last_busy &&
		    diff->time_idle[0] == last_idle) {
			if (pui32Loading) *pui32Loading = 0;
			if (pui32Idle)    *pui32Idle = 100;
			return;			/* 两拍读数相同 = 计数器没动 = 空闲 */
		}
		last_busy = diff->time_busy[0];
		last_idle = diff->time_idle[0];
		have_last = true;
	}

	if (delta_time == 0)
		delta_time = 1;
```

### 4.3 附带修复 B：同档位 IPI 节流

策略每 40ms 跑一次，如果目标档达不到就会一直重发同一个请求（实测日志刷屏、白耗 IPI 带宽）。
在 `mtk_dvfs_apply()` 的提交分支前加：

```c
	static int mtk_last_commit_target = -1;	/* 文件级静态变量 */
	static int mtk_commit_ticks;
	...
	if (target != cur_idx) {
		...
		if (target == mtk_last_commit_target && ++mtk_commit_ticks < 25) {
			mtk_down_ticks = 0;
			return;			/* 1 秒内不重发同一档位 */
		}
		mtk_last_commit_target = target;
		mtk_commit_ticks = 0;
```

效果：corot 一次开机周期内 commit 从数百次降到个位数。

---

## 5. ⚠ 不要做的事（我踩过）

**不要加「自动钳位不可达档位」的逻辑。**

我试过统计「提交后档位没动」来判断某档不可达，然后把它钳掉。结果因为
**IPI 提交是异步的**（下一拍 40ms 读到的还是旧档位），把**最低档 350 也判成了不可达**，
`target = max(target, 350)` 之后彻底无法降频，比原来的 bug 更糟。

如果确实要处理「高档达不到」的情况，判据必须满足：
- 只对**升档**方向判（降档天然滞后）
- 连续失败次数 ≥ 10
- 永远不钳最低档

---

## 6. 950MHz 到底能不能拿到

| | xaga | corot |
|---|---|---|
| GPU OPP 表 | 950 / 950(固件补丁) / 800 / 610 / 430 / 350 | 950 / 880 / 800 / 610 / 430 / 350 |
| STACK 表最高 | **852MHz**（40 档） | **695MHz**（29 档） |
| 实测可达上限 | **950MHz** ✅ | **800MHz** ❌ |

corot 上 950 拿不到是**固件决定**的，不是策略问题：

1. STACK 表最高只有 695MHz；
2. 反推函数 `__gpufreq_get_hw_constraint_fgpu()` 的循环是 `for (i = ...; i > 0; i--)`，
   结构上够不到 `index 0`（950）；
3. 直接提交 `TARGET_GPU` 尝试绕过——**实测是空操作**（`gpufreq_commit()` 返回 0 但档位不变）。

第 3 点我在代码里做了自动探测：`TARGET_GPU` 提一次不生效就永久回退 STACK 域（日志里
`dom=GPU` / `dom=STACK`）。两台设备实测都落在 `dom=STACK`。

STACK 表在 gpueb 固件里，**带 RSA 签名，改一个字节就起不来**，所以这条路封死。

> 顺带：corot 的 gpueb 固件尾部 `0x67230` 处有 `cert1` 段，内含 DER 编码的 X.509 证书
> （`30 82 06 c1 30 82 05 70 a0 03 02 01 02 ...`）。改 payload 会让 GPUEB 校验失败 → 卡 logo。
> 回滚镜像：`qqcandy_r54/gpueb_STOCK_restore.img`（430,080 B，补齐 1MB 后 md5 `4f5ba918...`）。

---

## 7. 构建

```bash
cd /home/bkaneko/new-port
KDIR=/home/bkaneko/work/xaga_kernel_5.10 \
TOOLCHAIN=/home/bkaneko/work/android-clang12/bin \
OUTDIR=/path/to/out \
bash scripts/build-mali-ko.sh
```

产出 `mali_kbase_mt6895.ko`，未 strip 约 46MB → strip 后约 2.57MB。

### 两台设备的 vermagic 处理

模块本身编出来的 vermagic 是 xaga 内核的：

```
vermagic=5.10.269-g56de8342020a  SMP preempt mod_unload modversions aarch64
```

- **xaga**：直接用，不需要改。
- **corot**（内核 `5.10.236-android12-Furruka`）：改成原厂模块的 vermagic，
  并做 `cancel_work → cancel_work_sync` 符号改指（该内核没导出前者）：

```python
import sys; sys.path.insert(0,'/home/bkaneko/new-port/scripts')
import mali_r54_port as M
M.patch_vermagic('原始.ko', 'step.ko', "5.10.209-android12-9-o-gcb6a7153770c")
M.alias_undefined_symbol('step.ko', 'final.ko', 'cancel_work', 'cancel_work_sync')
```

> 注：corot 那个自定义内核的 vermagic 校验被关掉了，所以 5.10.209 的模块能在 5.10.236 上加载。

---

## 8. 部署

### corot —— `/vendor_dlkm` 是 **ext4**，直接写文件

```bash
adb push mali_kbase_mt6895.ko /data/local/tmp/
adb shell
su
mount -o remount,rw /vendor_dlkm
cp -f /data/local/tmp/mali_kbase_mt6895.ko /vendor_dlkm/lib/modules/mali_kbase_mt6895.ko
sync
# 标签会跟着原文件（u:object_r:vendor_file:s0），无需重设
reboot
```

> OPPO 的 `baseband_guard` 只拦**裸块设备写**，走文件系统不受影响。
> 改一次模块 10 秒，不用进 recovery。

### xaga —— `/vendor_dlkm` 是 **erofs**，必须重打镜像

erofs 只读，`remount,rw` 会返回 0 但实际仍是 ro（`mount` 输出不变）。
需要：解出 `vendor_dlkm` 分区 → 换掉 `mali_kbase_mt6895.ko` → `mkfs.erofs` 重打 → 写回 super。

xaga 上 `vendor_dlkm` 是 super 里的逻辑分区（group `qti_dynamic_partitions_a`），
位置从 lpdump 取：

```bash
lpdump /dev/block/by-name/super | grep -A4 "Name: vendor_dlkm_a"
#   Extents:
#     0 .. 44847 linear super 16941056     ← 起始扇区 16941056，长度 44848 扇区
```

---

## 9. 验证

```bash
# 1) 代码在跑（日志带 dom= 域标记）
adb shell 'su -c "dmesg | grep GDBG | tail -5"'
#   GDBG: util=8 cur=5 up=0 hold=0 down=0 dom=STACK
#   GDBG commit: 5->3 STACK idx=3 stack_idx=3 gpu_idx=3 ret=0

# 2) 中间档位是否出现（这是本修复的验收点）
adb shell 'su -c "
  /data/local/tmp/glbench 1920 1080 30000 >/dev/null 2>&1 &
  for i in \$(seq 1 15); do
    grep \"GPU-OPP\" /proc/gpufreqv2/gpufreq_status | sed \"s/.*Index: //;s/,.*//\"
    sleep 1
  done"'
#   期望在 ramp up / ramp down 过程中看到 3(610) / 4(430) 被经过，
#   而不是只在 5 和 0(或2) 之间跳

# 3) 空闲回落（corot 之前的功能 bug）
#   负载停止后 1~3 秒内应回到最低档

# 4) commit 次数不应刷屏
adb shell 'su -c "dmesg | grep -c \"GDBG commit\""'
```

`glbench` 源码在 `xaga-mali-r54-port/tools/glbench.c`，编法：

```bash
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android21-clang \
  -O2 -o glbench glbench.c -lEGL -lGLESv2 -lm
```

---

## 10. 改动清单

只动一个文件：`gpu/arm/midgard/platform/mtk_platform_common/mtk_platform_dvfs.c`

1. `mtk_stack_idx_for_gpu()` —— 频率最接近 → 约束公式（**主修复**）
2. `mtk_common_cal_gpu_utilization_ex()` —— 计数器冻结保护
3. `mtk_dvfs_apply()` —— 同档位 IPI 节流 + TARGET_GPU 探测与回退

伴随新增文件级静态变量：`mtk_gpu_domain_ok`、`mtk_gpu_req`、`mtk_last_commit_target`、`mtk_commit_ticks`。
