#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "tiled_index.h"
#include "xsdk/log.h"

// Sampled D24S8 attachments 16 texels or less on a side (milestone 67), as PPSSPP's 1x1 framebuffers have (God of War:
// Chains of Olympus): their textures' bases at the corner, where a resolve writes, as small colour attachments' are
// (milestone 31). Each of a 1x1, a 4x4 and a 16x16 is drawn with depth 0.25 on its left half and 0.75 on its right,
// left to be sampled by its pass; two 8x8 ones the same, kept an attachment by their passes and left to be sampled by a
// barrier after each, as PPSSPP does, the first's pass followed by the second's, which clears depth at its size. Each
// one's depth is read from its memory; then each is sampled 2 by 2 pixels a texel into a larger attachment read back;
// then, after a 1280x720 pass with depth of its own has taken the eDRAM they were in, each is loaded into a pass, the
// 8x8 ones past a barrier back to an attachment, that draws a quad at 0.5, which lands only where the depth was 0.75.

enum {
	FENCE_TIMEOUT_NS = 1000000000,
	TARGETS = 5,
	SHOWN_WIDTH = 100,
	SHOWN_HEIGHT = 40,
	SHOWN_SCALE = 2,
	SHOWN_TOP = 4,
	// The pass between, over eDRAM's top, where depths go (docs/SUPPORT.md, Render passes).
	BIG_WIDTH = 1280,
	BIG_HEIGHT = 720,
	TEXEL_BYTES = 4,
	VERTEX_FLOATS = 6,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	UV_OFFSET_BYTES = 16,
	QUAD_VERTICES = 6,
	// A tiled texture's rows are whole tiles of 32 texels apart (xsdk:src/texture.c).
	TILE_TEXELS = 32,
	DEPTH_24_MAX = 0xFFFFFF,
	// D24's rounding, as xsdk's check of the resolve allows (xsdk:tests/test_depth.c).
	DEPTH_24_TOLERANCE = 2,
	// A depth through the sampler and the target can round by a step or two.
	TOLERANCE = 2,
	// The memory type the CPU caches, which buffers only copied to and from may take.
	CACHED_TYPE = 1,
	MAX_MISSES_LOGGED = 4,
};

typedef struct {
	const char *name;
	uint32_t side;
	// Its left edge in the shown attachment.
	uint32_t shown_x;
	// Its pass keeps depth an attachment, and a barrier leaves it to be read.
	bool barrier;
} Shape;

static const Shape shapes[TARGETS] = {
	{ "1x1", 1, 4, false },
	{ "4x4", 4, 12, false },
	{ "16x16", 16, 24, false },
	{ "the first 8x8", 8, 60, true },
	{ "the second 8x8", 8, 80, true },
};

static const float near_depth = 0.25f;
static const float far_depth = 0.75f;
static const float loaded_depth = 0.5f;
static const uint8_t near_color[4] = { 224, 64, 64, 255 };
static const uint8_t far_color[4] = { 64, 224, 64, 255 };
static const uint8_t loaded_color[4] = { 64, 64, 224, 255 };
static const uint8_t grey[4] = { 24, 24, 24, 255 };

static const VkVertexInputBindingDescription shown_binding = { 0, STRIDE_BYTES, VK_VERTEX_INPUT_RATE_VERTEX };
static const VkVertexInputAttributeDescription shown_attributes[] = {
	{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0 },
	{ .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = UV_OFFSET_BYTES },
};

typedef struct {
	VkImage colors[TARGETS];
	VkDeviceMemory color_memories[TARGETS];
	VkImageView color_views[TARGETS];
	VkImage depths[TARGETS];
	VkDeviceMemory depth_memories[TARGETS];
	// Depth and stencil, for the framebuffer; depth alone, to sample.
	VkImageView depth_views[TARGETS];
	VkImageView sampled_views[TARGETS];
	VkImage shown;
	VkDeviceMemory shown_memory;
	VkImageView shown_view;
	// Cleared, drawn and left to be sampled; loaded and drawn again. Each again with depth an attachment after and
	// before.
	VkRenderPass draw_pass;
	VkRenderPass load_pass;
	VkRenderPass draw_kept_pass;
	VkRenderPass load_kept_pass;
	VkFramebuffer framebuffers[TARGETS];
	VkRenderPass shown_pass;
	VkFramebuffer shown_framebuffer;
	VkImage big_color;
	VkDeviceMemory big_color_memory;
	VkImageView big_color_view;
	VkImage big_depth;
	VkDeviceMemory big_depth_memory;
	VkImageView big_depth_view;
	VkRenderPass big_pass;
	VkFramebuffer big_framebuffer;
	VkPipelineLayout push_layout;
	VkPipeline depth_pipeline;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout texture_layout;
	VkPipeline textured;
	VkSampler sampler;
	VkDescriptorPool pool;
	VkDescriptorSet sets[TARGETS];
	VkBuffer unit_quad;
	VkDeviceMemory unit_quad_memory;
	VkBuffer shown_quads;
	VkDeviceMemory shown_quads_memory;
	VkBuffer readback;
	VkDeviceMemory readback_memory;
	const uint8_t *read;
	VkCommandBuffer commands;
} Scene;

static uint32_t depth_24(float depth)
{
	return (uint32_t)(depth * (double)DEPTH_24_MAX + 0.5);
}

// The left half, or all of a texel-wide one, holds the near depth; the rest the far.
static uint32_t near_width(uint32_t side)
{
	return (side + 1) / 2;
}

// Where each target's colour lands in the read-back buffer, one after another; the shown attachment after.
static uint32_t texels_at(uint32_t target)
{
	uint32_t at = 0;
	for (uint32_t i = 0; i < target; i++)
		at += shapes[i].side * shapes[i].side * TEXEL_BYTES;
	return at;
}

// Stored, but for depth that's only loaded, so its memory keeps the first pass's.
static VkAttachmentDescription attachment(
	VkFormat format, VkAttachmentLoadOp load, VkImageLayout initial, VkImageLayout final)
{
	const bool depth = format == VK_FORMAT_D24_UNORM_S8_UINT;
	return (VkAttachmentDescription){ .format = format,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = load,
		.storeOp = depth && load == VK_ATTACHMENT_LOAD_OP_LOAD ? VK_ATTACHMENT_STORE_OP_DONT_CARE
															   : VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = format == VK_FORMAT_D24_UNORM_S8_UINT ? load : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp =
			format == VK_FORMAT_D24_UNORM_S8_UINT ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = initial,
		.finalLayout = final };
}

// Colour and depth; the depth left to be sampled by the first, and loaded from there by the second, or left an
// attachment and loaded from one when kept.
static VkResult create_pass(const Harness *harness, bool loads, bool kept, VkRenderPass *render_pass)
{
	const VkAttachmentLoadOp load = loads ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
	const VkImageLayout read =
		kept ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	const VkAttachmentDescription attachments[2] = {
		attachment(VK_FORMAT_B8G8R8A8_UNORM, load,
			loads ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
			loads ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
		attachment(VK_FORMAT_D24_UNORM_S8_UINT, load, loads ? read : VK_IMAGE_LAYOUT_UNDEFINED,
			loads ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : read),
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
		.pAttachments = attachments,
		.subpassCount = 1,
		.pSubpasses = &subpass,
	};
	return vkCreateRenderPass(harness->device, &info, NULL, render_pass);
}

static VkResult create_shown_pass(const Harness *harness, VkRenderPass *render_pass)
{
	const VkAttachmentDescription color = attachment(VK_FORMAT_R8G8B8A8_UNORM, VK_ATTACHMENT_LOAD_OP_CLEAR,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	const VkAttachmentReference reference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	const VkSubpassDescription subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &reference,
	};
	const VkRenderPassCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &color,
		.subpassCount = 1,
		.pSubpasses = &subpass,
	};
	return vkCreateRenderPass(harness->device, &info, NULL, render_pass);
}

// Colour and depth both cleared and neither kept.
static VkResult create_big_pass(const Harness *harness, VkRenderPass *render_pass)
{
	const VkAttachmentDescription attachments[2] = {
		{ .format = VK_FORMAT_B8G8R8A8_UNORM,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
		{ .format = VK_FORMAT_D24_UNORM_S8_UINT,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL },
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
		.pAttachments = attachments,
		.subpassCount = 1,
		.pSubpasses = &subpass,
	};
	return vkCreateRenderPass(harness->device, &info, NULL, render_pass);
}

static bool build_images(const Harness *harness, Scene *scene)
{
	VkResult result = VK_SUCCESS;
	for (uint32_t i = 0; i < TARGETS && result == VK_SUCCESS; i++) {
		const uint32_t side = shapes[i].side;
		result = harness_image(harness, VK_FORMAT_B8G8R8A8_UNORM, side, side,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &scene->colors[i],
			&scene->color_memories[i], &scene->color_views[i]);
		if (result == VK_SUCCESS)
			result = harness_image(harness, VK_FORMAT_D24_UNORM_S8_UINT, side, side,
				VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &scene->depths[i],
				&scene->depth_memories[i], &scene->depth_views[i]);
		const VkImageViewCreateInfo view_info = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = scene->depths[i],
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = VK_FORMAT_D24_UNORM_S8_UINT,
			.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 },
		};
		if (result == VK_SUCCESS)
			result = vkCreateImageView(harness->device, &view_info, NULL, &scene->sampled_views[i]);
	}
	if (result == VK_SUCCESS)
		result = harness_image(harness, VK_FORMAT_R8G8B8A8_UNORM, SHOWN_WIDTH, SHOWN_HEIGHT,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &scene->shown, &scene->shown_memory,
			&scene->shown_view);
	if (result == VK_SUCCESS)
		result = create_pass(harness, false, false, &scene->draw_pass);
	if (result == VK_SUCCESS)
		result = create_pass(harness, true, false, &scene->load_pass);
	if (result == VK_SUCCESS)
		result = create_pass(harness, false, true, &scene->draw_kept_pass);
	if (result == VK_SUCCESS)
		result = create_pass(harness, true, true, &scene->load_kept_pass);
	if (result == VK_SUCCESS)
		result = create_shown_pass(harness, &scene->shown_pass);
	for (uint32_t i = 0; i < TARGETS && result == VK_SUCCESS; i++) {
		const VkImageView views[2] = { scene->color_views[i], scene->depth_views[i] };
		const VkFramebufferCreateInfo info = {
			.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			.renderPass = scene->draw_pass,
			.attachmentCount = 2,
			.pAttachments = views,
			.width = shapes[i].side,
			.height = shapes[i].side,
			.layers = 1,
		};
		result = vkCreateFramebuffer(harness->device, &info, NULL, &scene->framebuffers[i]);
	}
	const VkFramebufferCreateInfo shown_info = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = scene->shown_pass,
		.attachmentCount = 1,
		.pAttachments = &scene->shown_view,
		.width = SHOWN_WIDTH,
		.height = SHOWN_HEIGHT,
		.layers = 1,
	};
	if (result == VK_SUCCESS)
		result = vkCreateFramebuffer(harness->device, &shown_info, NULL, &scene->shown_framebuffer);
	if (result == VK_SUCCESS)
		result = harness_image(harness, VK_FORMAT_B8G8R8A8_UNORM, BIG_WIDTH, BIG_HEIGHT,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &scene->big_color, &scene->big_color_memory, &scene->big_color_view);
	if (result == VK_SUCCESS)
		result = harness_image(harness, VK_FORMAT_D24_UNORM_S8_UINT, BIG_WIDTH, BIG_HEIGHT,
			VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, &scene->big_depth, &scene->big_depth_memory,
			&scene->big_depth_view);
	if (result == VK_SUCCESS)
		result = create_big_pass(harness, &scene->big_pass);
	const VkImageView big_views[2] = { scene->big_color_view, scene->big_depth_view };
	const VkFramebufferCreateInfo big_info = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = scene->big_pass,
		.attachmentCount = 2,
		.pAttachments = big_views,
		.width = BIG_WIDTH,
		.height = BIG_HEIGHT,
		.layers = 1,
	};
	if (result == VK_SUCCESS)
		result = vkCreateFramebuffer(harness->device, &big_info, NULL, &scene->big_framebuffer);
	return harness_ok("the images, passes and framebuffers", result);
}

// Depth tests less and writes; no stencil.
static VkResult build_depth_pipeline(const Harness *harness, Scene *scene)
{
	const VkPipelineDepthStencilStateCreateInfo depth_stencil = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_LESS,
	};
	VkResult result = harness_push_layout(harness, &scene->push_layout);
	const HarnessPipeline spec = {
		.vertex_spirv = push_vert_spirv,
		.vertex_bytes = PUSH_VERT_SPIRV_BYTES,
		.fragment_spirv = push_frag_spirv,
		.fragment_bytes = PUSH_FRAG_SPIRV_BYTES,
		.bindings = &harness_quad_binding,
		.binding_count = 1,
		.attributes = &harness_quad_attribute,
		.attribute_count = 1,
		.layout = scene->push_layout,
		.depth_stencil = &depth_stencil,
		.dynamic_viewport = true,
	};
	if (result == VK_SUCCESS)
		result = harness_pipeline(harness, &spec, scene->draw_pass, &scene->depth_pipeline);
	return result;
}

// The depth views sampled nearest, one set each.
static VkResult build_textured(const Harness *harness, Scene *scene)
{
	const VkDescriptorSetLayoutBinding binding = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	};
	const VkDescriptorSetLayoutCreateInfo set_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &binding,
	};
	VkResult result = vkCreateDescriptorSetLayout(harness->device, &set_info, NULL, &scene->set_layout);
	const VkPipelineLayoutCreateInfo layout_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &scene->set_layout,
	};
	if (result == VK_SUCCESS)
		result = vkCreatePipelineLayout(harness->device, &layout_info, NULL, &scene->texture_layout);
	const HarnessPipeline spec = {
		.vertex_spirv = texture_vert_spirv,
		.vertex_bytes = TEXTURE_VERT_SPIRV_BYTES,
		.fragment_spirv = texture_frag_spirv,
		.fragment_bytes = TEXTURE_FRAG_SPIRV_BYTES,
		.bindings = &shown_binding,
		.binding_count = 1,
		.attributes = shown_attributes,
		.attribute_count = 2,
		.layout = scene->texture_layout,
		.dynamic_viewport = true,
	};
	if (result == VK_SUCCESS)
		result = harness_pipeline(harness, &spec, scene->shown_pass, &scene->textured);
	const VkSamplerCreateInfo sampler_info = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_NEAREST,
		.minFilter = VK_FILTER_NEAREST,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	};
	if (result == VK_SUCCESS)
		result = vkCreateSampler(harness->device, &sampler_info, NULL, &scene->sampler);
	const VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, TARGETS };
	const VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = TARGETS,
		.poolSizeCount = 1,
		.pPoolSizes = &size,
	};
	if (result == VK_SUCCESS)
		result = vkCreateDescriptorPool(harness->device, &pool_info, NULL, &scene->pool);
	VkDescriptorSetLayout layouts[TARGETS];
	for (uint32_t i = 0; i < TARGETS; i++)
		layouts[i] = scene->set_layout;
	const VkDescriptorSetAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = scene->pool,
		.descriptorSetCount = TARGETS,
		.pSetLayouts = layouts,
	};
	if (result == VK_SUCCESS)
		result = vkAllocateDescriptorSets(harness->device, &allocate_info, scene->sets);
	for (uint32_t i = 0; i < TARGETS && result == VK_SUCCESS; i++) {
		const VkDescriptorImageInfo image = { scene->sampler, scene->sampled_views[i],
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

static float clip(float pixels, float size)
{
	return pixels / (size / 2.0f) - 1.0f;
}

// Each target's quad in the shown attachment, 2 by 2 pixels a texel, the whole texture across it.
static void put_quads(float quads[TARGETS][QUAD_VERTICES][VERTEX_FLOATS])
{
	for (uint32_t i = 0; i < TARGETS; i++) {
		const float left = clip((float)shapes[i].shown_x, SHOWN_WIDTH);
		const float right = clip((float)(shapes[i].shown_x + shapes[i].side * SHOWN_SCALE), SHOWN_WIDTH);
		const float top = clip(SHOWN_TOP, SHOWN_HEIGHT);
		const float bottom = clip((float)(SHOWN_TOP + shapes[i].side * SHOWN_SCALE), SHOWN_HEIGHT);
		const float corners[QUAD_VERTICES][VERTEX_FLOATS] = {
			{ left, top, 0.0f, 1.0f, 0.0f, 0.0f },
			{ right, top, 0.0f, 1.0f, 1.0f, 0.0f },
			{ left, bottom, 0.0f, 1.0f, 0.0f, 1.0f },
			{ right, top, 0.0f, 1.0f, 1.0f, 0.0f },
			{ right, bottom, 0.0f, 1.0f, 1.0f, 1.0f },
			{ left, bottom, 0.0f, 1.0f, 0.0f, 1.0f },
		};
		memcpy(quads[i], corners, sizeof corners);
	}
}

// The read-back buffer, in memory the CPU caches.
static VkResult build_readback(const Harness *harness, Scene *scene)
{
	const VkBufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = texels_at(TARGETS) + SHOWN_WIDTH * SHOWN_HEIGHT * TEXEL_BYTES,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	};
	VkResult result = vkCreateBuffer(harness->device, &info, NULL, &scene->readback);
	if (result != VK_SUCCESS)
		return result;
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(harness->device, scene->readback, &requirements);
	const VkMemoryAllocateInfo allocate = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
		.memoryTypeIndex = CACHED_TYPE,
	};
	void *mapped = NULL;
	result = vkAllocateMemory(harness->device, &allocate, NULL, &scene->readback_memory);
	if (result == VK_SUCCESS)
		result = vkBindBufferMemory(harness->device, scene->readback, scene->readback_memory, 0);
	if (result == VK_SUCCESS)
		result = vkMapMemory(harness->device, scene->readback_memory, 0, VK_WHOLE_SIZE, 0, &mapped);
	scene->read = mapped;
	return result;
}

static bool build_drawing(const Harness *harness, Scene *scene)
{
	static float quads[TARGETS][QUAD_VERTICES][VERTEX_FLOATS];
	put_quads(quads);
	const bool ok = harness_ok("depth pipeline", build_depth_pipeline(harness, scene)) &&
		harness_ok("sampling", build_textured(harness, scene)) &&
		harness_ok("read-back buffer", build_readback(harness, scene)) &&
		harness_ok("unit quad", harness_quad_buffer(harness, &scene->unit_quad, &scene->unit_quad_memory)) &&
		harness_ok("shown quads",
			harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, quads, sizeof quads, &scene->shown_quads,
				&scene->shown_quads_memory));
	const VkCommandBufferAllocateInfo commands = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = harness->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	return ok &&
		harness_ok("vkAllocateCommandBuffers", vkAllocateCommandBuffers(harness->device, &commands, &scene->commands));
}

static HarnessQuad quad_of(float x, float width, float height, float depth, const uint8_t rgba[4])
{
	HarnessQuad quad = { x, 0.0f, width, height, depth, { 0.0f, 0.0f, 0.0f, 1.0f }, false };
	for (uint32_t c = 0; c < 3; c++)
		quad.color[c] = rgba[c] / 255.0f;
	return quad;
}

static void begin_pass(const Scene *scene, VkRenderPass render_pass, uint32_t i)
{
	const VkClearValue clears[2] = {
		{ .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } },
		{ .depthStencil = { 1.0f, 0 } },
	};
	const uint32_t side = shapes[i].side;
	const VkRenderPassBeginInfo info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = render_pass,
		.framebuffer = scene->framebuffers[i],
		.renderArea = { { 0, 0 }, { side, side } },
		.clearValueCount = 2,
		.pClearValues = clears,
	};
	vkCmdBeginRenderPass(scene->commands, &info, VK_SUBPASS_CONTENTS_INLINE);
	const VkViewport viewport = { 0.0f, 0.0f, (float)side, (float)side, 0.0f, 1.0f };
	vkCmdSetViewport(scene->commands, 0, 1, &viewport);
	vkCmdSetScissor(scene->commands, 0, 1, &info.renderArea);
	vkCmdBindPipeline(scene->commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->depth_pipeline);
	const VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(scene->commands, 0, 1, &scene->unit_quad, &offset);
}

static void copy_out(const Scene *scene, VkImage image, uint32_t width, uint32_t height, uint32_t at)
{
	const VkBufferImageCopy region = {
		.bufferOffset = at,
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { width, height, 1 },
	};
	vkCmdCopyImageToBuffer(scene->commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scene->readback, 1, &region);
}

// A depth image's barrier between an attachment and being sampled, either way.
static void depth_barrier(VkCommandBuffer commands, VkImage image, VkImageLayout from, VkImageLayout to)
{
	const bool to_read = to == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	const VkAccessFlags attachment_access =
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	const VkImageMemoryBarrier barrier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = to_read ? attachment_access : VK_ACCESS_SHADER_READ_BIT,
		.dstAccessMask = to_read ? VK_ACCESS_SHADER_READ_BIT : attachment_access,
		.oldLayout = from,
		.newLayout = to,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = image,
		.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1 },
	};
	const VkPipelineStageFlags tests =
		VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	vkCmdPipelineBarrier(commands, to_read ? tests : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		to_read ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : tests, 0, 0, NULL, 0, NULL, 1, &barrier);
}

// Each target drawn, near on the left and far over all, left to be sampled by its pass or by a barrier after; the
// depths sampled into the shown attachment; each target loaded, past a barrier back for those, and a quad at the middle
// depth drawn over all; the colours read back.
static void record(const Scene *scene)
{
	const VkCommandBuffer commands = scene->commands;
	for (uint32_t i = 0; i < TARGETS; i++) {
		const float side = (float)shapes[i].side;
		begin_pass(scene, shapes[i].barrier ? scene->draw_kept_pass : scene->draw_pass, i);
		const HarnessQuad near = quad_of(0.0f, (float)near_width(shapes[i].side), side, near_depth, near_color);
		const HarnessQuad far = quad_of(0.0f, side, side, far_depth, far_color);
		harness_push_quad_in(commands, scene->push_layout, &near, shapes[i].side, shapes[i].side);
		harness_push_quad_in(commands, scene->push_layout, &far, shapes[i].side, shapes[i].side);
		vkCmdEndRenderPass(commands);
		if (shapes[i].barrier)
			depth_barrier(commands, scene->depths[i], VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
	const VkClearValue clear = { .color = {
									 .float32 = { grey[0] / 255.0f, grey[1] / 255.0f, grey[2] / 255.0f, 1.0f } } };
	const VkRect2D area = { { 0, 0 }, { SHOWN_WIDTH, SHOWN_HEIGHT } };
	const VkRenderPassBeginInfo shown_info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = scene->shown_pass,
		.framebuffer = scene->shown_framebuffer,
		.renderArea = area,
		.clearValueCount = 1,
		.pClearValues = &clear,
	};
	vkCmdBeginRenderPass(commands, &shown_info, VK_SUBPASS_CONTENTS_INLINE);
	const VkViewport viewport = { 0.0f, 0.0f, SHOWN_WIDTH, SHOWN_HEIGHT, 0.0f, 1.0f };
	vkCmdSetViewport(commands, 0, 1, &viewport);
	vkCmdSetScissor(commands, 0, 1, &area);
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->textured);
	const VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(commands, 0, 1, &scene->shown_quads, &offset);
	for (uint32_t i = 0; i < TARGETS; i++) {
		vkCmdBindDescriptorSets(
			commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->texture_layout, 0, 1, &scene->sets[i], 0, NULL);
		vkCmdDraw(commands, QUAD_VERTICES, 1, i * QUAD_VERTICES, 0);
	}
	vkCmdEndRenderPass(commands);
	const VkClearValue big_clears[2] = {
		{ .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } },
		{ .depthStencil = { 1.0f, 0 } },
	};
	const VkRenderPassBeginInfo big_info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = scene->big_pass,
		.framebuffer = scene->big_framebuffer,
		.renderArea = { { 0, 0 }, { BIG_WIDTH, BIG_HEIGHT } },
		.clearValueCount = 2,
		.pClearValues = big_clears,
	};
	vkCmdBeginRenderPass(commands, &big_info, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdEndRenderPass(commands);
	for (uint32_t i = 0; i < TARGETS; i++) {
		const float side = (float)shapes[i].side;
		if (shapes[i].barrier)
			depth_barrier(commands, scene->depths[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
		begin_pass(scene, shapes[i].barrier ? scene->load_kept_pass : scene->load_pass, i);
		const HarnessQuad loaded = quad_of(0.0f, side, side, loaded_depth, loaded_color);
		harness_push_quad_in(commands, scene->push_layout, &loaded, shapes[i].side, shapes[i].side);
		vkCmdEndRenderPass(commands);
		copy_out(scene, scene->colors[i], shapes[i].side, shapes[i].side, texels_at(i));
	}
	copy_out(scene, scene->shown, SHOWN_WIDTH, SHOWN_HEIGHT, texels_at(TARGETS));
}

static bool run(const Harness *harness, const Scene *scene)
{
	const VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	if (!harness_ok("vkBeginCommandBuffer", vkBeginCommandBuffer(scene->commands, &begin)))
		return false;
	record(scene);
	const VkSubmitInfo info = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &scene->commands,
	};
	const bool done = harness_ok("vkEndCommandBuffer", vkEndCommandBuffer(scene->commands)) &&
		harness_ok("vkResetFences", vkResetFences(harness->device, 1, &harness->fence)) &&
		harness_ok("vkQueueSubmit", vkQueueSubmit(harness->queue, 1, &info, harness->fence)) &&
		harness_ok("vkWaitForFences", vkWaitForFences(harness->device, 1, &harness->fence, VK_TRUE, FENCE_TIMEOUT_NS));
	(void)vkResetFences(harness->device, 1, &harness->fence);
	return done;
}

static uint32_t difference(uint32_t a, uint32_t b)
{
	return a > b ? a - b : b - a;
}

static float depth_at(uint32_t target, uint32_t x)
{
	return x < near_width(shapes[target].side) ? near_depth : far_depth;
}

// Each depth image's words as the CPU reads them, its base at the corner: depth above stencil 0.
static uint32_t words_wrong(const Harness *harness, const Scene *scene)
{
	uint32_t wrong = 0;
	for (uint32_t i = 0; i < TARGETS; i++) {
		void *mapped = NULL;
		if (!harness_ok(
				"vkMapMemory", vkMapMemory(harness->device, scene->depth_memories[i], 0, VK_WHOLE_SIZE, 0, &mapped)))
			return 1;
		volatile const uint32_t *const words = mapped;
		for (uint32_t y = 0; y < shapes[i].side; y++) {
			for (uint32_t x = 0; x < shapes[i].side; x++) {
				const uint32_t word = words[tiled_index(x, y, TILE_TEXELS)];
				const uint32_t depth = depth_24(depth_at(i, x));
				if (difference(word >> 8, depth) <= DEPTH_24_TOLERANCE && (word & 0xFF) == 0)
					continue;
				if (wrong++ < MAX_MISSES_LOGGED)
					xsdk_log_write("small depth: %s's word at (%u, %u) is %08X, not depth %06X above stencil 0",
						shapes[i].name, (unsigned)x, (unsigned)y, (unsigned)word, (unsigned)depth);
			}
		}
		vkUnmapMemory(harness->device, scene->depth_memories[i]);
	}
	return wrong;
}

static bool near_texel(const uint8_t *texel, const uint8_t want[4], uint32_t channels)
{
	for (uint32_t c = 0; c < channels; c++)
		if (difference(texel[c], want[c]) > TOLERANCE)
			return false;
	return true;
}

// What the shown attachment holds at a pixel: a target's depth in red, 2 by 2 pixels a texel, or the grey around them.
static void shown_want(uint32_t x, uint32_t y, uint8_t want[4])
{
	memcpy(want, grey, sizeof grey);
	for (uint32_t i = 0; i < TARGETS; i++) {
		const Shape *const shape = &shapes[i];
		if (x < shape->shown_x || x >= shape->shown_x + shape->side * SHOWN_SCALE || y < SHOWN_TOP ||
			y >= SHOWN_TOP + shape->side * SHOWN_SCALE)
			continue;
		want[0] = (uint8_t)(depth_at(i, (x - shape->shown_x) / SHOWN_SCALE) * 255.0f + 0.5f);
		want[1] = 0;
		want[2] = 0;
	}
}

static uint32_t shown_wrong(const Scene *scene)
{
	uint32_t wrong = 0;
	for (uint32_t y = 0; y < SHOWN_HEIGHT; y++) {
		for (uint32_t x = 0; x < SHOWN_WIDTH; x++) {
			const uint8_t *const texel = scene->read + texels_at(TARGETS) + (y * SHOWN_WIDTH + x) * TEXEL_BYTES;
			uint8_t want[4];
			shown_want(x, y, want);
			if (near_texel(texel, want, 3))
				continue;
			if (wrong++ < MAX_MISSES_LOGGED)
				xsdk_log_write("small depth: sampled, (%u, %u) is %u %u %u, not %u %u %u", (unsigned)x, (unsigned)y,
					texel[0], texel[1], texel[2], want[0], want[1], want[2]);
		}
	}
	return wrong;
}

// Each target's colour after the second pass: the near quad's on the left, the middle quad's where the far one was.
// B8G8R8A8 reads back blue first.
static uint32_t loaded_wrong(const Scene *scene)
{
	uint32_t wrong = 0;
	for (uint32_t i = 0; i < TARGETS; i++) {
		for (uint32_t y = 0; y < shapes[i].side; y++) {
			for (uint32_t x = 0; x < shapes[i].side; x++) {
				const uint8_t *const rgba = depth_at(i, x) == near_depth ? near_color : loaded_color;
				const uint8_t want[4] = { rgba[2], rgba[1], rgba[0], rgba[3] };
				const uint8_t *const texel = scene->read + texels_at(i) + (y * shapes[i].side + x) * TEXEL_BYTES;
				if (near_texel(texel, want, 4))
					continue;
				if (wrong++ < MAX_MISSES_LOGGED)
					xsdk_log_write("small depth: %s loaded, (%u, %u) is %u %u %u %u, not %u %u %u %u", shapes[i].name,
						(unsigned)x, (unsigned)y, texel[0], texel[1], texel[2], texel[3], want[0], want[1], want[2],
						want[3]);
			}
		}
	}
	return wrong;
}

static void destroy(const Harness *harness, const Scene *scene)
{
	const VkDevice device = harness->device;
	if (scene->commands)
		vkFreeCommandBuffers(device, harness->pool, 1, &scene->commands);
	vkDestroyBuffer(device, scene->readback, NULL);
	vkFreeMemory(device, scene->readback_memory, NULL);
	vkDestroyBuffer(device, scene->shown_quads, NULL);
	vkFreeMemory(device, scene->shown_quads_memory, NULL);
	vkDestroyBuffer(device, scene->unit_quad, NULL);
	vkFreeMemory(device, scene->unit_quad_memory, NULL);
	vkDestroyDescriptorPool(device, scene->pool, NULL);
	vkDestroySampler(device, scene->sampler, NULL);
	vkDestroyPipeline(device, scene->textured, NULL);
	vkDestroyPipelineLayout(device, scene->texture_layout, NULL);
	vkDestroyDescriptorSetLayout(device, scene->set_layout, NULL);
	vkDestroyPipeline(device, scene->depth_pipeline, NULL);
	vkDestroyPipelineLayout(device, scene->push_layout, NULL);
	vkDestroyFramebuffer(device, scene->big_framebuffer, NULL);
	vkDestroyRenderPass(device, scene->big_pass, NULL);
	vkDestroyImageView(device, scene->big_depth_view, NULL);
	vkDestroyImage(device, scene->big_depth, NULL);
	vkFreeMemory(device, scene->big_depth_memory, NULL);
	vkDestroyImageView(device, scene->big_color_view, NULL);
	vkDestroyImage(device, scene->big_color, NULL);
	vkFreeMemory(device, scene->big_color_memory, NULL);
	vkDestroyFramebuffer(device, scene->shown_framebuffer, NULL);
	for (uint32_t i = 0; i < TARGETS; i++)
		vkDestroyFramebuffer(device, scene->framebuffers[i], NULL);
	vkDestroyRenderPass(device, scene->shown_pass, NULL);
	vkDestroyRenderPass(device, scene->load_pass, NULL);
	vkDestroyRenderPass(device, scene->load_kept_pass, NULL);
	vkDestroyRenderPass(device, scene->draw_kept_pass, NULL);
	vkDestroyRenderPass(device, scene->draw_pass, NULL);
	vkDestroyImageView(device, scene->shown_view, NULL);
	vkDestroyImage(device, scene->shown, NULL);
	vkFreeMemory(device, scene->shown_memory, NULL);
	for (uint32_t i = 0; i < TARGETS; i++) {
		vkDestroyImageView(device, scene->sampled_views[i], NULL);
		vkDestroyImageView(device, scene->depth_views[i], NULL);
		vkDestroyImage(device, scene->depths[i], NULL);
		vkFreeMemory(device, scene->depth_memories[i], NULL);
		vkDestroyImageView(device, scene->color_views[i], NULL);
		vkDestroyImage(device, scene->colors[i], NULL);
		vkFreeMemory(device, scene->color_memories[i], NULL);
	}
}

void test_small_depth(const Harness *harness)
{
	static Scene scene;
	memset(&scene, 0, sizeof scene);
	const bool ok = build_images(harness, &scene) && build_drawing(harness, &scene);
	check("small depth: 1x1, 4x4, 16x16 and two 8x8 sampled D24S8 attachments, their passes and pipelines made", ok);
	const bool ran = ok && run(harness, &scene);
	check(
		"small depth: each one's depth resolved into its texture from the corner, every texel's: at its pass's end, or "
		"at a barrier after a pass that kept it an attachment",
		ran && words_wrong(harness, &scene) == 0);
	check("small depth: each one sampled 2 by 2 pixels a texel into a 100x40 attachment, every pixel its depth",
		ran && shown_wrong(&scene) == 0);
	check(
		"small depth: each one loaded into a pass after a 1280x720 one, a quad at 0.5 lands only where its depth was "
		"0.75",
		ran && loaded_wrong(&scene) == 0);
	destroy(harness, &scene);
}
