#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"

// Milestone 21's hardware loops, counted by an int in a block: push constants, a uniform buffer and a dynamic one, in a
// pixel shader, a vertex shader and both at once. The layer writes each count into the loop constant xost gave the
// loop, clamped to the 0 to 255 passes one runs. Each count puts its colour or its quad somewhere of its own. Then
// xsdk milestone 46's loop whose count is data, its loop constant the 255 passes the layer writes, and loops from 100
// and by 2, each the passes the layer works out from the count, the loop's start and its step. Then the rest of
// milestone 46, each a strip whose pixels take passes of their own from their place: breaks and continues on data in
// an unrolled loop and in a hardware loop, loops nested 3 deep, and samples in the passes of a loop whose count is
// data.

enum {
	FRAMES = 30,
	QUADS = 6,
	ROWS = 3,
	BANDS = 8,
	QUAD_VERTICES = 6,
	MAX_DRAWS = QUADS,
	// A strip's pixels from their place: a 4-pixel cell's column and row, and the strip's eighth, 160 pixels wide.
	CELL = 4,
	BAND_WIDTH = HARNESS_WIDTH / BANDS,
	STRIP_TOP = 340,
	STRIP_ROWS = 40,
	// The cell probed in each eighth, its 16 pixels, and the clear above the strip.
	PROBE_LEFT = 80,
	PROBE_TOP = 352,
	CELL_PIXELS = CELL * CELL,
	MAX_PROBES = BANDS * CELL_PIXELS + 1,
	UNROLLED_PASSES = 8,
	BREAK_COUNT = 24,
	NEST_COUNT = 3,
	// loop_sample.frag's mip chain, 64 texels a side, and the last level its gradients reach.
	TEXTURE_SIZE = 64,
	LEVELS = 7,
	TEXEL_BYTES = 4,
	MIPPED_BYTES = TEXEL_BYTES * (TEXTURE_SIZE * TEXTURE_SIZE * 4 - 1) / 3,
	GRADED_LAST = 4,
	// Its textureLod's levels, which each pixel cycles through from pass to pass.
	LOD_CYCLE = 4,
	// loop_count.frag's push constants: the step from byte 32, the count at 48.
	FRAGMENT_PUSH_OFFSET = 32,
	FRAGMENT_PUSH_BYTES = 32,
	// loop_count.vert's block a draw, at the dynamic offsets' 256-byte alignment (src/physical_device.c).
	ROW_STRIDE_BYTES = 256,
	TOLERANCE = 2,
	MAX_PASSES = 255,
	// The scenes again from shaders the bundle doesn't hold: their SPIR-V's generator word, its third, changed.
	GENERATOR_WORD = 2,
	GENERATOR_CHANGE = 0x00010000,
};

typedef struct {
	float position[4];
	float color[4];
} Vertex;

// loop_count.frag's push constants past push.vert's.
typedef struct {
	float step[4];
	int32_t count;
	int32_t padding[3];
} FragmentPush;

// loop_count.vert's block.
typedef struct {
	float scale[4];
	float offset[4];
	float step[4];
	float color[4];
	int32_t count;
	int32_t padding[3];
} RowBlock;

// loop_count_if.frag's block.
typedef struct {
	float colors[BANDS][4];
	float limit[4];
	int32_t count;
	int32_t padding[3];
} BandBlock;

typedef struct {
	// push.vert's place, pushed before the draw; or none, the vertex shader placing the quad itself.
	bool placed;
	HarnessQuad quad;
	bool pushed;
	FragmentPush push;
	uint32_t dynamic_offset;
} Draw;

typedef struct {
	VkRenderPass render_pass;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES];
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkPipeline pipeline;
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkBuffer uniforms;
	VkDeviceMemory uniform_memory;
	VkDescriptorPool pool;
	VkDescriptorSet set;
	bool dynamic;
	// loop_sample.frag's mip chain, its set's one binding, and what it's copied in from.
	bool sampled;
	VkImage image;
	VkDeviceMemory image_memory;
	VkImageView view;
	VkSampler sampler;
	VkBuffer staging;
	VkDeviceMemory staging_memory;
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	Draw draws[MAX_DRAWS];
	uint32_t draw_count;
	HarnessProbe probes[MAX_PROBES];
	HarnessProbes check;
} Scene;

// The layer compiles these on the console, with their literals, blocks and loop counts (docs/ROADMAP.md, milestone 13).
static uint32_t compiled_loop_count_frag[LOOP_COUNT_FRAG_SPIRV_BYTES / 4];
static uint32_t compiled_loop_count_if_frag[LOOP_COUNT_IF_FRAG_SPIRV_BYTES / 4];
static uint32_t compiled_loop_data_frag[LOOP_DATA_FRAG_SPIRV_BYTES / 4];
static uint32_t compiled_loop_steps_frag[LOOP_STEPS_FRAG_SPIRV_BYTES / 4];
static uint32_t compiled_loop_break_frag[LOOP_BREAK_FRAG_SPIRV_BYTES / 4];
static uint32_t compiled_loop_count_break_frag[LOOP_COUNT_BREAK_FRAG_SPIRV_BYTES / 4];
static uint32_t compiled_loop_nest_frag[LOOP_NEST_FRAG_SPIRV_BYTES / 4];
static uint32_t compiled_loop_sample_frag[LOOP_SAMPLE_FRAG_SPIRV_BYTES / 4];
static bool compiled;

// A dark slate no other test clears to.
static const VkClearValue clear = { .color = { .float32 = { 24.0f / 255, 40.0f / 255, 56.0f / 255, 1.0f } } };
static const Rgb clear_rgb = { 24, 40, 56 };

static const VkVertexInputBindingDescription screen_binding = { 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
static const VkVertexInputAttributeDescription screen_attributes[] = {
	{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0 },
	{ .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(Vertex, color) },
};

static const float unit_quad[QUAD_VERTICES][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };

// A step of colour a pass, small enough that six passes don't saturate, and one that takes 255 passes to reach white.
static const float coarse_step[4] = { 0.125f, 0.0625f, 0.03125f, 0.0f };
static const float fine_step[4] = { 1.0f / MAX_PASSES, 0.5f / MAX_PASSES, 0.25f / MAX_PASSES, 0.0f };
// A step of red and of green a pass, so each channel is its loop's passes.
static const float pass_step[4] = { 1.0f / MAX_PASSES, 1.0f / MAX_PASSES, 0.0f, 0.0f };
// The strips' steps of colour, small enough that none saturates.
static const float unrolled_step[4] = { 0.125f, 0.0625f, 0.015625f, 0.0f };
static const float break_step[4] = { 0.03125f, 0.03125f, 0.03125f, 0.0f };
static const float nest_step[4] = { 0.03125f, 0.0078125f, 0.015625f, 0.0f };
// loop_sample.frag's mip chain, each level flat: red for the LOD each pixel works out, green for the gradients, and
// blue the level's number in eighths, which the loop leaves on.
static const uint8_t level_texels[LEVELS][TEXEL_BYTES] = { { 220, 30, 0, 255 }, { 40, 200, 32, 255 },
	{ 160, 90, 64, 255 }, { 90, 150, 96, 255 }, { 250, 240, 128, 255 }, { 10, 10, 160, 255 }, { 0, 0, 192, 255 } };

// loop_count.vert's rows: each moves its quad right by the step a pass and adds the colour a pass.
static const float ROW_HOME = -0.9f;
static const float ROW_STEP = 0.4f;
static const float ROW_WIDTH = 0.2f;
static const float ROW_HEIGHT = 0.3f;
static const float row_color[4] = { 0.2f, 0.1f, 0.05f, 0.0f };
static const int32_t row_counts[ROWS] = { 1, 2, 4 };
// loop_count.frag's counts in the rows when it colours them.
static const int32_t row_fragment_counts[ROWS] = { 3, 5, 6 };

static uint32_t pixel_x(float x)
{
	return (uint32_t)((x + 1.0f) / 2.0f * HARNESS_WIDTH);
}

static uint32_t pixel_y(float y)
{
	return (uint32_t)((y + 1.0f) / 2.0f * HARNESS_HEIGHT);
}

static uint32_t passes_of(int32_t count)
{
	return count < 0 ? 0 : count > MAX_PASSES ? MAX_PASSES : (uint32_t)count;
}

static uint32_t to_byte(float value)
{
	return (uint32_t)((value > 1.0f ? 1.0f : value) * 255.0f + 0.5f);
}

// The colour passes steps make, from black.
static Rgb stepped(const float step[4], uint32_t passes)
{
	return (Rgb){ to_byte(step[0] * passes), to_byte(step[1] * passes), to_byte(step[2] * passes) };
}

// loop_steps.frag's colour at a count, as the GLSL runs it: the passes of its loop from 100 in red and of its loop by 2
// in green, each stopping at the 255 a loop runs.
static Rgb steps_rgb(int32_t count)
{
	uint32_t from = 0;
	for (int32_t i = 100; i < count && from < MAX_PASSES; i++)
		from++;
	uint32_t by = 0;
	for (int32_t j = 0; j < count && by < MAX_PASSES; j += 2)
		by++;
	return (Rgb){ to_byte(pass_step[0] * from), to_byte(pass_step[1] * by), 0 };
}

// loop_break.frag's colour at a pixel, as the GLSL runs it: its column and row in its cell and its eighth pick the
// pass it continues past and the pass it breaks at.
static Rgb unrolled_rgb(uint32_t x, uint32_t y)
{
	const uint32_t column = x % CELL;
	const uint32_t row = y % CELL;
	const uint32_t band = x / BAND_WIDTH;
	float sum[3] = { 0.0f, 0.0f, 0.0f };
	for (uint32_t i = 0; i < UNROLLED_PASSES; i++) {
		if (i == row + 2)
			continue;
		sum[0] += unrolled_step[0];
		if (column < 2) {
			sum[1] += unrolled_step[1];
			if (i >= band + column)
				break;
		} else if (i + column >= band + 5) {
			break;
		}
		sum[2] += (float)(i + 1) * unrolled_step[2];
	}
	return (Rgb){ to_byte(sum[0]), to_byte(sum[1]), to_byte(sum[2]) };
}

// loop_count_break.frag's colour at a pixel, as the GLSL runs it at a count of BREAK_COUNT.
static Rgb count_break_rgb(uint32_t x, uint32_t y)
{
	const uint32_t column = x % CELL;
	const uint32_t row = y % CELL;
	const uint32_t band = x / BAND_WIDTH;
	float sum[2] = { 0.0f, 0.0f };
	uint32_t i = 0;
	for (; i < BREAK_COUNT; i++) {
		if (i == row * 2 + 1)
			continue;
		sum[0] += break_step[0];
		if (column < 2) {
			sum[1] += break_step[1];
			if (i >= band * 2 + column)
				break;
		}
		if (i + column >= band * 4 + 5)
			break;
	}
	return (Rgb){ to_byte(sum[0]), to_byte(sum[1]), to_byte(break_step[2] * i) };
}

// loop_nest.frag's innermost loops in a pass of its while: the passes of the loop left on data, or of the do/while.
static float nest_inner(float t, float row)
{
	float passes = 0.0f;
	if (row < 2.0f) {
		for (uint32_t j = 0; j < NEST_COUNT && (float)j < t + row; j++)
			passes += 1.0f;
		return passes;
	}
	float k = 0.0f;
	do {
		k += 1.0f;
		passes += 1.0f;
	} while (k < t - row + 2.0f);
	return passes;
}

// loop_nest.frag's colour at a pixel, as the GLSL runs it at a count of NEST_COUNT: the while's passes in red, the
// do/whiles' in green and the inner loops' in blue.
static Rgb nest_rgb(uint32_t x, uint32_t y)
{
	const float column = (float)(x % CELL);
	const float row = (float)(y % CELL);
	const float band = (float)(x / BAND_WIDTH);
	float sum[3] = { 0.0f, 0.0f, 0.0f };
	for (uint32_t i = 0; i < NEST_COUNT; i++) {
		float t = 0.0f;
		while (t < band * 0.5f + column - (float)i) {
			t += 1.0f;
			sum[row < 2.0f ? 2 : 1] += nest_inner(t, row);
		}
		sum[0] += t;
	}
	return (Rgb){ to_byte(sum[0] * nest_step[0]), to_byte(sum[1] * nest_step[1]), to_byte(sum[2] * nest_step[2]) };
}

// loop_sample.frag's colour at a pixel, as the GLSL runs it but for its count stopping at 255: the levels its samples
// take, the last pass's counting half, and its counter's last three bits.
static Rgb sample_rgb(uint32_t x, uint32_t y)
{
	const uint32_t column = x % CELL;
	const uint32_t row = y % CELL;
	const uint32_t band = x / BAND_WIDTH;
	const uint32_t count = band * band * 6 + 3;
	float seen[2] = { 0.0f, 0.0f };
	uint32_t i = 0;
	for (; i < count && i < MAX_PASSES; i++) {
		const uint32_t graded = i + column / 2 < GRADED_LAST ? i + column / 2 : GRADED_LAST;
		if (graded >= band + row / 2)
			break;
		const uint32_t lod = (i + column + row) % LOD_CYCLE;
		seen[0] = seen[0] * 0.5f + level_texels[lod][0] / 255.0f * 0.5f;
		seen[1] = seen[1] * 0.5f + level_texels[graded][1] / 255.0f * 0.5f;
	}
	return (Rgb){ to_byte(seen[0]), to_byte(seen[1]), to_byte((float)(i % 8) * 0.125f) };
}

static void add_probe(Scene *scene, const char *name, uint32_t x, uint32_t y, Rgb want)
{
	scene->probes[scene->check.count++] = (HarnessProbe){ name, x, y, want, 0 };
}

static bool build_vertices(const Harness *harness, Scene *scene, bool screen)
{
	if (!screen)
		return harness_ok("vertex buffer", harness_quad_buffer(harness, &scene->vertices, &scene->vertex_memory));
	Vertex vertices[QUAD_VERTICES];
	for (uint32_t v = 0; v < QUAD_VERTICES; v++) {
		const float u = unit_quad[v][0];
		const float w = unit_quad[v][1];
		vertices[v] = (Vertex){ { 2.0f * u - 1.0f, 2.0f * w - 1.0f, 0.5f, 1.0f }, { u, w, 0.0f, 1.0f } };
	}
	return harness_ok("vertex buffer",
		harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices, sizeof vertices, &scene->vertices,
			&scene->vertex_memory));
}

static VkDescriptorType descriptor_type(const Scene *scene)
{
	if (scene->sampled)
		return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	return scene->dynamic ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
}

// The pipeline layout: a uniform buffer, or loop_sample.frag's mip chain, at set 0 binding 0 for the stage if any, and
// push constants if asked, both stages' ranges as push.vert and loop_count.frag read them.
static bool build_layout(const Harness *harness, Scene *scene, VkShaderStageFlags binding_stage, bool push)
{
	const VkPushConstantRange ranges[] = {
		{ VK_SHADER_STAGE_VERTEX_BIT, 0, FRAGMENT_PUSH_OFFSET },
		{ VK_SHADER_STAGE_FRAGMENT_BIT, FRAGMENT_PUSH_OFFSET, FRAGMENT_PUSH_BYTES },
	};
	if (binding_stage) {
		const VkDescriptorSetLayoutBinding binding = { 0, descriptor_type(scene), 1, binding_stage, NULL };
		const VkDescriptorSetLayoutCreateInfo info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = 1,
			.pBindings = &binding,
		};
		if (!harness_ok("vkCreateDescriptorSetLayout",
				vkCreateDescriptorSetLayout(harness->device, &info, NULL, &scene->set_layout)))
			return false;
	}
	const VkPipelineLayoutCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = binding_stage ? 1 : 0,
		.pSetLayouts = &scene->set_layout,
		.pushConstantRangeCount = push ? 2 : 0,
		.pPushConstantRanges = ranges,
	};
	return harness_ok("vkCreatePipelineLayout", vkCreatePipelineLayout(harness->device, &info, NULL, &scene->layout));
}

// A pool and the set of the layout's one binding, for what it holds to be written into.
static bool build_set(const Harness *harness, Scene *scene)
{
	const VkDescriptorPoolSize size = { descriptor_type(scene), 1 };
	const VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1,
		.poolSizeCount = 1,
		.pPoolSizes = &size,
	};
	if (!harness_ok("vkCreateDescriptorPool", vkCreateDescriptorPool(harness->device, &pool_info, NULL, &scene->pool)))
		return false;
	const VkDescriptorSetAllocateInfo allocate = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = scene->pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &scene->set_layout,
	};
	return harness_ok("vkAllocateDescriptorSets", vkAllocateDescriptorSets(harness->device, &allocate, &scene->set));
}

// The uniform buffer and a set pointing at its first range bytes.
static bool build_uniforms(
	const Harness *harness, Scene *scene, const void *bytes, uint32_t size_bytes, uint32_t range_bytes)
{
	if (!harness_ok("uniform buffer",
			harness_buffer(harness, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, bytes, size_bytes, &scene->uniforms,
				&scene->uniform_memory)) ||
		!build_set(harness, scene))
		return false;
	const VkDescriptorBufferInfo buffer = { scene->uniforms, 0, range_bytes };
	const VkWriteDescriptorSet write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = scene->set,
		.descriptorCount = 1,
		.descriptorType = descriptor_type(scene),
		.pBufferInfo = &buffer,
	};
	vkUpdateDescriptorSets(harness->device, 1, &write, 0, NULL);
	return true;
}

static uint32_t level_bytes(uint32_t level)
{
	return (TEXTURE_SIZE >> level) * (TEXTURE_SIZE >> level) * TEXEL_BYTES;
}

// loop_sample.frag's mip chain, its view, and a sampler taking the nearest level and texel.
static VkResult create_levels(const Harness *harness, Scene *scene)
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
	const VkSamplerCreateInfo sampler_info = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_NEAREST,
		.minFilter = VK_FILTER_NEAREST,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.maxLod = VK_LOD_CLAMP_NONE,
	};
	if (result == VK_SUCCESS)
		result = vkCreateSampler(harness->device, &sampler_info, NULL, &scene->sampler);
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
static bool upload_levels(const Harness *harness, const Scene *scene)
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
		for (uint32_t level = 0, offset = 0; level < LEVELS; offset += level_bytes(level++))
			regions[level] = (VkBufferImageCopy){
				.bufferOffset = offset,
				.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 },
				.imageExtent = { TEXTURE_SIZE >> level, TEXTURE_SIZE >> level, 1 },
			};
		transition(commands, scene->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		vkCmdCopyBufferToImage(
			commands, scene->staging, scene->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, LEVELS, regions);
		transition(
			commands, scene->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		ok = harness_ok("vkEndCommandBuffer", vkEndCommandBuffer(commands));
	}
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

// loop_sample.frag's mip chain, each level flat, copied in, and the set pointing at it.
static bool build_levels(const Harness *harness, Scene *scene)
{
	static uint8_t texels[MIPPED_BYTES];
	for (uint32_t level = 0, offset = 0; level < LEVELS; level++) {
		for (const uint32_t end = offset + level_bytes(level); offset < end; offset += TEXEL_BYTES)
			memcpy(texels + offset, level_texels[level], TEXEL_BYTES);
	}
	if (!harness_ok("mip chain", create_levels(harness, scene)) ||
		!harness_ok("staging buffer",
			harness_buffer(harness, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, texels, sizeof texels, &scene->staging,
				&scene->staging_memory)) ||
		!build_set(harness, scene))
		return false;
	const VkDescriptorImageInfo image = { scene->sampler, scene->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	const VkWriteDescriptorSet write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = scene->set,
		.descriptorCount = 1,
		.descriptorType = descriptor_type(scene),
		.pImageInfo = &image,
	};
	vkUpdateDescriptorSets(harness->device, 1, &write, 0, NULL);
	return upload_levels(harness, scene);
}

static bool build_pipeline(const Harness *harness, Scene *scene, const uint32_t *vertex_spirv, uint32_t vertex_bytes,
	const uint32_t *fragment_spirv, uint32_t fragment_bytes, bool screen)
{
	const HarnessPipeline spec = {
		.vertex_spirv = vertex_spirv,
		.vertex_bytes = vertex_bytes,
		.fragment_spirv = fragment_spirv,
		.fragment_bytes = fragment_bytes,
		.bindings = screen ? &screen_binding : &harness_quad_binding,
		.binding_count = 1,
		.attributes = screen ? screen_attributes : &harness_quad_attribute,
		.attribute_count = screen ? 2 : 1,
		.layout = scene->layout,
	};
	return harness_ok("render pass", harness_render_pass(harness, &scene->render_pass, scene->framebuffers)) &&
		harness_ok("pipeline", harness_pipeline(harness, &spec, scene->render_pass, &scene->pipeline)) &&
		build_vertices(harness, scene, screen);
}

// loop_count.frag in six quads, three columns by two rows, counted 0, 1, 3, 6, 300 and -5 by the push constants: the
// last two clamped to 255 and 0.
static bool build_push_counts(const Harness *harness, Scene *scene)
{
	static const int32_t counts[QUADS] = { 0, 1, 3, 6, 300, -5 };
	scene->check = (HarnessProbes){ "loops: push constants", scene->probes, 0, TOLERANCE, 0 };
	for (uint32_t q = 0; q < QUADS; q++) {
		const float *const step = counts[q] > 6 ? fine_step : coarse_step;
		const float x = 80.0f + (q % 3) * 400.0f;
		const float y = 100.0f + (q / 3) * 320.0f;
		Draw *const draw = &scene->draws[q];
		*draw = (Draw){
			.placed = true, .quad = { .x = x, .y = y, .width = 320.0f, .height = 200.0f, .depth = 0.5f }, .pushed = true
		};
		memcpy(draw->push.step, step, sizeof draw->push.step);
		draw->push.count = counts[q];
		add_probe(
			scene, "a quad by its passes", (uint32_t)x + 160, (uint32_t)y + 100, stepped(step, passes_of(counts[q])));
	}
	add_probe(scene, "the clear between the quads", 440, 360, clear_rgb);
	scene->draw_count = QUADS;
	return build_layout(harness, scene, 0, true) &&
		build_pipeline(harness, scene, push_vert_spirv, PUSH_VERT_SPIRV_BYTES,
			compiled ? compiled_loop_count_frag : loop_count_frag_spirv, LOOP_COUNT_FRAG_SPIRV_BYTES, false);
}

// loop_count.vert in three rows, counted 1, 2 and 4 by a dynamic uniform buffer, each at the place and in the colour
// its passes make; with loop_count.frag colouring them too, counted 3, 5 and 6 by the push constants, when both.
static bool build_rows(const Harness *harness, Scene *scene, bool both)
{
	uint8_t blocks[ROWS * ROW_STRIDE_BYTES];
	memset(blocks, 0, sizeof blocks);
	scene->dynamic = true;
	scene->check =
		(HarnessProbes){ both ? "loops: both stages" : "loops: a vertex shader", scene->probes, 0, TOLERANCE, 0 };
	for (uint32_t r = 0; r < ROWS; r++) {
		const float top = -0.8f + r * 0.6f;
		const RowBlock block = { { ROW_WIDTH, ROW_HEIGHT, 0.0f, 1.0f }, { ROW_HOME, top, 0.5f, 0.0f },
			{ ROW_STEP, 0.0f, 0.0f, 0.0f }, { row_color[0], row_color[1], row_color[2], 0.0f }, row_counts[r], { 0 } };
		memcpy(blocks + r * ROW_STRIDE_BYTES, &block, sizeof block);
		Draw *const draw = &scene->draws[r];
		*draw = (Draw){ .pushed = both, .dynamic_offset = r * ROW_STRIDE_BYTES };
		memcpy(draw->push.step, coarse_step, sizeof draw->push.step);
		draw->push.count = row_fragment_counts[r];
		const Rgb want =
			both ? stepped(coarse_step, (uint32_t)row_fragment_counts[r]) : stepped(row_color, (uint32_t)row_counts[r]);
		const uint32_t y = pixel_y(top + ROW_HEIGHT / 2);
		for (int32_t passes = row_counts[r] - 1; passes <= row_counts[r] + 1; passes++) {
			const float x = ROW_HOME + ROW_STEP * passes + ROW_WIDTH / 2;
			if (x < 1.0f)
				add_probe(scene,
					passes == row_counts[r] ? "a row's quad where its passes put it" : "the clear beside it",
					pixel_x(x), y, passes == row_counts[r] ? want : clear_rgb);
		}
	}
	scene->draw_count = ROWS;
	return build_layout(harness, scene, VK_SHADER_STAGE_VERTEX_BIT, both) &&
		build_uniforms(harness, scene, blocks, sizeof blocks, sizeof(RowBlock)) &&
		build_pipeline(harness, scene, loop_count_vert_spirv, LOOP_COUNT_VERT_SPIRV_BYTES,
			both ? loop_count_frag_spirv : triangle_frag_spirv,
			both ? LOOP_COUNT_FRAG_SPIRV_BYTES : TRIANGLE_FRAG_SPIRV_BYTES, false);
}

static bool build_vertex_rows(const Harness *harness, Scene *scene)
{
	return build_rows(harness, scene, false);
}

static bool build_both_rows(const Harness *harness, Scene *scene)
{
	return build_rows(harness, scene, true);
}

// loop_count_if.frag over the screen, counted 7 by a uniform buffer and broken out of after 5 passes: in each eighth
// of the screen, the passes whose threshold its u passes add that pass's colour, the others blue.
static bool build_bands(const Harness *harness, Scene *scene)
{
	enum { COUNT = 7, BREAK_AFTER = 5 };
	BandBlock block = { .limit = { 0.0f, 0.0f, 0.0f, (float)BREAK_AFTER }, .count = COUNT };
	for (uint32_t i = 0; i < BANDS; i++) {
		const float color[4] = { 0.125f, 0.03125f * (i + 1), 0.0f, 1.0f };
		memcpy(block.colors[i], color, sizeof color);
	}
	scene->check =
		(HarnessProbes){ "loops: a uniform buffer, with an if/else and a break", scene->probes, 0, TOLERANCE, 0 };
	for (uint32_t band = 0; band < BANDS; band++) {
		float seen[3] = { 0.0f, 0.0f, 0.0f };
		for (uint32_t i = 0; i < BREAK_AFTER; i++) {
			if (i <= band) {
				seen[0] += block.colors[i][0];
				seen[1] += block.colors[i][1];
			} else {
				seen[2] += 0.0625f;
			}
		}
		add_probe(scene, "a band by the passes before the break", band * (HARNESS_WIDTH / BANDS) + HARNESS_WIDTH / 16,
			HARNESS_HEIGHT / 2, (Rgb){ to_byte(seen[0]), to_byte(seen[1]), to_byte(seen[2]) });
	}
	scene->draws[0] = (Draw){ 0 };
	scene->draw_count = 1;
	return build_layout(harness, scene, VK_SHADER_STAGE_FRAGMENT_BIT, false) &&
		build_uniforms(harness, scene, &block, sizeof block, sizeof block) &&
		build_pipeline(harness, scene, triangle_vert_spirv, TRIANGLE_VERT_SPIRV_BYTES,
			compiled ? compiled_loop_count_if_frag : loop_count_if_frag_spirv, LOOP_COUNT_IF_FRAG_SPIRV_BYTES, true);
}

// loop_data.frag in a strip across the screen, each eighth's count from gl_FragCoord, 15 passes and 40 more an eighth:
// the last two run 255, the most a loop runs, for 255 and 295.
static bool build_data_count(const Harness *harness, Scene *scene)
{
	enum { STRIP_Y = 320, STRIP_HEIGHT = 80, FIRST_COUNT = 15, COUNT_STEP = 40 };
	scene->check = (HarnessProbes){ "loops: a count from data", scene->probes, 0, TOLERANCE, 0 };
	for (uint32_t band = 0; band < BANDS; band++)
		add_probe(scene, "an eighth by its count's passes", band * (HARNESS_WIDTH / BANDS) + HARNESS_WIDTH / 16,
			STRIP_Y + STRIP_HEIGHT / 2, stepped(fine_step, passes_of(FIRST_COUNT + COUNT_STEP * (int32_t)band)));
	add_probe(scene, "the clear above the strip", HARNESS_WIDTH / 2, STRIP_Y / 2, clear_rgb);
	Draw *const draw = &scene->draws[0];
	*draw = (Draw){ .placed = true,
		.quad = { .x = 0.0f, .y = STRIP_Y, .width = HARNESS_WIDTH, .height = STRIP_HEIGHT, .depth = 0.5f },
		.pushed = true };
	memcpy(draw->push.step, fine_step, sizeof draw->push.step);
	scene->draw_count = 1;
	return build_layout(harness, scene, 0, true) &&
		build_pipeline(harness, scene, push_vert_spirv, PUSH_VERT_SPIRV_BYTES,
			compiled ? compiled_loop_data_frag : loop_data_frag_spirv, LOOP_DATA_FRAG_SPIRV_BYTES, false);
}

// loop_steps.frag in six quads, three columns by two rows, counted 0, 100, 150, 300, 400 and 600 by the push
// constants: its loop from 100 runs 0, 0, 50, 200, 255 and 255 passes, its loop by 2 0, 50, 75, 150, 200 and 255.
static bool build_step_counts(const Harness *harness, Scene *scene)
{
	static const int32_t counts[QUADS] = { 0, 100, 150, 300, 400, 600 };
	scene->check = (HarnessProbes){ "loops: from 100 and by 2", scene->probes, 0, TOLERANCE, 0 };
	for (uint32_t q = 0; q < QUADS; q++) {
		const float x = 80.0f + (q % 3) * 400.0f;
		const float y = 100.0f + (q / 3) * 320.0f;
		Draw *const draw = &scene->draws[q];
		*draw = (Draw){
			.placed = true, .quad = { .x = x, .y = y, .width = 320.0f, .height = 200.0f, .depth = 0.5f }, .pushed = true
		};
		memcpy(draw->push.step, pass_step, sizeof draw->push.step);
		draw->push.count = counts[q];
		add_probe(scene, "a quad by its loops' passes", (uint32_t)x + 160, (uint32_t)y + 100, steps_rgb(counts[q]));
	}
	add_probe(scene, "the clear between the quads", 440, 360, clear_rgb);
	scene->draw_count = QUADS;
	return build_layout(harness, scene, 0, true) &&
		build_pipeline(harness, scene, push_vert_spirv, PUSH_VERT_SPIRV_BYTES,
			compiled ? compiled_loop_steps_frag : loop_steps_frag_spirv, LOOP_STEPS_FRAG_SPIRV_BYTES, false);
}

// A strip across the screen of a shader whose pixels take their passes from their place, pushed a step, if any, and a
// count; and a probe on each pixel of a cell in each eighth, its colour as rgb works it out.
static bool build_strip(const Harness *harness, Scene *scene, const char *test, const float *step, int32_t count,
	Rgb (*rgb)(uint32_t x, uint32_t y), const uint32_t *fragment_spirv, uint32_t fragment_bytes)
{
	scene->check = (HarnessProbes){ test, scene->probes, 0, TOLERANCE, 0 };
	for (uint32_t i = 0; i < BANDS * CELL_PIXELS; i++) {
		const uint32_t x = i / CELL_PIXELS * BAND_WIDTH + PROBE_LEFT + i % CELL;
		const uint32_t y = PROBE_TOP + i % CELL_PIXELS / CELL;
		add_probe(scene, "a pixel by its passes", x, y, rgb(x, y));
	}
	add_probe(scene, "the clear above the strip", HARNESS_WIDTH / 2, STRIP_TOP / 2, clear_rgb);
	Draw *const draw = &scene->draws[0];
	*draw = (Draw){ .placed = true,
		.quad = { .x = 0.0f, .y = STRIP_TOP, .width = HARNESS_WIDTH, .height = STRIP_ROWS, .depth = 0.5f },
		.pushed = true };
	if (step)
		memcpy(draw->push.step, step, sizeof draw->push.step);
	draw->push.count = count;
	scene->draw_count = 1;
	return build_layout(harness, scene, scene->sampled ? VK_SHADER_STAGE_FRAGMENT_BIT : 0, true) &&
		build_pipeline(harness, scene, push_vert_spirv, PUSH_VERT_SPIRV_BYTES, fragment_spirv, fragment_bytes, false);
}

// loop_break.frag in the strip: a loop of 8 passes xost unrolls, each pixel of a quad leaving at a pass of its own.
static bool build_unrolled_breaks(const Harness *harness, Scene *scene)
{
	return build_strip(harness, scene, "loops: breaks and continues on data in an unrolled loop", unrolled_step, 0,
		unrolled_rgb, compiled ? compiled_loop_break_frag : loop_break_frag_spirv, LOOP_BREAK_FRAG_SPIRV_BYTES);
}

// loop_count_break.frag in the strip, counted 24 by the push constants: the pixels of a quad, and of a group the GPU
// runs together, leave at passes of their own, some after all 24.
static bool build_count_breaks(const Harness *harness, Scene *scene)
{
	return build_strip(harness, scene, "loops: breaks and continues on data in a hardware loop", break_step,
		BREAK_COUNT, count_break_rgb, compiled ? compiled_loop_count_break_frag : loop_count_break_frag_spirv,
		LOOP_COUNT_BREAK_FRAG_SPIRV_BYTES);
}

// loop_nest.frag in the strip, its outer loop and its innermost for loop counted 3 by the push constants.
static bool build_nested(const Harness *harness, Scene *scene)
{
	return build_strip(harness, scene, "loops: nested 3 deep", nest_step, NEST_COUNT, nest_rgb,
		compiled ? compiled_loop_nest_frag : loop_nest_frag_spirv, LOOP_NEST_FRAG_SPIRV_BYTES);
}

// loop_sample.frag in the strip, on its mip chain: its eighths' counts 3 to 297, the last run as 255.
static bool build_samples(const Harness *harness, Scene *scene)
{
	scene->sampled = true;
	return build_strip(harness, scene, "loops: samples in a pass", NULL, 0, sample_rgb,
			   compiled ? compiled_loop_sample_frag : loop_sample_frag_spirv, LOOP_SAMPLE_FRAG_SPIRV_BYTES) &&
		build_levels(harness, scene);
}

static VkResult record(const Harness *harness, Scene *scene)
{
	VkResult result = harness_command_buffers(harness, scene->commands);
	for (uint32_t i = 0; i < harness->image_count && result == VK_SUCCESS; i++) {
		const VkRenderPassBeginInfo pass = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = scene->render_pass,
			.framebuffer = scene->framebuffers[i],
			.renderArea = { { 0, 0 }, { HARNESS_WIDTH, HARNESS_HEIGHT } },
			.clearValueCount = 1,
			.pClearValues = &clear,
		};
		const VkDeviceSize offset = 0;
		const VkCommandBuffer commands = scene->commands[i];
		vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipeline);
		vkCmdBindVertexBuffers(commands, 0, 1, &scene->vertices, &offset);
		for (uint32_t d = 0; d < scene->draw_count; d++) {
			const Draw *const draw = &scene->draws[d];
			if (scene->set)
				vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->layout, 0, 1, &scene->set,
					scene->dynamic ? 1 : 0, &draw->dynamic_offset);
			if (draw->placed)
				harness_push_place_in(commands, scene->layout, &draw->quad, HARNESS_WIDTH, HARNESS_HEIGHT);
			if (draw->pushed)
				vkCmdPushConstants(commands, scene->layout, VK_SHADER_STAGE_FRAGMENT_BIT, FRAGMENT_PUSH_OFFSET,
					FRAGMENT_PUSH_BYTES, &draw->push);
			vkCmdDraw(commands, QUAD_VERTICES, 1, 0, 0);
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
	vkDestroyDescriptorPool(device, scene->pool, NULL);
	vkDestroySampler(device, scene->sampler, NULL);
	vkDestroyImageView(device, scene->view, NULL);
	vkDestroyImage(device, scene->image, NULL);
	vkFreeMemory(device, scene->image_memory, NULL);
	vkDestroyBuffer(device, scene->staging, NULL);
	vkFreeMemory(device, scene->staging_memory, NULL);
	vkDestroyBuffer(device, scene->uniforms, NULL);
	vkFreeMemory(device, scene->uniform_memory, NULL);
	vkDestroyBuffer(device, scene->vertices, NULL);
	vkFreeMemory(device, scene->vertex_memory, NULL);
	vkDestroyPipeline(device, scene->pipeline, NULL);
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	vkDestroyDescriptorSetLayout(device, scene->set_layout, NULL);
	harness_destroy_render_pass(harness, scene->render_pass, scene->framebuffers);
}

static void run(const Harness *harness, const char *name, bool (*build)(const Harness *, Scene *), const char *holds)
{
	Scene scene = { .render_pass = VK_NULL_HANDLE };
	const bool built = build(harness, &scene);
	char message[200];
	snprintf(message, sizeof message, "loops: %s: pipelines and buffers made", name);
	check(message, built);
	const bool recorded = built && harness->swapchain && harness_ok("recording", record(harness, &scene));
	uint32_t misses = 0;
	const uint32_t frames =
		recorded ? harness_frames(harness, scene.commands, FRAMES, harness_probes_hold, &scene.check, &misses) : 0;
	snprintf(message, sizeof message, "loops: %s: 30 frames drawn and presented", name);
	check(message, frames == FRAMES);
	snprintf(message, sizeof message, "loops: %s: every frame %s", name, holds);
	check(message, frames == FRAMES && misses == 0);
	snprintf(message, sizeof message, "loops: %s: the image on screen does too", name);
	check(message, harness_screen(harness, harness_probes_hold, &scene.check));
	destroy(harness, &scene);
}

void test_loops(const Harness *harness)
{
	memcpy(compiled_loop_count_frag, loop_count_frag_spirv, sizeof compiled_loop_count_frag);
	memcpy(compiled_loop_count_if_frag, loop_count_if_frag_spirv, sizeof compiled_loop_count_if_frag);
	memcpy(compiled_loop_data_frag, loop_data_frag_spirv, sizeof compiled_loop_data_frag);
	memcpy(compiled_loop_steps_frag, loop_steps_frag_spirv, sizeof compiled_loop_steps_frag);
	compiled_loop_count_frag[GENERATOR_WORD] ^= GENERATOR_CHANGE;
	compiled_loop_count_if_frag[GENERATOR_WORD] ^= GENERATOR_CHANGE;
	compiled_loop_data_frag[GENERATOR_WORD] ^= GENERATOR_CHANGE;
	compiled_loop_steps_frag[GENERATOR_WORD] ^= GENERATOR_CHANGE;
	memcpy(compiled_loop_break_frag, loop_break_frag_spirv, sizeof compiled_loop_break_frag);
	memcpy(compiled_loop_count_break_frag, loop_count_break_frag_spirv, sizeof compiled_loop_count_break_frag);
	memcpy(compiled_loop_nest_frag, loop_nest_frag_spirv, sizeof compiled_loop_nest_frag);
	memcpy(compiled_loop_sample_frag, loop_sample_frag_spirv, sizeof compiled_loop_sample_frag);
	compiled_loop_break_frag[GENERATOR_WORD] ^= GENERATOR_CHANGE;
	compiled_loop_count_break_frag[GENERATOR_WORD] ^= GENERATOR_CHANGE;
	compiled_loop_nest_frag[GENERATOR_WORD] ^= GENERATOR_CHANGE;
	compiled_loop_sample_frag[GENERATOR_WORD] ^= GENERATOR_CHANGE;
	compiled = true;
	run(harness, "counted by push constants, the shader compiled here", build_push_counts,
		"colours each quad by its count's passes, 300 run as 255 and -5 as none");
	run(harness, "counted by a uniform buffer, the shader compiled here", build_bands,
		"colours each band by the passes before the break");
	run(harness, "a count from data, the shader compiled here", build_data_count,
		"colours each eighth by its count's passes, 295 run as 255");
	run(harness, "from 100 and by 2, the shader compiled here", build_step_counts,
		"colours each quad by its loops' passes, ceil((count - start) / step) to 255");
	run(harness, "breaks and continues on data in an unrolled loop, the shader compiled here", build_unrolled_breaks,
		"colours each pixel by the passes it continued past and left at");
	run(harness, "breaks and continues on data in a hardware loop, the shader compiled here", build_count_breaks,
		"colours each pixel by the passes it continued past and left at, and its counter after");
	run(harness, "nested 3 deep, two in an if/else on data, the shader compiled here", build_nested,
		"colours each pixel by the passes it ran at each level");
	run(harness, "samples in a pass, the shader compiled here", build_samples,
		"colours each pixel by the levels it sampled and its passes, 297 run as 255");
	compiled = false;

	run(harness, "counted by push constants", build_push_counts,
		"colours each quad by its count's passes, 300 run as 255 and -5 as none");
	run(harness, "counted by a dynamic uniform buffer in a vertex shader", build_vertex_rows,
		"places and colours each row's quad by its count's passes");
	run(harness, "counted in both stages", build_both_rows,
		"places each row's quad by the vertex shader's count and colours it by the pixel shader's");
	run(harness, "counted by a uniform buffer, with an if/else and a break", build_bands,
		"colours each band by the passes before the break");
	run(harness, "a count from data", build_data_count, "colours each eighth by its count's passes, 295 run as 255");
	run(harness, "from 100 and by 2", build_step_counts,
		"colours each quad by its loops' passes, ceil((count - start) / step) to 255");
	run(harness, "breaks and continues on data in an unrolled loop", build_unrolled_breaks,
		"colours each pixel by the passes it continued past and left at");
	run(harness, "breaks and continues on data in a hardware loop", build_count_breaks,
		"colours each pixel by the passes it continued past and left at, and its counter after");
	run(harness, "nested 3 deep, two in an if/else on data", build_nested,
		"colours each pixel by the passes it ran at each level");
	run(harness, "samples in a pass", build_samples,
		"colours each pixel by the levels it sampled and its passes, 297 run as 255");
}
