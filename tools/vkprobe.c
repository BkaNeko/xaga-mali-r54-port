/*
 * vkprobe - minimal Vulkan instance/physical-device probe
 *
 * Confirms that the Vulkan ICD loads and reports the expected GPU, which is
 * separate from GLES: on MediaTek devices vulkan.mali.so is a small shim that
 * binds into libGLES_mali.so.
 *
 * Build (Android NDK):
 *   aarch64-linux-android30-clang -O2 -o vkprobe vkprobe.c -lvulkan
 */
#include <vulkan/vulkan.h>
#include <stdio.h>

int main(void)
{
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	VkInstance inst = VK_NULL_HANDLE;
	VkPhysicalDevice pd[4];
	VkResult r;
	uint32_t n = 0, i;

	app.apiVersion = VK_API_VERSION_1_1;
	ci.pApplicationInfo = &app;

	r = vkCreateInstance(&ci, NULL, &inst);
	printf("vkCreateInstance: %d\n", r);
	if (r != VK_SUCCESS)
		return 1;

	vkEnumeratePhysicalDevices(inst, &n, NULL);
	printf("physical devices: %u\n", n);
	if (n > 4)
		n = 4;
	vkEnumeratePhysicalDevices(inst, &n, pd);

	for (i = 0; i < n; i++) {
		VkPhysicalDeviceProperties p;

		vkGetPhysicalDeviceProperties(pd[i], &p);
		printf("  [%u] %s | api %u.%u.%u | driver 0x%x | type %d\n",
		       i, p.deviceName,
		       VK_VERSION_MAJOR(p.apiVersion),
		       VK_VERSION_MINOR(p.apiVersion),
		       VK_VERSION_PATCH(p.apiVersion),
		       p.driverVersion, p.deviceType);
		printf("      maxComputeWG=%u maxImage2D=%u\n",
		       p.limits.maxComputeWorkGroupInvocations,
		       p.limits.maxImageDimension2D);
	}

	vkDestroyInstance(inst, NULL);
	return 0;
}
