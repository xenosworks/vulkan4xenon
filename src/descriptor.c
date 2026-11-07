#include <vulkan/vulkan.h>

#include "host.h"
#include "objects.h"

// Descriptor sets are host memory: a descriptor a binding, read when a draw replays.

static bool supported_type(VkDescriptorType type)
{
	return type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
		type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice device,
	const VkDescriptorSetLayoutCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator,
	VkDescriptorSetLayout *pSetLayout)
{
	for (uint32_t i = 0; i < pCreateInfo->bindingCount; i++) {
		const VkDescriptorSetLayoutBinding *const binding = &pCreateInfo->pBindings[i];
		if (binding->binding >= V4X_MAX_DESCRIPTOR_BINDINGS || binding->descriptorCount > 1 ||
			binding->pImmutableSamplers || !supported_type(binding->descriptorType))
			return v4x_host_unsupported("bindings other than one uniform buffer or combined image sampler, below 32");
	}
	DescriptorSetLayout *const layout = v4x_host_alloc(
		v4x_host_device_allocator(device, pAllocator), sizeof *layout, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!layout)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	for (uint32_t i = 0; i < pCreateInfo->bindingCount; i++) {
		const VkDescriptorSetLayoutBinding *const binding = &pCreateInfo->pBindings[i];
		layout->bindings[binding->binding] = (LayoutBinding){
			.type = binding->descriptorType,
			.stages = binding->stageFlags,
			.used = binding->descriptorCount == 1,
		};
	}
	// Dynamic offsets come in binding order (Vulkan 1.0 spec, vkCmdBindDescriptorSets).
	for (uint32_t b = 0; b < V4X_MAX_DESCRIPTOR_BINDINGS; b++) {
		LayoutBinding *const binding = &layout->bindings[b];
		if (binding->used && binding->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
			binding->dynamic_index = layout->dynamic_count++;
	}
	*pSetLayout = v4x_handle(layout);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorSetLayout(
	VkDevice device, VkDescriptorSetLayout descriptorSetLayout, const VkAllocationCallbacks *pAllocator)
{
	v4x_host_free(v4x_host_device_allocator(device, pAllocator), v4x_object(descriptorSetLayout));
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorPool(VkDevice device, const VkDescriptorPoolCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator, VkDescriptorPool *pDescriptorPool)
{
	(void)pCreateInfo;
	DescriptorPool *const pool =
		v4x_host_alloc(v4x_host_device_allocator(device, pAllocator), sizeof *pool, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!pool)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	*pDescriptorPool = v4x_handle(pool);
	return VK_SUCCESS;
}

static void free_sets(VkDevice device, DescriptorPool *pool)
{
	const VkAllocationCallbacks *const allocator = v4x_host_device_allocator(device, NULL);
	while (pool->sets) {
		DescriptorSet *const next = pool->sets->next;
		v4x_host_free(allocator, pool->sets);
		pool->sets = next;
	}
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorPool(
	VkDevice device, VkDescriptorPool descriptorPool, const VkAllocationCallbacks *pAllocator)
{
	DescriptorPool *const pool = v4x_object(descriptorPool);
	if (!pool)
		return;
	free_sets(device, pool);
	v4x_host_free(v4x_host_device_allocator(device, pAllocator), pool);
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetDescriptorPool(
	VkDevice device, VkDescriptorPool descriptorPool, VkDescriptorPoolResetFlags flags)
{
	(void)flags;
	free_sets(device, v4x_object(descriptorPool));
	return VK_SUCCESS;
}

static void unlink_set(DescriptorPool *pool, const DescriptorSet *set)
{
	DescriptorSet **link = &pool->sets;
	while (*link != set)
		link = &(*link)->next;
	*link = set->next;
}

VKAPI_ATTR VkResult VKAPI_CALL vkFreeDescriptorSets(VkDevice device, VkDescriptorPool descriptorPool,
	uint32_t descriptorSetCount, const VkDescriptorSet *pDescriptorSets)
{
	DescriptorPool *const pool = v4x_object(descriptorPool);
	for (uint32_t i = 0; i < descriptorSetCount; i++) {
		DescriptorSet *const set = v4x_object(pDescriptorSets[i]);
		if (!set)
			continue;
		unlink_set(pool, set);
		v4x_host_free(v4x_host_device_allocator(device, NULL), set);
	}
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(
	VkDevice device, const VkDescriptorSetAllocateInfo *pAllocateInfo, VkDescriptorSet *pDescriptorSets)
{
	DescriptorPool *const pool = v4x_object(pAllocateInfo->descriptorPool);
	for (uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; i++) {
		DescriptorSet *const set =
			v4x_host_alloc(v4x_host_device_allocator(device, NULL), sizeof *set, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
		if (!set) {
			(void)vkFreeDescriptorSets(device, pAllocateInfo->descriptorPool, i, pDescriptorSets);
			for (uint32_t j = 0; j < pAllocateInfo->descriptorSetCount; j++)
				pDescriptorSets[j] = VK_NULL_HANDLE;
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		}
		set->layout = v4x_object(pAllocateInfo->pSetLayouts[i]);
		set->next = pool->sets;
		pool->sets = set;
		pDescriptorSets[i] = v4x_handle(set);
	}
	return VK_SUCCESS;
}

static void write_descriptor(Descriptor *descriptor, const VkWriteDescriptorSet *write, uint32_t i)
{
	descriptor->type = write->descriptorType;
	if (write->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
		const ImageView *const view = v4x_object(write->pImageInfo[i].imageView);
		descriptor->image = view->image;
		descriptor->texture = &view->texture;
		descriptor->sampler = v4x_object(write->pImageInfo[i].sampler);
		return;
	}
	const VkDescriptorBufferInfo *const info = &write->pBufferInfo[i];
	const Buffer *const buffer = v4x_object(info->buffer);
	const VkDeviceSize range = info->range == VK_WHOLE_SIZE ? buffer->size - info->offset : info->range;
	descriptor->buffer = buffer->memory->pointer + (buffer->offset_bytes + (uint32_t)info->offset) / 4;
	descriptor->buffer_address = buffer->memory->address + buffer->offset_bytes + (uint32_t)info->offset;
	descriptor->range_bytes = (uint32_t)range;
}

// A write of several descriptors fills consecutive bindings, one descriptor each here.
VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice device, uint32_t descriptorWriteCount,
	const VkWriteDescriptorSet *pDescriptorWrites, uint32_t descriptorCopyCount,
	const VkCopyDescriptorSet *pDescriptorCopies)
{
	(void)device;
	for (uint32_t w = 0; w < descriptorWriteCount; w++) {
		const VkWriteDescriptorSet *const write = &pDescriptorWrites[w];
		DescriptorSet *const set = v4x_object(write->dstSet);
		for (uint32_t i = 0; i < write->descriptorCount; i++)
			write_descriptor(&set->descriptors[write->dstBinding + i], write, i);
	}
	for (uint32_t c = 0; c < descriptorCopyCount; c++) {
		const VkCopyDescriptorSet *const copy = &pDescriptorCopies[c];
		const DescriptorSet *const source = v4x_object(copy->srcSet);
		DescriptorSet *const destination = v4x_object(copy->dstSet);
		for (uint32_t i = 0; i < copy->descriptorCount; i++)
			destination->descriptors[copy->dstBinding + i] = source->descriptors[copy->srcBinding + i];
	}
}
