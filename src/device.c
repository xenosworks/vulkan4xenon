#include <string.h>

#include <vulkan/vulkan.h>

#include "blit.h"
#include "host.h"
#include "objects.h"
#include "physical_device.h"
#include "shader.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"
#include "xsdk/splash.h"

// The GPU has one owner, so there's one device at a time.
static bool device_exists;

// Every feature off but those the device has (vkGetPhysicalDeviceFeatures).
static bool features_off(const VkPhysicalDeviceFeatures *features)
{
	if (!features)
		return true;
	VkPhysicalDeviceFeatures others = *features;
	others.drawIndirectFirstInstance = VK_FALSE;
	others.independentBlend = VK_FALSE;
	others.samplerAnisotropy = VK_FALSE;
	others.shaderClipDistance = VK_FALSE;
	others.largePoints = VK_FALSE;
	others.fillModeNonSolid = VK_FALSE;
	VkBool32 flags[sizeof others / sizeof(VkBool32)];
	memcpy(flags, &others, sizeof flags);
	for (uint32_t i = 0; i < sizeof flags / sizeof flags[0]; i++) {
		if (flags[i])
			return false;
	}
	return true;
}

static bool enabled(const VkDeviceCreateInfo *info, const char *extension)
{
	for (uint32_t i = 0; i < info->enabledExtensionCount; i++) {
		if (!strcmp(info->ppEnabledExtensionNames[i], extension))
			return true;
	}
	return false;
}

static VkResult check_create_info(const VkDeviceCreateInfo *info)
{
	if (info->enabledLayerCount)
		return VK_ERROR_LAYER_NOT_PRESENT;
	for (uint32_t i = 0; i < info->enabledExtensionCount; i++) {
		if (!v4x_physical_device_has_extension(info->ppEnabledExtensionNames[i]))
			return VK_ERROR_EXTENSION_NOT_PRESENT;
	}
	if (!features_off(info->pEnabledFeatures))
		return VK_ERROR_FEATURE_NOT_PRESENT;
	if (info->queueCreateInfoCount != 1 || info->pQueueCreateInfos[0].queueFamilyIndex != 0 ||
		info->pQueueCreateInfos[0].queueCount != 1)
		return VK_ERROR_INITIALIZATION_FAILED;
	return device_exists ? VK_ERROR_TOO_MANY_OBJECTS : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator, VkDevice *pDevice)
{
	const VkResult result = check_create_info(pCreateInfo);
	if (result != VK_SUCCESS)
		return result;
	const VkInstance instance = physicalDevice->instance;
	const VkAllocationCallbacks *const allocator =
		pAllocator ? pAllocator : (instance->has_allocator ? &instance->allocator : NULL);
	struct VkDevice_T *const device = v4x_host_alloc(allocator, sizeof *device, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
	if (!device)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	if (pthread_mutex_init(&device->shaders_lock, NULL) != 0) {
		v4x_host_free(allocator, device);
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	const NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("vulkan4xenon: xsdk_gpu_open failed: 0x%08X", (unsigned)status);
		(void)pthread_mutex_destroy(&device->shaders_lock);
		v4x_host_free(allocator, device);
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	// The boot logo; without a display, as in Xenia, the device works without it.
	const NTSTATUS splash = xsdk_splash_begin();
	if (!NT_SUCCESS(splash))
		xsdk_log_write("vulkan4xenon: no boot logo: 0x%08X", (unsigned)splash);
	const VkResult blit = v4x_blit_create(&device->queue.blit);
	if (blit != VK_SUCCESS) {
		xsdk_splash_end();
		const NTSTATUS closed = xsdk_gpu_close();
		if (!NT_SUCCESS(closed))
			xsdk_log_write("vulkan4xenon: xsdk_gpu_close failed: 0x%08X", (unsigned)closed);
		(void)pthread_mutex_destroy(&device->shaders_lock);
		v4x_host_free(allocator, device);
		return blit;
	}
	if (allocator) {
		device->allocator = *allocator;
		device->has_allocator = true;
	}
	device->queue.device = device;
	device->sampler_anisotropy = pCreateInfo->pEnabledFeatures && pCreateInfo->pEnabledFeatures->samplerAnisotropy;
	device->mirror_clamp_to_edge = enabled(pCreateInfo, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);
	// Resolves clear depth to far and stencil to 0 until a render pass says otherwise.
	xsdk_gpu_set_depth_clear(1.0f, 0);
	device->queue.depth_clear = 1.0f;
	device->queue.stencil_clear = 0;
	device_exists = true;
	*pDevice = device;
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator)
{
	if (!device)
		return;
	xsdk_gpu_free(device->queue.index_copies);
	v4x_blit_destroy(&device->queue.blit);
	xsdk_splash_end();
	const NTSTATUS status = xsdk_gpu_close();
	if (!NT_SUCCESS(status))
		xsdk_log_write("vulkan4xenon: xsdk_gpu_close failed: 0x%08X", (unsigned)status);
	device_exists = false;
	v4x_shader_release(device);
	v4x_host_free(v4x_host_device_allocator(device, pAllocator), device);
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(
	VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue *pQueue)
{
	(void)queueFamilyIndex;
	(void)queueIndex;
	*pQueue = &device->queue;
}

VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice device)
{
	return vkQueueWaitIdle(&device->queue);
}
