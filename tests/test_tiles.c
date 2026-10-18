#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "tiled_index.h"
#include "xsdk/log.h"

// Milestone 17's first part: render passes past eDRAM, drawn as xsdk's tiled passes at one sample
// (xsdk:docs/scope/44-tiling-and-msaa.md, "Render passes past eDRAM"). Every texel of each image is read back and
// checked against what the CPU works out from the quads drawn:
// - four colour attachments of four formats with depth at 1280x720, 4320 tiles, so depth stays whole at eDRAM's top: a
//   pass clears and draws all five, a second loads all five and draws over them, then an untiled pass of one attachment
//   loads the depth where they left it;
// - a 2048x2048 shadow map, its depth in bands: stored a band at a time and read from its memory, loaded by the next
//   pass, which draws it back, and sampled by another;
// - what such a pass refuses.
// Each scene shows on screen in its own colours.

enum {
	FRAMES = 3,
	FENCE_TIMEOUT_NS = 1000000000,
	WIDTH = HARNESS_WIDTH,
	HEIGHT = HARNESS_HEIGHT,
	BIG = 2048,
	COLORS = 4,
	CHANNELS = 4,
	// The quads before the second pass's: the far one, then the near one.
	FIRST_QUADS = 2,
	QUAD_VERTICES = 6,
	VERTEX_FLOATS = 6,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	UV_OFFSET_BYTES = 16,
	// push.vert's place, then four_targets.frag's four colours.
	PLACE_BYTES = 32,
	FOUR_COLOR_BYTES = COLORS * CHANNELS * 4,
	// The memory type the CPU caches, which buffers only copied to and from may take.
	CACHED_TYPE = 1,
	READBACK_BYTES = BIG * BIG * 4,
	MISSES_LOGGED = 4,
	DEPTH_24_MAX = 0xFFFFFF,
	// D24's rounding, as xsdk's check of the resolve allows (xsdk:tests/test_depth.c).
	DEPTH_24_TOLERANCE = 2,
	// A screen probe's channels, and a sampled depth's red.
	TOLERANCE = 1,
	BIG_RECTS = 4,
	PROBES = 5,
	SETS = 3,
	LABEL_BYTES = 200,
};

// How a texel reads back.
enum {
	KIND_BYTES,
	KIND_10_10_10_2,
	KIND_HALVES,
	KIND_FLOATS,
};

// The descriptor sets: the four attachments' first, the shadow map's colour, its depth.
enum {
	SET_FOUR,
	SET_BIG_COLOR,
	SET_BIG_DEPTH,
};

// Rectangles in pixels, right and bottom exclusive, and their depth.
typedef struct {
	uint32_t x0;
	uint32_t y0;
	uint32_t x1;
	uint32_t y1;
	float depth;
} Rect;

// The 1280x720 passes' quads, across the bands of 256 rows: the first pass's far and near ones, the second's between
// them, and the untiled pass's over all of it, tested against the depth they left.
static const Rect far_quad = { 400, 300, 1100, 650, 0.75f };
static const Rect near_quad = { 100, 160, 600, 400, 0.25f };
static const Rect second_quad = { 300, 200, 900, 700, 0.5f };
static const Rect last_quad = { 0, 0, WIDTH, HEIGHT, 0.6f };

typedef struct {
	const char *name;
	VkFormat format;
	uint32_t kind;
	uint32_t channels;
	uint32_t texel_bytes;
	// How far a channel may be from the value drawn: a unorm's step; floats exact.
	float tolerance;
	float clear[CHANNELS];
	// The far quad's colour, the near one's and the second pass's.
	float colors[3][CHANNELS];
} Attachment;

#define RGBA(r, g, b) { (r) / 255.0f, (g) / 255.0f, (b) / 255.0f, 1.0f }

static const Attachment attachments[COLORS] = {
	{ "R8G8B8A8_UNORM", VK_FORMAT_R8G8B8A8_UNORM, KIND_BYTES, 4, 4, 1.5f / 255, RGBA(32, 32, 64),
		{ RGBA(64, 224, 64), RGBA(224, 64, 64), RGBA(64, 64, 224) } },
	{ "A2B10G10R10_UNORM_PACK32", VK_FORMAT_A2B10G10R10_UNORM_PACK32, KIND_10_10_10_2, 4, 4, 2.0f / 1023,
		{ 0.2f, 0.4f, 0.6f, 1.0f / 3 },
		{ { 0.1f, 0.8f, 0.3f, 2.0f / 3 }, { 0.9f, 0.1f, 0.5f, 1.0f }, { 0.5f, 0.5f, 0.9f, 0.0f } } },
	{ "R16G16B16A16_SFLOAT", VK_FORMAT_R16G16B16A16_SFLOAT, KIND_HALVES, 4, 8, 0.0f, { 1.25f, 0.5f, 0.75f, 2.0f },
		{ { 100.0f, 0.25f, 7.5f, 0.5f }, { 2.5f, -3.0f, 0.125f, 1.0f }, { -1.5f, 6.0f, 0.0f, 3.25f } } },
	{ "R32_SFLOAT", VK_FORMAT_R32_SFLOAT, KIND_FLOATS, 1, 4, 0.0f, { 3.5f }, { { 1234.5f }, { -2.25f }, { 0.0625f } } },
};

static const float last_clear[CHANNELS] = RGBA(24, 24, 24);
static const float last_color[CHANNELS] = RGBA(200, 120, 40);

// The 2048x2048 shadow map: a far quad across three band edges, a square across the first, a strip down every band,
// and a nearer square on the far quad.
static const Rect big_rects[BIG_RECTS] = {
	{ 100, 300, 1900, 1800, 0.75f },
	{ 600, 400, 1200, 700, 0.375f },
	{ 1000, 0, 1100, BIG, 0.25f },
	{ 150, 600, 450, 900, 0.625f },
};

// The pass that loads it clears to this, then draws over all of it at depth 0.5 and 0.7, testing less-than, writing
// nothing: each pixel shows how deep the map is there.
static const float loaded_clear[CHANNELS] = RGBA(90, 30, 110);
static const float loaded_colors[2][CHANNELS] = { RGBA(220, 200, 60), RGBA(40, 160, 200) };
static const float loaded_depths[2] = { 0.5f, 0.7f };

static const VkClearValue screen_clear = { .color = { .float32 = RGBA(24, 24, 24) } };

// Clip space, then texture coordinates: over the whole viewport from the whole image, then from the 2048x2048 image's
// top left 1280x720, texel for texel on screen.
#define SHOWN_U ((float)WIDTH / BIG)
#define SHOWN_V ((float)HEIGHT / BIG)
static const float quads[2 * QUAD_VERTICES][VERTEX_FLOATS] = {
	{ -1.0f, -1.0f, 0.0f, 1.0f, 0.0f, 0.0f },
	{ 1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f },
	{ -1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f },
	{ 1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f },
	{ 1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f },
	{ -1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f },
	{ -1.0f, -1.0f, 0.0f, 1.0f, 0.0f, 0.0f },
	{ 1.0f, -1.0f, 0.0f, 1.0f, SHOWN_U, 0.0f },
	{ -1.0f, 1.0f, 0.0f, 1.0f, 0.0f, SHOWN_V },
	{ 1.0f, -1.0f, 0.0f, 1.0f, SHOWN_U, 0.0f },
	{ 1.0f, 1.0f, 0.0f, 1.0f, SHOWN_U, SHOWN_V },
	{ -1.0f, 1.0f, 0.0f, 1.0f, 0.0f, SHOWN_V },
};

static const VkVertexInputBindingDescription quad_binding = { 0, STRIDE_BYTES, VK_VERTEX_INPUT_RATE_VERTEX };
static const VkVertexInputAttributeDescription quad_attributes[] = {
	{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0 },
	{ .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = UV_OFFSET_BYTES },
};

#define RGB(r, g, b) { r, g, b }

// The first attachment on screen after the two passes, in each band.
static const HarnessProbe four_probes[PROBES] = {
	{ "the first pass's clear", 50, 50, RGB(32, 32, 64), 0 },
	{ "the near quad over the second pass's", 310, 250, RGB(224, 64, 64), 0 },
	{ "the second pass's quad over the far one", 700, 600, RGB(64, 64, 224), 0 },
	{ "the far quad in the last band", 1000, 640, RGB(64, 224, 64), 0 },
	{ "the second pass's quad over the clear in the last band", 800, 680, RGB(64, 64, 224), 0 },
};

// The 2048x2048 colour after the pass that loads the map, its top left on screen.
static const HarnessProbe big_probes[PROBES] = {
	{ "the map's clear", 50, 50, RGB(40, 160, 200), 0 },
	{ "the strip in band 0", 1050, 100, RGB(90, 30, 110), 0 },
	{ "the square across band 0's edge", 700, 600, RGB(90, 30, 110), 0 },
	{ "the far quad", 200, 500, RGB(40, 160, 200), 0 },
	{ "the nearer square on it", 300, 650, RGB(220, 200, 60), 0 },
};

typedef struct {
	VkImage four[COLORS];
	VkDeviceMemory four_memory[COLORS];
	VkImageView four_views[COLORS + 1];
	VkImage depth;
	VkDeviceMemory depth_memory;
	VkImage after;
	VkDeviceMemory after_memory;
	VkImageView after_views[2];
	VkImage big_color;
	VkDeviceMemory big_color_memory;
	VkImage big_depth;
	VkDeviceMemory big_depth_memory;
	VkImageView big_views[2];
	// Cleared, drawn and stored; then loaded; then the untiled pass, of the second's depth and one attachment.
	VkRenderPass first_pass;
	VkRenderPass second_pass;
	VkRenderPass last_pass;
	VkFramebuffer four_target;
	VkFramebuffer after_target;
	// The shadow map's pass, its colour a don't-care; the pass that loads its depth; the one that samples it.
	VkRenderPass shadow_pass;
	VkRenderPass load_pass;
	VkRenderPass sample_pass;
	VkFramebuffer shadow_target;
	VkFramebuffer load_target;
	VkFramebuffer sample_target;
	VkRenderPass screen_pass;
	VkFramebuffer screens[HARNESS_MAX_IMAGES];
	VkPipelineLayout push_layout;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout sample_layout;
	VkSampler sampler;
	VkDescriptorPool pool;
	VkDescriptorSet sets[SETS];
	// Four colours testing and writing depth; one colour writing depth; one testing alone; sampling into 8_8_8_8 and
	// onto the screen.
	VkPipeline four_pipeline;
	VkPipeline shadow_pipeline;
	VkPipeline test_pipeline;
	VkPipeline sample_pipeline;
	VkPipeline screen_pipeline;
	VkBuffer unit_quad;
	VkDeviceMemory unit_quad_memory;
	VkBuffer quads;
	VkDeviceMemory quads_memory;
	VkBuffer readback;
	VkDeviceMemory readback_memory;
	const uint8_t *read;
	VkCommandBuffer four_commands[HARNESS_MAX_IMAGES];
	VkCommandBuffer big_commands[HARNESS_MAX_IMAGES];
	VkCommandBuffer work;
} Scene;

static VkAttachmentDescription color_of(VkFormat format, VkAttachmentLoadOp load, VkAttachmentStoreOp store)
{
	return (VkAttachmentDescription){ .format = format,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = load,
		.storeOp = store,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
}

// Depth and stencil alike; left to be read, a sampled depth image is resolved into its texture.
static VkAttachmentDescription depth_of(VkAttachmentLoadOp load, VkAttachmentStoreOp store, VkImageLayout final_layout)
{
	return (VkAttachmentDescription){ .format = VK_FORMAT_D24_UNORM_S8_UINT,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = load,
		.storeOp = store,
		.stencilLoadOp = load,
		.stencilStoreOp = store,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = final_layout };
}

// One subpass of count colour attachments, then depth if has_depth.
static VkResult make_pass(const Harness *harness, const VkAttachmentDescription *described, uint32_t count,
	bool has_depth, VkRenderPass *render_pass)
{
	VkAttachmentReference colors[COLORS];
	for (uint32_t i = 0; i < count; i++)
		colors[i] = (VkAttachmentReference){ i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	const VkAttachmentReference depth = { count, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	const VkSubpassDescription subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = count,
		.pColorAttachments = colors,
		.pDepthStencilAttachment = has_depth ? &depth : NULL,
	};
	const VkRenderPassCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = count + has_depth,
		.pAttachments = described,
		.subpassCount = 1,
		.pSubpasses = &subpass,
	};
	return vkCreateRenderPass(harness->device, &info, NULL, render_pass);
}

static VkResult make_framebuffer(const Harness *harness, VkRenderPass render_pass, const VkImageView *views,
	uint32_t count, uint32_t side_x, uint32_t side_y, VkFramebuffer *framebuffer)
{
	const VkFramebufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = render_pass,
		.attachmentCount = count,
		.pAttachments = views,
		.width = side_x,
		.height = side_y,
		.layers = 1,
	};
	return vkCreateFramebuffer(harness->device, &info, NULL, framebuffer);
}

static VkResult make_images(const Harness *harness, Scene *s)
{
	const VkImageUsageFlags colors =
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	const VkImageUsageFlags sampled_depth = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	VkResult result = VK_SUCCESS;
	for (uint32_t i = 0; i < COLORS && result == VK_SUCCESS; i++)
		result = harness_image(
			harness, attachments[i].format, WIDTH, HEIGHT, colors, &s->four[i], &s->four_memory[i], &s->four_views[i]);
	if (result == VK_SUCCESS)
		result = harness_depth_image(harness, &s->depth, &s->depth_memory, &s->four_views[COLORS]);
	if (result == VK_SUCCESS)
		result = harness_image(
			harness, VK_FORMAT_R8G8B8A8_UNORM, WIDTH, HEIGHT, colors, &s->after, &s->after_memory, &s->after_views[0]);
	s->after_views[1] = s->four_views[COLORS];
	if (result == VK_SUCCESS)
		result = harness_image(
			harness, VK_FORMAT_R8G8B8A8_UNORM, BIG, BIG, colors, &s->big_color, &s->big_color_memory, &s->big_views[0]);
	if (result == VK_SUCCESS)
		result = harness_image(harness, VK_FORMAT_D24_UNORM_S8_UINT, BIG, BIG, sampled_depth, &s->big_depth,
			&s->big_depth_memory, &s->big_views[1]);
	return result;
}

static VkResult make_passes(const Harness *harness, Scene *s)
{
	VkAttachmentDescription first[COLORS + 1];
	VkAttachmentDescription second[COLORS + 1];
	for (uint32_t i = 0; i < COLORS; i++) {
		first[i] = color_of(attachments[i].format, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE);
		second[i] = color_of(attachments[i].format, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE);
	}
	const VkImageLayout kept = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	first[COLORS] = depth_of(VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kept);
	second[COLORS] = depth_of(VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, kept);
	const VkAttachmentDescription last[2] = {
		color_of(VK_FORMAT_R8G8B8A8_UNORM, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE),
		depth_of(VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_DONT_CARE, kept),
	};
	const VkAttachmentDescription shadow[2] = {
		color_of(VK_FORMAT_R8G8B8A8_UNORM, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE),
		depth_of(
			VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL),
	};
	const VkAttachmentDescription load[2] = {
		color_of(VK_FORMAT_R8G8B8A8_UNORM, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE),
		depth_of(VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_DONT_CARE, kept),
	};
	VkResult result = make_pass(harness, first, COLORS, true, &s->first_pass);
	if (result == VK_SUCCESS)
		result = make_pass(harness, second, COLORS, true, &s->second_pass);
	if (result == VK_SUCCESS)
		result = make_pass(harness, last, 1, true, &s->last_pass);
	if (result == VK_SUCCESS)
		result = make_pass(harness, shadow, 1, true, &s->shadow_pass);
	if (result == VK_SUCCESS)
		result = make_pass(harness, load, 1, true, &s->load_pass);
	if (result == VK_SUCCESS)
		result = make_pass(harness, load, 1, false, &s->sample_pass);
	if (result == VK_SUCCESS)
		result = harness_render_pass(harness, &s->screen_pass, s->screens);
	return result;
}

// Both 1280x720 passes take one framebuffer, so the second waits for the first's recording.
static VkResult make_framebuffers(const Harness *harness, Scene *s)
{
	VkResult result =
		make_framebuffer(harness, s->first_pass, s->four_views, COLORS + 1, WIDTH, HEIGHT, &s->four_target);
	if (result == VK_SUCCESS)
		result = make_framebuffer(harness, s->last_pass, s->after_views, 2, WIDTH, HEIGHT, &s->after_target);
	if (result == VK_SUCCESS)
		result = make_framebuffer(harness, s->shadow_pass, s->big_views, 2, BIG, BIG, &s->shadow_target);
	if (result == VK_SUCCESS)
		result = make_framebuffer(harness, s->load_pass, s->big_views, 2, BIG, BIG, &s->load_target);
	if (result == VK_SUCCESS)
		result = make_framebuffer(harness, s->sample_pass, s->big_views, 1, BIG, BIG, &s->sample_target);
	return result;
}

static VkResult make_layouts(const Harness *harness, Scene *s)
{
	const VkPushConstantRange push_ranges[] = {
		{ VK_SHADER_STAGE_VERTEX_BIT, 0, PLACE_BYTES },
		{ VK_SHADER_STAGE_FRAGMENT_BIT, PLACE_BYTES, FOUR_COLOR_BYTES },
	};
	const VkPipelineLayoutCreateInfo push_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.pushConstantRangeCount = 2,
		.pPushConstantRanges = push_ranges,
	};
	VkResult result = vkCreatePipelineLayout(harness->device, &push_info, NULL, &s->push_layout);
	const VkDescriptorSetLayoutBinding binding = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
		VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
	const VkDescriptorSetLayoutCreateInfo set_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &binding,
	};
	if (result == VK_SUCCESS)
		result = vkCreateDescriptorSetLayout(harness->device, &set_info, NULL, &s->set_layout);
	const VkPipelineLayoutCreateInfo sample_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &s->set_layout,
	};
	if (result == VK_SUCCESS)
		result = vkCreatePipelineLayout(harness->device, &sample_info, NULL, &s->sample_layout);
	return result;
}

// Nearest texels, so each lands whole: the first attachment, the shadow map's colour, and its depth.
static VkResult make_sets(const Harness *harness, Scene *s)
{
	const VkSamplerCreateInfo sampler_info = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_NEAREST,
		.minFilter = VK_FILTER_NEAREST,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	};
	VkResult result = vkCreateSampler(harness->device, &sampler_info, NULL, &s->sampler);
	const VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SETS };
	const VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = SETS,
		.poolSizeCount = 1,
		.pPoolSizes = &size,
	};
	if (result == VK_SUCCESS)
		result = vkCreateDescriptorPool(harness->device, &pool_info, NULL, &s->pool);
	const VkDescriptorSetLayout layouts[SETS] = { s->set_layout, s->set_layout, s->set_layout };
	const VkDescriptorSetAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = s->pool,
		.descriptorSetCount = SETS,
		.pSetLayouts = layouts,
	};
	if (result == VK_SUCCESS)
		result = vkAllocateDescriptorSets(harness->device, &allocate_info, s->sets);
	const VkImageView views[SETS] = { s->four_views[0], s->big_views[0], s->big_views[1] };
	for (uint32_t i = 0; i < SETS && result == VK_SUCCESS; i++) {
		const VkDescriptorImageInfo image = { s->sampler, views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		const VkWriteDescriptorSet write = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = s->sets[i],
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &image,
		};
		vkUpdateDescriptorSets(harness->device, 1, &write, 0, NULL);
	}
	return result;
}

// A quad pipeline of push.vert and a fragment shader of the push constants, testing depth less-than.
static VkResult make_quad_pipeline(
	const Harness *harness, const Scene *s, bool four, bool writes, VkRenderPass render_pass, VkPipeline *pipeline)
{
	const VkPipelineDepthStencilStateCreateInfo depth = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = writes,
		.depthCompareOp = VK_COMPARE_OP_LESS,
	};
	const VkPipelineColorBlendAttachmentState opaque = { .colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
			VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };
	const VkPipelineColorBlendAttachmentState blends[COLORS] = { opaque, opaque, opaque, opaque };
	const HarnessPipeline spec = {
		.vertex_spirv = push_vert_spirv,
		.vertex_bytes = PUSH_VERT_SPIRV_BYTES,
		.fragment_spirv = four ? four_targets_frag_spirv : push_frag_spirv,
		.fragment_bytes = four ? FOUR_TARGETS_FRAG_SPIRV_BYTES : PUSH_FRAG_SPIRV_BYTES,
		.bindings = &harness_quad_binding,
		.binding_count = 1,
		.attributes = &harness_quad_attribute,
		.attribute_count = 1,
		.layout = s->push_layout,
		.depth_stencil = &depth,
		.blend = blends,
		.blend_count = four ? COLORS : 1,
		.dynamic_viewport = true,
	};
	return harness_pipeline(harness, &spec, render_pass, pipeline);
}

static VkResult make_sample_pipeline(
	const Harness *harness, const Scene *s, VkRenderPass render_pass, VkPipeline *pipeline)
{
	const HarnessPipeline spec = {
		.vertex_spirv = texture_vert_spirv,
		.vertex_bytes = TEXTURE_VERT_SPIRV_BYTES,
		.fragment_spirv = texture_frag_spirv,
		.fragment_bytes = TEXTURE_FRAG_SPIRV_BYTES,
		.bindings = &quad_binding,
		.binding_count = 1,
		.attributes = quad_attributes,
		.attribute_count = 2,
		.layout = s->sample_layout,
		.dynamic_viewport = true,
	};
	return harness_pipeline(harness, &spec, render_pass, pipeline);
}

// The cached buffer every image is copied into to be read.
static VkResult make_readback(const Harness *harness, Scene *s)
{
	const VkBufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = READBACK_BYTES,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	};
	const VkMemoryAllocateInfo allocate = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = READBACK_BYTES,
		.memoryTypeIndex = CACHED_TYPE,
	};
	void *mapped = NULL;
	VkResult result = vkCreateBuffer(harness->device, &info, NULL, &s->readback);
	if (result == VK_SUCCESS)
		result = vkAllocateMemory(harness->device, &allocate, NULL, &s->readback_memory);
	if (result == VK_SUCCESS)
		result = vkBindBufferMemory(harness->device, s->readback, s->readback_memory, 0);
	if (result == VK_SUCCESS)
		result = vkMapMemory(harness->device, s->readback_memory, 0, VK_WHOLE_SIZE, 0, &mapped);
	s->read = mapped;
	const VkCommandBufferAllocateInfo buffer_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = harness->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	if (result == VK_SUCCESS)
		result = vkAllocateCommandBuffers(harness->device, &buffer_info, &s->work);
	return result;
}

static bool build(const Harness *harness, Scene *s)
{
	return harness_ok("images", make_images(harness, s)) && harness_ok("render passes", make_passes(harness, s)) &&
		harness_ok("framebuffers past eDRAM", make_framebuffers(harness, s)) &&
		harness_ok("layouts", make_layouts(harness, s)) && harness_ok("descriptor sets", make_sets(harness, s)) &&
		harness_ok(
			"four targets' pipeline", make_quad_pipeline(harness, s, true, true, s->first_pass, &s->four_pipeline)) &&
		harness_ok(
			"shadow pipeline", make_quad_pipeline(harness, s, false, true, s->shadow_pass, &s->shadow_pipeline)) &&
		harness_ok("test pipeline", make_quad_pipeline(harness, s, false, false, s->last_pass, &s->test_pipeline)) &&
		harness_ok("sample pipeline", make_sample_pipeline(harness, s, s->sample_pass, &s->sample_pipeline)) &&
		harness_ok("screen pipeline", make_sample_pipeline(harness, s, s->screen_pass, &s->screen_pipeline)) &&
		harness_ok("unit quad", harness_quad_buffer(harness, &s->unit_quad, &s->unit_quad_memory)) &&
		harness_ok("quads",
			harness_buffer(
				harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, quads, sizeof quads, &s->quads, &s->quads_memory)) &&
		harness_ok("readback", make_readback(harness, s));
}

static void begin_pass(VkCommandBuffer commands, VkRenderPass render_pass, VkFramebuffer framebuffer,
	const VkRect2D *area, const VkClearValue *clears, uint32_t clear_count)
{
	const VkRenderPassBeginInfo info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = render_pass,
		.framebuffer = framebuffer,
		.renderArea = *area,
		.clearValueCount = clear_count,
		.pClearValues = clears,
	};
	vkCmdBeginRenderPass(commands, &info, VK_SUBPASS_CONTENTS_INLINE);
	const VkViewport viewport = { (float)area->offset.x, (float)area->offset.y, (float)area->extent.width,
		(float)area->extent.height, 0.0f, 1.0f };
	vkCmdSetViewport(commands, 0, 1, &viewport);
	vkCmdSetScissor(commands, 0, 1, area);
}

static void begin_whole(VkCommandBuffer commands, VkRenderPass render_pass, VkFramebuffer framebuffer, uint32_t side_x,
	uint32_t side_y, const VkClearValue *clears, uint32_t clear_count)
{
	const VkRect2D whole = { { 0, 0 }, { side_x, side_y } };
	begin_pass(commands, render_pass, framebuffer, &whole, clears, clear_count);
}

static HarnessQuad quad_of(const Rect *r, const float color[CHANNELS])
{
	HarnessQuad quad = { (float)r->x0, (float)r->y0, (float)(r->x1 - r->x0), (float)(r->y1 - r->y0), r->depth, { 0.0f },
		false };
	memcpy(quad.color, color, sizeof quad.color);
	return quad;
}

// A rectangle in the four attachments, each its own colour.
static void draw_four(VkCommandBuffer commands, const Scene *s, const Rect *r, uint32_t color)
{
	float colors[COLORS][CHANNELS];
	for (uint32_t i = 0; i < COLORS; i++)
		memcpy(colors[i], attachments[i].colors[color], sizeof colors[i]);
	const HarnessQuad quad = quad_of(r, colors[0]);
	harness_push_place_in(commands, s->push_layout, &quad, WIDTH, HEIGHT);
	vkCmdPushConstants(commands, s->push_layout, VK_SHADER_STAGE_FRAGMENT_BIT, PLACE_BYTES, FOUR_COLOR_BYTES, colors);
	vkCmdDraw(commands, QUAD_VERTICES, 1, 0, 0);
}

static void draw_shown(VkCommandBuffer commands, const Scene *s, uint32_t index, uint32_t set, uint32_t first_vertex)
{
	const VkDeviceSize offset = 0;
	begin_whole(commands, s->screen_pass, s->screens[index], WIDTH, HEIGHT, &screen_clear, 1);
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, s->screen_pipeline);
	vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, s->sample_layout, 0, 1, &s->sets[set], 0, NULL);
	vkCmdBindVertexBuffers(commands, 0, 1, &s->quads, &offset);
	vkCmdDraw(commands, QUAD_VERTICES, 1, first_vertex, 0);
	vkCmdEndRenderPass(commands);
}

// Both 1280x720 passes, the untiled one, then the first attachment on screen.
static void record_four(VkCommandBuffer commands, const Scene *s, uint32_t index)
{
	VkClearValue clears[COLORS + 1];
	for (uint32_t i = 0; i < COLORS; i++)
		memcpy(clears[i].color.float32, attachments[i].clear, sizeof clears[i].color.float32);
	clears[COLORS].depthStencil = (VkClearDepthStencilValue){ 1.0f, 0 };
	const VkDeviceSize offset = 0;
	begin_whole(commands, s->first_pass, s->four_target, WIDTH, HEIGHT, clears, COLORS + 1);
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, s->four_pipeline);
	vkCmdBindVertexBuffers(commands, 0, 1, &s->unit_quad, &offset);
	draw_four(commands, s, &far_quad, 0);
	draw_four(commands, s, &near_quad, 1);
	vkCmdEndRenderPass(commands);
	begin_whole(commands, s->second_pass, s->four_target, WIDTH, HEIGHT, NULL, 0);
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, s->four_pipeline);
	vkCmdBindVertexBuffers(commands, 0, 1, &s->unit_quad, &offset);
	draw_four(commands, s, &second_quad, 2);
	vkCmdEndRenderPass(commands);
	VkClearValue last_clears[2];
	memcpy(last_clears[0].color.float32, last_clear, sizeof last_clears[0].color.float32);
	last_clears[1].depthStencil = (VkClearDepthStencilValue){ 1.0f, 0 };
	begin_whole(commands, s->last_pass, s->after_target, WIDTH, HEIGHT, last_clears, 2);
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, s->test_pipeline);
	vkCmdBindVertexBuffers(commands, 0, 1, &s->unit_quad, &offset);
	const HarnessQuad quad = quad_of(&last_quad, last_color);
	harness_push_quad_in(commands, s->push_layout, &quad, WIDTH, HEIGHT);
	vkCmdEndRenderPass(commands);
	draw_shown(commands, s, index, SET_FOUR, 0);
}

// The shadow map, the pass that loads its depth, then that pass's top left on screen.
static void record_big(VkCommandBuffer commands, const Scene *s, uint32_t index)
{
	VkClearValue clears[2] = { { .color = { .float32 = { 0.0f } } } };
	clears[1].depthStencil = (VkClearDepthStencilValue){ 1.0f, 0 };
	const VkDeviceSize offset = 0;
	begin_whole(commands, s->shadow_pass, s->shadow_target, BIG, BIG, clears, 2);
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, s->shadow_pipeline);
	vkCmdBindVertexBuffers(commands, 0, 1, &s->unit_quad, &offset);
	for (uint32_t i = 0; i < BIG_RECTS; i++) {
		const HarnessQuad quad = quad_of(&big_rects[i], last_color);
		harness_push_quad_in(commands, s->push_layout, &quad, BIG, BIG);
	}
	vkCmdEndRenderPass(commands);
	memcpy(clears[0].color.float32, loaded_clear, sizeof clears[0].color.float32);
	begin_whole(commands, s->load_pass, s->load_target, BIG, BIG, clears, 2);
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, s->test_pipeline);
	vkCmdBindVertexBuffers(commands, 0, 1, &s->unit_quad, &offset);
	for (uint32_t i = 0; i < 2; i++) {
		const Rect all = { 0, 0, BIG, BIG, loaded_depths[i] };
		const HarnessQuad quad = quad_of(&all, loaded_colors[i]);
		harness_push_quad_in(commands, s->push_layout, &quad, BIG, BIG);
	}
	vkCmdEndRenderPass(commands);
	draw_shown(commands, s, index, SET_BIG_COLOR, QUAD_VERTICES);
}

static VkResult record(
	const Harness *harness, Scene *s, void (*each)(VkCommandBuffer, const Scene *, uint32_t), VkCommandBuffer *commands)
{
	VkResult result = harness_command_buffers(harness, commands);
	for (uint32_t i = 0; i < harness->image_count && result == VK_SUCCESS; i++) {
		each(commands[i], s, i);
		result = vkEndCommandBuffer(commands[i]);
	}
	return result;
}

// Records into the work command buffer, then runs it and waits.
static bool run_work(const Harness *harness, const Scene *s, void (*each)(VkCommandBuffer, const Scene *, const void *),
	const void *context)
{
	const VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	const VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &s->work
	};
	bool ok = harness_ok("vkBeginCommandBuffer", vkBeginCommandBuffer(s->work, &begin_info));
	if (ok)
		each(s->work, s, context);
	ok = ok && harness_ok("vkEndCommandBuffer", vkEndCommandBuffer(s->work)) &&
		harness_ok("vkQueueSubmit", vkQueueSubmit(harness->queue, 1, &submit, harness->fence)) &&
		harness_ok("vkWaitForFences", vkWaitForFences(harness->device, 1, &harness->fence, VK_TRUE, FENCE_TIMEOUT_NS));
	(void)vkResetFences(harness->device, 1, &harness->fence);
	return ok;
}

typedef struct {
	VkImage image;
	uint32_t width;
	uint32_t height;
} Readback;

static void copy_out(VkCommandBuffer commands, const Scene *s, const void *context)
{
	const Readback *const r = context;
	const VkBufferImageCopy region = {
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { r->width, r->height, 1 },
	};
	vkCmdCopyImageToBuffer(commands, r->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->readback, 1, &region);
}

static bool read_back(const Harness *harness, const Scene *s, VkImage image, uint32_t width, uint32_t height)
{
	const Readback r = { image, width, height };
	return run_work(harness, s, copy_out, &r);
}

static void sample_big(VkCommandBuffer commands, const Scene *s, const void *context)
{
	(void)context;
	const VkClearValue clear = { .color = { .float32 = { 0.0f } } };
	const VkDeviceSize offset = 0;
	begin_whole(commands, s->sample_pass, s->sample_target, BIG, BIG, &clear, 1);
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, s->sample_pipeline);
	vkCmdBindDescriptorSets(
		commands, VK_PIPELINE_BIND_POINT_GRAPHICS, s->sample_layout, 0, 1, &s->sets[SET_BIG_DEPTH], 0, NULL);
	vkCmdBindVertexBuffers(commands, 0, 1, &s->quads, &offset);
	vkCmdDraw(commands, QUAD_VERTICES, 1, 0, 0);
	vkCmdEndRenderPass(commands);
}

static bool inside(const Rect *r, uint32_t x, uint32_t y)
{
	return x >= r->x0 && x < r->x1 && y >= r->y0 && y < r->y1;
}

static float half_value(uint32_t half)
{
	const uint32_t exponent = half >> 10 & 0x1F;
	const float mantissa = (float)(half & 0x3FF) / 1024.0f;
	const float magnitude = exponent ? ldexpf(1.0f + mantissa, (int)exponent - 15) : ldexpf(mantissa, -14);
	return half & 0x8000 ? -magnitude : magnitude;
}

// Channel c of a texel read back, as its format holds it.
static float channel(const Attachment *a, const uint8_t *texel, uint32_t c)
{
	uint32_t word;
	uint16_t half;
	float value;
	switch (a->kind) {
	case KIND_BYTES:
		return texel[c] / 255.0f;
	case KIND_10_10_10_2:
		memcpy(&word, texel, sizeof word);
		return c == 3 ? (float)(word >> 30) / 3.0f : (float)(word >> (10 * c) & 0x3FF) / 1023.0f;
	case KIND_HALVES:
		memcpy(&half, texel + 2 * c, sizeof half);
		return half_value(half);
	default:
		memcpy(&value, texel, sizeof value);
		return value;
	}
}

// What each pass leaves: the first's quads over its clear, the second's quad where it's nearer.
static const float *four_texel(const Attachment *a, uint32_t x, uint32_t y)
{
	if (inside(&second_quad, x, y) && !inside(&near_quad, x, y))
		return a->colors[2];
	if (inside(&near_quad, x, y))
		return a->colors[1];
	return inside(&far_quad, x, y) ? a->colors[0] : a->clear;
}

// The untiled pass's quad shows where the depth the tiled ones left is farther: past both near quads.
static const float *after_texel(uint32_t x, uint32_t y)
{
	return inside(&near_quad, x, y) || inside(&second_quad, x, y) ? last_clear : last_color;
}

// Texels of an image read back, width by height, away from what want says, the first few logged.
static uint32_t count_wrong(const Harness *harness, const Scene *s, const Attachment *a, VkImage image, uint32_t width,
	uint32_t height, const float *(*want)(const Attachment *, uint32_t, uint32_t), const char *name)
{
	if (!read_back(harness, s, image, width, height))
		return width * height;
	uint32_t wrong = 0;
	for (uint32_t y = 0; y < height; y++) {
		for (uint32_t x = 0; x < width; x++) {
			const uint8_t *const texel = s->read + (y * width + x) * a->texel_bytes;
			const float *const wanted = want(a, x, y);
			bool near = true;
			for (uint32_t c = 0; c < a->channels; c++)
				near = near && fabsf(channel(a, texel, c) - wanted[c]) <= a->tolerance;
			if (near)
				continue;
			if (wrong++ < MISSES_LOGGED)
				xsdk_log_write("tiles: %s: (%u, %u) is %g %g %g %g, not %g %g %g %g", name, (unsigned)x, (unsigned)y,
					channel(a, texel, 0), channel(a, texel, 1 % a->channels), channel(a, texel, 2 % a->channels),
					channel(a, texel, 3 % a->channels), wanted[0], wanted[1 % a->channels], wanted[2 % a->channels],
					wanted[3 % a->channels]);
		}
	}
	return wrong;
}

static const float *after_want(const Attachment *a, uint32_t x, uint32_t y)
{
	(void)a;
	return after_texel(x, y);
}

static void check_four(const Harness *harness, const Scene *s)
{
	for (uint32_t i = 0; i < COLORS; i++) {
		char name[LABEL_BYTES];
		snprintf(name, sizeof name, "attachment %u, %s", (unsigned)i, attachments[i].name);
		const uint32_t wrong = count_wrong(harness, s, &attachments[i], s->four[i], WIDTH, HEIGHT, four_texel, name);
		xsdk_log_write("tiles: %s: %u of %u texels wrong", name, (unsigned)wrong, (unsigned)(WIDTH * HEIGHT));
		char label[2 * LABEL_BYTES];
		snprintf(label, sizeof label,
			"tiles: four attachments past eDRAM at 1280x720, %s, every texel after a pass clearing all and one "
			"loading all and depth",
			name);
		check(label, wrong == 0);
	}
	const uint32_t wrong =
		count_wrong(harness, s, &attachments[0], s->after, WIDTH, HEIGHT, after_want, "the untiled pass after");
	xsdk_log_write("tiles: the untiled pass after: %u of %u texels wrong", (unsigned)wrong, (unsigned)(WIDTH * HEIGHT));
	check("tiles: an untiled pass after them loads the depth they left whole at eDRAM's top, every texel", wrong == 0);
}

static float big_depth_at(uint32_t x, uint32_t y)
{
	float depth = 1.0f;
	for (uint32_t i = 0; i < BIG_RECTS; i++) {
		if (inside(&big_rects[i], x, y) && big_rects[i].depth < depth)
			depth = big_rects[i].depth;
	}
	return depth;
}

static uint32_t difference(uint32_t a, uint32_t b)
{
	return a > b ? a - b : b - a;
}

// Every texel of the shadow map's memory: its depth above stencil 0 (xsdk:docs/HARDWARE.md, "Depth resolves").
static uint32_t count_wrong_depth(const Harness *harness, const Scene *s)
{
	void *mapped = NULL;
	if (!harness_ok("vkMapMemory", vkMapMemory(harness->device, s->big_depth_memory, 0, VK_WHOLE_SIZE, 0, &mapped)))
		return BIG * BIG;
	volatile const uint32_t *const words = mapped;
	uint32_t wrong = 0;
	for (uint32_t y = 0; y < BIG; y++) {
		for (uint32_t x = 0; x < BIG; x++) {
			const uint32_t word = words[tiled_index(x, y, BIG)];
			const uint32_t want = (uint32_t)(big_depth_at(x, y) * (double)DEPTH_24_MAX + 0.5);
			if (difference(word >> 8, want) <= DEPTH_24_TOLERANCE && (word & 0xFF) == 0)
				continue;
			if (wrong++ < MISSES_LOGGED)
				xsdk_log_write("tiles: shadow map: (%u, %u) holds %08X, for depth %06X", (unsigned)x, (unsigned)y,
					(unsigned)word, (unsigned)want);
		}
	}
	vkUnmapMemory(harness->device, s->big_depth_memory);
	return wrong;
}

static const Attachment big_format = { "R8G8B8A8_UNORM", VK_FORMAT_R8G8B8A8_UNORM, KIND_BYTES, 4, 4, 1.5f / 255,
	{ 0.0f }, { { 0.0f } } };

// The pass that loads the map: its second quad where the map is deeper than 0.7, its first deeper than 0.5, else its
// clear.
static const float *loaded_texel(const Attachment *a, uint32_t x, uint32_t y)
{
	(void)a;
	const float depth = big_depth_at(x, y);
	return depth > loaded_depths[1] ? loaded_colors[1] : depth > loaded_depths[0] ? loaded_colors[0] : loaded_clear;
}

// The map sampled into colour: its depth in red, green and blue 0 and alpha 1 (xsdk:docs/HARDWARE.md, "Depth
// resolves").
static const float *sampled_texel(const Attachment *a, uint32_t x, uint32_t y)
{
	(void)a;
	static float sampled[CHANNELS];
	sampled[0] = big_depth_at(x, y);
	sampled[1] = 0.0f;
	sampled[2] = 0.0f;
	sampled[3] = 1.0f;
	return sampled;
}

static void check_big(const Harness *harness, const Scene *s)
{
	const uint32_t texels = BIG * BIG;
	const uint32_t wrong_depth = count_wrong_depth(harness, s);
	xsdk_log_write("tiles: shadow map: %u of %u texels' depth wrong", (unsigned)wrong_depth, (unsigned)texels);
	check("tiles: a 2048x2048 shadow map past eDRAM, every texel's depth in its memory, stored a band at a time",
		wrong_depth == 0);
	const uint32_t wrong_loaded =
		count_wrong(harness, s, &big_format, s->big_color, BIG, BIG, loaded_texel, "pass loading the shadow map");
	xsdk_log_write(
		"tiles: pass loading the shadow map: %u of %u texels wrong", (unsigned)wrong_loaded, (unsigned)texels);
	check("tiles: a 2048x2048 pass loading the shadow map's depth, drawn back a band at a time, every texel",
		wrong_loaded == 0);
	const bool sampled = run_work(harness, s, sample_big, NULL);
	const uint32_t wrong_sampled = sampled
		? count_wrong(harness, s, &big_format, s->big_color, BIG, BIG, sampled_texel, "shadow map sampled")
		: texels;
	xsdk_log_write("tiles: shadow map sampled: %u of %u texels wrong", (unsigned)wrong_sampled, (unsigned)texels);
	check(
		"tiles: a 2048x2048 pass past eDRAM sampling the shadow map, every texel's red its depth", wrong_sampled == 0);
}

typedef struct {
	VkQueryPool pool;
	uint32_t refusal;
} Refused;

// A pass past eDRAM with a query in it, a time stamp, or an area smaller than the framebuffer.
static void record_refused(VkCommandBuffer commands, const Scene *s, const void *context)
{
	const Refused *const r = context;
	const VkRect2D area = { { 0, 0 }, { r->refusal == 2 ? WIDTH / 2 : WIDTH, r->refusal == 2 ? HEIGHT / 2 : HEIGHT } };
	VkClearValue clears[COLORS + 1];
	memset(clears, 0, sizeof clears);
	begin_pass(commands, s->first_pass, s->four_target, &area, clears, COLORS + 1);
	if (r->refusal == 0) {
		vkCmdBeginQuery(commands, r->pool, 0, 0);
		vkCmdEndQuery(commands, r->pool, 0);
	} else if (r->refusal == 1) {
		vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, r->pool, 0);
	}
	vkCmdEndRenderPass(commands);
}

static VkResult end_refused(const Scene *s, uint32_t refusal, VkQueryPool pool)
{
	const VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	const Refused r = { pool, refusal };
	VkResult result = vkBeginCommandBuffer(s->work, &begin_info);
	if (result == VK_SUCCESS) {
		record_refused(s->work, s, &r);
		result = vkEndCommandBuffer(s->work);
	}
	return result;
}

// Depth that can't stay whole, loaded where its image isn't sampled, so its texture can't hold it.
static VkResult unsampled_depth_framebuffer(const Harness *harness, const Scene *s)
{
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkFramebuffer framebuffer = VK_NULL_HANDLE;
	VkResult result = harness_image(harness, VK_FORMAT_D24_UNORM_S8_UINT, BIG, BIG,
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, &image, &memory, &view);
	const VkImageView views[2] = { s->big_views[0], view };
	if (result == VK_SUCCESS)
		result = make_framebuffer(harness, s->load_pass, views, 2, BIG, BIG, &framebuffer);
	vkDestroyFramebuffer(harness->device, framebuffer, NULL);
	vkDestroyImageView(harness->device, view, NULL);
	vkDestroyImage(harness->device, image, NULL);
	vkFreeMemory(harness->device, memory, NULL);
	return result;
}

static bool refusals_hold(const Harness *harness, const Scene *s)
{
	const VkQueryPoolCreateInfo occlusion_info = {
		.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
		.queryType = VK_QUERY_TYPE_OCCLUSION,
		.queryCount = 1,
	};
	VkQueryPoolCreateInfo timestamp_info = occlusion_info;
	timestamp_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
	VkQueryPool pools[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
	bool held = harness_ok("occlusion pool", vkCreateQueryPool(harness->device, &occlusion_info, NULL, &pools[0])) &&
		harness_ok("time stamp pool", vkCreateQueryPool(harness->device, &timestamp_info, NULL, &pools[1]));
	const VkResult results[] = {
		held ? end_refused(s, 0, pools[0]) : VK_SUCCESS,
		held ? end_refused(s, 1, pools[1]) : VK_SUCCESS,
		held ? end_refused(s, 2, pools[0]) : VK_SUCCESS,
		held ? unsampled_depth_framebuffer(harness, s) : VK_SUCCESS,
	};
	for (uint32_t i = 0; i < sizeof results / sizeof results[0]; i++) {
		if (results[i] != VK_ERROR_FEATURE_NOT_PRESENT)
			xsdk_log_write("tiles: refusal %u returned %d", (unsigned)i, (int)results[i]);
		held = held && results[i] == VK_ERROR_FEATURE_NOT_PRESENT;
	}
	vkDestroyQueryPool(harness->device, pools[0], NULL);
	vkDestroyQueryPool(harness->device, pools[1], NULL);
	VkPhysicalDeviceProperties properties;
	vkGetPhysicalDeviceProperties(harness->physical_device, &properties);
	return held && properties.limits.maxFramebufferWidth == 4096 && properties.limits.maxFramebufferHeight == 4096;
}

static bool frames_shown(
	const Harness *harness, const VkCommandBuffer *commands, const HarnessProbe *probes, const char *what)
{
	HarnessProbes check = { what, probes, PROBES, TOLERANCE, 0 };
	uint32_t misses = 0;
	const uint32_t frames = harness_frames(harness, commands, FRAMES, harness_probes_hold, &check, &misses);
	return frames == FRAMES && misses == 0;
}

static void destroy(const Harness *harness, Scene *s)
{
	const VkDevice device = harness->device;
	vkFreeCommandBuffers(device, harness->pool, harness->image_count, s->four_commands);
	vkFreeCommandBuffers(device, harness->pool, harness->image_count, s->big_commands);
	vkFreeCommandBuffers(device, harness->pool, 1, &s->work);
	vkDestroyBuffer(device, s->readback, NULL);
	vkFreeMemory(device, s->readback_memory, NULL);
	vkDestroyBuffer(device, s->quads, NULL);
	vkFreeMemory(device, s->quads_memory, NULL);
	vkDestroyBuffer(device, s->unit_quad, NULL);
	vkFreeMemory(device, s->unit_quad_memory, NULL);
	const VkPipeline pipelines[] = { s->four_pipeline, s->shadow_pipeline, s->test_pipeline, s->sample_pipeline,
		s->screen_pipeline };
	for (uint32_t i = 0; i < sizeof pipelines / sizeof pipelines[0]; i++)
		vkDestroyPipeline(device, pipelines[i], NULL);
	vkDestroyDescriptorPool(device, s->pool, NULL);
	vkDestroySampler(device, s->sampler, NULL);
	vkDestroyPipelineLayout(device, s->sample_layout, NULL);
	vkDestroyDescriptorSetLayout(device, s->set_layout, NULL);
	vkDestroyPipelineLayout(device, s->push_layout, NULL);
	const VkFramebuffer framebuffers[] = { s->four_target, s->after_target, s->shadow_target, s->load_target,
		s->sample_target };
	for (uint32_t i = 0; i < sizeof framebuffers / sizeof framebuffers[0]; i++)
		vkDestroyFramebuffer(device, framebuffers[i], NULL);
	harness_destroy_render_pass(harness, s->screen_pass, s->screens);
	const VkRenderPass passes[] = { s->first_pass, s->second_pass, s->last_pass, s->shadow_pass, s->load_pass,
		s->sample_pass };
	for (uint32_t i = 0; i < sizeof passes / sizeof passes[0]; i++)
		vkDestroyRenderPass(device, passes[i], NULL);
	const VkImageView views[] = { s->four_views[0], s->four_views[1], s->four_views[2], s->four_views[3],
		s->four_views[COLORS], s->after_views[0], s->big_views[0], s->big_views[1] };
	for (uint32_t i = 0; i < sizeof views / sizeof views[0]; i++)
		vkDestroyImageView(device, views[i], NULL);
	const VkImage images[] = { s->four[0], s->four[1], s->four[2], s->four[3], s->depth, s->after, s->big_color,
		s->big_depth };
	const VkDeviceMemory memories[] = { s->four_memory[0], s->four_memory[1], s->four_memory[2], s->four_memory[3],
		s->depth_memory, s->after_memory, s->big_color_memory, s->big_depth_memory };
	for (uint32_t i = 0; i < sizeof images / sizeof images[0]; i++) {
		vkDestroyImage(device, images[i], NULL);
		vkFreeMemory(device, memories[i], NULL);
	}
}

void test_tiles(const Harness *harness)
{
	static Scene scene;
	Scene *const s = &scene;
	memset(s, 0, sizeof *s);
	const bool built = harness->swapchain && build(harness, s);
	check("tiles: images, render passes past eDRAM and their framebuffers, and pipelines made", built);
	const bool four_recorded = built && harness_ok("recording", record(harness, s, record_four, s->four_commands));
	check(
		"tiles: 3 frames of four attachments past eDRAM loaded and stored across two passes, then an untiled one, the "
		"first attachment on screen in every band",
		four_recorded && frames_shown(harness, s->four_commands, four_probes, "tiles: four attachments"));
	if (four_recorded)
		check_four(harness, s);
	const bool big_recorded = built && harness_ok("recording", record(harness, s, record_big, s->big_commands));
	check("tiles: 3 frames of a 2048x2048 shadow map in bands, then a pass loading its depth, its top left on screen",
		big_recorded && frames_shown(harness, s->big_commands, big_probes, "tiles: shadow map"));
	if (big_recorded)
		check_big(harness, s);
	check(
		"tiles: a query, a time stamp and a render area smaller than the framebuffer refused in a pass past eDRAM, "
		"depth in bands loaded from an image that isn't sampled refused, and framebuffers to 4096x4096",
		built && refusals_hold(harness, s));
	destroy(harness, s);
}
