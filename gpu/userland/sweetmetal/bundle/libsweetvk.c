/*
 * libsweetvk - "Vulkan loader" for SweetMetal on iOS, which has none.
 *
 * metal2vulkan-runtime opens a loader library (DeviceOptions.loader) and
 * looks up vkGetInstanceProcAddr. Mesa ICDs export only the ICD interface
 * (vk_icdGetInstanceProcAddr), so this forwards to turnip, linked
 * directly (libvulkan_freedreno.dylib built with -Dfreedreno-kmds=sweet).
 */
#include <vulkan/vulkan_core.h>

extern PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);
extern VkResult vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *version);

__attribute__((visibility("default"))) PFN_vkVoidFunction
vkGetInstanceProcAddr(VkInstance instance, const char *name)
{
	static int negotiated;
	if (!negotiated) {
		uint32_t v = 5;     /* loader/ICD interface version, as the Khronos loader does */
		vk_icdNegotiateLoaderICDInterfaceVersion(&v);
		negotiated = 1;
	}
	return vk_icdGetInstanceProcAddr(instance, name);
}
