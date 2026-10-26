#include <string.h>

#include <vulkan/vulkan.h>

// Apps link the entry points directly; these look the same functions up by name, as the loader would.

typedef enum {
	SCOPE_GLOBAL,
	SCOPE_INSTANCE,
	SCOPE_DEVICE,
} Scope;

typedef struct {
	const char *name;
	PFN_vkVoidFunction function;
	Scope scope;
} Entry;

#define ENTRY(name, scope) { #name, (PFN_vkVoidFunction)name, scope }

static const Entry entries[] = {
	ENTRY(vkCreateInstance, SCOPE_GLOBAL),
	ENTRY(vkEnumerateInstanceExtensionProperties, SCOPE_GLOBAL),
	ENTRY(vkEnumerateInstanceLayerProperties, SCOPE_GLOBAL),
	ENTRY(vkGetInstanceProcAddr, SCOPE_GLOBAL),
	ENTRY(vkDestroyInstance, SCOPE_INSTANCE),
	ENTRY(vkEnumeratePhysicalDevices, SCOPE_INSTANCE),
	ENTRY(vkGetPhysicalDeviceProperties, SCOPE_INSTANCE),
	ENTRY(vkGetPhysicalDeviceFeatures, SCOPE_INSTANCE),
	ENTRY(vkGetPhysicalDeviceQueueFamilyProperties, SCOPE_INSTANCE),
	ENTRY(vkGetPhysicalDeviceMemoryProperties, SCOPE_INSTANCE),
	ENTRY(vkGetPhysicalDeviceFormatProperties, SCOPE_INSTANCE),
	ENTRY(vkGetPhysicalDeviceImageFormatProperties, SCOPE_INSTANCE),
	ENTRY(vkGetPhysicalDeviceSparseImageFormatProperties, SCOPE_INSTANCE),
	ENTRY(vkEnumerateDeviceExtensionProperties, SCOPE_INSTANCE),
	ENTRY(vkEnumerateDeviceLayerProperties, SCOPE_INSTANCE),
	ENTRY(vkCreateDevice, SCOPE_INSTANCE),
	// A device-level command, its first parameter a VkDevice, so it finds itself: Vulkan-Hpp's dynamic dispatcher asks
	// vkGetDeviceProcAddr(device, "vkGetDeviceProcAddr") first, and called the null it got (Flycast, milestone 67).
	ENTRY(vkGetDeviceProcAddr, SCOPE_DEVICE),
	ENTRY(vkGetPhysicalDeviceDisplayPropertiesKHR, SCOPE_INSTANCE),
	ENTRY(vkGetPhysicalDeviceDisplayPlanePropertiesKHR, SCOPE_INSTANCE),
	ENTRY(vkGetDisplayPlaneSupportedDisplaysKHR, SCOPE_INSTANCE),
	ENTRY(vkGetDisplayModePropertiesKHR, SCOPE_INSTANCE),
	ENTRY(vkCreateDisplayModeKHR, SCOPE_INSTANCE),
	ENTRY(vkGetDisplayPlaneCapabilitiesKHR, SCOPE_INSTANCE),
	ENTRY(vkCreateDisplayPlaneSurfaceKHR, SCOPE_INSTANCE),
	ENTRY(vkDestroySurfaceKHR, SCOPE_INSTANCE),
	ENTRY(vkGetPhysicalDeviceSurfaceSupportKHR, SCOPE_INSTANCE),
	ENTRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR, SCOPE_INSTANCE),
	ENTRY(vkGetPhysicalDeviceSurfaceFormatsKHR, SCOPE_INSTANCE),
	ENTRY(vkGetPhysicalDeviceSurfacePresentModesKHR, SCOPE_INSTANCE),
	ENTRY(vkDestroyDevice, SCOPE_DEVICE),
	ENTRY(vkGetDeviceQueue, SCOPE_DEVICE),
	ENTRY(vkDeviceWaitIdle, SCOPE_DEVICE),
	ENTRY(vkAllocateMemory, SCOPE_DEVICE),
	ENTRY(vkFreeMemory, SCOPE_DEVICE),
	ENTRY(vkMapMemory, SCOPE_DEVICE),
	ENTRY(vkUnmapMemory, SCOPE_DEVICE),
	ENTRY(vkFlushMappedMemoryRanges, SCOPE_DEVICE),
	ENTRY(vkInvalidateMappedMemoryRanges, SCOPE_DEVICE),
	ENTRY(vkGetDeviceMemoryCommitment, SCOPE_DEVICE),
	ENTRY(vkCreateBuffer, SCOPE_DEVICE),
	ENTRY(vkDestroyBuffer, SCOPE_DEVICE),
	ENTRY(vkGetBufferMemoryRequirements, SCOPE_DEVICE),
	ENTRY(vkBindBufferMemory, SCOPE_DEVICE),
	ENTRY(vkCreateBufferView, SCOPE_DEVICE),
	ENTRY(vkDestroyBufferView, SCOPE_DEVICE),
	ENTRY(vkCreateImage, SCOPE_DEVICE),
	ENTRY(vkDestroyImage, SCOPE_DEVICE),
	ENTRY(vkGetImageMemoryRequirements, SCOPE_DEVICE),
	ENTRY(vkGetImageSparseMemoryRequirements, SCOPE_DEVICE),
	ENTRY(vkGetImageSubresourceLayout, SCOPE_DEVICE),
	ENTRY(vkBindImageMemory, SCOPE_DEVICE),
	ENTRY(vkCreateSampler, SCOPE_DEVICE),
	ENTRY(vkDestroySampler, SCOPE_DEVICE),
	ENTRY(vkCreateDescriptorSetLayout, SCOPE_DEVICE),
	ENTRY(vkDestroyDescriptorSetLayout, SCOPE_DEVICE),
	ENTRY(vkCreateDescriptorPool, SCOPE_DEVICE),
	ENTRY(vkDestroyDescriptorPool, SCOPE_DEVICE),
	ENTRY(vkResetDescriptorPool, SCOPE_DEVICE),
	ENTRY(vkAllocateDescriptorSets, SCOPE_DEVICE),
	ENTRY(vkFreeDescriptorSets, SCOPE_DEVICE),
	ENTRY(vkUpdateDescriptorSets, SCOPE_DEVICE),
	ENTRY(vkCreateShaderModule, SCOPE_DEVICE),
	ENTRY(vkDestroyShaderModule, SCOPE_DEVICE),
	ENTRY(vkCreatePipelineLayout, SCOPE_DEVICE),
	ENTRY(vkDestroyPipelineLayout, SCOPE_DEVICE),
	ENTRY(vkCreateGraphicsPipelines, SCOPE_DEVICE),
	ENTRY(vkCreateComputePipelines, SCOPE_DEVICE),
	ENTRY(vkDestroyPipeline, SCOPE_DEVICE),
	ENTRY(vkCreatePipelineCache, SCOPE_DEVICE),
	ENTRY(vkDestroyPipelineCache, SCOPE_DEVICE),
	ENTRY(vkGetPipelineCacheData, SCOPE_DEVICE),
	ENTRY(vkMergePipelineCaches, SCOPE_DEVICE),
	ENTRY(vkCreateImageView, SCOPE_DEVICE),
	ENTRY(vkDestroyImageView, SCOPE_DEVICE),
	ENTRY(vkCreateRenderPass, SCOPE_DEVICE),
	ENTRY(vkDestroyRenderPass, SCOPE_DEVICE),
	ENTRY(vkCreateFramebuffer, SCOPE_DEVICE),
	ENTRY(vkDestroyFramebuffer, SCOPE_DEVICE),
	ENTRY(vkGetRenderAreaGranularity, SCOPE_DEVICE),
	ENTRY(vkCreateCommandPool, SCOPE_DEVICE),
	ENTRY(vkDestroyCommandPool, SCOPE_DEVICE),
	ENTRY(vkResetCommandPool, SCOPE_DEVICE),
	ENTRY(vkAllocateCommandBuffers, SCOPE_DEVICE),
	ENTRY(vkFreeCommandBuffers, SCOPE_DEVICE),
	ENTRY(vkBeginCommandBuffer, SCOPE_DEVICE),
	ENTRY(vkEndCommandBuffer, SCOPE_DEVICE),
	ENTRY(vkResetCommandBuffer, SCOPE_DEVICE),
	ENTRY(vkCmdBeginRenderPass, SCOPE_DEVICE),
	ENTRY(vkCmdNextSubpass, SCOPE_DEVICE),
	ENTRY(vkCmdEndRenderPass, SCOPE_DEVICE),
	ENTRY(vkCmdExecuteCommands, SCOPE_DEVICE),
	ENTRY(vkCmdBindPipeline, SCOPE_DEVICE),
	ENTRY(vkCmdBindVertexBuffers, SCOPE_DEVICE),
	ENTRY(vkCmdDraw, SCOPE_DEVICE),
	ENTRY(vkCmdBindDescriptorSets, SCOPE_DEVICE),
	ENTRY(vkCmdPushConstants, SCOPE_DEVICE),
	ENTRY(vkCmdCopyBufferToImage, SCOPE_DEVICE),
	ENTRY(vkCmdCopyImageToBuffer, SCOPE_DEVICE),
	ENTRY(vkCmdCopyImage, SCOPE_DEVICE),
	ENTRY(vkCmdBlitImage, SCOPE_DEVICE),
	ENTRY(vkCmdClearColorImage, SCOPE_DEVICE),
	ENTRY(vkCmdClearDepthStencilImage, SCOPE_DEVICE),
	ENTRY(vkCmdClearAttachments, SCOPE_DEVICE),
	ENTRY(vkCmdCopyBuffer, SCOPE_DEVICE),
	ENTRY(vkCmdUpdateBuffer, SCOPE_DEVICE),
	ENTRY(vkCmdFillBuffer, SCOPE_DEVICE),
	ENTRY(vkCmdPipelineBarrier, SCOPE_DEVICE),
	ENTRY(vkCmdSetViewport, SCOPE_DEVICE),
	ENTRY(vkCmdSetScissor, SCOPE_DEVICE),
	ENTRY(vkCmdSetLineWidth, SCOPE_DEVICE),
	ENTRY(vkCmdSetDepthBias, SCOPE_DEVICE),
	ENTRY(vkCmdSetBlendConstants, SCOPE_DEVICE),
	ENTRY(vkCmdSetDepthBounds, SCOPE_DEVICE),
	ENTRY(vkCmdSetStencilCompareMask, SCOPE_DEVICE),
	ENTRY(vkCmdSetStencilWriteMask, SCOPE_DEVICE),
	ENTRY(vkCmdSetStencilReference, SCOPE_DEVICE),
	ENTRY(vkCmdBindIndexBuffer, SCOPE_DEVICE),
	ENTRY(vkCmdDrawIndexed, SCOPE_DEVICE),
	ENTRY(vkCmdDrawIndirect, SCOPE_DEVICE),
	ENTRY(vkCmdDrawIndexedIndirect, SCOPE_DEVICE),
	ENTRY(vkCmdDispatch, SCOPE_DEVICE),
	ENTRY(vkCmdDispatchIndirect, SCOPE_DEVICE),
	ENTRY(vkCmdResolveImage, SCOPE_DEVICE),
	ENTRY(vkQueueSubmit, SCOPE_DEVICE),
	ENTRY(vkQueueBindSparse, SCOPE_DEVICE),
	ENTRY(vkQueueWaitIdle, SCOPE_DEVICE),
	ENTRY(vkCreateFence, SCOPE_DEVICE),
	ENTRY(vkDestroyFence, SCOPE_DEVICE),
	ENTRY(vkResetFences, SCOPE_DEVICE),
	ENTRY(vkGetFenceStatus, SCOPE_DEVICE),
	ENTRY(vkWaitForFences, SCOPE_DEVICE),
	ENTRY(vkCreateSemaphore, SCOPE_DEVICE),
	ENTRY(vkDestroySemaphore, SCOPE_DEVICE),
	ENTRY(vkCreateEvent, SCOPE_DEVICE),
	ENTRY(vkDestroyEvent, SCOPE_DEVICE),
	ENTRY(vkGetEventStatus, SCOPE_DEVICE),
	ENTRY(vkSetEvent, SCOPE_DEVICE),
	ENTRY(vkResetEvent, SCOPE_DEVICE),
	ENTRY(vkCmdSetEvent, SCOPE_DEVICE),
	ENTRY(vkCmdResetEvent, SCOPE_DEVICE),
	ENTRY(vkCmdWaitEvents, SCOPE_DEVICE),
	ENTRY(vkCreateQueryPool, SCOPE_DEVICE),
	ENTRY(vkDestroyQueryPool, SCOPE_DEVICE),
	ENTRY(vkGetQueryPoolResults, SCOPE_DEVICE),
	ENTRY(vkCmdResetQueryPool, SCOPE_DEVICE),
	ENTRY(vkCmdBeginQuery, SCOPE_DEVICE),
	ENTRY(vkCmdEndQuery, SCOPE_DEVICE),
	ENTRY(vkCmdWriteTimestamp, SCOPE_DEVICE),
	ENTRY(vkCmdCopyQueryPoolResults, SCOPE_DEVICE),
	ENTRY(vkCreateSwapchainKHR, SCOPE_DEVICE),
	ENTRY(vkDestroySwapchainKHR, SCOPE_DEVICE),
	ENTRY(vkGetSwapchainImagesKHR, SCOPE_DEVICE),
	ENTRY(vkAcquireNextImageKHR, SCOPE_DEVICE),
	ENTRY(vkQueuePresentKHR, SCOPE_DEVICE),
};

static const Entry *find(const char *name)
{
	for (uint32_t i = 0; i < sizeof entries / sizeof entries[0]; i++) {
		if (!strcmp(name, entries[i].name))
			return &entries[i];
	}
	return NULL;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *pName)
{
	const Entry *const entry = find(pName);
	if (!entry || (!instance && entry->scope != SCOPE_GLOBAL))
		return NULL;
	return entry->function;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *pName)
{
	(void)device;
	const Entry *const entry = find(pName);
	return entry && entry->scope == SCOPE_DEVICE ? entry->function : NULL;
}
