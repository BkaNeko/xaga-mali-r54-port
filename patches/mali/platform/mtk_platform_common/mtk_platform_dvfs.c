// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 MediaTek Inc.
 */
#include <mtk_ddk_define.h>  /* MTK_INLINE */
#include "mtk_compat_510.h"

/* 6.12 GED owns these pointers; on 5.10 they are kbase-local (nothing calls them) */
void (*ged_dvfs_gpu_freq_dual_commit_fp)(unsigned long gpuNewFreqID,
					 unsigned long stackNewFreqID, int *pbCommited) = NULL;
u64 (*mtk_get_system_timer_fp)(void) = NULL;

#if IS_ENABLED(CONFIG_PROC_FS)
#include <linux/proc_fs.h>
#include <linux/thermal.h>
#endif
#include <mali_kbase.h>
#include <platform/mtk_platform_common.h>
#include <backend/gpu/mali_kbase_pm_internal.h>
#include <backend/gpu/mali_kbase_pm_defs.h>
#include <csf/ipa_control/mali_kbase_csf_ipa_control_ex.h>
#include <mtk_gpufreq.h>
#include <mtk_gpu_utility.h>
#include <platform/mtk_platform_common/mtk_platform_dvfs.h>
#include <ged_dvfs.h>

#if IS_ENABLED(CONFIG_MALI_MIDGARD_DVFS) && \
	IS_ENABLED(CONFIG_MALI_MTK_DVFS_POLICY) && \
	IS_ENABLED(CONFIG_MALI_MTK_GPU_DVFS_HINT_26M_LOADING)
#include <platform/mtk_platform_common/mtk_platform_dvfs_hint_26m_perf_cnting.h>
#include "platform/mtk_platform_common/mtk_platform_dvfs_hint_26m_perf_cnting_ex.h"
#endif /* CONFIG_MALI_MIDGARD_DVFS && CONFIG_MALI_MTK_DVFS_POLICY && CONFIG_MALI_MTK_GPU_DVFS_HINT_26M_LOADING*/

#if IS_ENABLED(CONFIG_PROC_FS)
/* name of the proc entry */
#define	PROC_GPU_UTILIZATION "gpu_utilization"
static DEFINE_MUTEX(gpu_utilization_lock);
#endif

#if IS_ENABLED(CONFIG_MALI_MIDGARD_DVFS) && IS_ENABLED(CONFIG_MALI_MTK_DVFS_POLICY)
static unsigned int current_util_active;
static unsigned int current_util_3d;
static unsigned int current_util_ta;
static unsigned int current_util_compute;
static unsigned int current_util_iter;
static unsigned int current_util_mcu;
#endif

#if IS_ENABLED(CONFIG_MALI_MTK_DVFS_LOADING_MODE) && \
    IS_ENABLED(CONFIG_MALI_MTK_GPU_DVFS_ASYNC)
#define UTIL_ACTIVE_ID   0
#define UTIL_TA_ID       1
#define UTIL_COMPUTE_ID  2
#define UTIL_3D_ID       3
#define UTIL_ITER_ID     4
#define UTIL_MCU_ID      5
#define UTIL_TILER_ID    6
#define UTIL_SC_COMP_ID  7
#define UTIL_l2ext_ID    8
static unsigned long long pre_active_raw;
static bool counter_unchange;
#elif IS_ENABLED(CONFIG_MALI_MTK_DVFS_LOADING_MODE)
#define UTIL_ACTIVE_ID   0
#define UTIL_TA_ID       1
#define UTIL_COMPUTE_ID  2
#define UTIL_3D_ID       3
#define UTIL_ITER_ID     4
#define UTIL_MCU_ID      5
static unsigned long long pre_active_raw;
static bool counter_unchange;
#endif

#if IS_ENABLED(CONFIG_MALI_MIDGARD_DVFS) && IS_ENABLED(CONFIG_MALI_MTK_DVFS_POLICY)
/* jiffies of the last GED-driven frequency commit (0 = never) */
static unsigned long mtk_ged_last_commit;

void mtk_common_ged_dvfs_commit(unsigned long ui32NewFreqID,
                                GED_DVFS_COMMIT_TYPE eCommitType,
                                int *pbCommited)
{
	struct kbase_device *kbdev = (struct kbase_device *)mtk_common_get_kbdev();
	int ret;

	if (!IS_ERR_OR_NULL(kbdev)) {
		if (kbdev->pm.backend.gpu_ready) {
			ret = mtk_common_gpufreq_commit(ui32NewFreqID);
			if (ret == 0)
				mtk_ged_last_commit = jiffies;
			if (pbCommited) {
				*pbCommited = (ret == 0) ? true : false;
			}
		}
	}
}

void mtk_common_ged_dvfs_dual_commit(unsigned long gpuNewFreqID,
                                unsigned long stackNewFreqID,
                                int *pbCommited)
{
	struct kbase_device *kbdev = (struct kbase_device *)mtk_common_get_kbdev();
	int ret;

	if (!IS_ERR_OR_NULL(kbdev)) {
		if (kbdev->pm.backend.gpu_ready) {
			ret = mtk_common_gpufreq_dual_commit(gpuNewFreqID, stackNewFreqID);
			if (pbCommited) {
				*pbCommited = (ret == 0) ? true : false;
			}
		}
	}
}
#endif

#if IS_ENABLED(CONFIG_MALI_MIDGARD_DVFS) && IS_ENABLED(CONFIG_MALI_MTK_DVFS_POLICY)
void mtk_common_update_gpu_utilization(void)
{
	unsigned int loading, block, idle;
#if IS_ENABLED(CONFIG_MALI_MTK_DVFS_LOADING_MODE)
	struct GpuUtilization_Ex util_ex;
	mtk_common_cal_gpu_utilization_ex(&loading, &block, &idle, &util_ex);
#else
	mtk_common_cal_gpu_utilization(&loading, &block, &idle);
#endif
}

int mtk_common_get_util_active(void)
{
	if (ged_dvfs_get_util_active() >= 0)
		return counter_unchange ? 0 : ged_dvfs_get_util_active();
	return current_util_active;
}

int mtk_common_get_util_3d(void)
{
	if (ged_dvfs_get_util_3d() >= 0)
		return counter_unchange ? 0 : ged_dvfs_get_util_3d();
	return current_util_3d;
}

int mtk_common_get_util_ta(void)
{
	if (ged_dvfs_get_util_ta() >= 0)
		return counter_unchange ? 0 : ged_dvfs_get_util_ta();
	return current_util_ta;
}

int mtk_common_get_util_compute(void)
{
	if (ged_dvfs_get_util_comp() >= 0)
		return counter_unchange ? 0 : ged_dvfs_get_util_comp();
	return current_util_compute;
}

#if IS_ENABLED(CONFIG_MALI_MTK_DVFS_LOADING_MODE)
void mtk_common_cal_gpu_utilization_ex(unsigned int *pui32Loading,
                                       unsigned int *pui32Block,
                                       unsigned int *pui32Idle,
                                       void *Util_Ex)
#else
void mtk_common_cal_gpu_utilization(unsigned int *pui32Loading,
                                    unsigned int *pui32Block,
                                    unsigned int *pui32Idle)
#endif
{
	struct kbase_device *kbdev = (struct kbase_device *)mtk_common_get_kbdev();
#if IS_ENABLED(CONFIG_MALI_MTK_DVFS_LOADING_MODE)
	struct GpuUtilization_Ex *util_ex =
			(struct GpuUtilization_Ex *) Util_Ex;
#endif
	unsigned long long delta_time;

#if IS_ENABLED(CONFIG_MALI_MTK_DVFS_LOADING_MODE)
	int utilisation[NUM_PERF_COUNTERS*2];
#else
	int utilisation[NUM_PERF_COUNTERS];
#endif /* CONFIG_MALI_MTK_DVFS_LOADING_MODE */

	struct kbasep_pm_metrics *diff;
	int index = 0;

	KBASE_DEBUG_ASSERT(kbdev != NULL);

	diff = &kbdev->pm.backend.metrics.dvfs_diff;

	kbase_pm_get_dvfs_metrics(kbdev, &kbdev->pm.backend.metrics.dvfs_last,
				  diff);

	delta_time = max(diff->time_busy[0] + diff->time_idle[0], 1u);
	for (index = 0; index < NUM_PERF_COUNTERS; index++) {
		// delta time should be the same for all PMU, so simply reuse it
		utilisation[index] = (100 * diff->time_busy[index]) / delta_time;
#if IS_ENABLED(CONFIG_MALI_MTK_DVFS_LOADING_MODE)
		utilisation[index + NUM_PERF_COUNTERS] = diff->counterRaw[index];
#endif /* CONFIG_MALI_MTK_DVFS_LOADING_MODE */
	}

#if IS_ENABLED(CONFIG_MALI_MTK_DVFS_LOADING_MODE)
	util_ex->util_active    = utilisation[UTIL_ACTIVE_ID];
	util_ex->util_ta        = utilisation[UTIL_TA_ID];
	util_ex->util_compute   = utilisation[UTIL_COMPUTE_ID];
	util_ex->util_3d        = utilisation[UTIL_3D_ID];
#if IS_ENABLED(CONFIG_MALI_MTK_GPU_DVFS_ASYNC)
	util_ex->util_tiler_raw     = utilisation[UTIL_TILER_ID + NUM_PERF_COUNTERS];
	util_ex->util_sc_comp_raw   = utilisation[UTIL_SC_COMP_ID + NUM_PERF_COUNTERS];
	util_ex->util_l2ext_raw     = utilisation[UTIL_l2ext_ID + NUM_PERF_COUNTERS];
#endif /* CONFIG_MALI_MTK_GPU_DVFS_ASYNC */

#if IS_ENABLED(CONFIG_MALI_MTK_GPU_DVFS_HINT_26M_LOADING)
	util_ex->util_iter_u_mcu = mtk_dvfs_hint_26m_cal_prfcnt_utilization(
          SELECT_UNION_ITER_MCU, gpu_power_status);
#endif /* CONFIG_MALI_MTK_GPU_DVFS_HINT_26M_LOADING */
#endif

	if (pui32Loading)
		*pui32Loading = utilisation[0];

	if (pui32Idle)
		*pui32Idle = 100 - utilisation[0];

	if (utilisation[0] < 0 || utilisation[1] < 0 ||
	    utilisation[2] < 0 || utilisation[3] < 0 ||
		utilisation[4] < 0 || utilisation[5] < 0) {
		utilisation[0] = 0;
		utilisation[1] = 0;
		utilisation[2] = 0;
		utilisation[3] = 0;
		utilisation[4] = 0;
		utilisation[5] = 0;
	} else {
		current_util_active  = utilisation[0];
		current_util_ta      = utilisation[1];
		current_util_compute = utilisation[2];
		current_util_3d      = utilisation[3];
		current_util_iter    = utilisation[4];
		current_util_mcu     = utilisation[5];
	}

#if IS_ENABLED(CONFIG_MALI_MTK_DVFS_LOADING_MODE)
	// check if gpu power off
	if (pre_active_raw == util_ex->util_active)
		counter_unchange = true;
	else
		counter_unchange = false;
	pre_active_raw = util_ex->util_active;
#endif /* CONFIG_MALI_MTK_DVFS_LOADING_MODE */
}

#if IS_ENABLED(CONFIG_MALI_MIDGARD_DVFS) && \
	IS_ENABLED(CONFIG_MALI_MTK_DVFS_POLICY)
int (*mtk_common_rate_change_notify_fp)(struct kbase_device *kbdev,
					       u32 clk_index, u32 clk_rate_hz) = NULL;

EXPORT_SYMBOL(mtk_common_rate_change_notify_fp);

void MTKGPUFreq_change_notify(u32 clk_idx, u32 gpufreq)
{
	struct kbase_device *kbdev = (struct kbase_device *)mtk_common_get_kbdev();

	if (mtk_common_rate_change_notify_fp && !IS_ERR_OR_NULL(kbdev))
		mtk_common_rate_change_notify_fp(kbdev, clk_idx, gpufreq);
}
#endif
#endif

/* only work if CSF exit */
void mtk_set_gpu_idle_time(unsigned int val){
#if IS_ENABLED(CONFIG_MALI_MTK_GPU_IDLE_TEST)
		struct kbase_device *kbdev;

		kbdev = (struct kbase_device *)mtk_common_get_kbdev();
		if (IS_ERR_OR_NULL(kbdev)) {
			return;
		}

		kbase_csf_firmware_set_gpu_idle_hysteresis_time(kbdev, val);
#else
		return;
#endif

}

#if IS_ENABLED(CONFIG_MALI_MIDGARD_DVFS) && IS_ENABLED(CONFIG_MALI_MTK_DVFS_POLICY)
int mtk_set_core_mask(u64 core_mask)
{
	struct kbase_device *kbdev = (struct kbase_device *)mtk_common_get_kbdev();
	int ret = 0;
	u64 final_core_mask = 0;

	final_core_mask = core_mask & (u64)gpufreq_get_shader_present();
	if (final_core_mask > 0)
		kbase_devfreq_set_core_mask(kbdev, final_core_mask);
	else
		return 1;

	/* TODO: need to check get_core_mask hang issue, to verity the scaling result */
	// current_mask = kbdev->pm.backend.ca_cores_enabled;

	return ret;
}

u64 mtk_common_get_system_timer(void)
{
	struct kbase_device *kbdev = (struct kbase_device *)mtk_common_get_kbdev();
	u64 system_time_tmp = 0;
	bool gpu_ready;
	unsigned long flags;

	/* need to lock for using function point */
	spin_lock_irqsave(&kbdev->hwaccess_lock, flags);

	gpu_ready = kbdev->pm.backend.gpu_ready;

	if (gpu_ready)
		kbase_backend_get_gpu_time_norequest(kbdev, NULL, &system_time_tmp, NULL);

	spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);

	return system_time_tmp;
}
#endif

#if IS_ENABLED(CONFIG_PROC_FS)
static int mtk_dvfs_gpu_utilization_show(struct seq_file *m, void *v)
{
#if IS_ENABLED(CONFIG_MALI_MIDGARD_DVFS) && IS_ENABLED(CONFIG_MALI_MTK_DVFS_POLICY)
	unsigned int cur_opp_idx;

	mutex_lock(&gpu_utilization_lock);

	mtk_common_update_gpu_utilization();

#if defined(CONFIG_MTK_GPUFREQ_V2)
	cur_opp_idx = mtk_common_gpufreq_bringup() ?
		0 : gpufreq_get_cur_oppidx(TARGET_DEFAULT);
#else
	cur_opp_idx = mtk_common_gpufreq_bringup() ?
		0 : mt_gpufreq_get_cur_freq_index();
#endif /* CONFIG_MTK_GPUFREQ_V2 */

	seq_printf(m, "ACTIVE=%u 3D/TA/COMPUTE=%u/%u/%u OPP_IDX=%u MFG_PWR=%d\n",
	           mtk_common_get_util_active(), mtk_common_get_util_3d(), mtk_common_get_util_ta(),
	           mtk_common_get_util_compute(), cur_opp_idx, mtk_common_pm_is_mfg_active());

	mutex_unlock(&gpu_utilization_lock);
#else
	seq_puts(m, "GPU DVFS doesn't be enabled\n");
#endif

	return 0;
}
DEFINE_PROC_SHOW_ATTRIBUTE(mtk_dvfs_gpu_utilization);

int mtk_dvfs_procfs_init(struct kbase_device *kbdev, struct proc_dir_entry *parent)
{
	if (IS_ERR_OR_NULL(kbdev))
		return -1;

	proc_create(PROC_GPU_UTILIZATION, 0440, parent, &mtk_dvfs_gpu_utilization_proc_ops);

	return 0;
}

int mtk_dvfs_procfs_term(struct kbase_device *kbdev, struct proc_dir_entry *parent)
{
	if (IS_ERR_OR_NULL(kbdev))
		return -1;

	remove_proc_entry(PROC_GPU_UTILIZATION, parent);

	return 0;
}
#else
int mtk_dvfs_procfs_init(struct kbase_device *kbdev, struct proc_dir_entry *parent)
{
	return 0;
}

int mtk_dvfs_procfs_term(struct kbase_device *kbdev, struct proc_dir_entry *parent)
{
	return 0;
}
#endif /* CONFIG_PROC_FS */


/* ------------------------------------------------------------------
 * MTK GPU DVFS policy (kbase-driven, simple_ondemand semantics)
 *
 * Upstream Mali uses devfreq + the kernel simple_ondemand governor:
 *   load       = busy_time * 100 / (busy_time + idle_time)
 *   target_freq= max_freq * load / 100
 * MTK disables devfreq (no DT clock) and expects GED to do the same job.
 * GED's input path is dead on this kernel, so replicate the algorithm here.
 * ------------------------------------------------------------------ */
#if IS_ENABLED(CONFIG_MALI_MIDGARD_DVFS) && IS_ENABLED(CONFIG_MALI_MTK_DVFS_POLICY)
#define MTK_OPP_MAX 64
/* gpufreq_get_working_table() reports 40 frequency levels, but gpufreq_commit()
 * only accepts the coarse OPP indices (0..5 on mt6895: 950/880/800/610/430/350MHz).
 * The exact count is only exposed by gpufreq_get_debug_opp_info(), which is not
 * exported, so cap at the OPP count of this SoC. */
#define MTK_GPU_MAX_OPP 6
/* xaga/mt6895: the vendor gpufreq table exposes a 950 MHz entry that this
 * chip never runs at (it belongs to the signed/aging table).  Drop any OPP
 * above this cap so the governor cannot select it. */
#define MTK_GPU_FREQ_CAP_KHZ 880000

static struct delayed_work mtk_dvfs_policy_work;
static bool mtk_dvfs_policy_running;
#define MTK_STACK_OPP_MAX 64
static int mtk_opp_freq[MTK_OPP_MAX];
static int mtk_stack_freq[MTK_STACK_OPP_MAX];
static int mtk_stack_last = -1;
static int mtk_opp_index[MTK_OPP_MAX];	/* -> gpufreq OPP index */
static int mtk_opp_last = -1;	/* lowest OPP index (highest numeric) */
static int mtk_opp_max_freq;
static int mtk_down_ticks;
static int mtk_hold_ticks;
static bool mtk_runtime_tuned;

static void mtk_opp_table_init(void)
{
	int num_gpu, num_def, i;
	unsigned int f;

	mtk_opp_last = -1;

	/* Ask the driver how many OPP indices it supports.  The working table
	 * itself is much longer (it also lists signed/aging frequency levels),
	 * and gpufreq_commit() only accepts the coarse OPP indices. */
	num_gpu = gpufreq_get_opp_num(TARGET_GPU);
	num_def = gpufreq_get_opp_num(TARGET_DEFAULT);

	if (num_gpu <= 0)
		num_gpu = num_def;
	if (num_gpu <= 0 || num_gpu > MTK_OPP_MAX)
		num_gpu = MTK_OPP_MAX;

	for (i = 0; i < num_gpu; i++) {
		f = gpufreq_get_freq_by_idx(TARGET_GPU, i);
		if (f == 0)
			break;
		if (f > MTK_GPU_FREQ_CAP_KHZ) {
			pr_info("mtk-gpufreq: skipping OPP %d (%u kHz) above cap\n",
				i, f);
			continue;
		}
		mtk_opp_last++;
		mtk_opp_freq[mtk_opp_last] = f;
		mtk_opp_index[mtk_opp_last] = i;
	}

	mtk_opp_max_freq = (mtk_opp_last >= 0) ? mtk_opp_freq[0] : 0;

	pr_info("mtk-gpufreq: opp_num gpu=%d def=%d, using %d entries: %d/%d/%d/%d/%d/%d kHz (idx %d/%d/%d/%d/%d/%d)\n",
		num_gpu, num_def, mtk_opp_last + 1,
		mtk_opp_freq[0], mtk_opp_freq[1], mtk_opp_freq[2],
		mtk_opp_freq[3], mtk_opp_freq[4], mtk_opp_freq[5],
		mtk_opp_index[0], mtk_opp_index[1], mtk_opp_index[2],
		mtk_opp_index[3], mtk_opp_index[4], mtk_opp_index[5]);

	/* stack (SLC/system-cache) domain: separate fine-grained table */
	{
		const struct gpufreq_opp_info *stbl =
			gpufreq_get_working_table(TARGET_STACK);

		mtk_stack_last = -1;
		if (stbl) {
			for (i = 0; i < MTK_STACK_OPP_MAX; i++) {
				if (stbl[i].freq <= 0)
					break;
				mtk_stack_freq[i] = stbl[i].freq;
				mtk_stack_last = i;
			}
		}
		pr_info("mtk-gpufreq: stack table %d entries (%d..%d kHz)\n",
			mtk_stack_last + 1,
			mtk_stack_last >= 0 ? mtk_stack_freq[0] : 0,
			mtk_stack_last >= 0 ? mtk_stack_freq[mtk_stack_last] : 0);
	}
}

/* Keep the stack (SLC) domain frequency close to the GPU frequency.
 * Nothing else drives it on this kernel (GED's dual-commit path is dead),
 * and leaving it at the lowest OPP throttles memory bandwidth. */
static void __maybe_unused mtk_stack_track(int gpu_freq_khz)
{
	int j, best = 0;
	unsigned int d, bd = ~0u;

	if (mtk_stack_last < 0)
		return;

	for (j = 0; j <= mtk_stack_last; j++) {
		d = (unsigned int)(mtk_stack_freq[j] > gpu_freq_khz ?
			mtk_stack_freq[j] - gpu_freq_khz :
			gpu_freq_khz - mtk_stack_freq[j]);
		if (d < bd) {
			bd = d;
			best = j;
		}
	}

	if (gpufreq_get_cur_oppidx(TARGET_STACK) != best)
		gpufreq_commit(TARGET_STACK, best);
}

static void mtk_dvfs_policy_fn(struct work_struct *work)
{
	struct kbase_device *kbdev = (struct kbase_device *)mtk_common_get_kbdev();
	unsigned int loading = 0, block = 0, idle = 0;
	int cur_idx, target = -1;
#if IS_ENABLED(CONFIG_MALI_MTK_DVFS_LOADING_MODE)
	struct GpuUtilization_Ex util_ex;
#endif

	if (IS_ERR_OR_NULL(kbdev))
		goto out;

	/* gpufreq rejects commits before the GPU is up; skip early ticks */
	if (!kbdev->pm.backend.gpu_ready)
		goto out;

	if (mtk_opp_last < 0) {
		mtk_opp_table_init();
		if (mtk_opp_last < 0)
			goto out;
	}

	/* kbase programs its own defaults while loading the firmware, so the
	 * idle hysteresis / DVFS period have to be applied afterwards. */
	if (!mtk_runtime_tuned && kbdev->csf.firmware_inited) {
		extern u32 kbase_csf_firmware_set_gpu_idle_hysteresis_time(
			struct kbase_device *kbdev, u64 dur_ns);

		kbase_csf_firmware_set_gpu_idle_hysteresis_time(
			kbdev, 10ULL * NSEC_PER_MSEC);
		kbdev->pm.dvfs_period = 20;
		mtk_runtime_tuned = true;
		pr_info("mtk-gpufreq: runtime tune applied (hysteresis 10ms, dvfs 20ms)\n");
	}

#if IS_ENABLED(CONFIG_MALI_MTK_DVFS_LOADING_MODE)
	memset(&util_ex, 0, sizeof(util_ex));
	mtk_common_cal_gpu_utilization_ex(&loading, &block, &idle, &util_ex);
#else
	mtk_common_cal_gpu_utilization(&loading, &block, &idle);
#endif

	cur_idx = gpufreq_get_cur_oppidx(TARGET_GPU);

	/* If GED committed a frequency recently it is driving DVFS itself;
	 * stay out of the way (official MTK behaviour). */
	if (mtk_ged_last_commit &&
	    time_before(jiffies, mtk_ged_last_commit + 3 * HZ))
		goto out;

	/* Performance-first policy modelled on MTK GED behaviour: ramp up
	 * eagerly and hold the higher OPP for a few ticks before allowing a
	 * downscale, so short idle gaps do not cause frequency flapping. */
	if (loading >= 55)
		target = 0;
	else if (loading >= 35)
		target = 1;
	else if (loading >= 18)
		target = 2;
	else if (loading >= 6)
		target = 3;
	else
		target = 4;

	if (target > mtk_opp_last)
		target = mtk_opp_last;

	if (target < cur_idx) {
		/* upscale immediately, then hold */
		mtk_hold_ticks = 10;	/* 200ms at max after ramp */
		mtk_down_ticks = 0;
	} else if (target > cur_idx) {
		if (mtk_hold_ticks > 0) {
			mtk_hold_ticks--;
			goto out;
		}
		if (++mtk_down_ticks < 50)	/* 1s of sustained idle */
			goto out;
	} else {
		mtk_down_ticks = 0;
		mtk_hold_ticks = 0;
		goto out;
	}

	if (target != cur_idx) {
		int ret = mtk_common_gpufreq_commit(mtk_opp_index[target]);

		mtk_down_ticks = 0;
		if (ret)
			pr_info_ratelimited(
				"mtk-gpufreq: commit %d->%d(idx %d) failed ret=%d\n",
				cur_idx, target, mtk_opp_index[target], ret);
	}

out:
	if (mtk_dvfs_policy_running)
		schedule_delayed_work(&mtk_dvfs_policy_work,
				      msecs_to_jiffies(20));
}

/* ---- thermal cooling device ("thermal-devfreq-0") ---------------- */
static struct thermal_cooling_device *mtk_gpu_cdev;

static int mtk_gpu_cdev_get_max_state(struct thermal_cooling_device *cdev,
				      unsigned long *state)
{
	*state = (mtk_opp_last > 0) ? (unsigned long)mtk_opp_last : 1UL;
	return 0;
}

static int mtk_gpu_cdev_get_cur_state(struct thermal_cooling_device *cdev,
				      unsigned long *state)
{
	int idx = gpufreq_get_cur_limit_idx(TARGET_GPU, GPUPPM_CEILING);

	*state = (idx >= 0) ? (unsigned long)idx : 0UL;
	return 0;
}

static int mtk_gpu_cdev_set_cur_state(struct thermal_cooling_device *cdev,
				      unsigned long state)
{
	int ceiling = (int)state;

	if (mtk_opp_last < 0)
		return 0;
	if (ceiling > mtk_opp_last)
		ceiling = mtk_opp_last;

	/* ceiling 0 == highest frequency allowed */
	gpufreq_set_limit(TARGET_DEFAULT, LIMIT_THERMAL_AP, ceiling,
			  mtk_opp_last);
	return 0;
}

static const struct thermal_cooling_device_ops mtk_gpu_cdev_ops = {
	.get_max_state = mtk_gpu_cdev_get_max_state,
	.get_cur_state = mtk_gpu_cdev_get_cur_state,
	.set_cur_state = mtk_gpu_cdev_set_cur_state,
};

static void mtk_gpu_cdev_register_once(void)
{
	if (mtk_gpu_cdev || mtk_opp_last < 0)
		return;

	mtk_gpu_cdev = thermal_cooling_device_register("thermal-devfreq-0",
						       NULL, &mtk_gpu_cdev_ops);
	if (IS_ERR(mtk_gpu_cdev)) {
		pr_err("mtk-gpufreq: cooling device register failed %ld\n",
		       PTR_ERR(mtk_gpu_cdev));
		mtk_gpu_cdev = NULL;
	} else {
		pr_info("mtk-gpufreq: thermal cooling device registered (max_state %d)\n",
			mtk_opp_last);
	}
}

void mtk_dvfs_policy_start(void)
{
	if (mtk_dvfs_policy_running)
		return;

	if (mtk_opp_last < 0)
		mtk_opp_table_init();
	mtk_gpu_cdev_register_once();

	/* make sure every shader core is available (MTK normally does this
	 * from GED, whose input path is dead on this kernel) */
	mtk_set_core_mask((u64)gpufreq_get_shader_present());

	INIT_DELAYED_WORK(&mtk_dvfs_policy_work, mtk_dvfs_policy_fn);
	mtk_dvfs_policy_running = true;
	schedule_delayed_work(&mtk_dvfs_policy_work, msecs_to_jiffies(1000));
	pr_info("mtk-gpufreq: utilisation governor started\n");
}

void mtk_dvfs_policy_stop(void)
{
	if (!mtk_dvfs_policy_running)
		return;

	mtk_dvfs_policy_running = false;
	cancel_delayed_work_sync(&mtk_dvfs_policy_work);
}
#endif /* CONFIG_MALI_MIDGARD_DVFS && CONFIG_MALI_MTK_DVFS_POLICY */

int mtk_dvfs_init(struct kbase_device *kbdev)
{
	if (IS_ERR_OR_NULL(kbdev))
		return -1;

#if IS_ENABLED(CONFIG_MALI_MIDGARD_DVFS) && IS_ENABLED(CONFIG_MALI_MTK_DVFS_POLICY)
#if IS_ENABLED(CONFIG_MALI_MTK_DVFS_LOADING_MODE)
	ged_dvfs_cal_gpu_utilization_ex_fp = mtk_common_cal_gpu_utilization_ex;
	mtk_notify_gpu_freq_change_fp = MTKGPUFreq_change_notify;
#else
	ged_dvfs_cal_gpu_utilization_fp = mtk_common_cal_gpu_utilization;
#endif /* CONFIG_MALI_MTK_DVFS_LOADING_MODE */
	ged_dvfs_gpu_freq_commit_fp = mtk_common_ged_dvfs_commit;
	ged_dvfs_gpu_freq_dual_commit_fp = mtk_common_ged_dvfs_dual_commit;
	ged_dvfs_set_gpu_core_mask_fp = mtk_set_core_mask;
	mtk_get_system_timer_fp = mtk_common_get_system_timer;
#endif /* CONFIG_MALI_MIDGARD_DVFS && CONFIG_MALI_MTK_DVFS_POLICY */
	mtk_set_gpu_idle_fp = mtk_set_gpu_idle_time;

	/* Responsiveness tuning.
	 * idle_hysteresis_time defaults to 0, which powers the GPU down the
	 * instant it goes idle; every following submission then pays a power-up
	 * + firmware wake. 10 ms keeps it alive across frame gaps.
	 * dvfs_period is the utilisation sampling window (default 100 ms). */
	{
		extern u32 kbase_csf_firmware_set_gpu_idle_hysteresis_time(
			struct kbase_device *kbdev, u64 dur_ns);

		kbase_csf_firmware_set_gpu_idle_hysteresis_time(
			kbdev, 10ULL * NSEC_PER_MSEC);
		kbdev->pm.dvfs_period = 20;
		pr_info("mtk-gpufreq: idle hysteresis 10ms, dvfs period 20ms\n");
	}

#if IS_ENABLED(CONFIG_MALI_MIDGARD_DVFS) && IS_ENABLED(CONFIG_MALI_MTK_DVFS_POLICY)
	/* gpufreq is already up when kbase probes, so the OPP table and the
	 * thermal cooling device can be created before ThermalHAL starts. */
	mtk_dvfs_policy_start();
#endif
	return 0;
}

int mtk_dvfs_term(struct kbase_device *kbdev)
{
	if (IS_ERR_OR_NULL(kbdev))
		return -1;
#if IS_ENABLED(CONFIG_MALI_MIDGARD_DVFS) && IS_ENABLED(CONFIG_MALI_MTK_DVFS_POLICY)
#if IS_ENABLED(CONFIG_MALI_MTK_DVFS_LOADING_MODE)
	ged_dvfs_cal_gpu_utilization_ex_fp = NULL;
	mtk_notify_gpu_freq_change_fp = NULL;
#else
	ged_dvfs_cal_gpu_utilization_fp = NULL;
#endif /* CONFIG_MALI_MTK_DVFS_LOADING_MODE */
	ged_dvfs_gpu_freq_commit_fp = NULL;
	ged_dvfs_gpu_freq_dual_commit_fp = NULL;
	ged_dvfs_set_gpu_core_mask_fp = NULL;
#endif /* CONFIG_MALI_MIDGARD_DVFS && CONFIG_MALI_MTK_DVFS_POLICY */
	mtk_set_gpu_idle_fp = NULL;
#if IS_ENABLED(CONFIG_MALI_MIDGARD_DVFS) && IS_ENABLED(CONFIG_MALI_MTK_DVFS_POLICY)
	mtk_dvfs_policy_stop();
	if (mtk_gpu_cdev) {
		thermal_cooling_device_unregister(mtk_gpu_cdev);
		mtk_gpu_cdev = NULL;
	}
#endif
	return 0;
}

