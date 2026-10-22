/*
 * vktest.c - Vulkan end-to-end test on Linux, no phone needed:
 *
 *   this program -> Mesa turnip (libvulkan_freedreno.so, -Dfreedreno-kmds=sweet)
 *     -> tu_knl_sweet.cc -> libsweetgpu -> sgpu_host_dispatch
 *     -> the real SWEET kernel driver code (kernel/sweet_gpu_*.c)
 *     -> SoC model (CP interpreter, SMMU walker, GMU/HFI peer)
 *
 * The ICD is dlopen()ed directly (no loader). Checks: enumeration finds
 * "Turnip Adreno (TM) 650", device creation, memory types, a submission
 * whose command buffer writes memory on the "GPU" (vkCmdSetEvent at
 * TOP_OF_PIPE -> CP_MEM_WRITE; at ALL_COMMANDS -> CP_EVENT_WRITE
 * RB_DONE_TS), fences, and that turnip's real command streams never touch
 * a register our CP_PROTECT table blocks.
 *
 * Build: see hosttest/Makefile target "vktest".
 *
 * The same file runs on the phone (built by gpu/ci/build-ios-userland.sh):
 * there turnip talks to the real SweetAdreno, the model checks are left
 * out, and the ICD comes with the boot image (mkadt.py --run-lib), started
 * by ios/sweetrun.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#if defined(__APPLE__)
#include "sweetrun.h"
#else
int sgpu_host_boot(int verbose, int permissive);
int sgpu_host_model_errors(void);
unsigned sgpu_host_cp_stats(unsigned *skipped);
#endif

static int fails;
#define CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
	printf("FAIL %s = %d\n", #x, _r); fails++; goto out; } } while (0)
#define EXPECT(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int
main(int argc, char **argv)
{
#if defined(__APPLE__)
	const char *icd = argc > 1 ? argv[1] :
	    sweetrun_file("lib", "/private/var/tmp/libvulkan_freedreno.dylib");
	if (icd == NULL) {
		printf("FAIL: no ICD (mkadt.py --run-lib)\n");
		return 1;
	}
#else
	const char *icd = argc > 1 ? argv[1] :
	    "libvulkan_freedreno.so";
	int err = sgpu_host_boot(0, 1);
	if (err) {
		printf("FAIL: kernel bring-up %d\n", err);
		return 1;
	}
#endif
	void *lib = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
	if (!lib) {
		printf("FAIL: dlopen: %s\n", dlerror());
		return 1;
	}
	PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
	PFN_vkCreateInstance createInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");

	VkInstance inst = VK_NULL_HANDLE;
	VkDevice dev = VK_NULL_HANDLE;
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.apiVersion = VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app };
	CHECK(createInstance(&ici, NULL, &inst));

#define IFN(n) PFN_##n n = (PFN_##n)gipa(inst, #n)
	IFN(vkEnumeratePhysicalDevices);
	IFN(vkGetPhysicalDeviceProperties);
	IFN(vkGetPhysicalDeviceMemoryProperties);
	IFN(vkEnumerateDeviceExtensionProperties);
	IFN(vkCreateDevice);
	IFN(vkGetDeviceProcAddr);
	IFN(vkDestroyInstance);

	uint32_t n = 0;
	CHECK(vkEnumeratePhysicalDevices(inst, &n, NULL));
	EXPECT(n == 1, "%u physical devices", n);
	if (n != 1) goto out;
	VkPhysicalDevice pd;
	n = 1;
	CHECK(vkEnumeratePhysicalDevices(inst, &n, &pd));
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(pd, &props);
	printf("device: %s, Vulkan %u.%u.%u, driver %#x, vendor %#x device %#x\n", props.deviceName,
	    VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
	    VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion, props.vendorID,
	    props.deviceID);
	EXPECT(strstr(props.deviceName, "650") != NULL, "device name %s", props.deviceName);

	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
		printf("memory type %u: flags %#x heap %u\n", i, mp.memoryTypes[i].propertyFlags,
		    mp.memoryTypes[i].heapIndex);
	}

	uint32_t next = 0;
	vkEnumerateDeviceExtensionProperties(pd, NULL, &next, NULL);
	VkExtensionProperties *exts = calloc(next, sizeof(*exts));
	vkEnumerateDeviceExtensionProperties(pd, NULL, &next, exts);
	int bad = 0;
	for (uint32_t i = 0; i < next; i++) {
		if (!strcmp(exts[i].extensionName, "VK_KHR_external_memory_fd") ||
		    !strcmp(exts[i].extensionName, "VK_EXT_map_memory_placed") ||
		    !strcmp(exts[i].extensionName, "VK_KHR_external_semaphore_fd"))
			bad++;
	}
	printf("%u device extensions\n", next);
	EXPECT(bad == 0, "fd/placed-map extensions advertised");

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
	CHECK(vkCreateDevice(pd, &dci, NULL, &dev));

#define DFN(n) PFN_##n n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
	DFN(vkGetDeviceQueue);
	DFN(vkCreateCommandPool);
	DFN(vkAllocateCommandBuffers);
	DFN(vkBeginCommandBuffer);
	DFN(vkEndCommandBuffer);
	DFN(vkCmdSetEvent);
	DFN(vkCreateEvent);
	DFN(vkGetEventStatus);
	DFN(vkResetEvent);
	DFN(vkCreateFence);
	DFN(vkQueueSubmit);
	DFN(vkWaitForFences);
	DFN(vkResetFences);
	DFN(vkAllocateMemory);
	DFN(vkMapMemory);
	DFN(vkFreeMemory);
	DFN(vkDeviceWaitIdle);
	DFN(vkDestroyDevice);
	DFN(vkDestroyEvent);
	DFN(vkDestroyFence);
	DFN(vkDestroyCommandPool);

	/* memory: allocate + map + write */
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = 1 << 20, .memoryTypeIndex = 0 };
	VkDeviceMemory mem;
	CHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
	void *ptr = NULL;
	CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &ptr));
	memset(ptr, 0xab, 1 << 20);
	vkFreeMemory(dev, mem, NULL);

	VkQueue q;
	vkGetDeviceQueue(dev, 0, 0, &q);
	VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.queueFamilyIndex = 0 };
	VkCommandPool pool;
	CHECK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
	VkCommandBuffer cb[2];
	VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2 };
	CHECK(vkAllocateCommandBuffers(dev, &cbai, cb));

	VkEventCreateInfo eci = { .sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO };
	VkEvent ev[2];
	CHECK(vkCreateEvent(dev, &eci, NULL, &ev[0]));
	CHECK(vkCreateEvent(dev, &eci, NULL, &ev[1]));
	EXPECT(vkGetEventStatus(dev, ev[0]) == VK_EVENT_RESET, "event 0 not reset initially");

	VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	CHECK(vkBeginCommandBuffer(cb[0], &bi));
	vkCmdSetEvent(cb[0], ev[0], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);      /* CP_MEM_WRITE */
	CHECK(vkEndCommandBuffer(cb[0]));
	CHECK(vkBeginCommandBuffer(cb[1], &bi));
	vkCmdSetEvent(cb[1], ev[1], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);     /* RB_DONE_TS */
	CHECK(vkEndCommandBuffer(cb[1]));

	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence;
	CHECK(vkCreateFence(dev, &fci, NULL, &fence));
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 2,
		.pCommandBuffers = cb };
	CHECK(vkQueueSubmit(q, 1, &si, fence));
	CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 2000000000ull));
	EXPECT(vkGetEventStatus(dev, ev[0]) == VK_EVENT_SET, "GPU did not set event 0 (CP_MEM_WRITE)");
	EXPECT(vkGetEventStatus(dev, ev[1]) == VK_EVENT_SET, "GPU did not set event 1 (RB_DONE_TS)");

	/* many submits: ring wrap + timestamp ordering through turnip */
	for (int i = 0; i < 300; i++) {
		CHECK(vkResetFences(dev, 1, &fence));
		vkResetEvent(dev, ev[0]);
		CHECK(vkQueueSubmit(q, 1, &si, fence));
		CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 2000000000ull));
		if (vkGetEventStatus(dev, ev[0]) != VK_EVENT_SET) {
			printf("FAIL: event not set on iteration %d\n", i);
			fails++;
			break;
		}
	}
	/* an empty submit with only a fence (turnip's zero-IB path) */
	CHECK(vkResetFences(dev, 1, &fence));
	CHECK(vkQueueSubmit(q, 0, NULL, fence));
	CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 2000000000ull));
	CHECK(vkDeviceWaitIdle(dev));

	vkDestroyFence(dev, fence, NULL);
	vkDestroyEvent(dev, ev[0], NULL);
	vkDestroyEvent(dev, ev[1], NULL);
	vkDestroyCommandPool(dev, pool, NULL);
out:
	if (dev) {
		PFN_vkDestroyDevice dd = (PFN_vkDestroyDevice)gipa(inst, "vkDestroyDevice");
		dd(dev, NULL);
	}
	if (inst) {
		PFN_vkDestroyInstance di = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
		di(inst, NULL);
	}
#if !defined(__APPLE__)
	unsigned skipped, pk = sgpu_host_cp_stats(&skipped);
	printf("CP executed %u packets (%u unmodelled skipped), model errors %d\n", pk, skipped,
	    sgpu_host_model_errors());
	EXPECT(sgpu_host_model_errors() == 0, "model errors");
#endif
	printf("%s\n", fails ? "VKTEST FAILED" : "VKTEST PASSED");
	return fails ? 1 : 0;
}
