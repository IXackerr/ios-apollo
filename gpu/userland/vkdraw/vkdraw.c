/*
 * vkdraw - first rendering on the phone's GPU: a render pass clears an
 * image to blue and draws one flat orange triangle (tri.vert, tri.frag),
 * the image is copied to a buffer and every pixel is checked: blue or
 * orange, nothing else, and orange covering the triangle's share of the
 * image (32 %).
 *
 *   vkdraw [-f FRAMES] [-i ICD]
 *
 * Two sizes: 256x256 once, then the iOS screen (1125x2436) FRAMES times,
 * timed, which is large enough for turnip to render in GMEM bins.
 * Started on the phone by ios/sweetrun with turnip from the boot image.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include "sweetrun.h"
#include "tri_vert_spv.h"
#include "tri_frag_spv.h"

#define FMT		VK_FORMAT_R8G8B8A8_UNORM
#define CLEAR_PX	0xffff0000u     /* R 0, G 0, B 255, A 255 as little-endian RGBA8 */
#define TRI_PX		0xff4080ffu     /* R 255, G 128, B 64, A 255 */

static int fails;
#define CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
	printf("FAIL %s = %d\n", #x, _r); fails++; goto out; } } while (0)

static PFN_vkGetDeviceProcAddr gdpa;
static VkDevice dev;
static VkPhysicalDeviceMemoryProperties mp;
#define DFN(n) PFN_##n n = (PFN_##n)gdpa(dev, #n)

static double
now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static uint32_t
mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) {
			return i;
		}
	}
	return UINT32_MAX;
}

/* Render w x h `frames` times, check the last frame. */
static void
draw_test(VkQueue q, VkCommandBuffer cb, VkRenderPass rp, VkPipelineLayout pl,
    VkShaderModule vs, VkShaderModule fs, uint32_t w, uint32_t h, int frames)
{
	DFN(vkCreateImage);
	DFN(vkGetImageMemoryRequirements);
	DFN(vkAllocateMemory);
	DFN(vkBindImageMemory);
	DFN(vkCreateImageView);
	DFN(vkCreateFramebuffer);
	DFN(vkCreateGraphicsPipelines);
	DFN(vkCreateBuffer);
	DFN(vkGetBufferMemoryRequirements);
	DFN(vkBindBufferMemory);
	DFN(vkMapMemory);
	DFN(vkResetCommandBuffer);
	DFN(vkBeginCommandBuffer);
	DFN(vkEndCommandBuffer);
	DFN(vkCmdBeginRenderPass);
	DFN(vkCmdEndRenderPass);
	DFN(vkCmdBindPipeline);
	DFN(vkCmdDraw);
	DFN(vkCmdCopyImageToBuffer);
	DFN(vkCmdPipelineBarrier);
	DFN(vkCreateFence);
	DFN(vkResetFences);
	DFN(vkQueueSubmit);
	DFN(vkWaitForFences);

	VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D, .format = FMT, .extent = { w, h, 1 },
		.mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
	VkImage img;
	CHECK(vkCreateImage(dev, &ici, NULL, &img));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(dev, img, &mr);
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mr.size,
		.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
	VkDeviceMemory imem;
	CHECK(vkAllocateMemory(dev, &mai, NULL, &imem));
	CHECK(vkBindImageMemory(dev, img, imem, 0));
	VkImageViewCreateInfo ivci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = FMT,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	VkImageView view;
	CHECK(vkCreateImageView(dev, &ivci, NULL, &view));
	VkFramebufferCreateInfo fbci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = rp, .attachmentCount = 1, .pAttachments = &view,
		.width = w, .height = h, .layers = 1 };
	VkFramebuffer fb;
	CHECK(vkCreateFramebuffer(dev, &fbci, NULL, &fb));

	VkPipelineShaderStageCreateInfo stages[2] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		  .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		  .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" },
	};
	VkPipelineVertexInputStateCreateInfo vi = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
	VkRect2D sc = { { 0, 0 }, { w, h } };
	VkPipelineViewportStateCreateInfo vps = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
	VkPipelineRasterizationStateCreateInfo rs = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1 };
	VkPipelineMultisampleStateCreateInfo ms = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
	VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
	VkPipelineColorBlendStateCreateInfo cbs = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1, .pAttachments = &cba };
	VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2, .pStages = stages, .pVertexInputState = &vi,
		.pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs,
		.pMultisampleState = &ms, .pColorBlendState = &cbs, .layout = pl, .renderPass = rp };
	VkPipeline pipe;
	double t0 = now_s();
	CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));
	double t_pipe = now_s() - t0;

	VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = (VkDeviceSize)w * h * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
	VkBuffer buf;
	CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
	vkGetBufferMemoryRequirements(dev, buf, &mr);
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = mem_type(mr.memoryTypeBits,
	    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VkDeviceMemory bmem;
	CHECK(vkAllocateMemory(dev, &mai, NULL, &bmem));
	CHECK(vkBindBufferMemory(dev, buf, bmem, 0));
	uint32_t *px = NULL;
	CHECK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **)&px));
	memset(px, 0x55, (size_t)w * h * 4);

	VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	CHECK(vkResetCommandBuffer(cb, 0));
	CHECK(vkBeginCommandBuffer(cb, &bi));
	VkClearValue clear = { .color = { .float32 = { 0, 0, 1, 1 } } };
	VkRenderPassBeginInfo rpbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = rp, .framebuffer = fb, .renderArea = sc, .clearValueCount = 1,
		.pClearValues = &clear };
	vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdDraw(cb, 3, 1, 0, 0);
	vkCmdEndRenderPass(cb);
	VkBufferImageCopy copy = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { w, h, 1 } };
	vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &copy);
	VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
	    1, &mb, 0, NULL, 0, NULL);
	CHECK(vkEndCommandBuffer(cb));

	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence;
	CHECK(vkCreateFence(dev, &fci, NULL, &fence));
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
		.pCommandBuffers = &cb };
	t0 = now_s();
	for (int f = 0; f < frames; f++) {
		CHECK(vkResetFences(dev, 1, &fence));
		CHECK(vkQueueSubmit(q, 1, &si, fence));
		CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 10000000000ull));
	}
	double dt = (now_s() - t0) / frames;

	uint64_t tri = 0, bad = 0;
	uint32_t first = UINT32_MAX;
	for (uint32_t i = 0; i < w * h; i++) {
		if (px[i] == TRI_PX) {
			tri++;
		} else if (px[i] != CLEAR_PX) {
			if (first == UINT32_MAX) {
				first = i;
			}
			bad++;
		}
	}
	double cover = (double)tri / ((double)w * h);
	uint32_t center = px[(h / 2) * w + w / 2], corner = px[0];
	printf("%ux%u: pipeline %.1f ms, %.2f ms/frame over %d frames, triangle %.2f %%, "
	    "center %#x corner %#x, %llu other pixels", w, h, t_pipe * 1e3, dt * 1e3, frames,
	    cover * 100, center, corner, (unsigned long long)bad);
	if (bad) {
		printf(" (first at %u,%u: %#x)", first % w, first / w, px[first]);
	}
	printf("\n");
	if (bad || center != TRI_PX || corner != CLEAR_PX || cover < 0.31 || cover > 0.33) {
		fails++;
	}
out:
	return;
}

int
main(int argc, char **argv)
{
	const char *icd = NULL;
	int frames = 60, c;

	while ((c = getopt(argc, argv, "f:i:")) != -1) {
		switch (c) {
		case 'f': frames = atoi(optarg); break;
		case 'i': icd = optarg; break;
		default:
			fprintf(stderr, "usage: vkdraw [-f FRAMES] [-i ICD]\n");
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
	gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");

	VkPhysicalDevice pd;
	uint32_t n = 1;
	CHECK(vkEnumeratePhysicalDevices(inst, &n, &pd));
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(pd, &props);
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	printf("device: %s\n", props.deviceName);

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
	CHECK(vkCreateDevice(pd, &dci, NULL, &dev));

	DFN(vkGetDeviceQueue);
	DFN(vkCreateShaderModule);
	DFN(vkCreatePipelineLayout);
	DFN(vkCreateRenderPass);
	DFN(vkCreateCommandPool);
	DFN(vkAllocateCommandBuffers);
	DFN(vkDeviceWaitIdle);

	VkShaderModuleCreateInfo smci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = sizeof(tri_vert_spv), .pCode = tri_vert_spv };
	VkShaderModule vs, fs;
	CHECK(vkCreateShaderModule(dev, &smci, NULL, &vs));
	smci.codeSize = sizeof(tri_frag_spv);
	smci.pCode = tri_frag_spv;
	CHECK(vkCreateShaderModule(dev, &smci, NULL, &fs));
	VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	VkPipelineLayout pl;
	CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));

	VkAttachmentDescription att = { .format = FMT, .samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
	VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1, .pColorAttachments = &ref };
	VkSubpassDependency dep = { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
		.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
		.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT };
	VkRenderPassCreateInfo rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1, .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sub,
		.dependencyCount = 1, .pDependencies = &dep };
	VkRenderPass rp;
	CHECK(vkCreateRenderPass(dev, &rpci, NULL, &rp));

	VkQueue q;
	vkGetDeviceQueue(dev, 0, 0, &q);
	VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = 0 };
	VkCommandPool pool;
	CHECK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
	VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBuffer cb;
	CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb));

	draw_test(q, cb, rp, pl, vs, fs, 256, 256, 1);
	draw_test(q, cb, rp, pl, vs, fs, 1125, 2436, frames);
	CHECK(vkDeviceWaitIdle(dev));
out:
	printf("%s\n", fails ? "VKDRAW FAILED" : "VKDRAW PASSED");
	return fails ? 1 : 0;
}
