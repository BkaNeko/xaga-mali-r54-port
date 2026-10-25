# 13T 的 GPUEB 固件移植：为什么走不通

一句话：**13T（aristotle）那份 471 KB 的 gpueb 固件搬到 xaga 上跑不起来，
但原因不是流传的「架构级死路」——那个结论的两条决定性证据，在一台正常工作的设备上读数完全相同。**

本文逐条复核并推翻了该结论，给出真实的三层原因，并记录了实机证据与反汇编过程。
所有结论都附了可复验的 file:line、实测寄存器值或指令片段。

> 原始素材（`spmdump` 只读模块、E0 快照、固件切片、E1 刷写/回滚脚本）留在开发机的
> `new-port/out/mali_r54/glass-debug/`，未入库；本文只收录结论与复验方法。

---

## 0. 一句话

**"gpueb 是架构级死路"这个结论建立在一个错误前提上：EB 模式下 AP 侧根本没有 attach 那 12 个 `pd_mfg` genpd，
所以"pm_runtime resume 成功但 mfg1-12 全 off"是必然结果，不是"框架 trigger 不到"的证据。**
交接文档把它当成了架构证据，于是把一条**驱动层没接通**的问题判成了死路。

---

## 1. 需要修正的 5 处（按重要性）

### 1.1 xaga 原厂就是 gpufreq **v2 + EB**，不存在"把 v2 移植过来"这回事

| 证据 | 位置 |
|---|---|
| 原厂 `mtk_gpufreq_mt6895.ko`（md5 `37ae1347`）内含 `gpufreq platform probe only init reg_base/dfd/pmic/fp in EB mode` | `strings` 实测 |
| 原厂 wrapper 含 `gpufreq_ipi_to_gpueb` / `gpufreq_gpueb_init` / `gpueb_get_reserve_mem_*` | 同上 |
| 设备 DT：`gpufreq_wrapper { gpufreq-version = <0x02>; dual-buck = <0x01>; gpueb-support = <0x01>; }` | `dt_xaga.dts:9491-9497` |
| 13T DT 同构（同样 v2 + EB + 12 个 `pd_mfg`） | `dt_13t.dts:10245-10267` |
| 驱动从 DT 读 `gpueb-support` 决定 EB/AP | `gpufreq_v2.c:1989`、`gpufreq_mt6895.c:4913` |

→ §4.2 把"编好 v2 两模块"记成新增能力、§4.3 说第一次刷机是"13T 固件 + 原厂 **v1** gpufreq"，都不成立：
原厂没有 v1 形态，**变量从来只有固件 blob 一个**。

### 1.2 EB 模式下 12 个 `pd_mfg` genpd 从未 attach —— 这是"mfg 全 off"的真正原因

`gpufreq_mt6895.c` `__gpufreq_pdrv_probe()`：

```
ret = __gpufreq_init_platform_info(pdev);
ret = __gpufreq_init_pmic(pdev);
if (g_gpueb_support) {                     /* ← EB 模式在这里就跳走了 */
        GPUFREQ_LOGI("... only init reg_base/dfd/pmic/fp in EB mode");
        goto register_fp;
}
ret = __gpufreq_init_clk(pdev);            /* ← AP 模式才走 */
ret = __gpufreq_init_mtcmos(pdev);         /* ← 唯一 attach pd_mfg1..12 的地方 */
...
ret = __gpufreq_power_control(POWER_ON);   /* ← 首次上电也只在 AP 模式发生 */
```

而 attach 只发生在 `__gpufreq_init_mtcmos()` 里：

```
g_mtcmos->mfg1_dev = dev_pm_domain_attach_by_name(dev, "pd_mfg1");   /* :4660 */
...
dev_pm_syscore_device(g_mtcmos->mfg1_dev, true);                    /* :4666 */
```

**推论**：EB 模式下 `g_mtcmos == NULL`，`__gpufreq_mtcmos_control()` 那条"能真正上电"的路径被整条跳过。

### 1.2b 更正（第三轮自查）：genpd 其实**挂了**，但挂的是"哑的"那个

上一版这里写"EB 模式下 `13fa0000.gpufreq` 身上一个 genpd 都没挂"，**这是错的**，已更正：

- `drivers/base/platform.c:756` `platform_drv_probe()` 在调用 `drv->probe()` **之前**就调了
  `dev_pm_domain_attach(_dev, true)` → `genpd_dev_pm_attach(dev)`，**自动 attach `power-domains` 的第 0 个**
  （= `pd_mfg1`）。所以 kbase 补丁那句"驱动核心 probe 时已自动 attach"是**对的**。
- 但这个自动挂上去的域是**惰性**的：gpufreq v2 全程**从不** `pm_runtime_enable()` 自己
  （`grep pm_runtime_enable drivers/gpu/mediatek/gpufreq/v2/*.c` → 无命中），
  runtime PM 关闭时设备状态恒为 `RPM_ACTIVE`，`genpd_runtime_resume()` 永远不会跑 → 域保持 off。
- 真正能上电的是 `dev_pm_domain_attach_by_name()` 创建的**虚拟设备**
  （`genpd_dev_pm_attach_by_id()`，`drivers/base/power/domain.c:2746-2787`，结尾明确 `pm_runtime_enable(virt_dev)`），
  它才是 `pm_runtime_get_sync(g_mtcmos->mfg1_dev)` 的作用对象。而它只由 `__gpufreq_init_mtcmos()` 创建，
  **EB 模式跳过该函数 ⇒ 虚拟设备不存在 ⇒ AP 侧没有任何有效手段给 MFG 上电**。

这同时解释了 §4.4 第 4/5 条为什么长那样：pwtest 看到的 `runtime_status=ACTIVE` 是
**runtime PM 关闭时的默认态，不是"已上电"**；`ret=0` 也是空转返回值。

结论方向不变（EB 模式下 AP 侧无法上电），但机制表述已修正：
**不是"没挂"，是"挂了一个 inert 的，真正有效的那个被跳过了"。**
E0 的 `pm_genpd_summary` 会直接显示 `pd_mfg1` 的 attached devices 是不是 `13fa0000.gpufreq`，可当场验证。

→ §4.5「genpd 无法从任何路径 trigger 到它 → 说明还有一层固件协作 → 架构级死路」**不成立**。
目前只能说：**AP 侧的上电职责在 EB 模式下被代码主动跳过了，而固件那一侧没接上**。

### 1.3 §12.2 的 SPM 寄存器偏移是错的（spmtest.c 读的是无效地址）

| 项 | 交接文档 §12.2 | **实际（源码）** | 出处 |
|---|---|---|---|
| SPM_PWR_STATUS | `0x0600` | **`0xF3C`** | `gpufreq_mt6895.h:72` `PWR_STATUS_OFS` |
| SPM_PWR_STATUS_2ND | `0x0604` | **`0xF40`** | `gpufreq_mt6895.h:73` |
| MFG0/1 上电判据 | 未提 | **`MFG_0_1_PWR_MASK = 0x6`** | `gpufreq_mt6895.h:74` |
| MFG1 ctl_offs | `0xEBC` | `0xEBC` ✔ | `mtk-scpsys-mt6895.c:462-465` |
| sta_mask | `GENMASK(31,30)` | ✔ | 同上 |

（`mtk-scpsys.c:62-63` 里的 `0x060c/0x0610` 是**其它平台**的默认值，mt6895 走 `MTK_SCPD_IS_PWR_CON_ON`，
用 `ctl_offs` 自己判，别混用。）

`artifacts/spmtest/spmtest.c` 里 `readl(spm + 0x600)` / `0x604` 读出来的是无关寄存器，那两行输出请忽略。

### 1.4 panic 产物里已经有 SPM 硬数据，而且它证明"AP 写 SPM 能拿到 ACK"

`artifacts/ramoops_1st_flash_v1gpufreq_panic.txt:2805-2808`（同一次启动的 `__gpufreq_dump_infra_status`）：

```
[SPM] (0x1c001eb8-ec4): 0xc000224d 0x00001112 0x00001112 0x00001112
[SPM] (0x1c001ec8-ed4): 0x00001112 0x00001112 0x00001112 0x00001112
[SPM] (0x1c001ed8-ee8): 0x00001112 0x00001112 0x00001112 0x00001112 0x00001112
[SPM] (0x1c001f3c): 0x00000003, (0x1c001f40): 0x00000003
```

解码：
- `0x1C001EB8` = **MFG0** = `0xC000224D` → ACK(bits31:30)=`11`、PWR_ON/ON_2ND 置位、RST_B=1 → **MFG0 真的上电了**
- `0x1C001EBC` = **MFG1** = `0x00001112` → ACK=0、ISO=1、CLK_DIS=1、SRAM_PDN/ACK=1 → 教科书式"关"
- `0xEC0–0xEE8` = MFG2-12 全部 `0x1112`
- `PWR_STATUS/2ND = 0x3/0x3`，而 `MFG_0_1_PWR_MASK = 0x6` → MFG1 未上电（与 scpsys 判据一致）

**价值**：`0xC000224D` 就是 xaga 上"健康 MFG 域"的 PWR_CON 模板；说明 SPM 可写、ACK 会回。
这比 §12 计划的"先写再看"安全得多——先**读**到基线，再决定要不要写。

### 1.5 §4.3「第一次」的引用是跨日志混引，不能当证据

`ramoops_1st_flash_v1gpufreq_panic.txt` 里 grep 计数：

| 字符串 | 计数 |
|---|---|
| `[FHCTL]` | **0** |
| `ipi_register fail` | **0**（只有 `gpueb hwvoter mtk_ipi_register done`） |
| `using 0 entries` | **0** |
| `illegal shader` | **0** |
| `MFGPD` | **0** |
| `dual_buck` | 1（`gpufreq wrapper driver probe done, dual_buck: true, gpueb: off`）|

也就是说 §4.3 第一次那三行 panic 特征**不在这个产物里**；而这次启动 wrapper 明确报 `gpueb: off`，
随后 `__gpufreq_abort: fail to get clk_mux` → `kernel BUG at .../gpufreq_common.h:298`。
**这次启动是 AP 模式失败，与 13T 固件的 EB 行为无关**，不能用来证明"IPI 协议不兼容"。

---

## 2. 重新定位的失败机制（当前最可信的假设）

```
EB 模式（gpueb-support=1，原厂与 13T 皆然）
  ├─ AP 侧：probe 跳过 init_clk / init_mtcmos / 首次 POWER_ON
  │        ⇒ 没人 attach pd_mfg1..12，没人写 0xEBC
  └─ 固件侧：收到 CMD_POWER_CONTROL(POWER_ON) 后，负责整套 MFG 上电（含 MTCMOS）
           ├─ 原厂 128KB 固件：实测能上电（MFG0 已证 SPM 可写；stock 机器 GPU 正常工作）
           └─ 13T 471KB 固件：ACK 返回 0、自报 950MHz@0.7875V，但 0xEBC 仍是 0x1112
                              ⇒ 固件"以为"自己上电了，实际 SPM 没变化
```

三种可能，`spmdump` 一次就能分开：

| 观察 | 含义 | 下一步 |
|---|---|---|
| 0xEBC 仍是 `0x1112`（bit 一个没动） | 固件**根本没写** SPM：写的是别的地址 / 走 PDC 路径 / 被总线挡了 | 对比 13T 与 xaga 的 SPM 布局与 PDC 配置 |
| 0xEBC 出现 PWR_ON 位但 ACK 仍 0 | 固件写了，**硬件拒绝**（权限/安全侧） | 查 SPM 的 MTCMOS 权限寄存器 / secure 配置 |
| 0xEBC 变成 `0xC000224D` 且 GPU_ID 有值 | 固件其实成功了，问题在别处（时序/缓存） | 回到 kbase 侧 |

---

## 3. 已备好的工具：`artifacts/spmdump/`（只读，零风险）

相对旧 `spmtest` 的三个改变：**全程只读**、**卸载干净可反复取快照**、**偏移全部取自本内核源码**。

```bash
KO=/home/bkaneko/new-port/out/mali_r54/glass-debug/artifacts/spmdump/spmdump_vermagic_patched.ko
adb push $KO /data/local/tmp/
adb shell 'su -c "insmod /data/local/tmp/spmdump_vermagic_patched.ko tag=stock; dmesg | grep SPMDUMP"'
adb shell 'su -c "rmmod spmdump"'          # 安全
```

输出样例（每条都带 `ACK/PWR_ON/ON_2ND/RST_B/ISO/CLK_DIS/SRAM_PDN` 解码）：

```
SPMDUMP[stock] MFG0   = 0xc000224d  ACK=11 PWR_ON=1 ON_2ND=1 RST_B=1 ISO=0 CLK_DIS=0 SRAM_PDN=1 SRAM_ACK=1
SPMDUMP[stock] MFG1   = 0x00001112  ACK=00 PWR_ON=0 ON_2ND=0 RST_B=0 ISO=1 CLK_DIS=1 ...
SPMDUMP[stock] PWR_STATUS(0x1c001f3c)=0x00000003 ... MFG0/1 OFF
SPMDUMP[stock] MFG1 判据: ctl=0x00001112, sta_mask(0xC0000000) -> PWR_ACK 未置位(未上电)
SPMDUMP[stock] GPU_ID(0x13000000)=0x00000000 SHADER_PRESENT(0x13000140)=0x00000000
```

`insmod ... read_gpu=0` 可跳过 GPU 寄存器读取（域关闭时更保守）。
产物：`spmdump.ko`（树内 vermagic `5.10.269`）+ `spmdump_vermagic_patched.ko`（md5 `b2f08eee7962`，已改 226，与既有 pwtest/spmtest 一致）。

---

## 4. 实验阶梯（按风险从 0 往上）

| # | 实验 | 风险 | 判据 / 产出 |
|---|---|---|---|
| **E0** | stock 栈（原厂固件）下 GPU 忙时 dump 一次 | **0**（只读） | 拿到 xaga"健康 MFG1 PWR_CON"实测值，作为一切比较的基准 |
| **E1** | 13T 固件 + v2 EB 启动，mali 上电失败后 dump | **0**（只读，只需刷固件，已有回退包） | 区分 §2 三种可能；这是**决定继续还是放弃的唯一硬证据** |
| E2 | 让 EB 模式也 `dev_pm_domain_attach_by_name("pd_mfg1")`，kbase 里显式 `pm_runtime_get_sync(mfg1_dev)`（走内核标准接口，不手写 SPM） | 中（要刷 super，需重编两模块） | MFG1 ACK 置位 + `GPU_ID` 非 0 |
| E3 | 仅在 E2 也失败时：按 MFG0 的 `0xC000224D` 模板手写 `0xEBC` | 高（写电源寄存器） | 最后手段，需先有 super 备份 |

**E0/E1 之前不要写任何 SPM 寄存器**——`spmtest.c` 的"先写后读"顺序应当废弃，改为"先读、能解释再说"。

---

## 5. 风险与回滚（沿用交接文档）

- 已有完整 super 备份：`out/output/super_backups/super_backup_device.img.gz`（`d118b521c890`）
- 刷写必须 `fastboot -S 64M flash super super.img`
- **不要** `rmmod` gpufreq / unbind `13fa0000.gpufreq`（HANDOFF-2 §9.2，实测两次秒崩）
- 13T 固件在 `~/桌面/gpueb_13t.img`（471KB），回退用 `out/output/mali_r54_oneshot/gpueb_stock.img`（`d4e55dec`）

---

## 6. 还没确认、需要拍板的事

1. **收益是什么**：OPP 表两机逐字节一致（350–950MHz），stock 128KB 固件实测已在 950MHz@0.7875V。
   13T 固件多出来的只有 GPM3.0 / DDS / fhctl hopping / 温度补偿。若目标是"更高频"，这条路**注定没有收益**；
   若目标是续航/温控曲线，才值得继续投入。
2. **设备当前不在线**（`adb devices` 为空），E0/E1 都跑不了。
3. E2 需要重编 `mtk_gpufreq_mt6895.ko` + `mali_kbase_mt6895.ko` 并刷 super，是否授权。

---

## 7. 复验命令（本文每条结论）

```bash
GD=/home/bkaneko/new-port/out/mali_r54/glass-debug/artifacts
STOCK=/home/bkaneko/new-port/out/mali_r54/dna/vendor_dlkm/lib/modules
SRC=/home/bkaneko/work/xaga_kernel_5.10

# 1.1 原厂就是 v2/EB
strings -a $STOCK/mtk_gpufreq_mt6895.ko | grep "only init reg_base/dfd/pmic/fp in EB mode"
strings -a $STOCK/mtk_gpufreq_wrapper.ko | grep -c gpufreq_ipi_to_gpueb
grep -A6 "gpufreq_wrapper {" $GD/dt_xaga.dts

# 1.2 EB 分支跳过 init_mtcmos
grep -n "skip most of probe in EB mode" -A3 $SRC/drivers/gpu/mediatek/gpufreq/v2/gpufreq_mt6895.c
grep -n "dev_pm_domain_attach_by_name" $SRC/drivers/gpu/mediatek/gpufreq/v2/gpufreq_mt6895.c

# 1.3 偏移
grep -n "PWR_STATUS_OFS\|PWR_STATUS_2ND_OFS\|MFG_0_1_PWR_MASK" $SRC/drivers/gpu/mediatek/gpufreq/v2/include/gpufreq_mt6895.h

# 1.4 SPM 硬数据
sed -n '2805,2808p' $GD/ramoops_1st_flash_v1gpufreq_panic.txt

# 1.5 混引检查（应全为 0）
for p in FHCTL "ipi_register fail" "using 0 entries" "illegal shader"; do
  printf "%-22s %s\n" "$p" "$(grep -c "$p" $GD/ramoops_1st_flash_v1gpufreq_panic.txt)"
done
```

---

## 8. 静态补强：两机 gpueb 的 DT 契约本身就不一致（无需设备）

| 项 | xaga `dt_xaga.dts:4874-4893` | 13T `dt_13t.dts:5005-5025` |
|---|---|---|
| `MEM_ID_GPUFREQ` 预留块大小 | **0x1000** | **0x4000** |
| `mfg0_pwr_con` reg | **缺** | `0x1c001eb8` |
| `gpueb_gpr_base` / `gpueb_cfgreg_base` / `gpueb_intc_base` / `gpueb_dma_base` | **缺** | 全有 |
| `mbox0_init` | 缺 | 有 |
| `reg-names` 条目数 | 6 | 12 |

xaga 驱动自己确认这些是死路径（`ramoops_1st_flash_v1gpufreq_panic.txt:2775-2777`）：

```
[GPU/EB][INFO][__gpueb_pdrv_probe:202]: no gpueb_gpr_base (debug only, skipped)
[GPU/EB][INFO][__gpueb_pdrv_probe:210]: no gpueb_cfgreg_base (debug only, skipped)
[GPU/EB][INFO][__gpueb_pdrv_probe:218]: no mfg0_pwr_con (debug only, skipped)
```

意义：13T 的 gpueb 栈把 `0x1C001EB8`（正是实测读出 `0xC000224D` 的那个 MFG0_PWR_CON）
当成一等公民映射进来，xaga 连映射都没有；共享内存预留也只有 1/4。
**"固件 ACK 了但 SPM 一点没动"完全符合"固件按 13T 的契约访问一块 xaga 上并不存在的窗口"这一形态。**
这不是"架构级死路"，是**契约不匹配**——可以通过给 xaga 补上这些 reg/mem 来试（=E2 的一部分）。

## 9. E0 只读基线（脚本已就绪：`e0_dump.sh`）

```bash
cd /home/bkaneko/new-port/out/mali_r54/glass-debug && ./e0_dump.sh
```

做的事：`pm_genpd_summary` + gpufreq 设备 runtime 状态 + 三张 SPMDUMP 快照（空闲 / GPU 忙 / 忙后 3s），
全程只读，不刷任何分区。

**E0 能证明什么**：
- ✅ 「EB 模式下 12 个 `pd_mfg` 没有任何设备 attach」→ 直接推翻 §4.4/§4.5 的推论
- ✅ 「stock 固件是否自己写 SPM 给 MFG1 上电」→ 定位 13T 固件失败发生在固件侧
- ✅ 健康态 MFG1 PWR_CON 的实测值（后续一切比较的基准）

**E0 不能证明什么**：
- ❌ 「13T 固件被硬件拒绝」—— 这必须让 13T 固件真的跑起来才能观察到，即 E1（刷 gpueb 分区，
  可回退到 `out/output/mali_r54_oneshot/gpueb_stock.img`）。
  E0 只把"该看什么、什么算正常"钉死，不产生 13T 的结论。

---

## 10. 固件侧静态分析（RISC-V 反汇编级）——推翻「固件负责给 MFG 上电」

固件头部：`tinysys-gpueb-RV33_A` —— **RISC-V RV32，不是 ARM**。所以常量不走字面池，
必须按 RISC-V `lui/auipc` 指令扫（且必须 **2 字节对齐**，RVC 压缩指令会错位）。

### 10.1 三份固件对 SPM 的引用：只有一处，而且是「读」

| 固件 | 大小 | `lui 0x1C001xxx`（SPM 基址） | `lui 0x13FA0xxx`（MFG_PLL） | 字符串数 |
|---|---|---|---|---|
| `gpueb_stock.img` | 0x20000 | **1 处 @0xb8a2** | 2 处 | 498 |
| `gpueb.img`（950MHz 版） | 0x20000 | **1 处 @0xb8a2** | 2 处 | 498 |
| `gpueb_13t.img` | 0x73000 | **1 处 @0xbaf0** | 2 处 | 1077 |

反汇编上下文（stock @0xb8a2，13T @0xbaf0 同构）：

```
lui  x10, 0x1c001      ← SPM 基址
c.jal <读寄存器函数>    ← 调用读函数
sw   x10, 2000(x11)    ← 结果压栈，作为 printf 变参
```

三份固件都有同一行调试格式串：`[E]%s: (0x13F91070): 0x%x, (0x1C001EBC): 0x%x`。
**结论：任何一份 gpueb 固件都不写 SPM MTCMOS**，那唯一一处 SPM 引用只是把 `0x1C001EBC`
读出来打日志。固件真正碰的是 `MFG_PLL`（PLL/DVFS），这符合"固件只管 DVFS"。

→ **交接文档 §4.1/§4.5「固件多出整套 GPM 3.0 电源管理 → MFG 上电由固件完成」不成立。**
→ 也推翻我在 §2 里写的「原厂 128KB 固件能上电、13T 不能」——两份都不写 SPM，这个差异不存在。

### 10.2 三份固件都靠信号量跟别人协调，不是自己写

三份都有：`[F]%s: acquire SPM_SEMA_M4 timeout`（13T 另有 `fail to release SPM_SEMA_M4`）。
即固件通过 **SPM 信号量**请求/等待别人动 SPM，自己不动手。

### 10.3 13T 固件的额外能力（确认属实）

13T 独有字符串：`__gpufreq_fhctl_hopping`、`[F]%s: invalid DDS: 0x%x`、
`fail to release SPM_SEMA_M4`、`[GPUEB] Scheduler start!` 等；字符串总量 498 → 1077。
"多出 GPM3.0/DDS/hopping"这个描述本身是对的。

### 10.4 于是「谁给 MFG1 上电」这个问题变成这样

| 候选 | 结论 | 证据 |
|---|---|---|
| gpueb 固件（任意版本） | ❌ 不写 SPM | §10.1 反汇编 |
| AP 侧 genpd（EB 模式） | ❌ 被 `goto register_fp` 跳过，genpd 未 attach | §1.2 源码 |
| AP 侧 genpd（AP 模式） | ❌ `__gpufreq_init_clk()` 需要 `clk_mux`，xaga DT 的 gpufreq 节点**连 clocks 都没有** | `dt_xaga.dts:9475-9489` 无 `clocks` 属性 |
| `mtk_common_pm_mfg_active()` | ❌ 只是置 `mfg_powered = true` 标志 | `mtk_platform_common.c:54-59` |

**四条路全断**。最可能的事实是：**MFG1（0xEBC）在 xaga 正常工作态下本来就是 `0x1112`（关）**，
GPU 靠 MFG0（0xEB8，开机即 `0xC000224D`）+ PDCv2 控制的 MFG2-12 工作
（`GPUFREQ_PDCv2_ENABLE=1`，`__gpufreq_pdc_control` 注释明写"let GPU DDK control MTCMOS itself"）。

若这条成立，则交接文档把「mfg1-12 全 off」当成故障**本身就是误判**，
§4.4 那五条"决定性证据"里至少三条是无效观测。

**这条必须由 E0 定夺**：stock 正常工作、GPU 正在渲染时，`0xEBC` 到底是 `0x1112` 还是 `0xC000224D`。
- 是 `0x1112` → 上面成立，整条 gpueb 死路论证作废，问题回到 kbase/PDC 侧
- 是 `0xC000224D` → 确实有个我们还没找到的写入者，E0 的 `pm_genpd_summary` 会把它暴露出来

---

## 11. 收益核算：13T 固件的 OPP 表与 xaga 原厂**逐字节相同**（这条不依赖设备）

> 这张表的来龙去脉见 [`950MHZ.zh-CN.md`](950MHZ.zh-CN.md)；本节只补充「两机固件这一区间零字节差异」这个对比结论。

固件里的 OPP 表是 24 字节/条（freq, vgpu_uV, vsram_uV, flag, 0, 0）。三份固件实测：

| # | xaga stock (`0x15d60`) | 现役「950 版」(`0x15d60`) | 13T (`0x171e0`) |
|---|---|---|---|
| 0 | 950000 / 78750 / 78750 | 950000 / 78750 / 78750 | 950000 / 78750 / 78750 |
| 1 | **880000** / 78750 / 75000 | **950000 / 78750 / 78750** ← 改动 | **880000** / 78750 / 75000 |
| 2 | 800000 / 72500 / 75000 | 同左 | 同左 |
| 3 | 610000 / 65000 / 75000 | 同左 | 同左 |
| 4 | 430000 / 60000 / 75000 | 同左 | 同左 |
| 5 | 350000 / 57500 / 75000 | 同左 | 同左 |

- **stock 区段 vs 13T 区段：差异字节数 = 0**（`stock == 13T` 为 True）
- 「950 版」= stock 的 **5 字节**补丁：`0x15d78` 的 `880000→950000`（3 字节）+
  `0x15d80` 的 `vsram 75000→78750`（2 字节）

**结论（不需要设备即可定论）**：
1. 13T 固件在频率上**没有任何额外收益**——它的 OPP 表就是 xaga 原厂那张。
2. 现役「950 版」反而**比 13T 固件更高**（880 那档被提到了 950）。
3. 所以"移植 13T gpueb 固件"这件事，**最好情况下的收益也是 0**；
   唯一理论收益是 GPM3.0/DDS/fhctl hopping 的能效特性，而按 §8/§10，
   这些特性依赖 SPM 信号量与 PDC 契约——正是 xaga 缺的那部分。

→ **建议：不继续 13T 固件移植。** 现役 5 字节补丁版已是这张 OPP 表的上限形态。
   若还要在 GPU 上投入，应转向能效观测（`/sys/kernel/debug/gpufreq` 的 GPM/负载统计），
   而不是换固件。

---

## 12. 当前结论汇总

| 交接文档的观点 | 状态 | 依据 |
|---|---|---|
| §4.5 gpueb 移植是「架构级死路」 | **推翻**（理由错） | §1.2/§1.2b EB 分支跳过 init_mtcmos，有效上电路径不存在 |
| §4.1/§4.5 固件负责 MFG 上电（含 MTCMOS） | **推翻** | §10.1 三份固件都不写 SPM |
| §4.2 移植了 gpufreq v2 | **推翻** | 原厂模块与自编模块决定性字符串一致 |
| §4.3 第一次刷机的 panic 是 IPI 协议不兼容 | **推翻** | §1.5 那三行特征不在产物里，当次是 AP 模式 abort |
| §12.2 SPM 寄存器偏移 | **纠正** | §1.3 PWR_STATUS=0xF3C/0xF40 |
| 「OPP 表两机一致」 | **证实** | §11 零字节差异 |
| 13T 固件有收益 | **推翻** | §11 收益为 0，且现役补丁版更高 |
| 13T 固件能否在 xaga 跑通 | **仍未定论** | 需 E0/E1（设备只读诊断） |

**唯一还开着的技术问题**：stock 正常渲染时 `0xEBC` 是 `0x1112` 还是 `0xC000224D`。
它只影响"13T 固件失败的确切机理"，**不影响上面的收益结论**。

---

## 13. 第三轮补充验证（含一次自我更正）

### 13.1 `dt_xaga.dts` 就是原厂 DT，不是被改过的产物

从原厂 `vendor_boot.img`（`/home/bkaneko/rom/20-xaga-原厂镜像/xaga_stock/vendor_boot.img`）
按 DTB magic `d00dfeed` 抽出 DT（`@0x28c5040`，327308 B），dtc 反编译后逐节点比对：

| 节点 | 行号（stock / `dt_xaga.dts`） | 内容 |
|---|---|---|
| `mali@13000000` | 9125 / 9125 | 一致，**无 `power-domains`** |
| `gpufreq` | 9475 / 9475 | 一致，12 个 `pd_mfg` |
| `gpufreq_wrapper` | 9491 / 9491 | 一致，`gpufreq-version=2 / gpueb-support=1` |

→ 交接文档的 DT 素材可信；"xaga 原厂就是 v2+EB"这个结论站得住。

### 13.2 原厂 kbase 的电源手段（符号级）

`mali_kbase_mt6895.ko` 的导出符号里：`gpufreq_power_control`、`gpufreq_power_ctrl_enable`、
`gpufreq_get_shader_present`、`kbase_pm_runtime_init`，以及自己的 `pm_runtime_enable`；
**没有** `dev_pm_domain_attach_by_name`、**没有**任何 `genpd_*` 调用。

→ 原厂 kbase 给 MFG 上电的唯一手段就是 `gpufreq_power_control()`；
EB 模式下它走 IPI 到固件，而固件不写 SPM（§10.1）。
所以**原厂态下 AP 与固件都不写 `0xEBC`** —— 这让"`0xEBC = 0x1112` 在 stock 下就是常态"
成为目前最可能的解释（待 E0 证实）。

### 13.3 如果这条成立，交接文档的错误链是

1. 把「EB 模式下 AP 被设计成不管 MTCMOS」误读成「genpd 框架坏了」
2. 把「固件 ACK 但 0xEBC 不变」误读成「固件不兼容」
3. 于是给 kbase 打了个 `pm_runtime_resume_and_get(gpufreq_dev)` 补丁——而那个设备
   **runtime PM 从未 enable**，`ACTIVE` 是默认态，补丁本质是空转
4. 三次刷机全部失败 → 得出"架构级死路"

真实情况更可能是：**换固件本身没错，错在"以为 EB 模式下 GPU 上电还缺一层"**，
而真正缺的（时钟/PLL 或 r54 kbase 的 PDC 配置）一直没被检查。

### 13.4 静态分析到此为止

继续往下必须看实机：`0xEBC` 在 stock 正常渲染时到底是 `0x1112` 还是 `0xC000224D`。
这一条决定 §13.3 的两种叙事哪个为真。E0 脚本（`e0_dump.sh`）+ 升级版 `spmdump`
（含 `0x13F91070` 时钟判据）已就绪，只等设备接上。

---

## 14. E0 实机结果（2026-10-25，stock 栈，设备 XG4HDUI79DAYAQBA）

原始快照：`e0-e0a-174218.txt`。当前栈已确认是**原厂**：
`mtk_gpufreq_mt6895.ko` md5 `37ae1347`、`mali_kbase_mt6895.ko` md5 `d374e6df`；
gpueb 分区前 128KB md5 `e1160aa0`（= 5 字节 OPP 补丁版）。

### 14.1 GPU 明确在工作

```
dumpsys SurfaceFlinger → GLES: ARM, Mali-G610 MC6, OpenGL ES 3.2 v1.r54p1-...
dumpsys gfxinfo com.android.systemui → Pipeline=Skia (Vulkan)
/proc/mtk_mali/gpu_utilization → ACTIVE=9 ... MFG_PWR=1   （交互瞬间抓到的样本）
```

### 14.2 健康设备上 MFG1–12 就是「全关」

三次快照（空闲 / UI 忙 / 忙后 3s）**完全一致**：

```
MFG0  = 0xc000224d  ACK=11 PWR_ON=1 ON_2ND=1 RST_B=1 ISO=0 CLK_DIS=0
MFG1  = 0x00001112  ACK=00 PWR_ON=0 ON_2ND=0 RST_B=0 ISO=1 CLK_DIS=1 SRAM_PDN=1
MFG2..MFG12 = 0x00001112（同 MFG1）
PWR_STATUS(0x1c001f3c)=0x00000003  PWR_STATUS_2ND(0x1c001f40)=0x00000003
```

→ **`mfg1-12 全 off` 是健康设备的常态，不是故障特征。**
交接文档 §4.4 第 5 条与 §4.5 的核心论据（"genpd 全 off ⇒ 架构级死路"）**在实机上被证伪**。

### 14.3 pwtest 那套读法在健康设备上也读出 0

扩展采样窗口（`samples=1500 interval_us=4000`，连续滑动 UI 6 秒）：

```
SPMDUMP[win2] WINDOW: 1500 次采样(间隔 4000us)
              MFG1_ACK置位=0  GPU非零=0  max GPU_ID=0x00000000 max SHADER=0x00000000 max MFG1=0x00001112
```

同期 kbase 自己报 `MFG_PWR=1`、GPU 正在合成画面，但：
- `MFG1` 一次都没 ACK 过；
- `0x13000000` / `0x13000140` **一次都没读到非零**。

→ 用 `ioremap + readl` 直接读 GPU 寄存器，**在这台健康设备上恒为 0**
（XPU/DevAPC 拦截，或该 block 需特定 clock；kbase 自己能读出 `Mali-G610 MC6` 说明它走的是另一条合法路径）。
**结论：pwtest 的「GPU 寄存器全 0」「SHADER_PRESENT 峰值 0」不能区分故障与健康，是无效证据。**
交接文档 §4.4 第 3 条、§4.3 的 "AP 侧 GPU 寄存器始终读 0" 一并作废。

### 14.4 机制侧的两条实机确认

| 观测 | 值 | 含义 |
|---|---|---|
| `13fa0000.gpufreq/power/runtime_status` | **`unsupported`** | runtime PM 从未 `pm_runtime_enable` ⇒ 自动 attach 的 `pd_mfg1` 是惰性的（**证实 §1.2b**） |
| `13fa0000.gpufreq/power/control` | `auto` | 同上 |
| `/sys/kernel/debug/pm_genpd/` | **不存在** | 内核没开 `CONFIG_PM_GENERIC_DOMAINS` 的 debugfs，genpd 摘要这条路走不通（改用 `/proc/mtk_mali/gpu_utilization` 作替代判据） |

### 14.5 顺带排除「固件放不下」这个可能

```
gpueb_a = /dev/block/sdc35   size = 1048576 (1 MB)
gpueb_b = /dev/block/sdc64   size = 1048576 (1 MB)
13T 固件 471040 B  → 放得下，不存在截断
```

### 14.6 E0 之后，交接文档还剩什么

| 交接文档的"决定性证据" | 实机复核结果 |
|---|---|
| §4.4-1 固件共享内存自报 950MHz@0.7875V | 该值等于 OPP 表第 0 条，只说明固件读到了表，**不说明供上电** |
| §4.4-2 `gpufreq_power_control(ON)` ret=0 | 固件 ACK 而已，**不说明硬件动作** |
| §4.4-3 SHADER_PRESENT 峰值 0 | **健康设备同样为 0 → 无效** |
| §4.4-4/5 genpd mfg1-12 全 off | **健康设备同样全 off → 无效** |
| §4.5「架构级死路」 | **论据全部失效** |

**剩下的真实差异只有一个**：13T 固件那次启动里 kbase 自己读 GPU_ID 得到 0 并 probe 失败
（`illegal shader present HW: 0x0, SW: 0x130013` → `Unknown GPU Product ID 0, -22`），
而原厂固件下 kbase 能读出 `Mali-G610 MC6`。**这才是唯一站得住的故障现象**，
最可能指向「13T 固件没把 MFG 时钟/PLL 配起来」（`0x13F91070` 是固件自己的判据寄存器，
原厂基线值 = `0x00003312`）。要验证它必须跑 E1（刷 13T 固件）。

但按 §11，**13T 固件零收益**，所以 E1 只对"机理"有价值，对"要不要做"没有影响。

### 14.7 E1 的回滚已备好（只读备份，已做）

```
artifacts/gpueb_a_current_1MB.img   md5 bd0476b48c5b   1048576 B
  前 128KB == out/output/mali_r54_oneshot/gpueb.img（950 版）→ 完全一致
```

回滚命令（若跑了 E1）：
```bash
adb push artifacts/gpueb_a_current_1MB.img /data/local/tmp/
adb shell 'su -c "dd if=/data/local/tmp/gpueb_a_current_1MB.img of=/dev/block/by-name/gpueb_a bs=4096"'
```
（gpueb_a/b 各 1 MB，13T 固件 471040 B 放得下，不存在截断）

---

## 15. 静态分析（第四轮）：「471KB 固件」的真面目 + xfile 缺失假设

### 15.1 13T 那个 471KB 不是一份固件，是三段东西拼起来的

按 `88168858` 魔数扫 + 段熵分析：

| 段 | 区间 | 大小 | 熵(bit/B) | 内容 |
|---|---|---|---|---|
| 主镜像 | `0x00000-0x1fd40` | **127.3 KB** | 6.99 | `tinysys-gpueb-RV33_A`，真正的固件代码/数据 |
| cert1 | `0x1fd40` | 0x6c5 | 6.14 | X.509（issuer=Mediatek） |
| cert2 | `0x20610` | 0x3d6 | 6.14 | X.509 |
| **xfile** | `0x20bf0-0x715b0` | **322.4 KB** | **7.999** | `tinysys-gpueb-RV33_A_xfile`，加密/压缩数据表 |
| cert1' | `0x715b0` | 0x6c5 | — | 与 cert1 完全相同 |
| cert2' | `0x71e80` | 0x3d6 | — | 与 cert2 不同 |

xfile 子头（`0x20bf0`）：

```
88168858 b3070500 "tinysys-gpueb-RV33_A_xfile"
89168958 00020000 01000000 ...        ← 另一套魔数 89168958 + 长度字段
```

**所以交接文档 §4.1「13T 固件 471KB，多出整套 GPM 3.0、DDS、fhctl hopping、温度补偿」
这个归因是错的**：

- 真正的固件代码只从 **126764 B → 129848 B，多 3084 字节（3 KB）**；
- 另外 322 KB 是**加密的数据表**（xfile），不是代码。

### 15.2 xaga 侧完全没有 xfile 这个概念

| 检查 | 结果 |
|---|---|
| stock 固件里 `xfile` 出现次数 | **0** |
| 13T 固件里 | 1（就是那个子镜像名） |
| 内核 `gpueb612/` 驱动里 `xfile` | **0 处**（全树只有 apusys 有同名变量，无关） |
| xaga gpueb DT `gpueb_mem_table` | 只有 `MEM_ID_GPUFREQ` / `MEM_ID_LOG` 两项 |

也就是说：xaga 从出厂到现在，**bootloader 从来不需要加载 `_xfile` 子镜像**。
13T 的 gpueb 分区里之所以有它，是因为 13T 的 LK 会解析并加载。

### 15.3 这解释了「固件 ACK 但什么都不做」

- 主镜像（127 KB）能正常加载 → IPI 注册成功、`CMD_POWER_CONTROL` 回 ACK、
  OPP 表（在主镜像 `0x171e0`）可读 → 于是共享内存里自报 950MHz/787500µV **完全成立**；
- 但 DDS/hopping/GPM 要用的系数表在 **xfile 里，而 xaga 不会加载它** →
  PLL/DDS 配置阶段拿不到数据 → MFG 时钟没配起来 → kbase 读 GPU_ID 得 0 → probe -22。

这比"架构级死路"具体得多，也解释了为什么三次刷机都卡在同一个位置。

### 15.4 顺带排除 fhctl 契约差异

| 项 | xaga | 13T |
|---|---|---|
| `fhctl` 节点 `compatible` | `mediatek,mt6895-fhctl` | 同 |
| `reg` 列表（含 `0x13fa0000/0x13fa0800/0x13fa0c00`） | 完全相同 | 同 |
| `fhctl-gpueb` 映射数 | 3（gpu0/gpu2/gpu3） | 3 |
| `fh-id` / `pll-id` | `0` / `0x3e7` | 同 |

→ fhctl 这条排除，问题不在 fhctl 契约。

### 15.5 于是「可行路径」的技术答案是

**有，但没意义**：技术上要让 13T 固件跑起来，需要
① 让 bootloader 或内核把 `_xfile` 子镜像加载到 13T 固件期望的 DRAM 地址，
② 并把 `MEM_ID_GPUFREQ` 预留从 `0x1000` 提到 `0x4000`（§8）。
第 ① 步要改 LK（没有源码），第 ② 步要改 DT。
**而按 §11，13T 固件的 OPP 表与原厂零字节差异，做完这些收益仍然是 0。**

→ 结论不变，但理由更硬：**不是"架构不允许"，是"缺一份数据表 + 缺一个 DT 参数，且补上也没有收益"。**

### 15.6 再补一条：13T 把 intc/dma 并进 gpueb 节点，xaga 是拆开的

按 13T 的 `reg-names` 逐项对齐地址：

| reg-name | 13T 地址 | xaga 对应 |
|---|---|---|
| `gpueb_base` | `0x13c40000` +0x22000 | 同 |
| `gpueb_gpr_base` | `0x13c5fd1c` +0x64 | **无** |
| `gpueb_cfgreg_base` | `0x13c60600` +0x200 | **无** |
| `gpueb_intc_base` | `0x13c60300` +0x100 | xaga 单独一个 `g3d_gpueb@13c60300` 节点 |
| `gpueb_dma_base` | `0x13c60400` +0x200 | xaga 单独一个 `g3d_gpueb@13c60400` 节点 |
| `mbox0_*` | `0x13c5fd80`/`0x13c6200x` | 同 |
| `mfg0_pwr_con` | `0x1c001eb8` | **无** |

**`gpueb_dma_base` 是关键**：13T 的 gpueb 有一套 DMA 引擎映射，322 KB 的 `_xfile`
正是靠它搬进 MCU 视野的。xaga 不但没有这个映射，那两块地址还被独立的 `g3d_gpueb` 节点占着。

→ 与 §15.2/§15.3 一致：**13T 固件要的数据通路（xfile + DMA + intc）在 xaga 上整条不存在。**

---

## 16. 找到第一个硬根因：13T 固件新增了「共享内存尺寸校验」，xaga 给少了

### 16.1 证据：这条错误串是 13T 独有的

```
@0x14b5e: "FreeRTOS/Source/list.c\0"
          "[E]%s: shared memory size: 0x%x < status data size: 0x%x\n\0"
          "__gpuppm_convert_li…"
```

| 字符串 | stock 固件 | 13T 固件 |
|---|---|---|
| `shared memory size: 0x%x < status data size: 0x%x` | **0** | **1** |
| `invalid shared memory base: 0x%llx, size: 0x%x` | **0** | **1** |
| `fail to init GPUFREQ` | 1 | 1 |
| `FreeRTOS_v10.1.0` | 10 | 12 |

即：**13T 固件在 `gpufreq` 初始化时会校验共享内存尺寸，尺寸不够就报错并 `fail to init GPUFREQ`。**
原厂固件没有这个校验，所以 xaga 一直用 `0x1000` 也没事。

### 16.2 尺寸对不上，而且差 4 倍

| 项 | xaga | 13T |
|---|---|---|
| DT `gpueb_mem_table` 里 `MEM_ID_GPUFREQ` | **0x1000** | **0x4000** |
| 传给固件的 status 区 | `GPUFREQ_STATUS_MEM_SZ = 0x400` | 同 |
| 传给固件的 debug 区 | `0x1000 - 0x400 = 0xC00` | `0x4000 - 0x400 = 0x3C00` |

→ xaga 给 4 KB，13T 固件按自己的表期望 16 KB。
（固件里 `status data size` 的**确切数值**无法从无符号镜像里直接读出，0x4000 是从 13T 自己的 DT 值反推的；
但「xaga 给得比 13T 少」这一点由 DT 直接证明。）
**它会在初始化阶段直接判失败，然后 `gpufreq` 模块就不工作了** ——
但 IPI 通道、OPP 表读取这些不依赖该模块的部分照常，
所以外部看到的现象正是：**IPI 通、`CMD_POWER_CONTROL` 回 ACK、共享内存里自报 950MHz@787500µV，
但 PLL/SPM 一点没动。**

### 16.3 为什么之前没人发现这条

`ramoops_1st_flash_v1gpufreq_panic.txt:2781`：

```
[GPU/EB][INFO][__gpueb_pdrv_probe:245]: gpueb no logger support.
```

xaga 没有 gpueb logger，**固件打出来的这条错误串根本传不到 AP 侧**，
所以三次刷机都只看到"ACK 但不动"，看不到固件自己说的原因。

### 16.4 对应的补丁（一行 DT）

`arch/arm64/boot/dts/vendor/mediatek/` 里 gpueb 节点：

```diff
  gpueb@13c00000 {
          compatible = "mediatek,gpueb";
          ...
-         gpueb_mem_table = <0x00 0x1000 0x01 0x180000>;
+         gpueb_mem_table = <0x00 0x4000 0x01 0x180000>;
          gpueb_mem_name_table = "MEM_ID_GPUFREQ\0MEM_ID_LOG";
  };
```

（属性名保持 xaga 的下划线写法；`0x180000` 的 `MEM_ID_LOG` 不动。
改完要重打 vendor_boot 或对应的 dtbo，并确认 `gpueb_get_reserve_mem_size_by_name("MEM_ID_GPUFREQ")` 返回 0x4000。）

### 16.5 诚实的边界

- 这条能解释「ACK 但不动」的**初始化失败**，但**不能证明**它是唯一原因：
  §15 的 `_xfile`（322 KB 加密数据表，xaga 无加载通路）可能仍然是第二道坎。
- 即使两道都过，按 §11 收益仍是 **0**（OPP 表零字节差异）。
- 因此这只是"把机理钉死"的进展，**不改变"不建议做"的结论**。

### 16.6 静态分析现在的完整因果链

```
13T 固件启动
  ├─ 主镜像 127 KB 能加载（xaga 分区 1 MB，够）
  ├─ IPI 注册成功、OPP 表可读（表在主镜像里）→ 共享内存自报 950MHz/787500µV ✔
  ├─ 共享内存尺寸校验：xaga 给 0x1000 < 13T 期望 0x4000
  │    → "[E] shared memory size: 0x1000 < status data size: 0x4000"
  │    → "fail to init GPUFREQ"  ← gpufreq 模块未初始化
  ├─ （可能）_xfile 322 KB 从未被加载 → DDS/hopping 系数缺失
  └─ 结果：CMD_POWER_CONTROL 回 ACK，但 MFG_PLL/SPM 没有任何动作
        → kbase 读 GPU_ID 得 0 → "illegal shader present HW: 0x0" → probe -22
```

而交接文档把这整条链记成了"架构级死路"，并把 `mfg1-12 全 off`（实为健康常态，§14.2）
当成了核心证据。

### 14.8 关键相关性实验（第五轮，补做）：`MFG_PWR=1` 的那一刻，MFG1 仍是关的

用 `/proc/mtk_mali/gpu_utilization`（kbase 自己的账本）做触发条件，命中即 insmod 取 SPM 快照：

```
HIT  ACTIVE=0 3D/TA/COMPUTE=0/0/0 OPP_IDX=0 MFG_PWR=1     ← kbase 报"MFG 已上电"
[5864.507613] MFG1 ctl(0x1c001ebc)=0x00001112 -> PWR_ACK 未置位(未上电)
[5864.507617] GPU_ID(0x13000000)=0x00000000  SHADER_PRESENT(0x13000140)=0x00000000
[5864.507639] MFG_RPC_MON(0x13f91070)=0x00003312
```

（900 次扫描命中 1 次，`MFG_PWR=1` 本身很短暂。）

**这条比 §14.3 更硬**：不是"采样期间没抓到"，而是**在 kbase 明确认为已上电的那一帧，
SPM 的 MFG1 依旧 `0x1112`、GPU 寄存器依旧读 0**。三重结论：

1. `MFG1 PWR_ACK=0` 与"GPU 完全正常"可以共存 ⇒ 交接文档 §4.4-5/§4.5 的核心证据**无效**；
2. `ioremap+readl` 读 `0x13000000` 在 GPU 已上电时**仍返回 0** ⇒ pwtest 的读法**无鉴别力**（§4.4-3 无效）；
3. `MFG_RPC_MON` 在 idle 与 powered 两种状态下都是 `0x3312` ⇒ 它不是电源指示器，
   固件把它打出来只是**状态转储**，不能当"已上电"的判据。

---

## 17. 处理那份加密数据表（xfile）：格式解开了，但两条路都堵死

### 17.1 容器格式（完整解出）

绝对偏移以 `gpueb_13t.img` 为准：

```
0x020bf0  ┌─ xfile 子镜像头 ────────────────────────────────
          │ 0x88168858  magic
          │ 0x000507b3  子镜像长度 = 329651
          │ "tinysys-gpueb-RV33_A_xfile"
          │ 00…00 / ff ff ff ff 填充
0x020c20  ├─ 二级头 ─────────────────────────────────────
          │ 0x89168958  magic（与主镜像 0x88168858 不同）
          │ 0x00000200  = 512
          │ 0x00000001
          │ 0x00000010  = 16   ← 与 AES 块长一致
0x020c40  │ 0x00 填充
0x020c50  │ 0xFF 填充
0x020df0  ├─ 载荷头（24 B）──────────────────────────────
          │ 0x58901690  magic（第三个）
          │ 0x00000018  头长 = 24
          │ 0x00050770  正文长度 = 329584 = 20599 × 16（整数块）
          │ 0x00000004
          │ 0x00000063  = 99
          │ 0x00000000
0x020e08  ├─ 密文正文（329584 B，熵 7.95）
0x071578  ├─ 尾标（56 B）────────────────────────────────
          │ "END" + f962841ec57521647d39bae84df973ccaedb1c49
          │  ↑ 40 位十六进制 = SHA-1（明文串，熵 3.89）
0x0715b0  └─ xfile 结束，接 cert1 / cert2（各 0x8c5 / 0x5d6 字节：0x200 头 + DER）
```

### 17.2 密码学侧：解不开，而且不是"难"是"没有入手点"

| 检测 | 结果 | 含义 |
|---|---|---|
| 16 B 块唯一性 | 20604 块**全唯一**（重复率 0.00%） | 不是 ECB，没有块级结构泄漏 |
| 重合指数 IC（周期 16/32/64/128/256/512/1024/2048） | 全部 **0.0039**（= 纯随机） | 没有重复密钥/短周期流密码 |
| 常见解压（zlib / deflate-raw / gzip / lzma / bz2，多起点） | **全部失败** | 不是未加密的压缩流 |
| 明文特征（连续零块、GPM/DDS、950000 常量） | 全 0 | 无明文泄漏 |
| 全零 16 B 块 | 0 个 | 与 CBC/CTR 一致 |
| 尾部 SHA-1 | 与 `sha1(body)`、`sha1(payload[:END])` 均不匹配 | 哈希算在**明文**上，密文侧无法验证 |

→ AES 级别的加密，密钥在 MCU/安全启动链里，**静态分析没有入手点**。

### 17.3 但真正致命的是：xaga 的 bootloader 根本不认这个 xfile

把两机 `lk.img` 里的 `SYS_*` 镜像表全导出来对比（各 62 项）：

| | xaga LK | 13T LK |
|---|---|---|
| `SYS_GPUEB_XFILE` | **✗ 没有** | **✔ 有** |
| `SYS_GPUEB_COREDUMP` | ✔ | ✔ |
| `SYS_GPUEB_LAST_LOG` | ✔ | ✔ |
| `SYS_DPM_XFILE` | ✔ | ✔ |
| `SYS_SSPM_XFILE` | ✔ | ✔ |

xaga 的 LK 里，xfile 机制**存在**（DPM、SSPM 都有），但**唯独没有 gpueb 的那一项**。

已用**原厂** LK 复核（`rom/20-xaga-原厂镜像/xaga_stock/lk.img`，md5 `1aa9b95d`，
与底包 `30-xaga-自制与移植/v1.1_Hyper4.00.7test/images/lk.img` 同一文件）：
`SYS_GPUEB_XFILE` 出现 **0 次**，`SYS_DPM_XFILE` 出现 3 次。
对照 13T LK 里 `SYS_GPUEB_XFILE` 明确存在。

**结论：那 322 KB 数据表在 xaga 上永远不会被加载**——
不是因为加密解不开，而是因为**连装载它的代码路径都不存在**。
即使哪天把密文解开了，bootloader 依然不会把它送进 MCU。

### 17.4 想绕过去还剩什么路（都不现实）

| 思路 | 可行性 |
|---|---|
| 解密 xfile | ✗ 密钥在 MCU ROM / 安全启动链 |
| 改 xaga 的 LK 加 `SYS_GPUEB_XFILE` | ✗ 无 LK 源码；且 LK 有签名校验 |
| 由内核（AP）在开机后把密文拷进 gpueb 的 DRAM | △ 理论上可行，但：① 不知道固件期望的目标地址（主镜像里不引用 xfile，地址应是硬编码在 LK 侧）② MCU 访问 DRAM 依赖 LK 建立的 remap ③ 仍要 MCU 自己解密 |
| 干脆不用 xfile | ✗ 13T 固件的 DDS/hopping/GPM 系数全在里面 |

### 17.5 这条线的最终结论

**13T gpueb 固件在 xaga 上不可行，而且原因不止一个、层层叠加：**

1. **共享内存尺寸**：xaga 给 `0x1000`，13T 固件要 `0x4000` → `fail to init GPUFREQ`（§16，一行 DT 可修）
2. **数据表缺失**：322 KB 加密 xfile，xaga 的 LK 没有 `SYS_GPUEB_XFILE` → 永不加载（本节，**不可修**）
3. **收益为零**：OPP 表与原厂零字节差异（§11）

第 2 条是硬的、不可绕过的——它把"要不要修第 1 条"这个问题直接取消了。
