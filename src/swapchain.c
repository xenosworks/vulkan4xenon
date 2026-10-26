#include <assert.h>

#include <vulkan/vulkan.h>

#include "host.h"
#include "objects.h"
#include "profile.h"
#include "sync.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/texture.h"

// The swapchain is xsdk_present's front buffers, shown in order at vblank.

enum {
	FIFO_SWAP_INTERVAL = 1,
	NANOSECONDS_PER_MILLISECOND = 1000000,
	// xsdk_present_begin's timeout is 32 bits of milliseconds; longer waits use this.
	LONGEST_WAIT_MS = 60000,
};

// xsdk_present runs one display, so one swapchain at a time.
static bool swapchain_exists;

static VkResult check_create_info(const VkSwapchainCreateInfoKHR *info)
{
	if (swapchain_exists || info->oldSwapchain)
		return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
	if (info->imageFormat != VK_FORMAT_B8G8R8A8_UNORM || info->imageColorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR ||
		info->imageExtent.width != V4X_DISPLAY_WIDTH || info->imageExtent.height != V4X_DISPLAY_HEIGHT ||
		info->imageArrayLayers != 1 || info->imageUsage != VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT ||
		info->preTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ||
		info->compositeAlpha != VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR || info->presentMode != VK_PRESENT_MODE_FIFO_KHR)
		return v4x_host_unsupported("swapchains other than the surface's format, size and FIFO mode");
	return VK_SUCCESS;
}

// A front buffer holds what a resolve into a tiled B8G8R8A8 texture of its size writes (xsdk:src/gpu_resolve.c), so
// it's sampled as one when a render pass loads it.
static void describe_image(Image *image, uint32_t address, uint32_t width, uint32_t height)
{
	*image = (Image){ .address = address,
		.width = width,
		.height = height,
		.format = VK_FORMAT_B8G8R8A8_UNORM,
		.samples = 1,
		.is_attachment = true };
	const NTSTATUS status = xsdk_texture_init(&image->texture, XSDK_FORMAT_B8G8R8A8, width, height, 1, true);
	assert(NT_SUCCESS(status) && image->texture.size_bytes == xsdk_gpu_resolve_bytes(width, height));
	(void)status;
	xsdk_texture_attach(&image->texture, xsdk_present_pixels(address));
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo,
	const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain)
{
	const VkResult result = check_create_info(pCreateInfo);
	if (result != VK_SUCCESS)
		return result;
	Swapchain *const swapchain = v4x_host_alloc(
		v4x_host_device_allocator(device, pAllocator), sizeof *swapchain, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!swapchain)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	uint32_t count = pCreateInfo->minImageCount;
	count = count < V4X_MIN_SWAPCHAIN_IMAGES ? V4X_MIN_SWAPCHAIN_IMAGES : count;
	count = count > V4X_MAX_SWAPCHAIN_IMAGES ? V4X_MAX_SWAPCHAIN_IMAGES : count;
	uint32_t width = 0;
	uint32_t height = 0;
	const NTSTATUS status = xsdk_present_open(XSDK_DISPLAY_720P, count, FIFO_SWAP_INTERVAL, &width, &height);
	if (!NT_SUCCESS(status) || width != V4X_DISPLAY_WIDTH || height != V4X_DISPLAY_HEIGHT) {
		xsdk_log_write("vulkan4xenon: xsdk_present_open failed: 0x%08X, %ux%u", (unsigned)status, (unsigned)width,
			(unsigned)height);
		if (NT_SUCCESS(status))
			xsdk_present_close();
		v4x_host_free(v4x_host_device_allocator(device, pAllocator), swapchain);
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	swapchain->image_count = count;
	for (uint32_t i = 0; i < count; i++)
		describe_image(&swapchain->images[i], xsdk_present_buffer_address(i), width, height);
	swapchain_exists = true;
	*pSwapchain = v4x_handle(swapchain);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(
	VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *pAllocator)
{
	Swapchain *const object = v4x_object(swapchain);
	if (!object)
		return;
	xsdk_present_close();
	swapchain_exists = false;
	v4x_host_free(v4x_host_device_allocator(device, pAllocator), object);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(
	VkDevice device, VkSwapchainKHR swapchain, uint32_t *pSwapchainImageCount, VkImage *pSwapchainImages)
{
	(void)device;
	const Swapchain *const object = v4x_object(swapchain);
	VkImage images[V4X_MAX_SWAPCHAIN_IMAGES];
	for (uint32_t i = 0; i < object->image_count; i++)
		images[i] = v4x_handle(&object->images[i]);
	return v4x_host_enumerate(pSwapchainImageCount, pSwapchainImages, images, object->image_count, sizeof images[0]);
}

static uint32_t timeout_ms(uint64_t timeout_ns)
{
	const uint64_t ms = (timeout_ns + NANOSECONDS_PER_MILLISECOND - 1) / NANOSECONDS_PER_MILLISECOND;
	return ms > LONGEST_WAIT_MS ? LONGEST_WAIT_MS : (uint32_t)ms;
}

// Waits on the CPU for the next buffer to leave the screen, so the semaphore and fence are signalled on return.
VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
	VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex)
{
	(void)device;
	(void)semaphore;
	Swapchain *const object = v4x_object(swapchain);
	// xsdk_present hands out one buffer until it's presented (docs/SUPPORT.md).
	if (object->acquired)
		return timeout ? VK_TIMEOUT : VK_NOT_READY;
	uint32_t address = 0;
	NTSTATUS status;
	do
		status = xsdk_present_begin(&address, timeout_ms(timeout));
	while (status == STATUS_IO_TIMEOUT && timeout == UINT64_MAX);
	if (status == STATUS_IO_TIMEOUT)
		return timeout ? VK_TIMEOUT : VK_NOT_READY;
	for (uint32_t i = 0; i < object->image_count; i++) {
		if (object->images[i].address == address)
			object->acquired_index = i;
	}
	object->acquired = true;
	v4x_sync_signal(v4x_object(fence));
	*pImageIndex = object->acquired_index;
	return VK_SUCCESS;
}

// While the Guide is open, XAM draws it in eDRAM between frames (xsdk:include/xsdk/gpu.h, XsdkGpuStats).
static void forget_edram_after_system_draws(VkQueue queue)
{
	XsdkGpuStats stats;
	xsdk_gpu_stats(&stats);
	if (stats.system_draws == queue->system_draws)
		return;
	queue->system_draws = stats.system_draws;
	for (uint32_t i = 0; i < V4X_EDRAM_COLORS; i++)
		queue->colors[i] = (EdramPart){ .contents = EDRAM_UNKNOWN };
	for (uint32_t i = 0; i < V4X_EDRAM_DEPTHS; i++)
		queue->depths[i] = (EdramPart){ .contents = EDRAM_UNKNOWN };
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
	VkResult result = VK_SUCCESS;
	for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
		Swapchain *const swapchain = v4x_object(pPresentInfo->pSwapchains[i]);
		VkResult presented = VK_SUCCESS;
		if (!swapchain->acquired || pPresentInfo->pImageIndices[i] != swapchain->acquired_index) {
			presented = VK_ERROR_OUT_OF_DATE_KHR;
		} else {
			xsdk_present_end();
			swapchain->acquired = false;
		}
		if (pPresentInfo->pResults)
			pPresentInfo->pResults[i] = presented;
		if (presented != VK_SUCCESS)
			result = presented;
	}
	forget_edram_after_system_draws(queue);
	v4x_profile_present();
	return result;
}
