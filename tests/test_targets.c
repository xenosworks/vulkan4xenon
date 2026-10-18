#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "xsdk/log.h"

// Milestone 25: colour attachments in the new formats, four at once. A pass draws into four attachments of four
// formats, R8G8B8A8, A2B10G10R10, R16G16B16A16_SFLOAT and R32_SFLOAT: one draw writes each its own colour over the left
// half, a second over the top half blends and masks each its own way, and vkCmdClearAttachments clears the second and
// third in a corner. A second pass draws the half-float image back as its second attachment, beside an R16G16_SFLOAT
// one, and a quad over both. A third samples all five onto the screen, scaled so values past 1 show. Every image is
// read back and checked by value, each channel apart from the others.

enum {
	FRAMES = 10,
	TOLERANCE = 3,
	IMAGE_WIDTH = 128,
	IMAGE_HEIGHT = 64,
	// The first draw covers the columns left of this; the second, the rows above BLEND_BOTTOM.
	QUAD_RIGHT = 64,
	BLEND_BOTTOM = 32,
	// vkCmdClearAttachments' corner.
	RECT_X = 96,
	RECT_Y = 48,
	// The second pass's quad.
	MIDDLE_X = 32,
	MIDDLE_Y = 16,
	MIDDLE_WIDTH = 64,
	MIDDLE_HEIGHT = 32,
	COLORS = 4,
	CHANNELS = 4,
	// Where the screen's pass shows each image, at its size.
	SHOWN_X = 32,
	SHOWN_STEP = 248,
	SHOWN_Y = 120,
	VERTEX_FLOATS = 6,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	UV_OFFSET_BYTES = 16,
	QUAD_VERTICES = 6,
	FENCE_TIMEOUT_NS = 1000000000,
	// The memory type the CPU caches, which buffers only copied to and from may take.
	CACHED_TYPE = 1,
	MAX_TEXEL_BYTES = 8,
	READBACK_BYTES = IMAGE_WIDTH * IMAGE_HEIGHT * MAX_TEXEL_BYTES,
	MISSES_LOGGED = 4,
	LABEL_BYTES = 160,
};

enum {
	IMAGE_RGBA8,
	IMAGE_10_BIT,
	IMAGE_HALF4,
	IMAGE_FLOAT,
	IMAGE_HALF2,
	IMAGES,
};

enum {
	PIPELINE_PLAIN,
	PIPELINE_BLENDED,
	PIPELINE_AGAIN,
	PIPELINE_SAMPLE,
	PIPELINES,
};

// How an image's texel reads back.
enum {
	KIND_BYTES,
	KIND_10_10_10_2,
	KIND_HALVES,
	KIND_FLOATS,
};

typedef struct {
	const char *name;
	VkFormat format;
	uint32_t kind;
	uint32_t channels;
	uint32_t texel_bytes;
	// How far a channel may be from the CPU's sums: a unorm's step and a blend's rounding; floats exact.
	float tolerance;
} Format;

static const Format formats[IMAGES] = {
	[IMAGE_RGBA8] = { "R8G8B8A8_UNORM", VK_FORMAT_R8G8B8A8_UNORM, KIND_BYTES, 4, 4, 1.5f / 255 },
	[IMAGE_10_BIT] = { "A2B10G10R10_UNORM_PACK32", VK_FORMAT_A2B10G10R10_UNORM_PACK32, KIND_10_10_10_2, 4, 4,
		2.0f / 1023 },
	[IMAGE_HALF4] = { "R16G16B16A16_SFLOAT", VK_FORMAT_R16G16B16A16_SFLOAT, KIND_HALVES, 4, 8, 0.0f },
	[IMAGE_FLOAT] = { "R32_SFLOAT", VK_FORMAT_R32_SFLOAT, KIND_FLOATS, 1, 4, 0.0f },
	[IMAGE_HALF2] = { "R16G16_SFLOAT", VK_FORMAT_R16G16_SFLOAT, KIND_HALVES, 2, 4, 0.0f },
};

// What the four-attachment pass leaves in each quarter, and in vkCmdClearAttachments' corner.
enum {
	QUARTER_BOTH,
	QUARTER_CLEAR_SECOND,
	QUARTER_FIRST,
	QUARTER_CLEAR,
	QUARTER_RECT,
	QUARTERS,
};

// One of the four attachments: its clear, its colour in the first draw and the second, how the second blends and
// masks it, the corner's clear if vkCmdClearAttachments clears it, and what each quarter then holds.
typedef struct {
	const char *what;
	float clear[CHANNELS];
	float first[CHANNELS];
	float second[CHANNELS];
	VkPipelineColorBlendAttachmentState blend;
	bool rect_cleared;
	float rect[CHANNELS];
	float quarters[QUARTERS][CHANNELS];
} Attachment;

#define RGBA_MASK \
	(VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT)

static const Attachment attachments[COLORS] = {
	{ "the second draw added onto the first's", { 0.2f, 0.4f, 0.6f, 0.8f }, { 0.8f, 0.6f, 0.0f, 1.0f },
		{ 0.2f, 0.2f, 0.4f, 0.0f },
		{ VK_TRUE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE,
			VK_BLEND_OP_ADD, RGBA_MASK },
		false, { 0.0f },
		{ { 1.0f, 0.8f, 0.4f, 1.0f }, { 0.4f, 0.6f, 1.0f, 0.8f }, { 0.8f, 0.6f, 0.0f, 1.0f },
			{ 0.2f, 0.4f, 0.6f, 0.8f }, { 0.2f, 0.4f, 0.6f, 0.8f } } },
	// The second draw's alpha is one 2 bits hold, as the console rounds the source to the attachment's bits before it
	// blends (docs/ROADMAP.md, milestone 25's "On the console").
	{ "the second draw blended by its alpha, red, green and blue alone, and cleared in the corner",
		{ 0.2f, 0.4f, 0.6f, 1.0f / 3 }, { 0.8f, 0.6f, 0.4f, 1.0f }, { 0.0f, 1.0f, 0.2f, 1.0f / 3 },
		{ VK_TRUE, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE,
			VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
			VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT },
		true, { 0.9f, 0.1f, 0.5f, 2.0f / 3 },
		{ { 1.6f / 3, 2.2f / 3, 1.0f / 3, 1.0f }, { 0.4f / 3, 0.6f, 1.4f / 3, 1.0f / 3 }, { 0.8f, 0.6f, 0.4f, 1.0f },
			{ 0.2f, 0.4f, 0.6f, 1.0f / 3 }, { 0.9f, 0.1f, 0.5f, 2.0f / 3 } } },
	{ "past 1, the second draw's red and alpha alone, and cleared in the corner below 0 and past 1",
		{ 1.25f, 0.5f, 0.75f, 2.0f }, { 1.5f, 2.25f, 3.0f, 4.0f }, { 6.0f, 7.0f, 8.0f, 9.0f },
		{ VK_FALSE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE,
			VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_A_BIT },
		true, { -3.0f, 100.0f, 0.5f, 7.0f },
		{ { 6.0f, 2.25f, 3.0f, 9.0f }, { 6.0f, 0.5f, 0.75f, 9.0f }, { 1.5f, 2.25f, 3.0f, 4.0f },
			{ 1.25f, 0.5f, 0.75f, 2.0f }, { -3.0f, 100.0f, 0.5f, 7.0f } } },
	{ "past 1 from a float output, kept from the second draw", { 3.5f }, { 5.5f }, { 10.0f },
		{ VK_FALSE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE,
			VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, 0 },
		false, { 0.0f }, { { 5.5f }, { 3.5f }, { 5.5f }, { 3.5f }, { 3.5f } } },
};

// The second pass: the R16G16_SFLOAT image's clear, and the quad's colours, past 1 and below 0.
static const float half2_clear[CHANNELS] = { 0.25f, 0.5f };
static const float again_colors[COLORS][CHANNELS] = {
	{ 1.5f, 1000.0f, 0.0f, 0.0f },
	{ 2.0f, -3.0f, 0.5f, 6.5f },
};

// What the screen's pass scales each image by.
static const float scales[IMAGES][CHANNELS] = {
	[IMAGE_RGBA8] = { 1.0f, 1.0f, 1.0f, 1.0f },
	[IMAGE_10_BIT] = { 1.0f, 1.0f, 1.0f, 1.0f },
	[IMAGE_HALF4] = { 0.1f, 0.1f, 0.1f, 1.0f },
	[IMAGE_FLOAT] = { 0.1f, 1.0f, 1.0f, 1.0f },
	[IMAGE_HALF2] = { 0.5f, 0.0005f, 1.0f, 1.0f },
};

typedef struct {
	const char *name;
	uint32_t image;
	uint32_t x;
	uint32_t y;
	Rgb want;
} ShownTexel;

// Texels on screen, scaled, each in a quarter of its own: so a value read back unscaled, or a channel misplaced,
// shows here too.
static const ShownTexel shown_texels[] = {
	{ "R8G8B8A8, the first draw", IMAGE_RGBA8, 8, 56, { 204, 153, 0 } },
	{ "R8G8B8A8, both draws added", IMAGE_RGBA8, 8, 8, { 255, 204, 102 } },
	{ "A2B10G10R10, blended by alpha", IMAGE_10_BIT, 8, 8, { 136, 187, 85 } },
	{ "A2B10G10R10, the corner's clear", IMAGE_10_BIT, 110, 56, { 230, 26, 128 } },
	{ "R16G16B16A16, the second draw's red over the clear", IMAGE_HALF4, 120, 8, { 153, 13, 19 } },
	{ "R16G16B16A16, the first draw past 1", IMAGE_HALF4, 8, 56, { 38, 57, 77 } },
	{ "R16G16B16A16, the second pass's quad", IMAGE_HALF4, 64, 32, { 51, 0, 13 } },
	{ "R16G16B16A16, the corner's clear past 1", IMAGE_HALF4, 110, 56, { 0, 255, 13 } },
	{ "R32, the first draw past 1", IMAGE_FLOAT, 8, 56, { 140, 0, 0 } },
	{ "R32, the clear", IMAGE_FLOAT, 70, 56, { 89, 0, 0 } },
	{ "R16G16, the second pass's quad past 1", IMAGE_HALF2, 64, 32, { 191, 128, 0 } },
	{ "R16G16, its clear", IMAGE_HALF2, 8, 8, { 32, 0, 0 } },
};

enum { SHOWN_TEXELS = sizeof shown_texels / sizeof shown_texels[0] };

static const VkClearValue screen_clear = { .color = { .float32 = { 24 / 255.0f, 24 / 255.0f, 24 / 255.0f, 1.0f } } };

// Clip space, then texture coordinates, over the whole viewport.
static const float image_quad[QUAD_VERTICES][VERTEX_FLOATS] = {
	{ -1.0f, -1.0f, 0.0f, 1.0f, 0.0f, 0.0f },
	{ 1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f },
	{ -1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f },
	{ 1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f },
	{ 1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f },
	{ -1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f },
};

static const VkVertexInputBindingDescription image_binding = { 0, STRIDE_BYTES, VK_VERTEX_INPUT_RATE_VERTEX };
static const VkVertexInputAttributeDescription image_attributes[] = {
	{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0 },
	{ .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = UV_OFFSET_BYTES },
};

typedef struct {
	VkImage images[IMAGES];
	VkDeviceMemory image_memory[IMAGES];
	VkImageView views[IMAGES];
	VkRenderPass four_pass;
	VkRenderPass again_pass;
	VkFramebuffer four;
	VkFramebuffer again;
	VkRenderPass screen_pass;
	VkFramebuffer screens[HARNESS_MAX_IMAGES];
	VkPipelineLayout push_layout;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout sample_layout;
	VkSampler sampler;
	VkDescriptorPool pool;
	VkDescriptorSet sets[IMAGES];
	VkPipeline pipelines[PIPELINES];
	VkBuffer unit_quad;
	VkDeviceMemory unit_quad_memory;
	VkBuffer image_vertices;
	VkDeviceMemory image_vertex_memory;
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	HarnessProbe probes[SHOWN_TEXELS];
	HarnessProbes check;
} Scene;

// Images sampled after the pass that draws them, and the second pass loads the first's half-float image.
static const VkSubpassDependency dependencies[2] = {
	{ VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0 },
	{ 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, 0 },
};

static VkAttachmentDescription attachment_of(VkFormat format, VkAttachmentLoadOp load)
{
	return (VkAttachmentDescription){ .format = format,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = load,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout =
			load == VK_ATTACHMENT_LOAD_OP_LOAD ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
}

// One subpass with these colour attachments, in order.
static VkResult make_pass(
	const Harness *harness, const VkAttachmentDescription *described, uint32_t count, VkRenderPass *render_pass)
{
	VkAttachmentReference references[COLORS + 1];
	for (uint32_t i = 0; i < count; i++)
		references[i] = (VkAttachmentReference){ i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	const VkSubpassDescription subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = count,
		.pColorAttachments = references,
	};
	const VkRenderPassCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = count,
		.pAttachments = described,
		.subpassCount = 1,
		.pSubpasses = &subpass,
		.dependencyCount = 2,
		.pDependencies = dependencies,
	};
	return vkCreateRenderPass(harness->device, &info, NULL, render_pass);
}

static VkResult make_framebuffer(
	const Harness *harness, VkRenderPass render_pass, const VkImageView *views, uint32_t count, VkFramebuffer *out)
{
	const VkFramebufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = render_pass,
		.attachmentCount = count,
		.pAttachments = views,
		.width = IMAGE_WIDTH,
		.height = IMAGE_HEIGHT,
		.layers = 1,
	};
	return vkCreateFramebuffer(harness->device, &info, NULL, out);
}

static VkResult make_passes(const Harness *harness, Scene *scene)
{
	const VkImageUsageFlags usage =
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	VkResult result = VK_SUCCESS;
	for (uint32_t i = 0; i < IMAGES && result == VK_SUCCESS; i++)
		result = harness_image(harness, formats[i].format, IMAGE_WIDTH, IMAGE_HEIGHT, usage, &scene->images[i],
			&scene->image_memory[i], &scene->views[i]);
	VkAttachmentDescription four[COLORS];
	for (uint32_t i = 0; i < COLORS; i++)
		four[i] = attachment_of(formats[i].format, VK_ATTACHMENT_LOAD_OP_CLEAR);
	const VkAttachmentDescription again[2] = { attachment_of(VK_FORMAT_R16G16_SFLOAT, VK_ATTACHMENT_LOAD_OP_CLEAR),
		attachment_of(VK_FORMAT_R16G16B16A16_SFLOAT, VK_ATTACHMENT_LOAD_OP_LOAD) };
	if (result == VK_SUCCESS)
		result = make_pass(harness, four, COLORS, &scene->four_pass);
	if (result == VK_SUCCESS)
		result = make_pass(harness, again, 2, &scene->again_pass);
	if (result == VK_SUCCESS)
		result = make_framebuffer(harness, scene->four_pass, scene->views, COLORS, &scene->four);
	const VkImageView again_views[2] = { scene->views[IMAGE_HALF2], scene->views[IMAGE_HALF4] };
	if (result == VK_SUCCESS)
		result = make_framebuffer(harness, scene->again_pass, again_views, 2, &scene->again);
	if (result == VK_SUCCESS)
		result = harness_render_pass(harness, &scene->screen_pass, scene->screens);
	return result;
}

static VkResult make_layouts(const Harness *harness, Scene *scene)
{
	// push.vert's place, then four_targets.frag's four colours.
	const VkPushConstantRange push_ranges[] = {
		{ VK_SHADER_STAGE_VERTEX_BIT, 0, 32 },
		{ VK_SHADER_STAGE_FRAGMENT_BIT, 32, COLORS * 16 },
	};
	const VkPipelineLayoutCreateInfo push_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.pushConstantRangeCount = 2,
		.pPushConstantRanges = push_ranges,
	};
	VkResult result = vkCreatePipelineLayout(harness->device, &push_info, NULL, &scene->push_layout);
	const VkDescriptorSetLayoutBinding binding = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
		VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
	const VkDescriptorSetLayoutCreateInfo set_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &binding,
	};
	if (result == VK_SUCCESS)
		result = vkCreateDescriptorSetLayout(harness->device, &set_info, NULL, &scene->set_layout);
	const VkPushConstantRange scale_range = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
	const VkPipelineLayoutCreateInfo sample_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &scene->set_layout,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &scale_range,
	};
	if (result == VK_SUCCESS)
		result = vkCreatePipelineLayout(harness->device, &sample_info, NULL, &scene->sample_layout);
	return result;
}

// Nearest texels, so each lands whole, a set for each image.
static VkResult make_sets(const Harness *harness, Scene *scene)
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
	VkResult result = vkCreateSampler(harness->device, &sampler_info, NULL, &scene->sampler);
	const VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, IMAGES };
	const VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = IMAGES,
		.poolSizeCount = 1,
		.pPoolSizes = &size,
	};
	if (result == VK_SUCCESS)
		result = vkCreateDescriptorPool(harness->device, &pool_info, NULL, &scene->pool);
	VkDescriptorSetLayout layouts[IMAGES];
	for (uint32_t i = 0; i < IMAGES; i++)
		layouts[i] = scene->set_layout;
	const VkDescriptorSetAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = scene->pool,
		.descriptorSetCount = IMAGES,
		.pSetLayouts = layouts,
	};
	if (result == VK_SUCCESS)
		result = vkAllocateDescriptorSets(harness->device, &allocate_info, scene->sets);
	for (uint32_t i = 0; i < IMAGES && result == VK_SUCCESS; i++) {
		const VkDescriptorImageInfo image = { scene->sampler, scene->views[i],
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		const VkWriteDescriptorSet write = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = scene->sets[i],
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &image,
		};
		vkUpdateDescriptorSets(harness->device, 1, &write, 0, NULL);
	}
	return result;
}

static VkResult make_pipeline(const Harness *harness, Scene *scene, uint32_t kind)
{
	const VkPipelineColorBlendAttachmentState opaque = { .colorWriteMask = RGBA_MASK };
	VkPipelineColorBlendAttachmentState blends[COLORS];
	for (uint32_t i = 0; i < COLORS; i++)
		blends[i] = kind == PIPELINE_BLENDED ? attachments[i].blend : opaque;
	const bool sampled = kind == PIPELINE_SAMPLE;
	const HarnessPipeline spec = {
		.vertex_spirv = sampled ? texture_vert_spirv : push_vert_spirv,
		.vertex_bytes = sampled ? TEXTURE_VERT_SPIRV_BYTES : PUSH_VERT_SPIRV_BYTES,
		.fragment_spirv = sampled ? scaled_frag_spirv : four_targets_frag_spirv,
		.fragment_bytes = sampled ? SCALED_FRAG_SPIRV_BYTES : FOUR_TARGETS_FRAG_SPIRV_BYTES,
		.bindings = sampled ? &image_binding : &harness_quad_binding,
		.binding_count = 1,
		.attributes = sampled ? image_attributes : &harness_quad_attribute,
		.attribute_count = sampled ? 2 : 1,
		.layout = sampled ? scene->sample_layout : scene->push_layout,
		.blend = sampled ? NULL : blends,
		.blend_count = sampled       ? 0
			: kind == PIPELINE_AGAIN ? 2
									 : COLORS,
		.dynamic_viewport = true,
	};
	const VkRenderPass render_pass = sampled ? scene->screen_pass
		: kind == PIPELINE_AGAIN             ? scene->again_pass
											 : scene->four_pass;
	return harness_pipeline(harness, &spec, render_pass, &scene->pipelines[kind]);
}

static bool build(const Harness *harness, Scene *scene)
{
	bool ok = harness_ok("images and render passes", make_passes(harness, scene)) &&
		harness_ok("layouts", make_layouts(harness, scene)) && harness_ok("descriptor sets", make_sets(harness, scene));
	for (uint32_t i = 0; i < PIPELINES && ok; i++)
		ok = harness_ok("pipeline", make_pipeline(harness, scene, i));
	return ok && harness_ok("unit quad", harness_quad_buffer(harness, &scene->unit_quad, &scene->unit_quad_memory)) &&
		harness_ok("image quad",
			harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, image_quad, sizeof image_quad,
				&scene->image_vertices, &scene->image_vertex_memory));
}

static void set_area(VkCommandBuffer commands, const VkRect2D *area)
{
	const VkViewport viewport = { (float)area->offset.x, (float)area->offset.y, (float)area->extent.width,
		(float)area->extent.height, 0.0f, 1.0f };
	vkCmdSetViewport(commands, 0, 1, &viewport);
	vkCmdSetScissor(commands, 0, 1, area);
}

static void begin(VkCommandBuffer commands, VkRenderPass render_pass, VkFramebuffer framebuffer, uint32_t width,
	uint32_t height, const VkClearValue *clears, uint32_t clear_count)
{
	const VkRect2D whole = { { 0, 0 }, { width, height } };
	const VkRenderPassBeginInfo info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = render_pass,
		.framebuffer = framebuffer,
		.renderArea = whole,
		.clearValueCount = clear_count,
		.pClearValues = clears,
	};
	vkCmdBeginRenderPass(commands, &info, VK_SUBPASS_CONTENTS_INLINE);
	set_area(commands, &whole);
}

// A rectangle of the image's pixels, its four colours from the fragment range.
static void draw_colors(VkCommandBuffer commands, const Scene *scene, uint32_t pipeline, const VkRect2D *rect,
	const float colors[COLORS][CHANNELS])
{
	const VkDeviceSize offset = 0;
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipelines[pipeline]);
	vkCmdBindVertexBuffers(commands, 0, 1, &scene->unit_quad, &offset);
	const HarnessQuad quad = { (float)rect->offset.x, (float)rect->offset.y, (float)rect->extent.width,
		(float)rect->extent.height, 0.0f, { 0.0f }, false };
	harness_push_place_in(commands, scene->push_layout, &quad, IMAGE_WIDTH, IMAGE_HEIGHT);
	vkCmdPushConstants(commands, scene->push_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 32, COLORS * CHANNELS * 4, colors);
	vkCmdDraw(commands, QUAD_VERTICES, 1, 0, 0);
}

static void record_four(VkCommandBuffer commands, const Scene *scene)
{
	VkClearValue clears[COLORS];
	float first[COLORS][CHANNELS];
	float second[COLORS][CHANNELS];
	for (uint32_t i = 0; i < COLORS; i++) {
		memcpy(clears[i].color.float32, attachments[i].clear, sizeof clears[i].color.float32);
		memcpy(first[i], attachments[i].first, sizeof first[i]);
		memcpy(second[i], attachments[i].second, sizeof second[i]);
	}
	begin(commands, scene->four_pass, scene->four, IMAGE_WIDTH, IMAGE_HEIGHT, clears, COLORS);
	const VkRect2D left = { { 0, 0 }, { QUAD_RIGHT, IMAGE_HEIGHT } };
	const VkRect2D top = { { 0, 0 }, { IMAGE_WIDTH, BLEND_BOTTOM } };
	draw_colors(commands, scene, PIPELINE_PLAIN, &left, first);
	draw_colors(commands, scene, PIPELINE_BLENDED, &top, second);
	VkClearAttachment cleared[COLORS];
	uint32_t count = 0;
	for (uint32_t i = 0; i < COLORS; i++) {
		if (!attachments[i].rect_cleared)
			continue;
		cleared[count] = (VkClearAttachment){ .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = i };
		memcpy(cleared[count++].clearValue.color.float32, attachments[i].rect, sizeof attachments[i].rect);
	}
	const VkClearRect rect = { { { RECT_X, RECT_Y }, { IMAGE_WIDTH - RECT_X, IMAGE_HEIGHT - RECT_Y } }, 0, 1 };
	vkCmdClearAttachments(commands, count, cleared, 1, &rect);
	vkCmdEndRenderPass(commands);
}

static void record_again(VkCommandBuffer commands, const Scene *scene)
{
	VkClearValue clears[2] = { { .color = { .float32 = { 0.0f } } } };
	memcpy(clears[0].color.float32, half2_clear, sizeof half2_clear);
	begin(commands, scene->again_pass, scene->again, IMAGE_WIDTH, IMAGE_HEIGHT, clears, 2);
	const VkRect2D middle = { { MIDDLE_X, MIDDLE_Y }, { MIDDLE_WIDTH, MIDDLE_HEIGHT } };
	draw_colors(commands, scene, PIPELINE_AGAIN, &middle, again_colors);
	vkCmdEndRenderPass(commands);
}

// Each image at its size, scaled.
static void record_screen(VkCommandBuffer commands, const Scene *scene, uint32_t index)
{
	begin(commands, scene->screen_pass, scene->screens[index], HARNESS_WIDTH, HARNESS_HEIGHT, &screen_clear, 1);
	const VkDeviceSize offset = 0;
	for (uint32_t i = 0; i < IMAGES; i++) {
		const VkRect2D area = { { (int32_t)(SHOWN_X + i * SHOWN_STEP), SHOWN_Y }, { IMAGE_WIDTH, IMAGE_HEIGHT } };
		set_area(commands, &area);
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipelines[PIPELINE_SAMPLE]);
		vkCmdBindDescriptorSets(
			commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->sample_layout, 0, 1, &scene->sets[i], 0, NULL);
		vkCmdPushConstants(
			commands, scene->sample_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof scales[i], scales[i]);
		vkCmdBindVertexBuffers(commands, 0, 1, &scene->image_vertices, &offset);
		vkCmdDraw(commands, QUAD_VERTICES, 1, 0, 0);
	}
	vkCmdEndRenderPass(commands);
}

static VkResult record(const Harness *harness, Scene *scene)
{
	VkResult result = harness_command_buffers(harness, scene->commands);
	for (uint32_t i = 0; i < harness->image_count && result == VK_SUCCESS; i++) {
		record_four(scene->commands[i], scene);
		record_again(scene->commands[i], scene);
		record_screen(scene->commands[i], scene, i);
		result = vkEndCommandBuffer(scene->commands[i]);
	}
	return result;
}

static void make_probes(Scene *scene)
{
	for (uint32_t i = 0; i < SHOWN_TEXELS; i++) {
		const ShownTexel *const texel = &shown_texels[i];
		scene->probes[i] = (HarnessProbe){ texel->name, SHOWN_X + texel->image * SHOWN_STEP + texel->x,
			SHOWN_Y + texel->y, texel->want, 0 };
	}
	scene->check = (HarnessProbes){ "targets", scene->probes, SHOWN_TEXELS, TOLERANCE, 0 };
}

static float half_value(uint32_t half)
{
	const uint32_t exponent = half >> 10 & 0x1F;
	const float mantissa = (float)(half & 0x3FF) / 1024.0f;
	const float magnitude = exponent ? ldexpf(1.0f + mantissa, (int)exponent - 15) : ldexpf(mantissa, -14);
	return half & 0x8000 ? -magnitude : magnitude;
}

// Channel c of texel (x, y) of an image read back into bytes, as its format holds it.
static float channel(const Format *format, const uint8_t *bytes, uint32_t x, uint32_t y, uint32_t c)
{
	const uint8_t *const texel = bytes + (y * IMAGE_WIDTH + x) * format->texel_bytes;
	uint32_t word;
	uint16_t half;
	float value;
	switch (format->kind) {
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

// Counts the texels away from what want says each should hold, logging the first few.
static uint32_t count_wrong(const Format *format, const uint8_t *bytes, const char *name,
	const float *(*want)(uint32_t x, uint32_t y, const void *context), const void *context)
{
	uint32_t wrong = 0;
	for (uint32_t y = 0; y < IMAGE_HEIGHT; y++) {
		for (uint32_t x = 0; x < IMAGE_WIDTH; x++) {
			const float *const wanted = want(x, y, context);
			float got[CHANNELS] = { 0.0f };
			bool near = true;
			for (uint32_t c = 0; c < format->channels; c++) {
				got[c] = channel(format, bytes, x, y, c);
				near = near && fabsf(got[c] - wanted[c]) <= format->tolerance;
			}
			if (near)
				continue;
			if (wrong < MISSES_LOGGED)
				xsdk_log_write("targets: %s: (%u, %u) is %g %g %g %g, not %g %g %g %g", name, (unsigned)x, (unsigned)y,
					got[0], got[1], got[2], got[3], wanted[0], format->channels > 1 ? wanted[1] : 0.0f,
					format->channels > 2 ? wanted[2] : 0.0f, format->channels > 3 ? wanted[3] : 0.0f);
			wrong++;
		}
	}
	return wrong;
}

static uint32_t quarter_of(uint32_t x, uint32_t y)
{
	if (x >= RECT_X && y >= RECT_Y)
		return QUARTER_RECT;
	if (y < BLEND_BOTTOM)
		return x < QUAD_RIGHT ? QUARTER_BOTH : QUARTER_CLEAR_SECOND;
	return x < QUAD_RIGHT ? QUARTER_FIRST : QUARTER_CLEAR;
}

static bool in_middle(uint32_t x, uint32_t y)
{
	return x >= MIDDLE_X && x < MIDDLE_X + MIDDLE_WIDTH && y >= MIDDLE_Y && y < MIDDLE_Y + MIDDLE_HEIGHT;
}

// What the first pass left in attachment *context.
static const float *first_pass_texel(uint32_t x, uint32_t y, const void *context)
{
	return attachments[*(const uint32_t *)context].quarters[quarter_of(x, y)];
}

// The half-float image after the second pass: the first's, under the second's quad.
static const float *half4_texel(uint32_t x, uint32_t y, const void *context)
{
	(void)context;
	return in_middle(x, y) ? again_colors[1] : attachments[IMAGE_HALF4].quarters[quarter_of(x, y)];
}

static const float *half2_texel(uint32_t x, uint32_t y, const void *context)
{
	(void)context;
	return in_middle(x, y) ? again_colors[0] : half2_clear;
}

// The image as the last frame left it, copied into cached memory.
static bool read_back(const Harness *harness, const Scene *scene, uint32_t image, VkBuffer buffer)
{
	const VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	const VkBufferImageCopy region = {
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { IMAGE_WIDTH, IMAGE_HEIGHT, 1 },
	};
	const VkCommandBuffer commands = scene->commands[0];
	const VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &commands
	};
	bool ok = harness_ok("vkBeginCommandBuffer", vkBeginCommandBuffer(commands, &begin_info));
	if (ok)
		vkCmdCopyImageToBuffer(
			commands, scene->images[image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
	ok = ok && harness_ok("vkEndCommandBuffer", vkEndCommandBuffer(commands)) &&
		harness_ok("vkQueueSubmit", vkQueueSubmit(harness->queue, 1, &submit, harness->fence)) &&
		harness_ok("vkWaitForFences", vkWaitForFences(harness->device, 1, &harness->fence, VK_TRUE, FENCE_TIMEOUT_NS));
	(void)vkResetFences(harness->device, 1, &harness->fence);
	return ok;
}

// Each image read back after the frames, each texel checked by value.
static void check_images(const Harness *harness, const Scene *scene)
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
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	void *mapped = NULL;
	const bool made = harness_ok("vkCreateBuffer", vkCreateBuffer(harness->device, &info, NULL, &buffer)) &&
		harness_ok("vkAllocateMemory", vkAllocateMemory(harness->device, &allocate, NULL, &memory)) &&
		harness_ok("vkBindBufferMemory", vkBindBufferMemory(harness->device, buffer, memory, 0)) &&
		harness_ok("vkMapMemory", vkMapMemory(harness->device, memory, 0, VK_WHOLE_SIZE, 0, &mapped));
	for (uint32_t i = 0; i < IMAGES; i++) {
		char name[LABEL_BYTES];
		const bool read = made && read_back(harness, scene, i, buffer);
		uint32_t wrong = 0;
		if (i < COLORS) {
			snprintf(
				name, sizeof name, "colour attachment %u, %s, %s", (unsigned)i, formats[i].name, attachments[i].what);
			// The second pass drew the half-float image back and over it, so it's checked as it left it.
			if (read)
				wrong = i == IMAGE_HALF4 ? count_wrong(&formats[i], mapped, name, half4_texel, NULL)
										 : count_wrong(&formats[i], mapped, name, first_pass_texel, &i);
		} else {
			snprintf(name, sizeof name, "the second pass's R16G16_SFLOAT target, its clear and its quad past 1");
			if (read)
				wrong = count_wrong(&formats[i], mapped, name, half2_texel, NULL);
		}
		char label[2 * LABEL_BYTES];
		if (i == IMAGE_HALF4)
			snprintf(label, sizeof label,
				"targets: %s; then drawn back by a second pass as its second target, past 1 "
				"and below 0, under its quad",
				name);
		else
			snprintf(label, sizeof label, "targets: %s", name);
		check(label, read && wrong == 0);
	}
	vkDestroyBuffer(harness->device, buffer, NULL);
	vkFreeMemory(harness->device, memory, NULL);
}

static bool features_hold(const Harness *harness, VkFormat format, VkFormatFeatureFlags wanted)
{
	const VkFormatFeatureFlags checked = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
		VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
	VkFormatProperties properties;
	vkGetPhysicalDeviceFormatProperties(harness->physical_device, format, &properties);
	const bool held = (properties.optimalTilingFeatures & checked) == wanted;
	if (!held)
		xsdk_log_write(
			"targets: format %u has features 0x%X", (unsigned)format, (unsigned)properties.optimalTilingFeatures);
	return held;
}

static void check_limits(const Harness *harness)
{
	VkPhysicalDeviceProperties properties;
	VkPhysicalDeviceFeatures features;
	vkGetPhysicalDeviceProperties(harness->physical_device, &properties);
	vkGetPhysicalDeviceFeatures(harness->physical_device, &features);
	const VkFormatFeatureFlags sampled = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
	const VkFormatFeatureFlags target = sampled | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
	const VkFormatFeatureFlags blended = target | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
	const bool formats_hold = features_hold(harness, VK_FORMAT_A2B10G10R10_UNORM_PACK32, blended) &&
		features_hold(harness, VK_FORMAT_R16G16B16A16_SFLOAT, target) &&
		features_hold(harness, VK_FORMAT_R16G16_SFLOAT, target) &&
		features_hold(harness, VK_FORMAT_R32_SFLOAT, target) &&
		features_hold(harness, VK_FORMAT_R16G16B16A16_UNORM, sampled);
	check(
		"targets: four colour attachments a subpass and four outputs, independent blending, and the formats' features: "
		"A2B10G10R10 blending, the floats not, R16G16B16A16_UNORM sampled alone",
		properties.limits.maxColorAttachments == COLORS && properties.limits.maxFragmentOutputAttachments == COLORS &&
			features.independentBlend && formats_hold);
}

// A pipeline blending into the half-float attachment, which reports no blending, and five colour attachments.
static bool others_refused(const Harness *harness, const Scene *scene)
{
	VkPipelineColorBlendAttachmentState blends[COLORS];
	for (uint32_t i = 0; i < COLORS; i++)
		blends[i] = attachments[0].blend;
	const HarnessPipeline spec = {
		.vertex_spirv = push_vert_spirv,
		.vertex_bytes = PUSH_VERT_SPIRV_BYTES,
		.fragment_spirv = four_targets_frag_spirv,
		.fragment_bytes = FOUR_TARGETS_FRAG_SPIRV_BYTES,
		.bindings = &harness_quad_binding,
		.binding_count = 1,
		.attributes = &harness_quad_attribute,
		.attribute_count = 1,
		.layout = scene->push_layout,
		.blend = blends,
		.blend_count = COLORS,
		.dynamic_viewport = true,
	};
	VkPipeline pipeline = VK_NULL_HANDLE;
	const bool blend_refused =
		harness_pipeline(harness, &spec, scene->four_pass, &pipeline) == VK_ERROR_FEATURE_NOT_PRESENT;
	VkAttachmentDescription five[COLORS + 1];
	for (uint32_t i = 0; i <= COLORS; i++)
		five[i] = attachment_of(VK_FORMAT_R8G8B8A8_UNORM, VK_ATTACHMENT_LOAD_OP_CLEAR);
	VkRenderPass render_pass = VK_NULL_HANDLE;
	const bool five_refused = make_pass(harness, five, COLORS + 1, &render_pass) == VK_ERROR_FEATURE_NOT_PRESENT;
	vkDestroyPipeline(harness->device, pipeline, NULL);
	vkDestroyRenderPass(harness->device, render_pass, NULL);
	return blend_refused && five_refused;
}

static void destroy(const Harness *harness, Scene *scene)
{
	const VkDevice device = harness->device;
	vkFreeCommandBuffers(device, harness->pool, harness->image_count, scene->commands);
	vkDestroyBuffer(device, scene->image_vertices, NULL);
	vkFreeMemory(device, scene->image_vertex_memory, NULL);
	vkDestroyBuffer(device, scene->unit_quad, NULL);
	vkFreeMemory(device, scene->unit_quad_memory, NULL);
	for (uint32_t i = 0; i < PIPELINES; i++)
		vkDestroyPipeline(device, scene->pipelines[i], NULL);
	vkDestroyDescriptorPool(device, scene->pool, NULL);
	vkDestroySampler(device, scene->sampler, NULL);
	vkDestroyPipelineLayout(device, scene->sample_layout, NULL);
	vkDestroyDescriptorSetLayout(device, scene->set_layout, NULL);
	vkDestroyPipelineLayout(device, scene->push_layout, NULL);
	harness_destroy_render_pass(harness, scene->screen_pass, scene->screens);
	vkDestroyFramebuffer(device, scene->again, NULL);
	vkDestroyFramebuffer(device, scene->four, NULL);
	vkDestroyRenderPass(device, scene->again_pass, NULL);
	vkDestroyRenderPass(device, scene->four_pass, NULL);
	for (uint32_t i = 0; i < IMAGES; i++) {
		vkDestroyImageView(device, scene->views[i], NULL);
		vkDestroyImage(device, scene->images[i], NULL);
		vkFreeMemory(device, scene->image_memory[i], NULL);
	}
}

void test_targets(const Harness *harness)
{
	Scene scene = { .four_pass = VK_NULL_HANDLE };
	make_probes(&scene);
	const bool built = build(harness, &scene);
	check(
		"targets: images of the five formats, render passes of four colour attachments and of two, their "
		"framebuffers and pipelines made",
		built);
	const bool recorded = built && harness->swapchain && harness_ok("recording", record(harness, &scene));
	uint32_t misses = 0;
	const uint32_t frames =
		recorded ? harness_frames(harness, scene.commands, FRAMES, harness_probes_hold, &scene.check, &misses) : 0;
	check("targets: 10 frames drawn and presented", frames == FRAMES);
	check("targets: every frame samples the five images back onto the screen, values past 1 scaled down",
		frames == FRAMES && misses == 0);
	if (recorded)
		check_images(harness, &scene);
	check_limits(harness);
	check("targets: blending into a float attachment and five colour attachments are refused",
		built && others_refused(harness, &scene));
	destroy(harness, &scene);
}
