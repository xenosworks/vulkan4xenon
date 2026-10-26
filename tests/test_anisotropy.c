#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "xsdk/log.h"

// Thin vertical stripes on quads squeezed 8 to 1 down them, one sampled without anisotropy and one with 16, their
// contrast measured, as xsdk's own check does (xsdk:tests/test_anisotropy.c; milestone 14).

enum {
	FRAMES = 30,
	TEXTURE_SIZE = 256,
	LEVELS = 9,
	TEXEL_BYTES = 4,
	// Every level, each a quarter of the one above.
	STAGING_BYTES = TEXEL_BYTES * (TEXTURE_SIZE * TEXTURE_SIZE * 4 - 1) / 3,
	// Two texels wide: level 2 and up average them to grey.
	STRIPE_TEXELS = 2,
	SQUEEZE = 8,
	// One texel a pixel across, eight down: without anisotropy the GPU picks level 3.
	CELL_WIDTH = TEXTURE_SIZE,
	CELL_HEIGHT = TEXTURE_SIZE / SQUEEZE,
	CELLS = 2,
	CELL_Y = 200,
	MOST_ANISOTROPY = 16,
	VERTEX_FLOATS = 6,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	UV_OFFSET_BYTES = 16,
	QUAD_VERTICES = 6,
	WHITE = 255,
	PER_MILLE = 1000,
	// Level 0 gives a contrast of 1000 and level 2 on 0: kept is level 0.5 or under, greyed 1.5 or over.
	KEPT_MIN_PER_MILLE = 750,
	GREYED_MAX_PER_MILLE = 250,
	// Grey is level 2's 128 of 255; a cell not drawn holds the clear colour's 32.
	GREY_LOW_PER_MILLE = 400,
	GREY_HIGH_PER_MILLE = 600,
};

// Without anisotropy, then with the most.
static const uint32_t cell_x[CELLS] = { 128, 768 };
static const char *const cell_names[CELLS] = { "without anisotropy", "at most 16" };

static const VkVertexInputBindingDescription binding = { 0, STRIDE_BYTES, VK_VERTEX_INPUT_RATE_VERTEX };
static const VkVertexInputAttributeDescription attributes[] = {
	{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0 },
	{ .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = UV_OFFSET_BYTES },
};
static const VkClearValue clear = { .color = { .float32 = { 32.0f / 255, 32.0f / 255, 32.0f / 255, 1.0f } } };

typedef struct {
	int32_t contrast_per_mille;
	uint32_t mean_per_mille;
} Stripes;

typedef struct {
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkRenderPass render_pass;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES];
	VkPipeline pipeline;
	VkImage image;
	VkDeviceMemory image_memory;
	VkImageView view;
	VkSampler samplers[CELLS];
	VkDescriptorPool pool;
	VkDescriptorSet sets[CELLS];
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkBuffer staging;
	VkDeviceMemory staging_memory;
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	bool logged;
	uint32_t kept_misses;
	uint32_t greyed_misses;
} Scene;

static bool white_column(uint32_t x)
{
	return x / STRIPE_TEXELS % 2 == 0;
}

static uint32_t level_offset(uint32_t level)
{
	uint32_t offset = 0;
	for (uint32_t l = 0; l < level; l++)
		offset += (TEXTURE_SIZE >> l) * (TEXTURE_SIZE >> l) * TEXEL_BYTES;
	return offset;
}

// Each level the box-filtered average of the one above, as mips are made, every channel the same grey.
static void fill_staging(uint8_t *bytes)
{
	static uint8_t greys[2][TEXTURE_SIZE * TEXTURE_SIZE];
	for (uint32_t y = 0; y < TEXTURE_SIZE; y++)
		for (uint32_t x = 0; x < TEXTURE_SIZE; x++)
			greys[0][y * TEXTURE_SIZE + x] = white_column(x) ? WHITE : 0;
	for (uint32_t level = 0; level < LEVELS; level++) {
		const uint32_t size = TEXTURE_SIZE >> level;
		const uint8_t *const grey = greys[level % 2];
		uint8_t *const texels = bytes + level_offset(level);
		for (uint32_t i = 0; i < size * size; i++)
			memset(texels + i * TEXEL_BYTES, grey[i], TEXEL_BYTES);
		uint8_t *const below = greys[(level + 1) % 2];
		for (uint32_t y = 0; y < size / 2; y++)
			for (uint32_t x = 0; x < size / 2; x++) {
				const uint8_t *const quad = grey + 2 * y * size + 2 * x;
				below[y * (size / 2) + x] = (uint8_t)((quad[0] + quad[1] + quad[size] + quad[size + 1] + 2) / 4);
			}
	}
}

// A cell's two triangles in clip space, the whole texture over it.
static void cell_quad(uint32_t cell, float vertices[QUAD_VERTICES][VERTEX_FLOATS])
{
	const float left = (float)cell_x[cell] / (HARNESS_WIDTH / 2) - 1.0f;
	const float right = (float)(cell_x[cell] + CELL_WIDTH) / (HARNESS_WIDTH / 2) - 1.0f;
	const float top = (float)CELL_Y / (HARNESS_HEIGHT / 2) - 1.0f;
	const float bottom = (float)(CELL_Y + CELL_HEIGHT) / (HARNESS_HEIGHT / 2) - 1.0f;
	const float corners[QUAD_VERTICES][VERTEX_FLOATS] = {
		{ left, top, 0.0f, 1.0f, 0.0f, 0.0f },
		{ right, top, 0.0f, 1.0f, 1.0f, 0.0f },
		{ left, bottom, 0.0f, 1.0f, 0.0f, 1.0f },
		{ right, top, 0.0f, 1.0f, 1.0f, 0.0f },
		{ right, bottom, 0.0f, 1.0f, 1.0f, 1.0f },
		{ left, bottom, 0.0f, 1.0f, 0.0f, 1.0f },
	};
	memcpy(vertices, corners, sizeof corners);
}

// The white stripes' average less the black ones', and the average of both, in green.
static Stripes measure(uint32_t address, uint32_t cell)
{
	uint64_t sums[2] = { 0, 0 };
	uint64_t counts[2] = { 0, 0 };
	for (uint32_t y = CELL_Y; y < CELL_Y + CELL_HEIGHT; y++)
		for (uint32_t x = cell_x[cell]; x < cell_x[cell] + CELL_WIDTH; x++) {
			const bool white = white_column(x - cell_x[cell]);
			sums[white] += harness_pixel(address, x, y).green;
			counts[white]++;
		}
	const int64_t difference = (int64_t)(sums[1] * counts[0]) - (int64_t)(sums[0] * counts[1]);
	return (
		Stripes){ .contrast_per_mille = (int32_t)(difference * PER_MILLE / (int64_t)(counts[0] * counts[1] * WHITE)),
		.mean_per_mille = (uint32_t)((sums[0] + sums[1]) * PER_MILLE / ((counts[0] + counts[1]) * WHITE)) };
}

// A HarnessCheck: with the most anisotropy the stripes keep their contrast, and without it they grey; the first
// frame's measures, and any miss, are logged.
static bool stripes_hold(uint32_t address, const char *what, void *context)
{
	Scene *const scene = context;
	Stripes stripes[CELLS];
	for (uint32_t i = 0; i < CELLS; i++)
		stripes[i] = measure(address, i);
	const Stripes *const none = &stripes[0];
	const Stripes *const most = &stripes[1];
	const bool kept = most->contrast_per_mille >= (int32_t)KEPT_MIN_PER_MILLE;
	const bool greyed = none->contrast_per_mille <= (int32_t)GREYED_MAX_PER_MILLE &&
		none->mean_per_mille >= GREY_LOW_PER_MILLE && none->mean_per_mille <= GREY_HIGH_PER_MILLE;
	for (uint32_t i = 0; i < CELLS && (!scene->logged || !kept || !greyed); i++)
		xsdk_log_write("anisotropy: %s, %s: stripes' contrast %d, mean %u, per mille", what, cell_names[i],
			(int)stripes[i].contrast_per_mille, (unsigned)stripes[i].mean_per_mille);
	scene->logged = true;
	scene->kept_misses += !kept;
	scene->greyed_misses += !greyed;
	return kept && greyed;
}

static VkResult create_image(const Harness *harness, Scene *scene)
{
	const VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = { TEXTURE_SIZE, TEXTURE_SIZE, 1 },
		.mipLevels = LEVELS,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VkResult result = vkCreateImage(harness->device, &info, NULL, &scene->image);
	if (result != VK_SUCCESS)
		return result;
	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(harness->device, scene->image, &requirements);
	const VkMemoryAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
	};
	result = vkAllocateMemory(harness->device, &allocate_info, NULL, &scene->image_memory);
	if (result == VK_SUCCESS)
		result = vkBindImageMemory(harness->device, scene->image, scene->image_memory, 0);
	const VkImageViewCreateInfo view_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = scene->image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, LEVELS, 0, 1 },
	};
	if (result == VK_SUCCESS)
		result = vkCreateImageView(harness->device, &view_info, NULL, &scene->view);
	return result;
}

// Trilinear both, one with the device's most anisotropy.
static VkResult create_descriptors(const Harness *harness, Scene *scene)
{
	VkResult result = VK_SUCCESS;
	for (uint32_t i = 0; i < CELLS && result == VK_SUCCESS; i++) {
		const VkSamplerCreateInfo sampler_info = {
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.magFilter = VK_FILTER_LINEAR,
			.minFilter = VK_FILTER_LINEAR,
			.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
			.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.anisotropyEnable = i == 1,
			.maxAnisotropy = i == 1 ? (float)MOST_ANISOTROPY : 1.0f,
			.maxLod = VK_LOD_CLAMP_NONE,
		};
		result = vkCreateSampler(harness->device, &sampler_info, NULL, &scene->samplers[i]);
	}
	const VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, CELLS };
	const VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = CELLS,
		.poolSizeCount = 1,
		.pPoolSizes = &size,
	};
	if (result == VK_SUCCESS)
		result = vkCreateDescriptorPool(harness->device, &pool_info, NULL, &scene->pool);
	const VkDescriptorSetLayout layouts[CELLS] = { scene->set_layout, scene->set_layout };
	const VkDescriptorSetAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = scene->pool,
		.descriptorSetCount = CELLS,
		.pSetLayouts = layouts,
	};
	if (result == VK_SUCCESS)
		result = vkAllocateDescriptorSets(harness->device, &allocate_info, scene->sets);
	for (uint32_t i = 0; i < CELLS && result == VK_SUCCESS; i++) {
		const VkDescriptorImageInfo image = { scene->samplers[i], scene->view,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		const VkWriteDescriptorSet write = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = scene->sets[i],
			.dstBinding = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &image,
		};
		vkUpdateDescriptorSets(harness->device, 1, &write, 0, NULL);
	}
	return result;
}

static VkResult create_pipeline(const Harness *harness, Scene *scene)
{
	const VkDescriptorSetLayoutBinding binding_info = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	};
	const VkDescriptorSetLayoutCreateInfo set_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &binding_info,
	};
	VkResult result = vkCreateDescriptorSetLayout(harness->device, &set_info, NULL, &scene->set_layout);
	const VkPipelineLayoutCreateInfo layout_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &scene->set_layout,
	};
	if (result == VK_SUCCESS)
		result = vkCreatePipelineLayout(harness->device, &layout_info, NULL, &scene->layout);
	const HarnessPipeline spec = {
		.vertex_spirv = texture_vert_spirv,
		.vertex_bytes = TEXTURE_VERT_SPIRV_BYTES,
		.fragment_spirv = texture_frag_spirv,
		.fragment_bytes = TEXTURE_FRAG_SPIRV_BYTES,
		.bindings = &binding,
		.binding_count = 1,
		.attributes = attributes,
		.attribute_count = 2,
		.layout = scene->layout,
	};
	if (result == VK_SUCCESS)
		result = harness_render_pass(harness, &scene->render_pass, scene->framebuffers);
	if (result == VK_SUCCESS)
		result = harness_pipeline(harness, &spec, scene->render_pass, &scene->pipeline);
	return result;
}

static void transition(VkCommandBuffer commands, VkImage image, VkImageLayout from, VkImageLayout to)
{
	const VkImageMemoryBarrier barrier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = from == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask =
			to == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT,
		.oldLayout = from,
		.newLayout = to,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = image,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, LEVELS, 0, 1 },
	};
	vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL,
		0, NULL, 1, &barrier);
}

// One submission copies every level in.
static bool upload(const Harness *harness, const Scene *scene)
{
	VkCommandBuffer commands;
	const VkCommandBufferAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = harness->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	if (!harness_ok("upload commands", vkAllocateCommandBuffers(harness->device, &allocate_info, &commands)))
		return false;
	const VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	bool ok = harness_ok("vkBeginCommandBuffer", vkBeginCommandBuffer(commands, &begin));
	if (ok) {
		VkBufferImageCopy regions[LEVELS];
		for (uint32_t level = 0; level < LEVELS; level++)
			regions[level] = (VkBufferImageCopy){
				.bufferOffset = level_offset(level),
				.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 },
				.imageExtent = { TEXTURE_SIZE >> level, TEXTURE_SIZE >> level, 1 },
			};
		transition(commands, scene->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		vkCmdCopyBufferToImage(
			commands, scene->staging, scene->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, LEVELS, regions);
		transition(
			commands, scene->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
	ok = ok && harness_ok("vkEndCommandBuffer", vkEndCommandBuffer(commands));
	const VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &commands,
	};
	ok = ok && harness_ok("vkResetFences", vkResetFences(harness->device, 1, &harness->fence)) &&
		harness_ok("vkQueueSubmit", vkQueueSubmit(harness->queue, 1, &submit, harness->fence)) &&
		harness_ok("vkWaitForFences", vkWaitForFences(harness->device, 1, &harness->fence, VK_TRUE, UINT64_MAX));
	(void)vkResetFences(harness->device, 1, &harness->fence);
	vkFreeCommandBuffers(harness->device, harness->pool, 1, &commands);
	return ok;
}

static bool build(const Harness *harness, Scene *scene)
{
	static uint8_t staging[STAGING_BYTES];
	static float vertices[CELLS][QUAD_VERTICES][VERTEX_FLOATS];
	fill_staging(staging);
	for (uint32_t i = 0; i < CELLS; i++)
		cell_quad(i, vertices[i]);
	bool ok = harness_ok("pipeline", create_pipeline(harness, scene));
	ok = ok && harness_ok("image", create_image(harness, scene));
	ok = ok && harness_ok("descriptors", create_descriptors(harness, scene));
	ok = ok &&
		harness_ok("vertex buffer",
			harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices, sizeof vertices, &scene->vertices,
				&scene->vertex_memory));
	return ok &&
		harness_ok("staging buffer",
			harness_buffer(harness, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging, STAGING_BYTES, &scene->staging,
				&scene->staging_memory));
}

static VkResult record(const Harness *harness, Scene *scene)
{
	VkResult result = harness_command_buffers(harness, scene->commands);
	for (uint32_t frame = 0; result == VK_SUCCESS && frame < harness->image_count; frame++) {
		const VkCommandBuffer commands = scene->commands[frame];
		const VkRenderPassBeginInfo begin = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = scene->render_pass,
			.framebuffer = scene->framebuffers[frame],
			.renderArea = { { 0, 0 }, { HARNESS_WIDTH, HARNESS_HEIGHT } },
			.clearValueCount = 1,
			.pClearValues = &clear,
		};
		vkCmdBeginRenderPass(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipeline);
		const VkDeviceSize offset = 0;
		vkCmdBindVertexBuffers(commands, 0, 1, &scene->vertices, &offset);
		for (uint32_t i = 0; i < CELLS; i++) {
			vkCmdBindDescriptorSets(
				commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->layout, 0, 1, &scene->sets[i], 0, NULL);
			vkCmdDraw(commands, QUAD_VERTICES, 1, i * QUAD_VERTICES, 0);
		}
		vkCmdEndRenderPass(commands);
		result = vkEndCommandBuffer(commands);
	}
	return result;
}

static void destroy(const Harness *harness, Scene *scene)
{
	const VkDevice device = harness->device;
	vkFreeCommandBuffers(device, harness->pool, harness->image_count, scene->commands);
	vkDestroyPipeline(device, scene->pipeline, NULL);
	harness_destroy_render_pass(harness, scene->render_pass, scene->framebuffers);
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	vkDestroyDescriptorPool(device, scene->pool, NULL);
	vkDestroyDescriptorSetLayout(device, scene->set_layout, NULL);
	for (uint32_t i = 0; i < CELLS; i++)
		vkDestroySampler(device, scene->samplers[i], NULL);
	vkDestroyImageView(device, scene->view, NULL);
	vkDestroyImage(device, scene->image, NULL);
	vkFreeMemory(device, scene->image_memory, NULL);
	vkDestroyBuffer(device, scene->vertices, NULL);
	vkFreeMemory(device, scene->vertex_memory, NULL);
	vkDestroyBuffer(device, scene->staging, NULL);
	vkFreeMemory(device, scene->staging_memory, NULL);
}

// The image's one layer as a 2D array, as PPSSPP views every texture, and a view of its levels past the first refused.
static bool views_as_array(const Harness *harness, const Scene *scene)
{
	VkImageViewCreateInfo view_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = scene->image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, LEVELS, 0, 1 },
	};
	VkImageView view = VK_NULL_HANDLE;
	const bool made = vkCreateImageView(harness->device, &view_info, NULL, &view) == VK_SUCCESS;
	vkDestroyImageView(harness->device, view, NULL);
	view_info.subresourceRange.baseMipLevel = 1;
	view_info.subresourceRange.levelCount = LEVELS - 1;
	const bool part = vkCreateImageView(harness->device, &view_info, NULL, &view) == VK_ERROR_FEATURE_NOT_PRESENT;
	return made && part;
}

void test_anisotropy(const Harness *harness)
{
	VkPhysicalDeviceFeatures features;
	VkPhysicalDeviceProperties properties;
	vkGetPhysicalDeviceFeatures(harness->physical_device, &features);
	vkGetPhysicalDeviceProperties(harness->physical_device, &properties);
	check("anisotropy: the device has samplerAnisotropy, at most 16",
		features.samplerAnisotropy && properties.limits.maxSamplerAnisotropy == (float)MOST_ANISOTROPY);
	Scene scene = { .render_pass = VK_NULL_HANDLE };
	const bool built = build(harness, &scene) && upload(harness, &scene);
	check("anisotropy: a striped image with its levels copied in, and samplers without anisotropy and with 16", built);
	check("anisotropy: a 2D array view of the image's one layer, and none of its levels past the first",
		built && views_as_array(harness, &scene));
	const bool recorded = built && harness->swapchain && harness_ok("recording", record(harness, &scene));
	uint32_t misses = 0;
	const uint32_t frames =
		recorded ? harness_frames(harness, scene.commands, FRAMES, stripes_hold, &scene, &misses) : 0;
	check("anisotropy: 30 frames drawn and presented", frames == FRAMES);
	check("anisotropy: every frame, at most 16, the stripes keep their contrast",
		frames == FRAMES && scene.kept_misses == 0);
	check("anisotropy: every frame, without, the level the GPU picks greys the stripes",
		frames == FRAMES && scene.greyed_misses == 0);
	destroy(harness, &scene);
}
