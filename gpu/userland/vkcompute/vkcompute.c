/*
 * vkcompute - first shader on the phone's GPU: turnip compiles
 * square.comp (ir3), the GPU runs it over a buffer, the CPU checks every
 * element against the same arithmetic.
 *
 *   vkcompute [-n ELEMENTS] [-r ROUNDS] [-i ICD]
 *
 * Two runs: a small one (1 round) that must be exact, and a timed one
 * with ROUNDS iterations per element, printed as GPU throughput. Started
 * on the phone by ios/sweetrun with turnip from the boot image.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include "sweetrun.h"
#include "square_spv.h"

static int fails;
#define CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
	printf("FAIL %s = %d\n", #x, _r); fails++; goto out; } } while (0)

static uint32_t
expect(uint32_t i, uint32_t rounds)
{
	uint32_t x = i;
	for (uint32_t r = 0; r < rounds; r++) {
		x = x * x + (i ^ 0x9e3779b9u);
	}
	return x;
}

static double
now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int
main(int argc, char **argv)
{
	const char *icd = NULL;
	uint32_t count = 1u << 20, rounds = 256;
	int c;

	while ((c = getopt(argc, argv, "n:r:i:")) != -1) {
		switch (c) {
		case 'n': count = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'r': rounds = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'i': icd = optarg; break;
		default:
			fprintf(stderr, "usage: vkcompute [-n ELEMENTS] [-r ROUNDS] [-i ICD]\n");
			return 2;
		}
	}
	if (icd == NULL) {
		icd = sweetrun_file("lib", "/private/var/tmp/libvulkan_freedreno.dylib");
	}
	if (icd == NULL) {
		printf("FAIL: no ICD (-i, or mkadt.py --run-lib)\n");
		return 1;
	}
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
	IFN(vkGetPhysicalDeviceMemoryProperties);
	IFN(vkGetPhysicalDeviceProperties);
	IFN(vkCreateDevice);
	IFN(vkGetDeviceProcAddr);

	VkPhysicalDevice pd;
	uint32_t n = 1;
	CHECK(vkEnumeratePhysicalDevices(inst, &n, &pd));
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(pd, &props);
	printf("device: %s, timestamp period %.3f ns\n", props.deviceName,
	    props.limits.timestampPeriod);

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
	CHECK(vkCreateDevice(pd, &dci, NULL, &dev));

#define DFN(n) PFN_##n n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
	DFN(vkGetDeviceQueue);
	DFN(vkCreateBuffer);
	DFN(vkGetBufferMemoryRequirements);
	DFN(vkAllocateMemory);
	DFN(vkBindBufferMemory);
	DFN(vkMapMemory);
	DFN(vkCreateShaderModule);
	DFN(vkCreateDescriptorSetLayout);
	DFN(vkCreatePipelineLayout);
	DFN(vkCreateComputePipelines);
	DFN(vkCreateDescriptorPool);
	DFN(vkAllocateDescriptorSets);
	DFN(vkUpdateDescriptorSets);
	DFN(vkCreateCommandPool);
	DFN(vkAllocateCommandBuffers);
	DFN(vkBeginCommandBuffer);
	DFN(vkEndCommandBuffer);
	DFN(vkResetCommandBuffer);
	DFN(vkCmdBindPipeline);
	DFN(vkCmdBindDescriptorSets);
	DFN(vkCmdPushConstants);
	DFN(vkCmdDispatch);
	DFN(vkCmdPipelineBarrier);
	DFN(vkCreateFence);
	DFN(vkResetFences);
	DFN(vkQueueSubmit);
	DFN(vkWaitForFences);
	DFN(vkDeviceWaitIdle);
	DFN(vkDestroyDevice);

	/* host-visible, coherent buffer */
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	uint32_t want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
		if ((mp.memoryTypes[i].propertyFlags & want) == want) {
			type = i;
			break;
		}
	}
	if (type == UINT32_MAX) {
		printf("FAIL: no host-visible coherent memory\n");
		fails++;
		goto out;
	}
	VkDeviceSize size = (VkDeviceSize)count * 4;
	VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
	VkBuffer buf;
	CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(dev, buf, &mr);
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mr.size, .memoryTypeIndex = type };
	VkDeviceMemory mem;
	CHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
	CHECK(vkBindBufferMemory(dev, buf, mem, 0));
	uint32_t *data = NULL;
	CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&data));

	/* pipeline */
	VkShaderModuleCreateInfo smci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = sizeof(square_spv), .pCode = square_spv };
	VkShaderModule sm;
	double t0 = now_s();
	CHECK(vkCreateShaderModule(dev, &smci, NULL, &sm));
	VkDescriptorSetLayoutBinding b = { .binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
	VkDescriptorSetLayoutCreateInfo dslci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1, .pBindings = &b };
	VkDescriptorSetLayout dsl;
	CHECK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));
	VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 8 };
	VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1, .pSetLayouts = &dsl, .pushConstantRangeCount = 1,
		.pPushConstantRanges = &pcr };
	VkPipelineLayout pl;
	CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
	VkComputePipelineCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" },
		.layout = pl };
	VkPipeline pipe;
	CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));
	printf("pipeline: compiled in %.1f ms\n", (now_s() - t0) * 1e3);

	VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
	VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
	VkDescriptorPool dp;
	CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &dp));
	VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl };
	VkDescriptorSet ds;
	CHECK(vkAllocateDescriptorSets(dev, &dsai, &ds));
	VkDescriptorBufferInfo dbi = { buf, 0, VK_WHOLE_SIZE };
	VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds,
		.dstBinding = 0, .descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi };
	vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);

	VkQueue q;
	vkGetDeviceQueue(dev, 0, 0, &q);
	VkCommandPoolCreateInfo cpoolci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = 0 };
	VkCommandPool pool;
	CHECK(vkCreateCommandPool(dev, &cpoolci, NULL, &pool));
	VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBuffer cb;
	CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb));
	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence;
	CHECK(vkCreateFence(dev, &fci, NULL, &fence));

	/* run 0: exact check, 1 round; run 1: timed, `rounds` rounds */
	for (int run = 0; run < 2; run++) {
		uint32_t rr = run == 0 ? 1 : rounds;
		for (uint32_t i = 0; i < count; i++) {
			data[i] = i;
		}
		VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
		CHECK(vkResetCommandBuffer(cb, 0));
		CHECK(vkBeginCommandBuffer(cb, &bi));
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
		uint32_t push[2] = { count, rr };
		vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
		vkCmdDispatch(cb, (count + 63) / 64, 1, 1);
		VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
		    0, 1, &mb, 0, NULL, 0, NULL);
		CHECK(vkEndCommandBuffer(cb));
		VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
			.pCommandBuffers = &cb };
		CHECK(vkResetFences(dev, 1, &fence));
		t0 = now_s();
		CHECK(vkQueueSubmit(q, 1, &si, fence));
		CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 10000000000ull));
		double dt = now_s() - t0;

		uint32_t bad = 0, first = UINT32_MAX;
		for (uint32_t i = 0; i < count; i++) {
			if (data[i] != expect(i, rr)) {
				if (first == UINT32_MAX) {
					first = i;
				}
				bad++;
			}
		}
		printf("run %d: %u elements x %u rounds in %.2f ms (%.1f M iterations/s), %u wrong",
		    run, count, rr, dt * 1e3, (double)count * rr / dt / 1e6, bad);
		if (bad) {
			printf(", first at %u: %#x, expected %#x", first, data[first], expect(first, rr));
			fails++;
		}
		printf("\n");
	}
	CHECK(vkDeviceWaitIdle(dev));
out:
	if (dev) {
		PFN_vkDestroyDevice dd = (PFN_vkDestroyDevice)gipa(inst, "vkDestroyDevice");
		dd(dev, NULL);
	}
	printf("%s\n", fails ? "VKCOMPUTE FAILED" : "VKCOMPUTE PASSED");
	return fails ? 1 : 0;
}
