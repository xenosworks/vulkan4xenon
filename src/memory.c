#include <malloc.h>

#include <vulkan/vulkan.h>

#include "host.h"
#include "objects.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"

enum {
	// A vertex fetch constant needs a dword address (xsdk:src/gpu_draw.c), and the GPU loads uniform blocks from
	// 64-byte aligned memory (xsdk:include/xsdk/gpu.h).
	BUFFER_ALIGNMENT = 64,
	// A buffer only copied to and from, at a texture fetch's 4 KB, so a copy into an image can draw its rows
	// (command_buffer.c, drawn_upload).
	TRANSFER_ALIGNMENT = 4096,
	// Type 0 is the GPU's write-combined memory; type 1 the CPU's cached memory, for buffers only copied to and from
	// (physical_device.c, vkGetPhysicalDeviceMemoryProperties).
	MEMORY_TYPE_GPU = 0,
	MEMORY_TYPE_CACHED = 1,
	// Cache lines, so a copy's reads don't share a line with anything else.
	CACHED_ALIGNMENT = 128,
	// Copies from and to cached memory run on the CPU, so a buffer only copied to and from may take it.
	TRANSFER_USAGE = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
};

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *pAllocateInfo,
	const VkAllocationCallbacks *pAllocator, VkDeviceMemory *pMemory)
{
	const uint32_t type = pAllocateInfo->memoryTypeIndex;
	if (type != MEMORY_TYPE_GPU && type != MEMORY_TYPE_CACHED)
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	if (pAllocateInfo->allocationSize > UINT32_MAX)
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	const VkAllocationCallbacks *const allocator = v4x_host_device_allocator(device, pAllocator);
	DeviceMemory *const memory = v4x_host_alloc(allocator, sizeof *memory, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!memory)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	memory->size_bytes = (uint32_t)pAllocateInfo->allocationSize;
	memory->cached = type == MEMORY_TYPE_CACHED;
	memory->pointer =
		memory->cached ? memalign(CACHED_ALIGNMENT, memory->size_bytes) : xsdk_gpu_alloc(memory->size_bytes);
	if (!memory->pointer) {
		xsdk_log_write("vulkan4xenon: %u bytes of %s memory not allocated", (unsigned)memory->size_bytes,
			memory->cached ? "cached" : "GPU");
		v4x_host_free(allocator, memory);
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}
	memory->address = memory->cached ? 0 : xsdk_gpu_address(memory->pointer);
	*pMemory = v4x_handle(memory);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *pAllocator)
{
	DeviceMemory *const object = v4x_object(memory);
	if (!object)
		return;
	if (object->cached)
		free((void *)(uintptr_t)object->pointer);
	else
		xsdk_gpu_free(object->pointer);
	v4x_host_free(v4x_host_device_allocator(device, pAllocator), object);
}

// Write-combined memory: memcpy into it is safe, and an 8-byte store needs an 8-byte-aligned address (docs/SUPPORT.md).
VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
	VkDeviceSize size, VkMemoryMapFlags flags, void **ppData)
{
	(void)device;
	(void)size;
	(void)flags;
	const DeviceMemory *const object = v4x_object(memory);
	*ppData = (uint8_t *)(uintptr_t)object->pointer + offset;
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice device, VkDeviceMemory memory)
{
	(void)device;
	(void)memory;
}

// Coherent: vkQueueSubmit drains the CPU's write-combining before the GPU reads.
VKAPI_ATTR VkResult VKAPI_CALL vkFlushMappedMemoryRanges(
	VkDevice device, uint32_t memoryRangeCount, const VkMappedMemoryRange *pMemoryRanges)
{
	(void)device;
	(void)memoryRangeCount;
	(void)pMemoryRanges;
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkInvalidateMappedMemoryRanges(
	VkDevice device, uint32_t memoryRangeCount, const VkMappedMemoryRange *pMemoryRanges)
{
	(void)device;
	(void)memoryRangeCount;
	(void)pMemoryRanges;
	return VK_SUCCESS;
}

// No memory type is lazily allocated, so all of an allocation is committed.
VKAPI_ATTR void VKAPI_CALL vkGetDeviceMemoryCommitment(
	VkDevice device, VkDeviceMemory memory, VkDeviceSize *pCommittedMemoryInBytes)
{
	(void)device;
	*pCommittedMemoryInBytes = ((const DeviceMemory *)v4x_object(memory))->size_bytes;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(
	VkDevice device, const VkBufferCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer)
{
	if (pCreateInfo->flags)
		return v4x_host_unsupported("sparse buffers");
	Buffer *const buffer = v4x_host_alloc(
		v4x_host_device_allocator(device, pAllocator), sizeof *buffer, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!buffer)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	buffer->size = pCreateInfo->size;
	buffer->usage = pCreateInfo->usage;
	*pBuffer = v4x_handle(buffer);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *pAllocator)
{
	v4x_host_free(v4x_host_device_allocator(device, pAllocator), v4x_object(buffer));
}

VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(
	VkDevice device, VkBuffer buffer, VkMemoryRequirements *pMemoryRequirements)
{
	(void)device;
	const Buffer *const object = v4x_object(buffer);
	const bool transfer = !(object->usage & ~TRANSFER_USAGE);
	*pMemoryRequirements = (VkMemoryRequirements){
		.size = (object->size + BUFFER_ALIGNMENT - 1) & ~(VkDeviceSize)(BUFFER_ALIGNMENT - 1),
		.alignment = transfer ? TRANSFER_ALIGNMENT : BUFFER_ALIGNMENT,
		.memoryTypeBits = 1u << MEMORY_TYPE_GPU | (transfer ? 1u << MEMORY_TYPE_CACHED : 0),
	};
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(
	VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize memoryOffset)
{
	(void)device;
	Buffer *const object = v4x_object(buffer);
	const DeviceMemory *const bound = v4x_object(memory);
	// Invalid use: only buffers copied to and from take cached memory (vkGetBufferMemoryRequirements).
	if (bound->cached && (object->usage & ~TRANSFER_USAGE)) {
		xsdk_log_write("vulkan4xenon: a buffer the GPU reads bound to cached memory");
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}
	object->memory = bound;
	object->offset_bytes = (uint32_t)memoryOffset;
	return VK_SUCCESS;
}

// No format has texel buffer features (docs/SUPPORT.md, Formats), so no view is ever made.
VKAPI_ATTR VkResult VKAPI_CALL vkCreateBufferView(VkDevice device, const VkBufferViewCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator, VkBufferView *pView)
{
	(void)device;
	(void)pCreateInfo;
	(void)pAllocator;
	(void)pView;
	return v4x_host_unsupported("buffer views");
}

VKAPI_ATTR void VKAPI_CALL vkDestroyBufferView(
	VkDevice device, VkBufferView bufferView, const VkAllocationCallbacks *pAllocator)
{
	(void)device;
	(void)bufferView;
	(void)pAllocator;
}
