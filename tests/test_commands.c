#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"

// Milestone 19's commands that answer without drawing, and those refused for features the layer doesn't report.

enum {
	// A pipeline cache's header: its size, its version, vendor and device IDs, then the cache's UUID.
	CACHE_HEADER_BYTES = 16 + VK_UUID_SIZE,
	MEMORY_BYTES = 4096,
	IMAGE_SIDE = 64,
	// A count the queries must overwrite.
	UNTOUCHED = 7,
};

static const char *const step_1_commands[] = {
	"vkGetDeviceMemoryCommitment",
	"vkCreateBufferView",
	"vkDestroyBufferView",
	"vkGetImageSparseMemoryRequirements",
	"vkCreateComputePipelines",
	"vkCreatePipelineCache",
	"vkDestroyPipelineCache",
	"vkGetPipelineCacheData",
	"vkMergePipelineCaches",
	"vkGetRenderAreaGranularity",
	"vkCmdSetLineWidth",
	"vkCmdSetDepthBounds",
	"vkCmdDispatch",
	"vkCmdDispatchIndirect",
	"vkCmdResolveImage",
	"vkQueueBindSparse",
	"vkCreateEvent",
	"vkDestroyEvent",
	"vkGetEventStatus",
	"vkSetEvent",
	"vkResetEvent",
};

static void check_names(const Harness *harness)
{
	bool found = vkGetInstanceProcAddr(harness->instance, "vkGetPhysicalDeviceSparseImageFormatProperties") ==
		(PFN_vkVoidFunction)vkGetPhysicalDeviceSparseImageFormatProperties;
	for (uint32_t i = 0; i < sizeof step_1_commands / sizeof step_1_commands[0]; i++)
		found = found && vkGetDeviceProcAddr(harness->device, step_1_commands[i]);
	check("commands: step 1's 22 commands are found by name", found);
	check("commands: vkGetDeviceProcAddr finds itself, as Vulkan-Hpp's dispatcher asks first",
		vkGetDeviceProcAddr(harness->device, "vkGetDeviceProcAddr") == (PFN_vkVoidFunction)vkGetDeviceProcAddr &&
			vkGetInstanceProcAddr(harness->instance, "vkGetDeviceProcAddr") == (PFN_vkVoidFunction)vkGetDeviceProcAddr);
}

static void check_pipeline_cache(const Harness *harness)
{
	// Its words least significant byte first: its size, then version one; the vendor, device and UUID 0.
	const uint8_t header[CACHE_HEADER_BYTES] = { CACHE_HEADER_BYTES, 0, 0, 0, VK_PIPELINE_CACHE_HEADER_VERSION_ONE };
	// Another device's data, which a cache ignores.
	uint8_t foreign[CACHE_HEADER_BYTES];
	memcpy(foreign, header, sizeof header);
	foreign[8] = 0x02;
	foreign[16] = 0xAB;
	VkPipelineCacheCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
		.initialDataSize = sizeof foreign,
		.pInitialData = foreign,
	};
	VkPipelineCache caches[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
	bool made = harness_ok("vkCreatePipelineCache", vkCreatePipelineCache(harness->device, &info, NULL, &caches[0]));
	size_t size = 0;
	const bool sized =
		made && vkGetPipelineCacheData(harness->device, caches[0], &size, NULL) == VK_SUCCESS && size == sizeof header;
	// A byte past the header shows nothing more is written.
	uint8_t data[CACHE_HEADER_BYTES + 1];
	memset(data, 0xFF, sizeof data);
	size = sizeof data;
	const bool read = made && vkGetPipelineCacheData(harness->device, caches[0], &size, data) == VK_SUCCESS &&
		size == sizeof header && !memcmp(data, header, sizeof header) && data[sizeof header] == 0xFF;
	check("commands: a pipeline cache's data is a header of 32 bytes, its words least significant byte first",
		sized && read);
	memset(data, 0xFF, sizeof data);
	size = sizeof header - 1;
	check("commands: a pipeline cache's data too small for its header writes nothing",
		made && vkGetPipelineCacheData(harness->device, caches[0], &size, data) == VK_INCOMPLETE && !size &&
			data[0] == 0xFF);
	info.pInitialData = header;
	made = made && harness_ok("vkCreatePipelineCache", vkCreatePipelineCache(harness->device, &info, NULL, &caches[1]));
	check("commands: a pipeline cache made from another's data merges into it",
		made && vkMergePipelineCaches(harness->device, caches[0], 1, &caches[1]) == VK_SUCCESS);
	vkDestroyPipelineCache(harness->device, caches[0], NULL);
	vkDestroyPipelineCache(harness->device, caches[1], NULL);
}

static void check_events(const Harness *harness)
{
	const VkEventCreateInfo info = { .sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO };
	VkEvent event = VK_NULL_HANDLE;
	const bool made = harness_ok("vkCreateEvent", vkCreateEvent(harness->device, &info, NULL, &event));
	const bool starts_reset = made && vkGetEventStatus(harness->device, event) == VK_EVENT_RESET;
	const bool set = made && vkSetEvent(harness->device, event) == VK_SUCCESS &&
		vkGetEventStatus(harness->device, event) == VK_EVENT_SET;
	const bool reset = made && vkResetEvent(harness->device, event) == VK_SUCCESS &&
		vkGetEventStatus(harness->device, event) == VK_EVENT_RESET;
	check("commands: an event starts reset, and the host sets and resets it", starts_reset && set && reset);
	vkDestroyEvent(harness->device, event, NULL);
}

static void check_answers(const Harness *harness)
{
	VkRenderPass render_pass = VK_NULL_HANDLE;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES] = { VK_NULL_HANDLE };
	VkExtent2D granularity = { 0, 0 };
	if (harness_ok("render pass", harness_render_pass(harness, &render_pass, framebuffers))) {
		vkGetRenderAreaGranularity(harness->device, render_pass, &granularity);
		harness_destroy_render_pass(harness, render_pass, framebuffers);
	}
	check("commands: a render pass's render area granularity is 8x8, the resolve's grid (milestone 27)",
		granularity.width == 8 && granularity.height == 8);
	const VkMemoryAllocateInfo allocate = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = MEMORY_BYTES,
	};
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkDeviceSize committed = 0;
	if (harness_ok("vkAllocateMemory", vkAllocateMemory(harness->device, &allocate, NULL, &memory))) {
		vkGetDeviceMemoryCommitment(harness->device, memory, &committed);
		vkFreeMemory(harness->device, memory, NULL);
	}
	check("commands: an allocation is committed in full", committed == MEMORY_BYTES);
	VkImage image;
	VkDeviceMemory image_memory;
	VkImageView view;
	uint32_t image_count = UNTOUCHED;
	if (harness_ok("image",
			harness_image(harness, VK_FORMAT_R8G8B8A8_UNORM, IMAGE_SIDE, IMAGE_SIDE, VK_IMAGE_USAGE_SAMPLED_BIT, &image,
				&image_memory, &view))) {
		vkGetImageSparseMemoryRequirements(harness->device, image, &image_count, NULL);
		vkDestroyImageView(harness->device, view, NULL);
		vkDestroyImage(harness->device, image, NULL);
		vkFreeMemory(harness->device, image_memory, NULL);
	}
	uint32_t format_count = UNTOUCHED;
	vkGetPhysicalDeviceSparseImageFormatProperties(harness->physical_device, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
		VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_TILING_OPTIMAL, &format_count, NULL);
	check("commands: the sparse queries find nothing for an image or a format", !image_count && !format_count);
}

static void check_refusals(const Harness *harness)
{
	const VkComputePipelineCreateInfo compute = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
	VkPipeline pipeline = (VkPipeline)UNTOUCHED;
	const bool no_compute = vkCreateComputePipelines(harness->device, VK_NULL_HANDLE, 1, &compute, NULL, &pipeline) ==
			VK_ERROR_FEATURE_NOT_PRESENT &&
		pipeline == VK_NULL_HANDLE;
	const VkBufferViewCreateInfo view_info = { .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO };
	VkBufferView view;
	const bool no_view = vkCreateBufferView(harness->device, &view_info, NULL, &view) == VK_ERROR_FEATURE_NOT_PRESENT;
	const bool no_sparse = vkQueueBindSparse(harness->queue, 0, NULL, VK_NULL_HANDLE) == VK_ERROR_FEATURE_NOT_PRESENT;
	check(
		"commands: compute pipelines, buffer views and sparse binding are refused", no_compute && no_view && no_sparse);
}

// Records the command numbered which into the buffer, and returns what ending it gives.
static VkResult record(VkCommandBuffer commands, uint32_t which)
{
	const VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	(void)vkBeginCommandBuffer(commands, &begin);
	switch (which) {
	case 0:
		vkCmdDispatch(commands, 1, 1, 1);
		break;
	case 1:
		vkCmdDispatchIndirect(commands, VK_NULL_HANDLE, 0);
		break;
	case 2:
		vkCmdSetDepthBounds(commands, 0.0f, 1.0f);
		break;
	case 3:
		vkCmdSetLineWidth(commands, 2.0f);
		break;
	default:
		vkCmdSetLineWidth(commands, 1.0f);
		break;
	}
	return vkEndCommandBuffer(commands);
}

// The first four fail their command buffer; a line width of 1 doesn't. vkCmdResolveImage resolves since milestone 17
// (test_multisample.c).
static void check_recorded(const Harness *harness)
{
	const VkCommandBufferAllocateInfo allocate = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = harness->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBuffer commands = VK_NULL_HANDLE;
	const bool made =
		harness_ok("vkAllocateCommandBuffers", vkAllocateCommandBuffers(harness->device, &allocate, &commands));
	bool refused = made;
	for (uint32_t i = 0; i < 4 && made; i++)
		refused = refused && record(commands, i) == VK_ERROR_FEATURE_NOT_PRESENT;
	check("commands: dispatches, depth bounds and a line width of 2 fail their command buffer", refused);
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkRenderPass render_pass = VK_NULL_HANDLE;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES] = { VK_NULL_HANDLE };
	VkPipeline pipeline = VK_NULL_HANDLE;
	bool ok = made && harness_ok("push layout", harness_push_layout(harness, &layout));
	ok = ok && harness_ok("render pass", harness_render_pass(harness, &render_pass, framebuffers));
	const VkPipelineInputAssemblyStateCreateInfo lines = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
	};
	const HarnessPipeline spec = {
		.vertex_spirv = push_vert_spirv,
		.vertex_bytes = PUSH_VERT_SPIRV_BYTES,
		.fragment_spirv = push_frag_spirv,
		.fragment_bytes = PUSH_FRAG_SPIRV_BYTES,
		.bindings = &harness_quad_binding,
		.binding_count = 1,
		.attributes = &harness_quad_attribute,
		.attribute_count = 1,
		.layout = layout,
		.assembly = &lines,
		.dynamic_line_width = true,
	};
	ok = ok && harness_ok("dynamic line width pipeline", harness_pipeline(harness, &spec, render_pass, &pipeline));
	check("commands: a pipeline of dynamic line width ignores its own, and takes 1 from vkCmdSetLineWidth",
		ok && record(commands, 4) == VK_SUCCESS);
	vkDestroyPipeline(harness->device, pipeline, NULL);
	if (render_pass)
		harness_destroy_render_pass(harness, render_pass, framebuffers);
	vkDestroyPipelineLayout(harness->device, layout, NULL);
	if (made)
		vkFreeCommandBuffers(harness->device, harness->pool, 1, &commands);
}

void test_commands(const Harness *harness)
{
	check_names(harness);
	check_pipeline_cache(harness);
	check_events(harness);
	check_answers(harness);
	check_refusals(harness);
	check_recorded(harness);
}
