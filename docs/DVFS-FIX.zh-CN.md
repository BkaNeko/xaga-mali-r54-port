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

---

## 11. 追加修复 C（2026-10-30）：cur_idx 映射查不到时按频率解析

**症状**：xaga 刷入主修复后中间档能选中并提交（commit 日志可见 idx=18/27/10 映射正确），
但 GPU 钉死在 950 不降频。GDBG 每一行 `cur=4` 永远不变。

**根因**：950 固件补丁让 raw idx 0/1 **都是 950000**，STACK 域反推恰好落到 **idx 1**；
而 `mtk_opp_table_init()` 去重后映射表只登记了 idx 0。`mtk_dvfs_apply()` 里
`gpufreq_get_cur_oppidx(TARGET_GPU)` 返回 1，在映射表里查不到，代码落到了
`cur_idx = mtk_opp_last`（350 档）——策略从此认为自己在 350，util 再低也判
"无需动作"，开环钉死在最高档。

**修法**：映射查不到时**按当前频率解析策略档**（表按频率降序，第一个
`<= 当前频率` 的即当前档），不再默认最低档：

```c
	if (cur_idx == mtk_opp_last) {
		int freq_khz = gpufreq_get_cur_freq(TARGET_GPU);

		if (freq_khz > 0)
			for (i = 0; i <= mtk_opp_last; i++)
				if (mtk_opp_freq[i] <= freq_khz) {
					cur_idx = i;
					break;
				}
	}
```

> 本文第 10 节配套的 `files/mtk_platform_dvfs.c` 与 `final/mtk_platform_dvfs.c`
> 之前是**主修复之前的中间快照**（含修复 A/B、缺主修复），已同步为内核树
> （xaga_kernel_5.10）的权威版本。

**部署**：xaga 走 new-port 流水线（payload/xaga_r54 换装重编模块，vermagic 由
补丁统一改写为 226），super 重打后 md5 `319ec6ca…`。corot 若要同步此修复：
ext4 remount + cp 同一个 `mtk_platform_dvfs.c` 重编出的模块，vermagic 按
第 7 节改写为 5.10.209。

---

# 附录 A：commit 与 fix 的状态机完整推导

> **本附录推翻了早前的一个结论。**
> 早前记录为「xaga 的固件无视 commit，FIX 是唯一控制面」——那个结论是**在 GPU 已下电的
> 空载状态下测出来的**，测得的是假象。下面是完整推导和证据。

## A.1 状态变量：`g_dvfs_state`

```c
enum gpufreq_dvfs_state {          /* gpufreq_v2.h */
	DVFS_FREE        = 0,
	DVFS_DISABLE     = 1 << 0,   /* 0x01 */
	DVFS_POWEROFF    = 1 << 1,   /* 0x02 */
	DVFS_DEBUG_KEEP  = 1 << 2,   /* 0x04 */
	DVFS_AVS_KEEP    = 1 << 3,   /* 0x08 */
	DVFS_AGING_KEEP  = 1 << 4,   /* 0x10 */
};
```

全生命周期只有这几处改动它（`gpufreq_mt6895.c`）：

| 行 | 触发 | 动作 | 时机 |
|---|---|---|---|
| 3743 | 初始化，DVFS 被关 | `= DVFS_DISABLE` | 开机一次 |
| 3756 | 初始化，DVFS 开启 | `= DVFS_FREE` | 开机一次 |
| 3805 | `__gpufreq_pause_dvfs()` | `\|= DVFS_AGING_KEEP` | 老化/AVS 校准期间 |
| 3825 | `__gpufreq_resume_dvfs()` | `&= ~DVFS_AGING_KEEP` | 校准结束 |
| 827 | `__gpufreq_power_control(POWER_ON)` | `&= ~DVFS_POWEROFF` | GPU 上电 |
| 832 | `__gpufreq_power_control(POWER_OFF)` | `\|= DVFS_POWEROFF` | GPU 下电 |
| 1163 | `fix_target_oppidx(idx>=0)` | `\|= DVFS_DEBUG_KEEP` | 调用 fix |
| 1160 | `fix_target_oppidx(-1)` | `&= ~DVFS_DEBUG_KEEP` | 调用 fix(-1) |

老化/AVS 只在初始化跑一次且 resume 会清位，所以开机之后只剩三种可能：

- **GPU 上电运行中：`0x0`**
- **GPU 下电空闲：`0x2`**（POWEROFF）
- 调用过 fix：多一个 `0x4`，只有 `fix(-1)` 能清

`/proc/gpufreqv2/gpufreq_status` 里那行 `DVFSState: 0x%04x` 走
`gpufreq_get_dvfs_state()`，EB 模式下读的就是**固件 shared_status 里的同一个变量**。

## A.2 提交门

`__gpufreq_generic_commit_stack()`（1040 行附近）：

```c
	mutex_lock(&gpufreq_lock);

	/* check dvfs state */
	if (g_dvfs_state & ~key) {
		GPUFREQ_LOGD("unavailable DVFS state (0x%x)", g_dvfs_state);
		ret = GPUFREQ_SUCCESS;      /* 静默丢弃，返回 0 */
		goto done_unlock;
	}
```

`key` 不是"要设置成什么状态"，而是 **"这次提交允许在哪些状态位下生效"的掩码**。
判据是 `g_dvfs_state & ~key`：

| 调用方 | `key` | 判据化简 | 结果 |
|---|---|---|---|
| 普通 commit | `DVFS_FREE`(0) | `g_dvfs_state` | **只要状态非 0 就丢弃** |
| fix | `DVFS_DEBUG_KEEP`(4) | `g_dvfs_state & ~4` | 状态 ≤0x4 时通过 |

普通 commit 的 key 来自 `gpuppm_limited_commit()`（`gpuppm.c:533`）：

```c
	if (target == TARGET_STACK)
		ret = __gpufreq_generic_commit_stack(limited_idx, DVFS_FREE);
	else
		ret = __gpufreq_generic_commit_gpu(limited_idx, DVFS_FREE);
```

## A.3 两条完整链路

### 普通 commit（`gpufreq_commit`）

```
策略 → gpufreq_commit(TARGET_DEFAULT, idx)
  └ gpufreq_validate_target(): dual_buck=True → TARGET_STACK
  └ EB: IPI cmd 7 (CMD_COMMIT)
      └ 固件 gpufreq_ipi_handler → limited_commit(STACK, idx)
          └ 按 [ceiling, floor] 钳位        ← 限频器在这里生效
          └ __gpufreq_generic_commit_stack(limited_idx, DVFS_FREE)
              └ 状态门：g_dvfs_state & ~0 = g_dvfs_state
                  ├ 上电中(0x0) → 通过 → 应用
                  └ 下电中(0x2) → 丢弃，返回 0
```

### fix（`gpufreq_fix_target_oppidx`）

```
策略 → gpufreq_fix_target_oppidx(TARGET_DEFAULT, idx)
  └ EB: IPI cmd 14 (CMD_FIX_TARGET_OPPIDX)
      └ 固件 __gpufreq_fix_target_oppidx_stack(idx)
          └ __gpufreq_power_control(POWER_ON)      ← 先强制上电，清掉 0x2
          └ __gpufreq_set_dvfs_state(true, DVFS_DEBUG_KEEP)   ← 状态变 0x4
          └ __gpufreq_generic_commit_stack(idx, DVFS_DEBUG_KEEP)
              └ 状态门：0x4 & ~0x4 = 0 → 通过
          └ __gpufreq_power_control(POWER_OFF)
```

注意 fix **不经过 `limited_commit`**，直接调 `generic_commit_stack`，
所以 ceiling/floor 完全被跳过。

## A.4 由此得到三个结论

### 结论 1：空载时 commit 必然无效，这是设计

GPU 下电 → `g_dvfs_state = 0x2` → 普通 commit 的 key 是 `DVFS_FREE`(0) →
`0x2 & ~0 ≠ 0` → **丢弃并返回 0**。

这不是 xaga 的固件特性，**任何设备在空载下电时都一样**。
而且此刻 GPU 本来就没在跑，`[GPU-REALOPP] FmeterFreq: 0`，改档位也没有意义。

### 结论 2：早前「xaga 固件无视 commit」是测量假象

那份证据是「20 秒空载采样，GPU-OPP 恒为 1（950）」。而同一份 status 里：

```
[GPU-Power]  PowerCount: 0, CG: 0, MTCMOS: 0, BUCK: 0     ← GPU 没上电
[GPU-REALOPP] FmeterFreq: 0, Con1Freq: 0                  ← 实测频率 0
[Common-Status] DVFSState: 0x0002                          ← 就是 POWEROFF
```

`GPU-OPP Index` 是**上一次成功提交留在 shared_status 里的目标值**，不是当前正在跑的频率。
所以「钉死 950」= 最后一次成功提交发生在 GPU 还醒着的时候，值是 950；之后所有降档提交都被门吃掉了。

反证在同一份记录的别处：策略日志里出现过 `cur=4`（430MHz），
commit 在 `4->2 / 4->0 / 4->3 / 4->1` 之间来回，说明**频率确实在动**。

corot 空载显示 350 也是同一机制：最后一次成功提交恰好是 350。

### 结论 3：用 fix 当策略控制面是错的

fix 能"在空载下生效"的原因恰恰是它 `POWER_ON` 强制上电。如果拿它当每 40ms 一次的
调频手段，会带来三个问题：

1. **绕过限频器** —— thermal / 电池 / fpsgo 全部失效，GPU 会被钉死在请求档位；
2. **反复强制上电** —— 每拍 POWER_ON/POWER_OFF，功耗和时序都不对；
3. **`DVFS_DEBUG_KEEP` 永久置位** —— 之后普通 commit 再也无法生效（`0x4 & ~0 ≠ 0`），
   只有显式 `fix(-1)` 能清。

**正确做法：继续用 commit。** 验证时必须在**有负载、`FmeterFreq > 0`** 的条件下采样，
否则测到的全是下电残留值。

## A.5 那 xaga 上到底还剩什么问题

如果 commit 在负载下是好的，剩下的现象就是：

> 降档提交往往**赶在 GPU 下电之后**到达，被门丢掉，于是 shared_status 里保留的是
> 上一次较激进的目标值（950）。下次上电时 GPU 从高档起步，功耗偏高。

这不是"调频坏了"，而是"最后一步没落地"。两种处理：

- **A**：策略在检测到 `util≈0` 时，**赶在 GPU 下电前**把目标提交下去
  （现在的 `down_ticks < 8` + `hold_ticks=5` 加起来约 0.5 秒，可能太慢）；
- **B**：接受它——上电路径本身会用 `SRAMRC` / `GPUFREQ_SAFE_VLOGIC` 重新选档
  （`__gpufreq_init_opp_idx`），不依赖残留值。

需要实机确认是哪一种，判据见 A.6。

## A.6 实机验证判据（必须带负载）

```bash
# 判据 1：GPU 必须是醒的（否则一切读数无意义）
adb shell 'su -c "cat /proc/gpufreqv2/gpufreq_status"' | grep -E "REALOPP|DVFSState|GPU-OPP"
#   要求：FmeterFreq > 0 且 DVFSState 不含 0x0002

# 判据 2：commit 是否真的生效（带负载）
adb shell 'su -c "
  /data/local/tmp/glbench 1920 1080 40000 >/dev/null 2>&1 &
  for i in \$(seq 1 20); do
    grep -E \"GPU-OPP|REALOPP\" /proc/gpufreqv2/gpufreq_status
    sleep 1
  done"'
#   要求：GPU-OPP 的 Index 会变，且 REALOPP.FmeterFreq 跟着变

# 判据 3：dmesg 里只要出现 "unavailable DVFS state"（需要开 GPUFREQ_LOGD）
#   就说明还有提交被门吃掉
```

## A.7 本推导里唯一没有直接证据的一环

「固件把 cmd 7 路由到 `limited_commit`」是从 **AP 侧同源代码**推的：
固件里嵌了同一份 gpufreq v2（`gpufreq_ipi_handler`、`__gpufreq_generic_commit_stack`、
`invalid IPI cmd: %d` 等字符串在两份固件里完全一致）。

要直接证明，需要在固件里定位 `gpufreq_ipi_handler` 的 cmd 分发表。
RV33 payload 是未压缩的 RISC-V 代码，可反汇编，但 0x200 处不是代码起点（前面有段表），
需要先定位代码段基址。这是本次未完成的部分。

**不过 A.6 的判据 2 可以替代它**：如果带负载时 `FmeterFreq` 跟随 `GPU-OPP`，说明
commit 链路是通的，上面这个未经证实的环节也就无关紧要了。
