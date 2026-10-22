/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Compatibility shims for MTK GPU kernel interfaces that exist in the 6.12
 * MTK GPU stack but are absent from the 5.10 MTK GPU stack.
 *
 * These are intentionally conservative: query functions report "unavailable"
 * and tuning knobs become no-ops, so the 5.10 GED/GPUFREQ implementation stays
 * authoritative for DVFS decisions.
 */
#ifndef _MTK_COMPAT_510_H_
#define _MTK_COMPAT_510_H_

#include <linux/types.h>
#include <ged_dvfs.h>
#include <mtk_gpufreq.h>
#include <gpufreq_v2.h>

/* 6.12 renamed the GPUFREQ power states; 5.10 uses POWER_OFF/POWER_ON */
#ifndef GPU_PWR_ON
#define GPU_PWR_OFF POWER_OFF
#define GPU_PWR_ON POWER_ON
#endif

/* 6.12 introduced an enum for the GPU clock switch notification */
#ifndef GED_POWER_ON
enum ged_gpu_power_state {
	GED_POWER_OFF = 0,
	GED_SLEEP,
	GED_POWER_ON,
};
#endif

/* 6.12 IRQ monitor (AEE) hook: not present on 5.10 */
static inline void irq_mon_aee_period_set(unsigned int irq, unsigned int period_ns)
{
}

/* GPU utilization counters: 5.10 only exposes a single loading counter */
static inline int ged_dvfs_get_util_3d(void)
{
	/* 5.10 GED exports only the combined loading counter */
	return -1;
}

static inline int ged_dvfs_get_util_ta(void)
{
	return -1;
}

static inline int ged_dvfs_get_util_comp(void)
{
	return -1;
}

static inline int ged_dvfs_get_util_active(void)
{
	return -1;
}

/* last-commit bookkeeping: 5.10 has a single GPU commit index */
static inline unsigned long ged_dvfs_get_last_commit_top_idx(void)
{
	return ged_dvfs_get_last_commit_idx();
}

static inline unsigned long ged_dvfs_get_last_commit_stack_idx(void)
{
	return 0;
}

static inline unsigned long ged_dvfs_write_sysram_last_commit_idx(void)
{
	return 0;
}

static inline unsigned long ged_dvfs_write_sysram_last_commit_idx_test(int commit_idx)
{
	return 0;
}

static inline unsigned long ged_dvfs_write_sysram_last_commit_top_idx(void)
{
	return 0;
}

static inline unsigned long ged_dvfs_write_sysram_last_commit_top_idx_test(int commit_idx)
{
	return 0;
}

static inline unsigned long ged_dvfs_write_sysram_last_commit_stack_idx(void)
{
	return 0;
}

static inline unsigned long ged_dvfs_write_sysram_last_commit_stack_idx_test(int commit_idx)
{
	return 0;
}

static inline unsigned long ged_dvfs_write_sysram_last_commit_dual(void)
{
	return 0;
}

static inline unsigned long ged_dvfs_write_sysram_last_commit_dual_test(int top_idx, int stack_idx)
{
	return 0;
}

/* DVFS governor knobs introduced after 5.10 */
static inline unsigned int ged_dvfs_get_gov_mask_enable(void)
{
	return 0;
}

static inline void ged_dvfs_set_gov_mask_enable(unsigned int enable)
{
}

static inline unsigned int ged_dvfs_get_desire_mask(void)
{
	return 0;
}

static inline int ged_dvfs_get_dcs_enable(void)
{
	return -1;
}

static inline int ged_dvfs_update_step_size(int low_step, int med_step, int high_step)
{
	return 0;
}

static inline void ged_set_apo_api_sync_status(int apo_api_sync_status)
{
}

/* 5.10 has no gpufreq_dual_commit(). Commit the GPU domain only: the gpufreq
 * backend derives the STACK OPP itself under the hardware constraint
 * Fgpu >= 1.1 * Fstack, so passing the GPU index through TARGET_DEFAULT
 * (which dual-buck remaps to STACK) would index the 40-entry STACK table with
 * a 6-entry GPU index.
 */
static inline int gpufreq_dual_commit(int gpu_oppidx, int stack_oppidx)
{
	(void)stack_oppidx;
	return gpufreq_commit(TARGET_GPU, gpu_oppidx);
}

#endif /* _MTK_COMPAT_510_H_ */
