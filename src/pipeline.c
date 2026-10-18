#include <string.h>

#include <vulkan/vulkan.h>

#include "format.h"
#include "host.h"
#include "objects.h"
#include "pipeline_state.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"

enum {
	// A pipeline cache's header: its size, its version, vendor and device IDs, then the cache's UUID.
	CACHE_HEADER_BYTES = 16 + VK_UUID_SIZE,
	// Vertex streams take a texture slot's group of fetch constants three at a time, from slot 31 down
	// (xsdk:include/xsdk/gpu.h, xsdk_gpu_set_vertices).
	STREAMS_A_SLOT = 3,
};

VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator, VkPipelineLayout *pPipelineLayout)
{
	if (pCreateInfo->setLayoutCount > V4X_MAX_DESCRIPTOR_SETS)
		return v4x_host_unsupported("more than 4 descriptor sets");
	for (uint32_t i = 0; i < pCreateInfo->pushConstantRangeCount; i++) {
		const VkPushConstantRange *const range = &pCreateInfo->pPushConstantRanges[i];
		if (range->offset + range->size > V4X_MAX_PUSH_CONSTANT_BYTES)
			return v4x_host_unsupported("push constants past 128 bytes");
	}
	PipelineLayout *const layout = v4x_host_alloc(
		v4x_host_device_allocator(device, pAllocator), sizeof *layout, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!layout)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	layout->set_count = pCreateInfo->setLayoutCount;
	for (uint32_t i = 0; i < pCreateInfo->setLayoutCount; i++)
		layout->sets[i] = v4x_object(pCreateInfo->pSetLayouts[i]);
	for (uint32_t i = 0; i < pCreateInfo->pushConstantRangeCount; i++)
		layout->push_stages |= pCreateInfo->pPushConstantRanges[i].stageFlags;
	*pPipelineLayout = v4x_handle(layout);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout(
	VkDevice device, VkPipelineLayout pipelineLayout, const VkAllocationCallbacks *pAllocator)
{
	v4x_host_free(v4x_host_device_allocator(device, pAllocator), v4x_object(pipelineLayout));
}

// The usage xost gives an input at this location (xsdk:src/xsc_resolve.c:344-346).
static void location_usage(uint32_t location, XsdkVertexElement *element)
{
	if (location == 0)
		element->usage = XSDK_USAGE_POSITION;
	else if (location == 1)
		element->usage = XSDK_USAGE_COLOR;
	else
		element->usage = XSDK_USAGE_TEXCOORD;
	element->usage_index = location >= 3 ? location - 2 : 0;
}

static VkResult find_stages(const VkGraphicsPipelineCreateInfo *info, Pipeline *pipeline)
{
	for (uint32_t i = 0; i < info->stageCount; i++) {
		const VkPipelineShaderStageCreateInfo *const stage = &info->pStages[i];
		if (strcmp(stage->pName, "main") || stage->pSpecializationInfo)
			return v4x_host_unsupported("entry points other than main, and specialization constants");
		const ShaderModule *const module = v4x_object(stage->module);
		if (stage->stage == VK_SHADER_STAGE_VERTEX_BIT)
			pipeline->vertex_source = module->shader;
		else if (stage->stage == VK_SHADER_STAGE_FRAGMENT_BIT)
			pipeline->pixel_source = module->shader;
		else
			return v4x_host_unsupported("stages other than vertex and fragment");
	}
	if (!pipeline->vertex_source || !pipeline->pixel_source)
		return v4x_host_unsupported("pipelines without both a vertex and a fragment shader");
	return VK_SUCCESS;
}

// Each binding becomes the next stream; each attribute an element of the vertex shader's layout. A binding at the
// instance's rate is fetched with a stride of 0, every vertex reading the element its stream starts at
// (xenia:src/xenia/gpu/shader_interpreter.cc:1013-1014), and each instance's draw moves the stream.
static VkResult lay_out_vertices(const VkPipelineVertexInputStateCreateInfo *input, Pipeline *pipeline)
{
	uint32_t strides[XSDK_VERTEX_STREAMS] = { 0 };
	for (uint32_t i = 0; i < input->vertexBindingDescriptionCount; i++) {
		const VkVertexInputBindingDescription *const binding = &input->pVertexBindingDescriptions[i];
		if (binding->stride % 4 || binding->stride > V4X_MAX_VERTEX_STRIDE_BYTES)
			return v4x_host_unsupported("strides not a multiple of 4, or past 1020 bytes");
		const bool per_instance = binding->inputRate == VK_VERTEX_INPUT_RATE_INSTANCE;
		pipeline->streams[i] = (VertexStream){
			.binding = binding->binding, .stride_bytes = binding->stride, .per_instance = per_instance
		};
		strides[i] = per_instance ? 0 : binding->stride;
	}
	pipeline->stream_count = input->vertexBindingDescriptionCount;
	XsdkVertexElement elements[V4X_MAX_VERTEX_ATTRIBUTES];
	for (uint32_t i = 0; i < input->vertexAttributeDescriptionCount; i++) {
		const VkVertexInputAttributeDescription *const attribute = &input->pVertexAttributeDescriptions[i];
		XsdkVertexElement *const element = &elements[i];
		*element = (XsdkVertexElement){ .offset_bytes = attribute->offset, .stream = XSDK_VERTEX_STREAMS };
		location_usage(attribute->location, element);
		if (!v4x_format_vertex(attribute->format, element) || attribute->offset % 4)
			return v4x_host_unsupported(
				"vertex formats docs/SUPPORT.md doesn't list, or at offsets not a multiple of 4");
		for (uint32_t s = 0; s < pipeline->stream_count; s++) {
			if (pipeline->streams[s].binding == attribute->binding)
				element->stream = s;
		}
	}
	const NTSTATUS status =
		xsdk_shader_set_vertex_layout(&pipeline->vertex, elements, input->vertexAttributeDescriptionCount, strides);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("vulkan4xenon: xsdk_shader_set_vertex_layout failed: 0x%08X", (unsigned)status);
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	return VK_SUCCESS;
}

static VkResult load_shaders(Pipeline *pipeline)
{
	NTSTATUS status = xsdk_shader_load(
		&pipeline->vertex, pipeline->vertex_source->container, pipeline->vertex_source->container_size_bytes);
	if (NT_SUCCESS(status))
		status = xsdk_shader_load(
			&pipeline->pixel, pipeline->pixel_source->container, pipeline->pixel_source->container_size_bytes);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("vulkan4xenon: xsdk_shader_load failed: 0x%08X", (unsigned)status);
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}
	if (pipeline->vertex.type != XSDK_SHADER_VERTEX || pipeline->pixel.type != XSDK_SHADER_PIXEL)
		return v4x_host_unsupported("a shader module in a stage other than the one it was compiled for");
	return VK_SUCCESS;
}

// Whether the layout has what a shader resource reads, for the shader's stage.
static bool layout_has(const PipelineLayout *layout, const V4xShaderResource *resource, VkShaderStageFlags stage)
{
	if (resource->kind == V4X_RESOURCE_PUSH_CONSTANTS || resource->kind == V4X_RESOURCE_PUSH_LOOP)
		return layout->push_stages & stage;
	if (resource->set >= layout->set_count || resource->binding >= V4X_MAX_DESCRIPTOR_BINDINGS)
		return false;
	const LayoutBinding *const binding = &layout->sets[resource->set]->bindings[resource->binding];
	if (!binding->used || !(binding->stages & stage))
		return false;
	if (resource->kind == V4X_RESOURCE_SAMPLER || resource->kind == V4X_RESOURCE_COMPARISON)
		return binding->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	return binding->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
		binding->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
}

// Each stage's resources, checked against the layout and the GPU's constants: a fragment shader's stay below the group
// xsdk keeps for gl_FragCoord (xsdk:include/xsdk/gpu.h, XSDK_PIXEL_CONSTANTS).
static VkResult gather_resources(const PipelineLayout *layout, const V4xShader *shader, bool vertex, Pipeline *pipeline)
{
	const VkShaderStageFlags stage = vertex ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
	const uint32_t constants = vertex ? V4X_FLOAT_CONSTANTS : XSDK_PIXEL_CONSTANTS;
	for (uint32_t i = 0; i < shader->literal_count; i++) {
		if (shader->literals[i].reg >= constants)
			return v4x_host_unsupported(
				"a shader's constants past its stage's: 256 a vertex shader's, 252 a fragment's");
	}
	for (uint32_t i = 0; i < shader->resource_count; i++) {
		const V4xShaderResource *const resource = &shader->resources[i];
		// No binding of the layout's: each instance's draw writes it (queue.c).
		if (resource->kind == V4X_RESOURCE_INSTANCE_INDEX) {
			pipeline->reads_instance_index = true;
			pipeline->instance_index_constant = resource->first;
			continue;
		}
		if (!layout_has(layout, resource, stage)) {
			xsdk_log_write("vulkan4xenon: the pipeline layout lacks set %u binding %u, kind %u, for the %s shader",
				(unsigned)resource->set, (unsigned)resource->binding, (unsigned)resource->kind,
				vertex ? "vertex" : "fragment");
			return VK_ERROR_INITIALIZATION_FAILED;
		}
		const bool loop = resource->kind == V4X_RESOURCE_PUSH_LOOP || resource->kind == V4X_RESOURCE_UNIFORM_LOOP;
		const bool block =
			resource->kind == V4X_RESOURCE_PUSH_CONSTANTS || resource->kind == V4X_RESOURCE_UNIFORM_BUFFER;
		if (resource->kind == V4X_RESOURCE_SAMPLER && resource->first >= V4X_MAX_SAMPLERS)
			return v4x_host_unsupported("more than 16 samplers a stage");
		if ((block || resource->kind == V4X_RESOURCE_COMPARISON) && resource->first + resource->count > constants)
			return v4x_host_unsupported(
				"a shader's constants past its stage's: 256 a vertex shader's, 252 a fragment's");
		if (loop &&
			(resource->count >= XSDK_LOOP_CONSTANTS ||
				(resource->kind == V4X_RESOURCE_PUSH_LOOP && resource->first + 4u > V4X_MAX_PUSH_CONSTANT_BYTES)))
			return v4x_host_unsupported("a loop counted past the push constants or the loop constants");
		pipeline->loop_count += loop;
		pipeline->resources[pipeline->resource_count++] = (StageResource){ .resource = *resource, .vertex = vertex };
	}
	return VK_SUCCESS;
}

// The fetch constants each stage's samplers take: one past the highest xost gave it.
static uint32_t sampler_slots(const Pipeline *pipeline, bool vertex)
{
	uint32_t slots = 0;
	for (uint32_t i = 0; i < pipeline->resource_count; i++) {
		const StageResource *const r = &pipeline->resources[i];
		if (r->resource.kind == V4X_RESOURCE_SAMPLER && r->vertex == vertex && r->resource.first >= slots)
			slots = r->resource.first + 1u;
	}
	return slots;
}

// The 32 fetch constants shared out: the fragment shader's samplers keep xost's slots from 0, the vertex shader's
// follow them, moved in its microcode, and the streams take a slot for every three bindings from 31 down, stream 0's
// always, as the layer's blits and clears draw from it (src/blit.c).
static VkResult share_fetch_constants(Pipeline *pipeline)
{
	const uint32_t fragment_slots = sampler_slots(pipeline, false);
	const uint32_t vertex_slots = sampler_slots(pipeline, true);
	const uint32_t streams = pipeline->stream_count ? pipeline->stream_count : 1;
	const uint32_t stream_slots = (streams + STREAMS_A_SLOT - 1) / STREAMS_A_SLOT;
	if (fragment_slots + vertex_slots + stream_slots > V4X_TEXTURE_SLOTS) {
		xsdk_log_write(
			"vulkan4xenon: not supported: %u fragment and %u vertex sampler slots with %u vertex bindings, "
			"past the %u fetch constants",
			(unsigned)fragment_slots, (unsigned)vertex_slots, (unsigned)pipeline->stream_count,
			(unsigned)V4X_TEXTURE_SLOTS);
		return VK_ERROR_FEATURE_NOT_PRESENT;
	}
	if (!vertex_slots)
		return VK_SUCCESS;
	const NTSTATUS status = xsdk_shader_move_textures(&pipeline->vertex, fragment_slots);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("vulkan4xenon: xsdk_shader_move_textures failed: 0x%08X", (unsigned)status);
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	for (uint32_t i = 0; i < pipeline->resource_count; i++) {
		StageResource *const r = &pipeline->resources[i];
		if (r->resource.kind == V4X_RESOURCE_SAMPLER && r->vertex)
			r->resource.first = (uint16_t)(r->resource.first + fragment_slots);
	}
	return VK_SUCCESS;
}

// Whether a vertex shader's push constants or uniform block holds a constant register.
static bool holds(const StageResource *stage_resource, uint32_t reg)
{
	const V4xShaderResource *const resource = &stage_resource->resource;
	const bool block = resource->kind == V4X_RESOURCE_PUSH_CONSTANTS || resource->kind == V4X_RESOURCE_UNIFORM_BUFFER;
	return stage_resource->vertex && block && reg >= resource->first &&
		reg < (uint32_t)resource->first + resource->count;
}

// Each clip distance's plane, in the vertex shader's push constants or a uniform block, as xost found it
// (xost:docs/xsc.md, "Interface conventions"); a draw reads it from there (queue.c).
static VkResult find_clip_planes(Pipeline *pipeline)
{
	const V4xShader *const shader = pipeline->vertex_source;
	for (uint32_t i = 0; i < shader->clip_plane_count; i++) {
		const V4xShaderClipPlane *const plane = &shader->clip_planes[i];
		uint32_t r = 0;
		while (r < pipeline->resource_count && !holds(&pipeline->resources[r], plane->reg))
			r++;
		const uint32_t offset_bytes =
			r < pipeline->resource_count ? (plane->reg - pipeline->resources[r].resource.first) * 16u : 0;
		const bool pushed =
			r < pipeline->resource_count && pipeline->resources[r].resource.kind == V4X_RESOURCE_PUSH_CONSTANTS;
		if (r == pipeline->resource_count || plane->plane >= XSDK_CLIP_PLANES ||
			(pushed && offset_bytes + 16 > V4X_MAX_PUSH_CONSTANT_BYTES)) {
			xsdk_log_write(
				"vulkan4xenon: clip distance %u's plane, constant %u, isn't in a block the vertex shader reads",
				(unsigned)plane->plane, (unsigned)plane->reg);
			return VK_ERROR_INITIALIZATION_FAILED;
		}
		pipeline->clip_planes[i] = (ClipPlane){ .plane = plane->plane, .resource = r, .offset_bytes = offset_bytes };
		pipeline->clip_plane_mask |= 1u << plane->plane;
	}
	pipeline->clip_plane_count = shader->clip_plane_count;
	return VK_SUCCESS;
}

static void free_pipeline(const VkAllocationCallbacks *allocator, Pipeline *pipeline)
{
	if (pipeline->vertex.microcode)
		xsdk_shader_free(&pipeline->vertex);
	if (pipeline->pixel.microcode)
		xsdk_shader_free(&pipeline->pixel);
	v4x_host_free(allocator, pipeline);
}

static VkResult create_pipeline(
	const VkGraphicsPipelineCreateInfo *info, const VkAllocationCallbacks *allocator, VkPipeline *out)
{
	Pipeline *const pipeline = v4x_host_alloc(allocator, sizeof *pipeline, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!pipeline)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	const RenderPassSubpasses *const render_pass = v4x_object(info->renderPass);
	VkResult result = v4x_pipeline_state(info, &render_pass->subpasses[info->subpass], pipeline);
	if (result == VK_SUCCESS)
		result = find_stages(info, pipeline);
	const PipelineLayout *const layout = v4x_object(info->layout);
	if (result == VK_SUCCESS)
		result = gather_resources(layout, pipeline->vertex_source, true, pipeline);
	if (result == VK_SUCCESS)
		result = gather_resources(layout, pipeline->pixel_source, false, pipeline);
	if (result == VK_SUCCESS)
		result = find_clip_planes(pipeline);
	if (result == VK_SUCCESS)
		result = load_shaders(pipeline);
	if (result == VK_SUCCESS)
		result = lay_out_vertices(info->pVertexInputState, pipeline);
	if (result == VK_SUCCESS)
		result = share_fetch_constants(pipeline);
	if (result != VK_SUCCESS) {
		free_pipeline(allocator, pipeline);
		return result;
	}
	*out = v4x_handle(pipeline);
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache pipelineCache,
	uint32_t createInfoCount, const VkGraphicsPipelineCreateInfo *pCreateInfos, const VkAllocationCallbacks *pAllocator,
	VkPipeline *pPipelines)
{
	(void)pipelineCache;
	const VkAllocationCallbacks *const allocator = v4x_host_device_allocator(device, pAllocator);
	VkResult result = VK_SUCCESS;
	for (uint32_t i = 0; i < createInfoCount; i++) {
		const VkResult created = create_pipeline(&pCreateInfos[i], allocator, &pPipelines[i]);
		if (created != VK_SUCCESS) {
			pPipelines[i] = VK_NULL_HANDLE;
			result = created;
		}
	}
	return result;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(
	VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks *pAllocator)
{
	Pipeline *const object = v4x_object(pipeline);
	if (!object)
		return;
	free_pipeline(v4x_host_device_allocator(device, pAllocator), object);
}

// Xenos has no compute stage, and the one queue family runs graphics alone (docs/SUPPORT.md, Device).
VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice device, VkPipelineCache pipelineCache,
	uint32_t createInfoCount, const VkComputePipelineCreateInfo *pCreateInfos, const VkAllocationCallbacks *pAllocator,
	VkPipeline *pPipelines)
{
	(void)device;
	(void)pipelineCache;
	(void)pCreateInfos;
	(void)pAllocator;
	for (uint32_t i = 0; i < createInfoCount; i++)
		pPipelines[i] = VK_NULL_HANDLE;
	return v4x_host_unsupported("compute pipelines");
}

// Pipelines come from the shader bundle and the layer's own cache of compiled shaders (milestone 13), so a pipeline
// cache holds nothing: its data is the header alone.
VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineCache(VkDevice device, const VkPipelineCacheCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator, VkPipelineCache *pPipelineCache)
{
	(void)pCreateInfo;
	PipelineCache *const cache =
		v4x_host_alloc(v4x_host_device_allocator(device, pAllocator), sizeof *cache, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!cache)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	*pPipelineCache = v4x_handle(cache);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineCache(
	VkDevice device, VkPipelineCache pipelineCache, const VkAllocationCallbacks *pAllocator)
{
	v4x_host_free(v4x_host_device_allocator(device, pAllocator), v4x_object(pipelineCache));
}

// The header's words go least significant byte first, whatever the CPU's order; the vendor, device and UUID are the
// zeros vkGetPhysicalDeviceProperties reports (Vulkan 1.0 spec, vkGetPipelineCacheData).
VKAPI_ATTR VkResult VKAPI_CALL vkGetPipelineCacheData(
	VkDevice device, VkPipelineCache pipelineCache, size_t *pDataSize, void *pData)
{
	(void)device;
	(void)pipelineCache;
	if (!pData) {
		*pDataSize = CACHE_HEADER_BYTES;
		return VK_SUCCESS;
	}
	if (*pDataSize < CACHE_HEADER_BYTES) {
		*pDataSize = 0;
		return VK_INCOMPLETE;
	}
	uint8_t *const header = pData;
	memset(header, 0, CACHE_HEADER_BYTES);
	header[0] = CACHE_HEADER_BYTES;
	header[4] = VK_PIPELINE_CACHE_HEADER_VERSION_ONE;
	*pDataSize = CACHE_HEADER_BYTES;
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkMergePipelineCaches(
	VkDevice device, VkPipelineCache dstCache, uint32_t srcCacheCount, const VkPipelineCache *pSrcCaches)
{
	(void)device;
	(void)dstCache;
	(void)srcCacheCount;
	(void)pSrcCaches;
	return VK_SUCCESS;
}
