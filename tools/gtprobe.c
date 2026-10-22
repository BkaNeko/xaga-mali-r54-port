// SPDX-License-Identifier: GPL-2.0
/*
 * Probe whether the GPUEB firmware honours a commit to a given domain/OPP.
 * All MTK frequency control funnels through gpufreq_commit() -> IPI, so this
 * answers the question "can AP make the firmware select this OPP?" without
 * touching any production code.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>

#define TARGET_GPU   1

extern int gpufreq_commit(unsigned int target, int oppidx);
extern int gpufreq_get_cur_oppidx(unsigned int target);
extern unsigned int gpufreq_get_cur_freq(unsigned int target);
extern int gpufreq_get_opp_num(unsigned int target);
extern int gpufreq_fix_custom_freq_volt(unsigned int target, unsigned int freq,
					unsigned int volt);

static unsigned int target = TARGET_GPU;
static int idx = 0;
static unsigned int cus_freq;
static unsigned int cus_volt;
module_param(target, uint, 0644);
module_param(idx, int, 0644);
module_param(cus_freq, uint, 0644);
module_param(cus_volt, uint, 0644);

static struct delayed_work gt_work;
static int gt_count;

static void gt_fn(struct work_struct *w)
{
	int cur, ret;
	unsigned int freq;

	if (cus_freq) {
		ret = gpufreq_fix_custom_freq_volt(target, cus_freq, cus_volt);
		cur = gpufreq_get_cur_oppidx(TARGET_GPU);
		freq = gpufreq_get_cur_freq(TARGET_GPU);
		pr_info("GTPROBE: custom(target=%u, %u kHz, %u) ret=%d -> cur_idx=%d freq=%u kHz (n=%d)\n",
			target, cus_freq, cus_volt, ret, cur, freq, gt_count);
	} else {
		ret = gpufreq_commit(target, idx);
		cur = gpufreq_get_cur_oppidx(TARGET_GPU);
		freq = gpufreq_get_cur_freq(TARGET_GPU);
		pr_info("GTPROBE: commit(target=%u, idx=%d) ret=%d -> cur_idx=%d freq=%u kHz (n=%d)\n",
			target, idx, ret, cur, freq, gt_count);
	}

	if (++gt_count < 40)
		schedule_delayed_work(&gt_work, msecs_to_jiffies(200));
}

static int __init gt_init(void)
{
	pr_info("GTPROBE: start, target=%u idx=%d custom=%u/%u, gpu opp_num=%d\n",
		target, idx, cus_freq, cus_volt, gpufreq_get_opp_num(TARGET_GPU));
	INIT_DELAYED_WORK(&gt_work, gt_fn);
	gt_count = 0;
	schedule_delayed_work(&gt_work, msecs_to_jiffies(1000));
	return 0;
}

static void __exit gt_exit(void)
{
	cancel_delayed_work_sync(&gt_work);
	pr_info("GTPROBE: exit\n");
}

module_init(gt_init);
module_exit(gt_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("gpufreq domain/OPP commit probe");
