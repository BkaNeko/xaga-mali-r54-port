/*
 * gedvsyncd - keep MediaTek GED's DVFS engine fed with vsync events
 *
 * Background: GED's frequency policy is event driven. On stock MediaTek ROMs
 * the composer/framework pushes a timestamped vsync notification over
 * /proc/ged every frame; GED then folds it into its loading window and commits
 * an OPP. AOSP-based ROMs ship no such component, so GED sits idle forever
 * (gpu_loading == 0) even though the GPU is busy.
 *
 * This daemon emits the same notification on a fixed timer. It is deliberately
 * dumb: GED already ignores events while the GPU is idle and stops its own
 * backup timer, so there is nothing to gain from trying to be clever here.
 *
 * Build (Android NDK):
 *   aarch64-linux-android30-clang -O2 -o gedvsyncd gedvsyncd.c
 * Run as root, e.g. from a KernelSU/Magisk service script:
 *   /data/local/tmp/gedvsyncd &
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

struct GED_BRIDGE_PACKAGE {
	uint32_t ui32FunctionID;
	int32_t i32Size;
	void *pvParamIn;
	int32_t i32InBufferSize;
	void *pvParamOut;
	int32_t i32OutBufferSize;
};

struct ged_in_notify_vsync {
	int eType;		/* GED_VSYNC_SW_EVENT = 0 */
};

#define GED_MAGIC		'g'
#define GED_CMD_NOTIFY_VSYNC	6
#define GED_IOWR(i)		_IOWR(GED_MAGIC, i, struct GED_BRIDGE_PACKAGE)

/* While the GPU is busy keep GED's loading window fresh (4 ms); once it goes
 * idle there is nothing to measure, so back off and stop burning cycles. */
#define INTERVAL_BUSY_US	4000
#define INTERVAL_IDLE_US	100000

/* first field of gpu_utilization is the current active percentage */
static int gpu_is_idle(void)
{
	char buf[64];
	int fd, n, util = 0;

	fd = open("/sys/kernel/ged/hal/gpu_utilization", O_RDONLY);
	if (fd < 0)
		return 0;	/* cannot tell, assume busy */
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = '\0';
	util = atoi(buf);
	return util == 0;
}

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

int main(int argc, char **argv)
{
	struct ged_in_notify_vsync in = { .eType = 0 };
	char outbuf[256];
	int verbose = (argc > 1 && !strcmp(argv[1], "-v"));
	unsigned long sent = 0, failed = 0;
	int fd;

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	fd = open("/proc/ged", O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "gedvsyncd: open /proc/ged: %s\n",
			strerror(errno));
		return 1;
	}

	while (!g_stop) {
		struct GED_BRIDGE_PACKAGE pkg;
		int ret;

		memset(outbuf, 0, sizeof(outbuf));
		pkg.ui32FunctionID = GED_IOWR(GED_CMD_NOTIFY_VSYNC);
		pkg.i32Size = sizeof(pkg);
		pkg.pvParamIn = &in;
		pkg.i32InBufferSize = sizeof(in);
		pkg.pvParamOut = outbuf;
		pkg.i32OutBufferSize = sizeof(outbuf);

		ret = ioctl(fd, GED_IOWR(GED_CMD_NOTIFY_VSYNC), &pkg);
		if (ret < 0) {
			failed++;
			if (verbose)
				fprintf(stderr, "gedvsyncd: ioctl: %s\n",
					strerror(errno));
			/* the GED module may have been reloaded */
			close(fd);
			usleep(500000);
			fd = open("/proc/ged", O_RDWR);
			if (fd < 0)
				return 1;
			continue;
		}
		sent++;
		if (verbose && !(sent % 250))
			fprintf(stderr, "gedvsyncd: sent=%lu failed=%lu\n",
				sent, failed);
		usleep(gpu_is_idle() ? INTERVAL_IDLE_US : INTERVAL_BUSY_US);
	}

	close(fd);
	return 0;
}
