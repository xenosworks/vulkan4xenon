#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "xsdk/log.h"

// Milestone 17's second part: multisampled images (xsdk:docs/scope/44-tiling-and-msaa.md, "Multisampled images"). Each
// image is read back and checked by value:
// - a triangle with a shallow, a steep and a diagonal edge at 2 and 4 samples in both formats that take them, drawn and
//   averaged into a resolve attachment: off the edges as the CPU works it out, on them at a step of the samples' blend,
//   every step seen, as xsdk_test counts them (xsdk:tests/test_multisample.c);
// - vkCmdResolveImage of each attachment's memory, which its store averaged: every texel as the resolve attachment's;
// - a second pass loading each attachment, which draws that average back into every sample, and a square over it: every
//   texel as the first pass's resolve but for the square's;
// - alpha to coverage at 1, 2 and 4 samples, squares at alphas 0 to 1 in 16ths, every pixel the share of its samples
//   xsdk's probe found covered (xsdk:docs/HARDWARE.md, "Alpha to coverage, milestone 44 step 4");
// - the formats' sample counts, the limits, and what's refused.
// Every image shows on screen in its own place and colours.

enum {
	FRAMES = 3,
	FENCE_TIMEOUT_NS = 1000000000,
	// xsdk_test's triangle and target (xsdk:tests/test_multisample.c).
	TRI_WIDTH = 240,
	TRI_HEIGHT = 128,
	TRI_BYTES = TRI_WIDTH * TRI_HEIGHT * 4,
	CASES = 4,
	TRIANGLE_VERTICES = 3,
	QUAD_VERTICES = 6,
	MAX_SAMPLES = 4,
	CHANNELS = 4,
	// A pixel's samples sit within 3/8 of a pixel of its centre (xsdk:include/xsdk/gpu.h), well inside this.
	EDGE_DISTANCE = 1,
	// Alpha to coverage: an image a count, squares of 48 pixels at alphas 0 to 16/16 a cell of 64 apart, on even
	// pixels, so each is whole quads, as xsdk_test draws them.
	BAND_WIDTH = 1280,
	BAND_HEIGHT = 64,
	ALPHA_STEPS = 16,
	SQUARES = ALPHA_STEPS + 1,
	SQUARE_LEFT = 72,
	SQUARE_TOP = 8,
	SQUARE_CELL = 64,
	SQUARE_SIZE = 48,
	ALPHA_CASES = 3,
	// Each case's resolve attachment, its vkCmdResolveImage copy and its second pass, then the bands.
	ROWS = 3,
	SHOWN = ROWS * CASES + ALPHA_CASES,
	VERTEX_FLOATS = 6,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	UV_OFFSET_BYTES = 16,
	// The memory type the CPU caches, which buffers only copied to and from may take.
	CACHED_TYPE = 1,
	// A case's three images one after another, more than a band's.
	READBACK_BYTES = ROWS * TRI_BYTES,
	MISSES_LOGGED = 4,
	LABEL_BYTES = 160,
	// A probe's channels, through the screen's 8 bits.
	PROBE_TOLERANCE = 2,
	PROBES = 18,
	// What's tried and must be refused.
	REFUSALS = 10,
	ALL_SAMPLES = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT,
};

// How a texel reads back.
enum {
	KIND_BYTES,
	KIND_10_10_10_2,
};

// Where a pixel's centre lies against the triangle.
enum {
	PIXEL_OUTSIDE,
	PIXEL_INSIDE,
	// Within EDGE_DISTANCE of an edge, where its samples may differ.
	PIXEL_EDGE,
};

typedef struct {
	const char *name;
	VkFormat format;
	VkSampleCountFlagBits samples;
	uint32_t kind;
	// A channel's step and a half, as a blend rounds either way.
	float tolerance;
	// The triangle's colour; each channel the clear's plus a multiple of a quarter, so every blend is a value.
	float shape[CHANNELS];
} Case;

static const Case cases[CASES] = {
	{ "R8G8B8A8_UNORM at 2 samples", VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_2_BIT, KIND_BYTES, 1.5f / 255,
		{ 0.875f, 0.625f, 0.375f, 1.0f } },
	{ "R8G8B8A8_UNORM at 4 samples", VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_4_BIT, KIND_BYTES, 1.5f / 255,
		{ 0.375f, 0.875f, 0.625f, 1.0f } },
	{ "A2B10G10R10_UNORM_PACK32 at 2 samples", VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_SAMPLE_COUNT_2_BIT,
		KIND_10_10_10_2, 1.5f / 1023, { 0.625f, 0.375f, 0.875f, 1.0f } },
	{ "A2B10G10R10_UNORM_PACK32 at 4 samples", VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_SAMPLE_COUNT_4_BIT,
		KIND_10_10_10_2, 1.5f / 1023, { 0.875f, 0.375f, 0.625f, 1.0f } },
};

// A count's band: its colour, and the first alpha in 16ths at which each pixel covers 1, 2 and so on of its samples, as
// xsdk's probe found them (xsdk:include/xsdk/gpu.h, XsdkBlend's alpha_to_coverage).
typedef struct {
	VkSampleCountFlagBits samples;
	float shape[CHANNELS];
	uint8_t thresholds[MAX_SAMPLES];
} AlphaCase;

static const AlphaCase alpha_cases[ALPHA_CASES] = {
	{ VK_SAMPLE_COUNT_1_BIT, { 0.875f, 0.625f, 0.375f, 1.0f }, { 8 } },
	{ VK_SAMPLE_COUNT_2_BIT, { 0.375f, 0.875f, 0.625f, 1.0f }, { 4, 12 } },
	{ VK_SAMPLE_COUNT_4_BIT, { 0.625f, 0.375f, 0.875f, 1.0f }, { 2, 6, 10, 14 } },
};

static const float clear_color[CHANNELS] = { 0.125f, 0.125f, 0.125f, 1.0f };
// The second pass's square, on whole pixels clear of the triangle.
static const float square_color[CHANNELS] = { 0.25f, 0.75f, 0.5f, 1.0f };
static const uint32_t square_rect[4] = { 176, 80, 232, 120 };

typedef struct {
	float x;
	float y;
} Point;

// In pixels: a shallow edge, a steep one and a diagonal (xsdk:tests/test_multisample.c).
static const Point corners[TRIANGLE_VERTICES] = { { 16.3f, 12.7f }, { 224.1f, 40.4f }, { 40.6f, 116.2f } };

// Where each image shows, left and top: the cases' three rows of 240x128, then the bands one above another.
static const uint32_t shown_left[CASES] = { 40, 344, 648, 952 };
static const uint32_t shown_tops[ROWS] = { 24, 168, 312 };
static const uint32_t band_tops[ALPHA_CASES] = { 464, 552, 640 };

#define RGB(r, g, b) { r, g, b }

// Inside each case's triangle in its three rows and its square, the clear between them, and each band's opaque square
// and its background, through the screen's 8 bits.
static const HarnessProbe probes[PROBES] = {
	{ "the 2-sample 8-bit triangle", 40 + 60, 24 + 30, RGB(223, 159, 96), 0 },
	{ "its copy", 40 + 60, 168 + 30, RGB(223, 159, 96), 0 },
	{ "its second pass's square", 40 + 200, 312 + 100, RGB(64, 191, 128), 0 },
	{ "the 4-sample 8-bit triangle", 344 + 60, 24 + 30, RGB(96, 223, 159), 0 },
	{ "its copy", 344 + 60, 168 + 30, RGB(96, 223, 159), 0 },
	{ "its second pass's square", 344 + 200, 312 + 100, RGB(64, 191, 128), 0 },
	{ "the 2-sample 10-bit triangle", 648 + 60, 24 + 30, RGB(159, 96, 223), 0 },
	{ "its copy", 648 + 60, 168 + 30, RGB(159, 96, 223), 0 },
	{ "its second pass's square", 648 + 200, 312 + 100, RGB(64, 191, 128), 0 },
	{ "the 4-sample 10-bit triangle", 952 + 60, 24 + 30, RGB(223, 96, 159), 0 },
	{ "its copy", 952 + 60, 168 + 30, RGB(223, 96, 159), 0 },
	{ "its second pass's square", 952 + 200, 312 + 100, RGB(64, 191, 128), 0 },
	{ "a resolve's clear", 40 + 5, 24 + 5, RGB(32, 32, 32), 0 },
	{ "the 1-sample band's opaque square", 72 + 16 * 64 + 24, 464 + 32, RGB(223, 159, 96), 0 },
	{ "the 2-sample band's opaque square", 72 + 16 * 64 + 24, 552 + 32, RGB(96, 223, 159), 0 },
	{ "the 4-sample band's opaque square", 72 + 16 * 64 + 24, 640 + 32, RGB(159, 96, 223), 0 },
	{ "the 4-sample band's clear", 20, 640 + 32, RGB(32, 32, 32), 0 },
	{ "the screen's clear", 640, 450, RGB(16, 16, 48), 0 },
};

static const VkClearValue screen_clear = { .color = { .float32 = { 16 / 255.0f, 16 / 255.0f, 48 / 255.0f, 1.0f } } };

typedef struct {
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
} Picture;

typedef struct {
	// Each case's multisampled attachment, its resolve attachment, the copy vkCmdResolveImage makes and the second
	// pass's resolve attachment.
	Picture drawn[CASES];
	Picture resolved[CASES];
	Picture copied[CASES];
	Picture loaded[CASES];
	// Each band's image: at 1 sample the one drawn, else the multisampled one and its resolve attachment.
	Picture band_drawn[ALPHA_CASES];
	Picture band_resolved[ALPHA_CASES];
	// Cleared, drawn, stored and resolved; loaded, drawn over and resolved; and each band's.
	VkRenderPass first_passes[CASES];
	VkRenderPass second_passes[CASES];
	VkRenderPass band_passes[ALPHA_CASES];
	VkFramebuffer first_targets[CASES];
	VkFramebuffer second_targets[CASES];
	VkFramebuffer band_targets[ALPHA_CASES];
	VkRenderPass screen_pass;
	VkFramebuffer screens[HARNESS_MAX_IMAGES];
	VkPipelineLayout push_layout;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout sample_layout;
	VkSampler sampler;
	VkDescriptorPool pool;
	VkDescriptorSet sets[SHOWN];
	VkPipeline case_pipelines[CASES];
	VkPipeline band_pipelines[ALPHA_CASES];
	VkPipeline screen_pipeline;
	VkBuffer triangle;
	VkDeviceMemory triangle_memory;
	VkBuffer unit_quad;
	VkDeviceMemory unit_quad_memory;
	VkBuffer quads;
	VkDeviceMemory quads_memory;
	VkBuffer readback;
	VkDeviceMemory readback_memory;
	uint8_t *read;
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	VkCommandBuffer work;
} Scene;

static VkAttachmentDescription attachment_of(
	VkFormat format, VkSampleCountFlagBits samples, VkAttachmentLoadOp load, VkAttachmentStoreOp store)
{
	return (VkAttachmentDescription){ .format = format,
		.samples = samples,
		.loadOp = load,
		.storeOp = store,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
}

// A subpass of one colour attachment at samples, loaded and stored as asked, and a resolve attachment after it when
// it's multisampled.
static VkResult make_pass(const Harness *harness, VkFormat format, VkSampleCountFlagBits samples,
	VkAttachmentLoadOp load, VkAttachmentStoreOp store, VkRenderPass *render_pass)
{
	const bool resolves = samples != VK_SAMPLE_COUNT_1_BIT;
	const VkAttachmentDescription described[2] = {
		attachment_of(format, samples, load, store),
		attachment_of(format, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE),
	};
	const VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	const VkAttachmentReference resolve = { 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	const VkSubpassDescription subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &color,
		.pResolveAttachments = resolves ? &resolve : NULL,
	};
	const VkRenderPassCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = resolves ? 2 : 1,
		.pAttachments = described,
		.subpassCount = 1,
		.pSubpasses = &subpass,
	};
	return vkCreateRenderPass(harness->device, &info, NULL, render_pass);
}

static VkResult make_framebuffer(const Harness *harness, VkRenderPass render_pass, const Picture *drawn,
	const Picture *resolved, uint32_t width, uint32_t height, VkFramebuffer *framebuffer)
{
	const VkImageView views[2] = { drawn->view, resolved ? resolved->view : VK_NULL_HANDLE };
	const VkFramebufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = render_pass,
		.attachmentCount = resolved ? 2 : 1,
		.pAttachments = views,
		.width = width,
		.height = height,
		.layers = 1,
	};
	return vkCreateFramebuffer(harness->device, &info, NULL, framebuffer);
}

static VkResult make_picture(const Harness *harness, VkFormat format, uint32_t width, uint32_t height,
	VkSampleCountFlagBits samples, VkImageUsageFlags usage, Picture *picture)
{
	return harness_multisampled_image(
		harness, format, width, height, samples, usage, &picture->image, &picture->memory, &picture->view);
}

// Multisampled images are drawn, stored and resolved from; the rest are sampled onto the screen and read back.
static VkResult make_images(const Harness *harness, Scene *s)
{
	const VkImageUsageFlags drawn = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	const VkImageUsageFlags shown = drawn | VK_IMAGE_USAGE_SAMPLED_BIT;
	const VkImageUsageFlags copied =
		VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	VkResult result = VK_SUCCESS;
	for (uint32_t i = 0; i < CASES && result == VK_SUCCESS; i++) {
		const Case *const c = &cases[i];
		result = make_picture(harness, c->format, TRI_WIDTH, TRI_HEIGHT, c->samples, drawn, &s->drawn[i]);
		if (result == VK_SUCCESS)
			result = make_picture(harness, c->format, TRI_WIDTH, TRI_HEIGHT, 1, shown, &s->resolved[i]);
		if (result == VK_SUCCESS)
			result = make_picture(harness, c->format, TRI_WIDTH, TRI_HEIGHT, 1, copied, &s->copied[i]);
		if (result == VK_SUCCESS)
			result = make_picture(harness, c->format, TRI_WIDTH, TRI_HEIGHT, 1, shown, &s->loaded[i]);
	}
	for (uint32_t i = 0; i < ALPHA_CASES && result == VK_SUCCESS; i++) {
		const VkSampleCountFlagBits samples = alpha_cases[i].samples;
		const VkImageUsageFlags usage = samples == VK_SAMPLE_COUNT_1_BIT ? shown : drawn;
		result =
			make_picture(harness, VK_FORMAT_R8G8B8A8_UNORM, BAND_WIDTH, BAND_HEIGHT, samples, usage, &s->band_drawn[i]);
		if (result == VK_SUCCESS && samples != VK_SAMPLE_COUNT_1_BIT)
			result = make_picture(
				harness, VK_FORMAT_R8G8B8A8_UNORM, BAND_WIDTH, BAND_HEIGHT, 1, shown, &s->band_resolved[i]);
	}
	return result;
}

// The bands' multisampled images aren't stored, their resolve attachments taking their average alone.
static VkResult make_passes(const Harness *harness, Scene *s)
{
	VkResult result = VK_SUCCESS;
	for (uint32_t i = 0; i < CASES && result == VK_SUCCESS; i++) {
		result = make_pass(harness, cases[i].format, cases[i].samples, VK_ATTACHMENT_LOAD_OP_CLEAR,
			VK_ATTACHMENT_STORE_OP_STORE, &s->first_passes[i]);
		if (result == VK_SUCCESS)
			result = make_pass(harness, cases[i].format, cases[i].samples, VK_ATTACHMENT_LOAD_OP_LOAD,
				VK_ATTACHMENT_STORE_OP_STORE, &s->second_passes[i]);
		if (result == VK_SUCCESS)
			result = make_framebuffer(harness, s->first_passes[i], &s->drawn[i], &s->resolved[i], TRI_WIDTH, TRI_HEIGHT,
				&s->first_targets[i]);
		if (result == VK_SUCCESS)
			result = make_framebuffer(harness, s->second_passes[i], &s->drawn[i], &s->loaded[i], TRI_WIDTH, TRI_HEIGHT,
				&s->second_targets[i]);
	}
	for (uint32_t i = 0; i < ALPHA_CASES && result == VK_SUCCESS; i++) {
		const bool one = alpha_cases[i].samples == VK_SAMPLE_COUNT_1_BIT;
		result = make_pass(harness, VK_FORMAT_R8G8B8A8_UNORM, alpha_cases[i].samples, VK_ATTACHMENT_LOAD_OP_CLEAR,
			one ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE, &s->band_passes[i]);
		if (result == VK_SUCCESS)
			result = make_framebuffer(harness, s->band_passes[i], &s->band_drawn[i], one ? NULL : &s->band_resolved[i],
				BAND_WIDTH, BAND_HEIGHT, &s->band_targets[i]);
	}
	if (result == VK_SUCCESS)
		result = harness_render_pass(harness, &s->screen_pass, s->screens);
	return result;
}

// The image each shown quad samples: the cases' rows, then the bands.
static const Picture *shown_picture(const Scene *s, uint32_t index)
{
	if (index >= ROWS * CASES) {
		const uint32_t band = index - ROWS * CASES;
		return alpha_cases[band].samples == VK_SAMPLE_COUNT_1_BIT ? &s->band_drawn[band] : &s->band_resolved[band];
	}
	const uint32_t i = index % CASES;
	const uint32_t row = index / CASES;
	return row == 0 ? &s->resolved[i] : row == 1 ? &s->copied[i] : &s->loaded[i];
}

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
	const VkDescriptorSetLayoutBinding binding = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
		VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
	const VkDescriptorSetLayoutCreateInfo set_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &binding,
	};
	if (result == VK_SUCCESS)
		result = vkCreateDescriptorSetLayout(harness->device, &set_info, NULL, &s->set_layout);
	const VkPipelineLayoutCreateInfo layout_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &s->set_layout,
	};
	if (result == VK_SUCCESS)
		result = vkCreatePipelineLayout(harness->device, &layout_info, NULL, &s->sample_layout);
	const VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SHOWN };
	const VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = SHOWN,
		.poolSizeCount = 1,
		.pPoolSizes = &size,
	};
	if (result == VK_SUCCESS)
		result = vkCreateDescriptorPool(harness->device, &pool_info, NULL, &s->pool);
	for (uint32_t i = 0; i < SHOWN && result == VK_SUCCESS; i++) {
		const VkDescriptorSetAllocateInfo allocate_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = s->pool,
			.descriptorSetCount = 1,
			.pSetLayouts = &s->set_layout,
		};
		result = vkAllocateDescriptorSets(harness->device, &allocate_info, &s->sets[i]);
		const VkDescriptorImageInfo image = { s->sampler, shown_picture(s, i)->view,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		const VkWriteDescriptorSet write = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = s->sets[i],
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &image,
		};
		if (result == VK_SUCCESS)
			vkUpdateDescriptorSets(harness->device, 1, &write, 0, NULL);
	}
	return result;
}

static const VkVertexInputBindingDescription shown_binding = { 0, STRIDE_BYTES, VK_VERTEX_INPUT_RATE_VERTEX };
static const VkVertexInputAttributeDescription shown_attributes[] = {
	{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0 },
	{ .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = UV_OFFSET_BYTES },
};

// push.vert and push.frag at the pass's samples, alpha to coverage on for the bands.
static VkResult make_push_pipeline(const Harness *harness, const Scene *s, VkRenderPass render_pass,
	VkSampleCountFlagBits samples, bool alpha_to_coverage, VkPipeline *pipeline)
{
	const VkPipelineMultisampleStateCreateInfo multisample = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = samples,
		.alphaToCoverageEnable = alpha_to_coverage,
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
		.layout = s->push_layout,
		.dynamic_viewport = true,
		.multisample = &multisample,
	};
	return harness_pipeline(harness, &spec, render_pass, pipeline);
}

static VkResult make_pipelines(const Harness *harness, Scene *s)
{
	VkResult result = harness_push_layout(harness, &s->push_layout);
	for (uint32_t i = 0; i < CASES && result == VK_SUCCESS; i++)
		result = make_push_pipeline(harness, s, s->first_passes[i], cases[i].samples, false, &s->case_pipelines[i]);
	for (uint32_t i = 0; i < ALPHA_CASES && result == VK_SUCCESS; i++)
		result = make_push_pipeline(harness, s, s->band_passes[i], alpha_cases[i].samples, true, &s->band_pipelines[i]);
	const HarnessPipeline screen = {
		.vertex_spirv = texture_vert_spirv,
		.vertex_bytes = TEXTURE_VERT_SPIRV_BYTES,
		.fragment_spirv = texture_frag_spirv,
		.fragment_bytes = TEXTURE_FRAG_SPIRV_BYTES,
		.bindings = &shown_binding,
		.binding_count = 1,
		.attributes = shown_attributes,
		.attribute_count = 2,
		.layout = s->sample_layout,
	};
	if (result == VK_SUCCESS)
		result = harness_pipeline(harness, &screen, s->screen_pass, &s->screen_pipeline);
	return result;
}

// A rectangle of the screen in clip space, Vulkan's y down, and texture coordinates over the whole image.
static void put_quad(float *vertices, uint32_t left, uint32_t top, uint32_t width, uint32_t height)
{
	const float x0 = (float)left / (HARNESS_WIDTH / 2.0f) - 1.0f;
	const float y0 = (float)top / (HARNESS_HEIGHT / 2.0f) - 1.0f;
	const float x1 = (float)(left + width) / (HARNESS_WIDTH / 2.0f) - 1.0f;
	const float y1 = (float)(top + height) / (HARNESS_HEIGHT / 2.0f) - 1.0f;
	const float corners_uv[QUAD_VERTICES][4] = { { x0, y0, 0.0f, 0.0f }, { x1, y0, 1.0f, 0.0f }, { x0, y1, 0.0f, 1.0f },
		{ x1, y0, 1.0f, 0.0f }, { x1, y1, 1.0f, 1.0f }, { x0, y1, 0.0f, 1.0f } };
	for (uint32_t v = 0; v < QUAD_VERTICES; v++) {
		float *const vertex = vertices + v * VERTEX_FLOATS;
		const float values[VERTEX_FLOATS] = { corners_uv[v][0], corners_uv[v][1], 0.0f, 1.0f, corners_uv[v][2],
			corners_uv[v][3] };
		memcpy(vertex, values, sizeof values);
	}
}

// The triangle in clip space at the cases' size, and every shown image's quad.
static VkResult make_buffers(const Harness *harness, Scene *s)
{
	float triangle[TRIANGLE_VERTICES][4];
	for (uint32_t i = 0; i < TRIANGLE_VERTICES; i++) {
		const float place[4] = { corners[i].x / (TRI_WIDTH / 2.0f) - 1.0f, corners[i].y / (TRI_HEIGHT / 2.0f) - 1.0f,
			0.0f, 1.0f };
		memcpy(triangle[i], place, sizeof place);
	}
	static float quads[SHOWN * QUAD_VERTICES * VERTEX_FLOATS];
	for (uint32_t i = 0; i < ROWS * CASES; i++)
		put_quad(&quads[i * QUAD_VERTICES * VERTEX_FLOATS], shown_left[i % CASES], shown_tops[i / CASES], TRI_WIDTH,
			TRI_HEIGHT);
	for (uint32_t i = 0; i < ALPHA_CASES; i++)
		put_quad(&quads[(ROWS * CASES + i) * QUAD_VERTICES * VERTEX_FLOATS], 0, band_tops[i], BAND_WIDTH, BAND_HEIGHT);
	VkResult result = harness_buffer(
		harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, triangle, sizeof triangle, &s->triangle, &s->triangle_memory);
	if (result == VK_SUCCESS)
		result = harness_quad_buffer(harness, &s->unit_quad, &s->unit_quad_memory);
	if (result == VK_SUCCESS)
		result = harness_buffer(
			harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, quads, sizeof quads, &s->quads, &s->quads_memory);
	return result;
}

// The cached buffer images are copied into to be read, and a command buffer for the copies.
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
		harness_ok("descriptor sets", make_sets(harness, s)) && harness_ok("pipelines", make_pipelines(harness, s)) &&
		harness_ok("buffers", make_buffers(harness, s)) && harness_ok("readback", make_readback(harness, s));
}

static void begin_pass(
	VkCommandBuffer commands, VkRenderPass render_pass, VkFramebuffer framebuffer, uint32_t width, uint32_t height)
{
	const VkClearValue clear = { .color = {
									 .float32 = { clear_color[0], clear_color[1], clear_color[2], clear_color[3] } } };
	const VkRect2D area = { { 0, 0 }, { width, height } };
	const VkRenderPassBeginInfo info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = render_pass,
		.framebuffer = framebuffer,
		.renderArea = area,
		.clearValueCount = 1,
		.pClearValues = &clear,
	};
	vkCmdBeginRenderPass(commands, &info, VK_SUBPASS_CONTENTS_INLINE);
	const VkViewport viewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
	vkCmdSetViewport(commands, 0, 1, &viewport);
	vkCmdSetScissor(commands, 0, 1, &area);
}

// The triangle, its corners already in clip space: push.vert's scale 1 and offset 0.
static void draw_triangle(VkCommandBuffer commands, const Scene *s, const Case *c)
{
	const float place[8] = { 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f };
	const VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(commands, 0, 1, &s->triangle, &offset);
	vkCmdPushConstants(commands, s->push_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof place, place);
	vkCmdPushConstants(commands, s->push_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 32, sizeof c->shape, c->shape);
	vkCmdDraw(commands, TRIANGLE_VERTICES, 1, 0, 0);
}

static void draw_square(VkCommandBuffer commands, const Scene *s)
{
	const VkDeviceSize offset = 0;
	const HarnessQuad quad = { (float)square_rect[0], (float)square_rect[1], (float)(square_rect[2] - square_rect[0]),
		(float)(square_rect[3] - square_rect[1]), 0.0f,
		{ square_color[0], square_color[1], square_color[2], square_color[3] }, false };
	vkCmdBindVertexBuffers(commands, 0, 1, &s->unit_quad, &offset);
	harness_push_quad_in(commands, s->push_layout, &quad, TRI_WIDTH, TRI_HEIGHT);
}

// A band's squares, each at its alpha.
static void draw_band(VkCommandBuffer commands, const Scene *s, const AlphaCase *a)
{
	const VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(commands, 0, 1, &s->unit_quad, &offset);
	for (uint32_t k = 0; k < SQUARES; k++) {
		const HarnessQuad quad = { (float)(SQUARE_LEFT + k * SQUARE_CELL), (float)SQUARE_TOP, (float)SQUARE_SIZE,
			(float)SQUARE_SIZE, 0.0f, { a->shape[0], a->shape[1], a->shape[2], (float)k / ALPHA_STEPS }, false };
		harness_push_quad_in(commands, s->push_layout, &quad, BAND_WIDTH, BAND_HEIGHT);
	}
}

// Every case's first pass, then its attachment resolved by vkCmdResolveImage, then its second pass, so each second pass
// finds eDRAM holding another case's target and draws its image back; then the bands, and all of them on screen.
static void record_frame(VkCommandBuffer commands, const Scene *s, uint32_t index)
{
	for (uint32_t i = 0; i < CASES; i++) {
		begin_pass(commands, s->first_passes[i], s->first_targets[i], TRI_WIDTH, TRI_HEIGHT);
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, s->case_pipelines[i]);
		draw_triangle(commands, s, &cases[i]);
		vkCmdEndRenderPass(commands);
	}
	for (uint32_t i = 0; i < CASES; i++) {
		const VkImageResolve region = {
			.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.extent = { TRI_WIDTH, TRI_HEIGHT, 1 },
		};
		vkCmdResolveImage(commands, s->drawn[i].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->copied[i].image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	}
	for (uint32_t i = 0; i < CASES; i++) {
		begin_pass(commands, s->second_passes[i], s->second_targets[i], TRI_WIDTH, TRI_HEIGHT);
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, s->case_pipelines[i]);
		draw_square(commands, s);
		vkCmdEndRenderPass(commands);
	}
	for (uint32_t i = 0; i < ALPHA_CASES; i++) {
		begin_pass(commands, s->band_passes[i], s->band_targets[i], BAND_WIDTH, BAND_HEIGHT);
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, s->band_pipelines[i]);
		draw_band(commands, s, &alpha_cases[i]);
		vkCmdEndRenderPass(commands);
	}
	const VkRenderPassBeginInfo screen = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = s->screen_pass,
		.framebuffer = s->screens[index],
		.renderArea = { { 0, 0 }, { HARNESS_WIDTH, HARNESS_HEIGHT } },
		.clearValueCount = 1,
		.pClearValues = &screen_clear,
	};
	const VkDeviceSize offset = 0;
	vkCmdBeginRenderPass(commands, &screen, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, s->screen_pipeline);
	vkCmdBindVertexBuffers(commands, 0, 1, &s->quads, &offset);
	for (uint32_t i = 0; i < SHOWN; i++) {
		vkCmdBindDescriptorSets(
			commands, VK_PIPELINE_BIND_POINT_GRAPHICS, s->sample_layout, 0, 1, &s->sets[i], 0, NULL);
		vkCmdDraw(commands, QUAD_VERTICES, 1, i * QUAD_VERTICES, 0);
	}
	vkCmdEndRenderPass(commands);
}

static VkResult record(const Harness *harness, Scene *s)
{
	VkResult result = harness_command_buffers(harness, s->commands);
	for (uint32_t i = 0; i < harness->image_count && result == VK_SUCCESS; i++) {
		record_frame(s->commands[i], s, i);
		result = vkEndCommandBuffer(s->commands[i]);
	}
	return result;
}

// Copies count images, each width by height, one after another into the readback buffer, and waits.
static bool read_back(
	const Harness *harness, const Scene *s, const VkImage *images, uint32_t count, uint32_t width, uint32_t height)
{
	const VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	const VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &s->work
	};
	bool ok = harness_ok("vkBeginCommandBuffer", vkBeginCommandBuffer(s->work, &begin_info));
	for (uint32_t i = 0; i < count && ok; i++) {
		const VkBufferImageCopy region = {
			.bufferOffset = (VkDeviceSize)i * width * height * 4,
			.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.imageExtent = { width, height, 1 },
		};
		vkCmdCopyImageToBuffer(s->work, images[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->readback, 1, &region);
	}
	ok = ok && harness_ok("vkEndCommandBuffer", vkEndCommandBuffer(s->work)) &&
		harness_ok("vkQueueSubmit", vkQueueSubmit(harness->queue, 1, &submit, harness->fence)) &&
		harness_ok("vkWaitForFences", vkWaitForFences(harness->device, 1, &harness->fence, VK_TRUE, FENCE_TIMEOUT_NS));
	(void)vkResetFences(harness->device, 1, &harness->fence);
	return ok;
}

// Channel c of a texel as its format holds it: a byte, or 10 bits from the low end and alpha in the top 2.
static float channel(uint32_t kind, const uint8_t *texel, uint32_t c)
{
	if (kind == KIND_BYTES)
		return texel[c] / 255.0f;
	uint32_t word;
	memcpy(&word, texel, sizeof word);
	return c == 3 ? (float)(word >> 30) / 3.0f : (float)(word >> (10 * c) & 0x3FF) / 1023.0f;
}

// Which k of samples the texel's red, green and blue are, k samples the shape and the rest the clear; -1 for none.
static int blend_level(
	uint32_t kind, float tolerance, const float shape[CHANNELS], uint32_t samples, const uint8_t *texel)
{
	for (uint32_t k = 0; k <= samples; k++) {
		bool matches = true;
		for (uint32_t c = 0; c < 3; c++) {
			const float want = clear_color[c] + (float)k * (shape[c] - clear_color[c]) / (float)samples;
			matches = matches && fabsf(channel(kind, texel, c) - want) <= tolerance;
		}
		if (matches)
			return (int)k;
	}
	return -1;
}

static float edge_side(Point a, Point b, float x, float y)
{
	return (x - a.x) * (b.y - a.y) - (y - a.y) * (b.x - a.x);
}

// Where pixel (x, y)'s centre, at its half in Vulkan, lies against the triangle.
static uint32_t place_of(uint32_t x, uint32_t y)
{
	const float area = edge_side(corners[0], corners[1], corners[2].x, corners[2].y);
	bool inside = true;
	bool edge = false;
	for (uint32_t i = 0; i < TRIANGLE_VERTICES; i++) {
		const Point a = corners[i];
		const Point b = corners[(i + 1) % TRIANGLE_VERTICES];
		const float side = edge_side(a, b, (float)x + 0.5f, (float)y + 0.5f);
		const float length = sqrtf((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
		inside = inside && side * area > 0.0f;
		edge = edge || fabsf(side) <= EDGE_DISTANCE * length;
	}
	return edge ? PIXEL_EDGE : inside ? PIXEL_INSIDE : PIXEL_OUTSIDE;
}

static void log_texel(
	const char *name, const char *what, uint32_t x, uint32_t y, uint32_t kind, const uint8_t *texel, uint32_t *logged)
{
	if ((*logged)++ >= MISSES_LOGGED)
		return;
	xsdk_log_write("multisample: %s: %s (%u, %u) is %g %g %g", name, what, (unsigned)x, (unsigned)y,
		channel(kind, texel, 0), channel(kind, texel, 1), channel(kind, texel, 2));
}

// The resolve attachment read back at the readback's start: off the edges each pixel the clear or the shape, on them a
// step of the samples' blend, and every step between seen (xsdk:tests/test_multisample.c, tally_case).
static void check_edges(const Case *c, const uint8_t *texels)
{
	uint32_t levels[MAX_SAMPLES + 1] = { 0 };
	uint32_t unstepped = 0;
	uint32_t wrong = 0;
	uint32_t logged = 0;
	for (uint32_t y = 0; y < TRI_HEIGHT; y++) {
		for (uint32_t x = 0; x < TRI_WIDTH; x++) {
			const uint8_t *const texel = texels + (y * TRI_WIDTH + x) * 4;
			const uint32_t place = place_of(x, y);
			const int level = blend_level(c->kind, c->tolerance, c->shape, c->samples, texel);
			if (place != PIXEL_EDGE && level == (place == PIXEL_INSIDE ? (int)c->samples : 0))
				continue;
			if (place != PIXEL_EDGE) {
				wrong++;
				log_texel(c->name, "off the edges", x, y, c->kind, texel, &logged);
			} else if (level >= 0) {
				levels[level]++;
			} else {
				unstepped++;
				log_texel(c->name, "on an edge", x, y, c->kind, texel, &logged);
			}
		}
	}
	bool every_step = true;
	for (uint32_t k = 1; k < c->samples; k++)
		every_step = every_step && levels[k] > 0;
	xsdk_log_write(
		"multisample: %s: edge pixels by samples covered: %u, %u, %u, %u, %u; %u between steps, %u wrong off "
		"the edges",
		c->name, (unsigned)levels[0], (unsigned)levels[1], (unsigned)levels[2], (unsigned)levels[3],
		(unsigned)levels[4], (unsigned)unstepped, (unsigned)wrong);
	char label[LABEL_BYTES];
	snprintf(label, sizeof label,
		"multisample: %s: the resolve attachment's edge pixels blend in %s, the rest as worked out", c->name,
		c->samples == VK_SAMPLE_COUNT_2_BIT ? "halves" : "quarters");
	check(label, every_step && !unstepped && !wrong);
}

// The copy vkCmdResolveImage made, read back after the resolve attachment: the same bits.
static void check_copy(const Case *c, const uint8_t *texels)
{
	const uint8_t *const copied = texels + TRI_BYTES;
	uint32_t wrong = 0;
	for (uint32_t i = 0; i < TRI_BYTES / 4; i++)
		wrong += memcmp(texels + 4 * i, copied + 4 * i, 4) != 0;
	xsdk_log_write("multisample: %s: vkCmdResolveImage: %u of %u texels unlike the resolve attachment's", c->name,
		(unsigned)wrong, (unsigned)(TRI_BYTES / 4));
	char label[LABEL_BYTES];
	snprintf(label, sizeof label,
		"multisample: %s: vkCmdResolveImage copies the stored average, every texel as the resolve attachment's",
		c->name);
	check(label, wrong == 0);
}

// The second pass's resolve, read back after the first's: each texel as the first's, but the square's own colour.
static void check_loaded(const Case *c, const uint8_t *texels)
{
	const uint8_t *const loaded = texels + 2 * TRI_BYTES;
	uint32_t wrong = 0;
	uint32_t logged = 0;
	for (uint32_t y = 0; y < TRI_HEIGHT; y++) {
		for (uint32_t x = 0; x < TRI_WIDTH; x++) {
			const uint32_t at = (y * TRI_WIDTH + x) * 4;
			const bool square = x >= square_rect[0] && x < square_rect[2] && y >= square_rect[1] && y < square_rect[3];
			bool near = true;
			for (uint32_t ch = 0; ch < CHANNELS; ch++) {
				const float want = square ? square_color[ch] : channel(c->kind, texels + at, ch);
				near = near && fabsf(channel(c->kind, loaded + at, ch) - want) <= c->tolerance;
			}
			if (near)
				continue;
			wrong++;
			log_texel(c->name, "the second pass's", x, y, c->kind, loaded + at, &logged);
		}
	}
	xsdk_log_write(
		"multisample: %s: second pass: %u of %u texels wrong", c->name, (unsigned)wrong, (unsigned)(TRI_BYTES / 4));
	char label[LABEL_BYTES];
	snprintf(label, sizeof label,
		"multisample: %s: a pass loading the attachment draws its average back, every texel as before but its square",
		c->name);
	check(label, wrong == 0);
}

static void check_case(const Harness *harness, const Scene *s, uint32_t i)
{
	const VkImage images[ROWS] = { s->resolved[i].image, s->copied[i].image, s->loaded[i].image };
	const bool read = read_back(harness, s, images, ROWS, TRI_WIDTH, TRI_HEIGHT);
	if (!read)
		memset(s->read, 0, 3 * TRI_BYTES);
	check_edges(&cases[i], s->read);
	check_copy(&cases[i], s->read);
	check_loaded(&cases[i], s->read);
}

// The samples a square at alpha k/16 covers.
static uint32_t covered(const AlphaCase *a, uint32_t k)
{
	uint32_t n = 0;
	for (uint32_t i = 0; i < a->samples; i++)
		n += a->thresholds[i] <= k;
	return n;
}

// Each pixel of the band: in square k, the share of samples alpha k/16 covers; elsewhere the clear.
static void check_band(const Harness *harness, const Scene *s, uint32_t i)
{
	const AlphaCase *const a = &alpha_cases[i];
	const VkImage image = (a->samples == VK_SAMPLE_COUNT_1_BIT ? &s->band_drawn[i] : &s->band_resolved[i])->image;
	const bool read = read_back(harness, s, &image, 1, BAND_WIDTH, BAND_HEIGHT);
	char name[LABEL_BYTES];
	snprintf(name, sizeof name, "alpha to coverage at %u sample%s", (unsigned)a->samples, a->samples > 1 ? "s" : "");
	uint32_t wrong = read ? 0 : BAND_WIDTH * BAND_HEIGHT;
	uint32_t logged = 0;
	for (uint32_t y = 0; y < BAND_HEIGHT && read; y++) {
		for (uint32_t x = 0; x < BAND_WIDTH; x++) {
			const uint32_t k = x >= SQUARE_LEFT ? (x - SQUARE_LEFT) / SQUARE_CELL : SQUARES;
			const bool square = k < SQUARES && (x - SQUARE_LEFT) % SQUARE_CELL < SQUARE_SIZE && y >= SQUARE_TOP &&
				y < SQUARE_TOP + SQUARE_SIZE;
			const uint8_t *const texel = s->read + (y * BAND_WIDTH + x) * 4;
			const int want = square ? (int)covered(a, k) : 0;
			if (blend_level(KIND_BYTES, 1.5f / 255, a->shape, a->samples, texel) == want)
				continue;
			wrong++;
			log_texel(name, square ? "a square's" : "the clear's", x, y, KIND_BYTES, texel, &logged);
		}
	}
	xsdk_log_write(
		"multisample: %s: %u of %u pixels wrong", name, (unsigned)wrong, (unsigned)(BAND_WIDTH * BAND_HEIGHT));
	char label[2 * LABEL_BYTES];
	snprintf(label, sizeof label,
		"multisample: %s: squares at alphas 0 to 1 in 16ths, each pixel the share of its samples covered", name);
	check(label, wrong == 0);
}

static VkSampleCountFlags counts_of(const Harness *harness, VkFormat format, VkImageUsageFlags usage)
{
	VkImageFormatProperties properties;
	const VkResult result = vkGetPhysicalDeviceImageFormatProperties(
		harness->physical_device, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &properties);
	return result == VK_SUCCESS ? properties.sampleCounts : 0;
}

// The formats xsdk takes at 2 and 4 samples report them, the floats 1, and anything sampled 1; the device's framebuffer
// counts are 1, 2 and 4, its sampled ones 1, and its samples aren't at Vulkan's standard places.
static void check_counts(const Harness *harness)
{
	const VkImageUsageFlags color = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	const VkImageUsageFlags depth = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	const VkFormat taking[] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
		VK_FORMAT_A2B10G10R10_UNORM_PACK32 };
	const VkFormat floats[] = { VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_SFLOAT };
	bool counted = true;
	for (uint32_t i = 0; i < 3; i++) {
		counted = counted && counts_of(harness, taking[i], color) == ALL_SAMPLES &&
			counts_of(harness, taking[i], color | VK_IMAGE_USAGE_SAMPLED_BIT) == VK_SAMPLE_COUNT_1_BIT &&
			counts_of(harness, floats[i], color) == VK_SAMPLE_COUNT_1_BIT;
	}
	counted = counted && counts_of(harness, VK_FORMAT_D24_UNORM_S8_UINT, depth) == ALL_SAMPLES &&
		counts_of(harness, VK_FORMAT_D24_UNORM_S8_UINT, depth | VK_IMAGE_USAGE_SAMPLED_BIT) == VK_SAMPLE_COUNT_1_BIT;
	VkPhysicalDeviceProperties properties;
	vkGetPhysicalDeviceProperties(harness->physical_device, &properties);
	const VkPhysicalDeviceLimits *const limits = &properties.limits;
	xsdk_log_write("multisample: framebuffer sample counts %X %X %X, sampled %X, standard locations %u",
		(unsigned)limits->framebufferColorSampleCounts, (unsigned)limits->framebufferDepthSampleCounts,
		(unsigned)limits->framebufferStencilSampleCounts, (unsigned)limits->sampledImageColorSampleCounts,
		(unsigned)limits->standardSampleLocations);
	check(
		"multisample: 8-bit, 10-bit and depth attachments take 1, 2 and 4 samples, the floats and sampled images 1; "
		"the limits say so, and the samples aren't at Vulkan's standard places",
		counted && limits->framebufferColorSampleCounts == ALL_SAMPLES &&
			limits->framebufferDepthSampleCounts == ALL_SAMPLES &&
			limits->framebufferStencilSampleCounts == ALL_SAMPLES &&
			limits->sampledImageColorSampleCounts == VK_SAMPLE_COUNT_1_BIT && !limits->standardSampleLocations);
}

static VkResult try_image(
	const Harness *harness, VkFormat format, VkSampleCountFlagBits samples, VkImageUsageFlags usage)
{
	Picture picture = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
	const VkResult result = make_picture(harness, format, 64, 64, samples, usage, &picture);
	vkDestroyImageView(harness->device, picture.view, NULL);
	vkDestroyImage(harness->device, picture.image, NULL);
	vkFreeMemory(harness->device, picture.memory, NULL);
	return result;
}

// A subpass of a colour attachment at color_samples and a depth one at depth_samples.
static VkResult try_pass(
	const Harness *harness, VkFormat format, VkSampleCountFlagBits color_samples, VkSampleCountFlagBits depth_samples)
{
	const VkAttachmentDescription described[2] = {
		attachment_of(format, color_samples, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE),
		attachment_of(
			VK_FORMAT_D24_UNORM_S8_UINT, depth_samples, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE),
	};
	const VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	const VkAttachmentReference depth = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	const VkSubpassDescription subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &color,
		.pDepthStencilAttachment = &depth,
	};
	const VkRenderPassCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 2,
		.pAttachments = described,
		.subpassCount = 1,
		.pSubpasses = &subpass,
	};
	VkRenderPass render_pass = VK_NULL_HANDLE;
	const VkResult result = vkCreateRenderPass(harness->device, &info, NULL, &render_pass);
	vkDestroyRenderPass(harness->device, render_pass, NULL);
	return result;
}

// A pipeline in the 4-sample case's pass with the multisample state given.
static VkResult try_pipeline(
	const Harness *harness, const Scene *s, const VkPipelineMultisampleStateCreateInfo *multisample)
{
	const HarnessPipeline spec = {
		.vertex_spirv = push_vert_spirv,
		.vertex_bytes = PUSH_VERT_SPIRV_BYTES,
		.fragment_spirv = push_frag_spirv,
		.fragment_bytes = PUSH_FRAG_SPIRV_BYTES,
		.bindings = &harness_quad_binding,
		.binding_count = 1,
		.attributes = &harness_quad_attribute,
		.attribute_count = 1,
		.layout = s->push_layout,
		.dynamic_viewport = true,
		.multisample = multisample,
	};
	VkPipeline pipeline = VK_NULL_HANDLE;
	const VkResult result = harness_pipeline(harness, &spec, s->first_passes[1], &pipeline);
	vkDestroyPipeline(harness->device, pipeline, NULL);
	return result;
}

// A 4-sample pass over half its framebuffer, which xsdk can't resolve a rectangle of; ending the buffer fails.
static VkResult try_area(const Scene *s)
{
	const VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	VkResult result = vkBeginCommandBuffer(s->work, &begin_info);
	if (result != VK_SUCCESS)
		return result;
	const VkClearValue clear = { .color = { .float32 = { 0.0f } } };
	const VkRenderPassBeginInfo info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = s->first_passes[1],
		.framebuffer = s->first_targets[1],
		.renderArea = { { 0, 0 }, { TRI_WIDTH / 2, TRI_HEIGHT / 2 } },
		.clearValueCount = 1,
		.pClearValues = &clear,
	};
	vkCmdBeginRenderPass(s->work, &info, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdEndRenderPass(s->work);
	return vkEndCommandBuffer(s->work);
}

static void check_refusals(const Harness *harness, const Scene *s)
{
	const VkImageUsageFlags color = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	const VkSampleMask one_sample = 0x1;
	const VkPipelineMultisampleStateCreateInfo four = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_4_BIT,
	};
	VkPipelineMultisampleStateCreateInfo masked = four;
	masked.pSampleMask = &one_sample;
	VkPipelineMultisampleStateCreateInfo to_one = four;
	to_one.alphaToOneEnable = VK_TRUE;
	VkPipelineMultisampleStateCreateInfo shading = four;
	shading.sampleShadingEnable = VK_TRUE;
	const VkResult results[REFUSALS] = {
		try_image(harness, VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_4_BIT, color),
		try_image(harness, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_4_BIT, color | VK_IMAGE_USAGE_SAMPLED_BIT),
		try_image(harness, VK_FORMAT_D24_UNORM_S8_UINT, VK_SAMPLE_COUNT_2_BIT,
			VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT),
		try_image(harness, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_8_BIT, color),
		try_pass(harness, VK_FORMAT_R32_SFLOAT, VK_SAMPLE_COUNT_2_BIT, VK_SAMPLE_COUNT_2_BIT),
		try_pass(harness, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_4_BIT, VK_SAMPLE_COUNT_2_BIT),
		try_pipeline(harness, s, &masked),
		try_pipeline(harness, s, &to_one),
		try_pipeline(harness, s, &shading),
		try_area(s),
	};
	bool refused = true;
	for (uint32_t i = 0; i < REFUSALS; i++) {
		if (results[i] != VK_ERROR_FEATURE_NOT_PRESENT)
			xsdk_log_write("multisample: refusal %u returned %d", (unsigned)i, (int)results[i]);
		refused = refused && results[i] == VK_ERROR_FEATURE_NOT_PRESENT;
	}
	const bool allowed = try_pipeline(harness, s, &four) == VK_SUCCESS;
	check(
		"multisample: float attachments, sampled images and 8 samples refused at 2 and 4 samples, attachments of "
		"two counts, sample masks, alpha to one, sample shading and smaller render areas refused; a plain 4-sample "
		"pipeline made",
		refused && allowed);
}

static void destroy_picture(const Harness *harness, Picture *picture)
{
	vkDestroyImageView(harness->device, picture->view, NULL);
	vkDestroyImage(harness->device, picture->image, NULL);
	vkFreeMemory(harness->device, picture->memory, NULL);
}

static void destroy(const Harness *harness, Scene *s)
{
	const VkDevice device = harness->device;
	vkFreeCommandBuffers(device, harness->pool, harness->image_count, s->commands);
	vkFreeCommandBuffers(device, harness->pool, 1, &s->work);
	const VkBuffer buffers[] = { s->readback, s->quads, s->unit_quad, s->triangle };
	const VkDeviceMemory memories[] = { s->readback_memory, s->quads_memory, s->unit_quad_memory, s->triangle_memory };
	for (uint32_t i = 0; i < sizeof buffers / sizeof buffers[0]; i++) {
		vkDestroyBuffer(device, buffers[i], NULL);
		vkFreeMemory(device, memories[i], NULL);
	}
	vkDestroyPipeline(device, s->screen_pipeline, NULL);
	for (uint32_t i = 0; i < CASES; i++)
		vkDestroyPipeline(device, s->case_pipelines[i], NULL);
	for (uint32_t i = 0; i < ALPHA_CASES; i++)
		vkDestroyPipeline(device, s->band_pipelines[i], NULL);
	vkDestroyDescriptorPool(device, s->pool, NULL);
	vkDestroySampler(device, s->sampler, NULL);
	vkDestroyPipelineLayout(device, s->sample_layout, NULL);
	vkDestroyDescriptorSetLayout(device, s->set_layout, NULL);
	vkDestroyPipelineLayout(device, s->push_layout, NULL);
	harness_destroy_render_pass(harness, s->screen_pass, s->screens);
	for (uint32_t i = 0; i < CASES; i++) {
		vkDestroyFramebuffer(device, s->first_targets[i], NULL);
		vkDestroyFramebuffer(device, s->second_targets[i], NULL);
		vkDestroyRenderPass(device, s->first_passes[i], NULL);
		vkDestroyRenderPass(device, s->second_passes[i], NULL);
		destroy_picture(harness, &s->drawn[i]);
		destroy_picture(harness, &s->resolved[i]);
		destroy_picture(harness, &s->copied[i]);
		destroy_picture(harness, &s->loaded[i]);
	}
	for (uint32_t i = 0; i < ALPHA_CASES; i++) {
		vkDestroyFramebuffer(device, s->band_targets[i], NULL);
		vkDestroyRenderPass(device, s->band_passes[i], NULL);
		destroy_picture(harness, &s->band_drawn[i]);
		destroy_picture(harness, &s->band_resolved[i]);
	}
}

void test_multisample(const Harness *harness)
{
	static Scene scene;
	Scene *const s = &scene;
	memset(s, 0, sizeof *s);
	const bool built = harness->swapchain && build(harness, s);
	check("multisample: images at 2 and 4 samples, render passes with resolve attachments, and pipelines made", built);
	const bool recorded = built && harness_ok("recording", record(harness, s));
	HarnessProbes probed = { "multisample", probes, PROBES, PROBE_TOLERANCE, 0 };
	uint32_t misses = 0;
	const uint32_t frames =
		recorded ? harness_frames(harness, s->commands, FRAMES, harness_probes_hold, &probed, &misses) : 0;
	check(
		"multisample: 3 frames of triangles at 2 and 4 samples resolved, copied and loaded back, and alpha to "
		"coverage at 1, 2 and 4 samples, on screen",
		frames == FRAMES && misses == 0);
	for (uint32_t i = 0; i < CASES && recorded; i++)
		check_case(harness, s, i);
	for (uint32_t i = 0; i < ALPHA_CASES && recorded; i++)
		check_band(harness, s, i);
	check_counts(harness);
	if (built)
		check_refusals(harness, s);
	destroy(harness, s);
}
