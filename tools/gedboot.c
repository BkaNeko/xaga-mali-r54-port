/*
 * gedboot - send one SW-vsync event to the MediaTek GED kernel module
 *
 * Why this exists: GED's DVFS engine is entirely event driven. Userspace (the
 * MTK composer/framework) is expected to push a timestamped vsync notification
 * over /proc/ged every frame, and GED only then updates its loading counters
 * and commits a frequency. AOSP-based ROMs have no such component, so GED sits
 * idle forever: gpu_loading stays 0 and gpu_idle stays 100.
 *
 * Sending a single event while the GPU is actually busy starts GED's internal
 * hrtimer (g_HT_hwvsync_emu), which re-arms itself, so native GED DVFS keeps
 * running until the GPU idles out.
 *
 * Build (Android NDK):
 *   aarch64-linux-android30-clang -O2 -o gedboot gedboot.c
 * Run as root:
 *   adb shell su -c /data/local/tmp/gedboot
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct GED_BRIDGE_PACKAGE {
	uint32_t ui32FunctionID;
	int32_t i32Size;
	void *pvParamIn;
	int32_t i32InBufferSize;
	void *pvParamOut;
	int32_t i32OutBufferSize;
};

/* struct GED_BRIDGE_IN_NOTIFY_VSYNC */
struct ged_in_notify_vsync {
	int eType;		/* GED_VSYNC_SW_EVENT = 0, HW = 1 */
};

#define GED_MAGIC		'g'
#define GED_CMD_NOTIFY_VSYNC	6
#define GED_IOWR(i)		_IOWR(GED_MAGIC, i, struct GED_BRIDGE_PACKAGE)

int main(void)
{
	struct ged_in_notify_vsync in = { .eType = 0 };
	struct GED_BRIDGE_PACKAGE pkg;
	char outbuf[256];
	int fd, ret;

	fd = open("/proc/ged", O_RDWR);
	if (fd < 0) {
		printf("open /proc/ged failed: %s\n", strerror(errno));
		return 1;
	}

	memset(outbuf, 0, sizeof(outbuf));
	pkg.ui32FunctionID = GED_IOWR(GED_CMD_NOTIFY_VSYNC);
	pkg.i32Size = sizeof(pkg);
	pkg.pvParamIn = &in;
	pkg.i32InBufferSize = sizeof(in);
	pkg.pvParamOut = outbuf;
	pkg.i32OutBufferSize = sizeof(outbuf);

	ret = ioctl(fd, GED_IOWR(GED_CMD_NOTIFY_VSYNC), &pkg);
	printf("notify_vsync -> %d (errno=%d %s)\n", ret, errno, strerror(errno));
	close(fd);
	return ret < 0 ? 1 : 0;
}
