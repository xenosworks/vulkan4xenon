#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "xsdk/log.h"

// Milestone 31: colour attachments 16 texels or less on a side, their bases at the corner. Each is drawn texel for
// texel from a coded source of its own, as Aurora draws a copy, then read back by vkCmdCopyImageToBuffer, and sampled
// 2 by 2 pixels a texel into a larger attachment read back too, so a texel from the packed tail or another image shows.

enum {
	FENCE_TIMEOUT_NS = 1000000000,
	TARGETS = 4,
	// The sources, then the targets, then the attachment they're sampled into.
	IMAGES = 2 * TARGETS + 1,
	SHOWN = 2 * TARGETS,
	SHOWN_WIDTH = 128,
	SHOWN_HEIGHT = 64,
	SHOWN_SCALE = 2,
	SHOWN_TOP = 4,
	TEXEL_BYTES = 4,
	VERTEX_FLOATS = 6,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	UV_OFFSET_BYTES = 16,
	QUAD_VERTICES = 6,
	// A quad over a whole target, then one for each target in the shown attachment.
	QUADS = 1 + TARGETS,
	// The memory type the CPU caches, which buffers only copied to and from may take.
	CACHED_TYPE = 1,
	// A texel through the sampler and the target can round by one.
	TOLERANCE = 1,
	MAX_MISSES_LOGGED = 4,
};

typedef struct {
	const char *name;
	uint32_t width;
	uint32_t height;
	// Its left edge in the shown attachment.
	uint32_t shown_x;
} Shape;

// Wide as Dusklight's bloom's smallest copy, tall, square, and 2x1, whose tail is a square's
// (xsdk:docs/HARDWARE.md, "A 2x1 texture's tail").
static const Shape shapes[TARGETS] = {
	{ "20x11", 20, 11, 4 },
	{ "11x20", 11, 20, 48 },
	{ "16x16", 16, 16, 74 },
	{ "2x1", 2, 1, 110 },
};

static const uint8_t grey[4] = { 64, 64, 64, 255 };

static const VkVertexInputBindingDescription binding = { 0, STRIDE_BYTES, VK_VERTEX_INPUT_RATE_VERTEX };
static const VkVertexInputAttributeDescription attributes[] = {
	{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0 },
	{ .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = UV_OFFSET_BYTES },
};

typedef struct {
	VkImage images[IMAGES];
	VkDeviceMemory memories[IMAGES];
	VkImageView views[IMAGES];
	VkRenderPass render_pass;
	// Each target's, then the shown attachment's.
	VkFramebuffer framebuffers[TARGETS + 1];
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkPipeline pipeline;
	VkSampler sampler;
	VkDescriptorPool pool;
	// Each source's, then each target's.
	VkDescriptorSet sets[2 * TARGETS];
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkBuffer staging;
	VkDeviceMemory staging_memory;
	VkBuffer readback;
	VkDeviceMemory readback_memory;
	const uint8_t *read;
	VkCommandBuffer commands;
} Scene;

// Red holds x and green y, ten steps apart, and blue the image.
static void code(uint32_t target, uint32_t x, uint32_t y, uint8_t rgba[4])
{
	rgba[0] = (uint8_t)(16 + x * 10);
	rgba[1] = (uint8_t)(16 + y * 10);
	rgba[2] = (uint8_t)(40 + target * 50);
	rgba[3] = 255;
}

// Where each target's texels sit in the staging and read-back buffers, one after another; the shown attachment after.
static uint32_t texels_at(uint32_t target)
{
	uint32_t at = 0;
	for (uint32_t i = 0; i < target; i++)
		at += shapes[i].width * shapes[i].height * TEXEL_BYTES;
	return at;
}

static float clip(float pixels, float size)
{
	return pixels / (size / 2.0f) - 1.0f;
}

// A quad from (left, top) to (right, bottom) in clip space, the whole texture across it.
static void put_quad(float quad[QUAD_VERTICES][VERTEX_FLOATS], float left, float top, float right, float bottom)
{
	const float corners[QUAD_VERTICES][VERTEX_FLOATS] = {
		{ left, top, 0.0f, 1.0f, 0.0f, 0.0f },
		{ right, top, 0.0f, 1.0f, 1.0f, 0.0f },
		{ left, bottom, 0.0f, 1.0f, 0.0f, 1.0f },
		{ right, top, 0.0f, 1.0f, 1.0f, 0.0f },
		{ right, bottom, 0.0f, 1.0f, 1.0f, 1.0f },
		{ left, bottom, 0.0f, 1.0f, 0.0f, 1.0f },
	};
	memcpy(quad, corners, sizeof corners);
}

static bool build_images(const Harness *harness, Scene *scene)
{
	const VkImageUsageFlags source = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	const VkImageUsageFlags target =
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	VkResult result = VK_SUCCESS;
	for (uint32_t i = 0; i < 2 * TARGETS && result == VK_SUCCESS; i++)
		result = harness_image(harness, VK_FORMAT_R8G8B8A8_UNORM, shapes[i % TARGETS].width, shapes[i % TARGETS].height,
			i < TARGETS ? source : target, &scene->images[i], &scene->memories[i], &scene->views[i]);
	if (result == VK_SUCCESS)
		result = harness_image(harness, VK_FORMAT_R8G8B8A8_UNORM, SHOWN_WIDTH, SHOWN_HEIGHT,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &scene->images[SHOWN],
			&scene->memories[SHOWN], &scene->views[SHOWN]);
	const VkAttachmentDescription attachment = {
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	};
	const VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	const VkSubpassDescription subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &color,
	};
	const VkRenderPassCreateInfo pass_info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &attachment,
		.subpassCount = 1,
		.pSubpasses = &subpass,
	};
	if (result == VK_SUCCESS)
		result = vkCreateRenderPass(harness->device, &pass_info, NULL, &scene->render_pass);
	for (uint32_t i = 0; i <= TARGETS && result == VK_SUCCESS; i++) {
		const VkFramebufferCreateInfo info = {
			.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			.renderPass = scene->render_pass,
			.attachmentCount = 1,
			.pAttachments = &scene->views[TARGETS + i],
			.width = i < TARGETS ? shapes[i].width : SHOWN_WIDTH,
			.height = i < TARGETS ? shapes[i].height : SHOWN_HEIGHT,
			.layers = 1,
		};
		result = vkCreateFramebuffer(harness->device, &info, NULL, &scene->framebuffers[i]);
	}
	return harness_ok("the images, render pass and framebuffers", result);
}

// One set for each source and each target, sampled nearest and clamped.
static VkResult build_descriptors(const Harness *harness, Scene *scene)
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
	const VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * TARGETS };
	const VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 2 * TARGETS,
		.poolSizeCount = 1,
		.pPoolSizes = &size,
	};
	if (result == VK_SUCCESS)
		result = vkCreateDescriptorPool(harness->device, &pool_info, NULL, &scene->pool);
	VkDescriptorSetLayout layouts[2 * TARGETS];
	for (uint32_t i = 0; i < 2 * TARGETS; i++)
		layouts[i] = scene->set_layout;
	const VkDescriptorSetAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = scene->pool,
		.descriptorSetCount = 2 * TARGETS,
		.pSetLayouts = layouts,
	};
	if (result == VK_SUCCESS)
		result = vkAllocateDescriptorSets(harness->device, &allocate_info, scene->sets);
	for (uint32_t i = 0; i < 2 * TARGETS && result == VK_SUCCESS; i++) {
		const VkDescriptorImageInfo image = { scene->sampler, scene->views[i],
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

static VkResult build_pipeline(const Harness *harness, Scene *scene)
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
		.dynamic_viewport = true,
	};
	if (result == VK_SUCCESS)
		result = harness_pipeline(harness, &spec, scene->render_pass, &scene->pipeline);
	return result;
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
	// Room for four targets of up to 20x20.
	static uint8_t staging[TARGETS * 20 * 20 * TEXEL_BYTES];
	for (uint32_t i = 0; i < TARGETS; i++)
		for (uint32_t y = 0; y < shapes[i].height; y++)
			for (uint32_t x = 0; x < shapes[i].width; x++)
				code(i, x, y, staging + texels_at(i) + (y * shapes[i].width + x) * TEXEL_BYTES);
	static float quads[QUADS][QUAD_VERTICES][VERTEX_FLOATS];
	put_quad(quads[0], -1.0f, -1.0f, 1.0f, 1.0f);
	for (uint32_t i = 0; i < TARGETS; i++) {
		const float left = (float)shapes[i].shown_x;
		const float right = left + (float)(shapes[i].width * SHOWN_SCALE);
		const float bottom = (float)(SHOWN_TOP + shapes[i].height * SHOWN_SCALE);
		put_quad(quads[1 + i], clip(left, SHOWN_WIDTH), clip(SHOWN_TOP, SHOWN_HEIGHT), clip(right, SHOWN_WIDTH),
			clip(bottom, SHOWN_HEIGHT));
	}
	bool ok = harness_ok("pipeline", build_pipeline(harness, scene)) &&
		harness_ok("descriptors", build_descriptors(harness, scene)) &&
		harness_ok("read-back buffer", build_readback(harness, scene)) &&
		harness_ok("vertex buffer",
			harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, quads, sizeof quads, &scene->vertices,
				&scene->vertex_memory)) &&
		harness_ok("staging buffer",
			harness_buffer(harness, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging, texels_at(TARGETS), &scene->staging,
				&scene->staging_memory));
	const VkCommandBufferAllocateInfo commands = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = harness->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	return ok &&
		harness_ok("vkAllocateCommandBuffers", vkAllocateCommandBuffers(harness->device, &commands, &scene->commands));
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
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL,
		0, NULL, 1, &barrier);
}

// A pass over the whole framebuffer, the viewport and scissor over it too.
static void begin_pass(const Scene *scene, uint32_t framebuffer, uint32_t width, uint32_t height, const uint8_t rgba[4])
{
	const VkClearValue clear = {
		.color = { .float32 = { rgba[0] / 255.0f, rgba[1] / 255.0f, rgba[2] / 255.0f, rgba[3] / 255.0f } }
	};
	const VkRect2D area = { { 0, 0 }, { width, height } };
	const VkRenderPassBeginInfo info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = scene->render_pass,
		.framebuffer = scene->framebuffers[framebuffer],
		.renderArea = area,
		.clearValueCount = 1,
		.pClearValues = &clear,
	};
	vkCmdBeginRenderPass(scene->commands, &info, VK_SUBPASS_CONTENTS_INLINE);
	const VkViewport viewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
	vkCmdSetViewport(scene->commands, 0, 1, &viewport);
	vkCmdSetScissor(scene->commands, 0, 1, &area);
	vkCmdBindPipeline(scene->commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipeline);
	const VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(scene->commands, 0, 1, &scene->vertices, &offset);
}

static void copy_out(const Scene *scene, uint32_t image, uint32_t width, uint32_t height, uint32_t at)
{
	const VkBufferImageCopy region = {
		.bufferOffset = at,
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { width, height, 1 },
	};
	vkCmdCopyImageToBuffer(
		scene->commands, scene->images[image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scene->readback, 1, &region);
}

// The sources copied in; each target cleared magenta, drawn from its source and read back; then the targets sampled
// into the shown attachment, cleared grey, and that read back.
static void record(const Scene *scene)
{
	const VkCommandBuffer commands = scene->commands;
	static const uint8_t magenta[4] = { 255, 0, 255, 255 };
	for (uint32_t i = 0; i < TARGETS; i++) {
		transition(commands, scene->images[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		const VkBufferImageCopy region = {
			.bufferOffset = texels_at(i),
			.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.imageExtent = { shapes[i].width, shapes[i].height, 1 },
		};
		vkCmdCopyBufferToImage(
			commands, scene->staging, scene->images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		transition(
			commands, scene->images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
	for (uint32_t i = 0; i < TARGETS; i++) {
		begin_pass(scene, i, shapes[i].width, shapes[i].height, magenta);
		vkCmdBindDescriptorSets(
			commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->layout, 0, 1, &scene->sets[i], 0, NULL);
		vkCmdDraw(commands, QUAD_VERTICES, 1, 0, 0);
		vkCmdEndRenderPass(commands);
		copy_out(scene, TARGETS + i, shapes[i].width, shapes[i].height, texels_at(i));
		transition(commands, scene->images[TARGETS + i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
	begin_pass(scene, TARGETS, SHOWN_WIDTH, SHOWN_HEIGHT, grey);
	for (uint32_t i = 0; i < TARGETS; i++) {
		vkCmdBindDescriptorSets(
			commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->layout, 0, 1, &scene->sets[TARGETS + i], 0, NULL);
		vkCmdDraw(commands, QUAD_VERTICES, 1, (1 + i) * QUAD_VERTICES, 0);
	}
	vkCmdEndRenderPass(commands);
	copy_out(scene, SHOWN, SHOWN_WIDTH, SHOWN_HEIGHT, texels_at(TARGETS));
}

// Records it all into one command buffer and waits for it.
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

static bool near(const uint8_t *texel, const uint8_t want[4])
{
	for (uint32_t c = 0; c < 4; c++)
		if ((texel[c] > want[c] ? texel[c] - want[c] : want[c] - texel[c]) > TOLERANCE)
			return false;
	return true;
}

static void log_miss(const char *what, uint32_t x, uint32_t y, const uint8_t *texel, const uint8_t want[4])
{
	xsdk_log_write("small targets: %s: (%u, %u) is %u %u %u %u, not %u %u %u %u", what, (unsigned)x, (unsigned)y,
		texel[0], texel[1], texel[2], texel[3], want[0], want[1], want[2], want[3]);
}

// Each target's texels as read back, against its source's.
static uint32_t read_back_wrong(const Scene *scene)
{
	uint32_t total = 0;
	for (uint32_t i = 0; i < TARGETS; i++) {
		uint32_t wrong = 0;
		for (uint32_t y = 0; y < shapes[i].height; y++) {
			for (uint32_t x = 0; x < shapes[i].width; x++) {
				const uint8_t *const texel = scene->read + texels_at(i) + (y * shapes[i].width + x) * TEXEL_BYTES;
				uint8_t want[4];
				code(i, x, y, want);
				if (near(texel, want))
					continue;
				if (wrong++ < MAX_MISSES_LOGGED)
					log_miss(shapes[i].name, x, y, texel, want);
			}
		}
		xsdk_log_write("small targets: %s read back: %u of %u texels wrong", shapes[i].name, (unsigned)wrong,
			(unsigned)(shapes[i].width * shapes[i].height));
		total += wrong;
	}
	return total;
}

// What the shown attachment holds at a pixel: a target's texel, 2 by 2 pixels each, or the grey around them.
static void shown_want(uint32_t x, uint32_t y, uint8_t want[4])
{
	memcpy(want, grey, sizeof grey);
	for (uint32_t i = 0; i < TARGETS; i++) {
		const Shape *const shape = &shapes[i];
		if (x >= shape->shown_x && x < shape->shown_x + shape->width * SHOWN_SCALE && y >= SHOWN_TOP &&
			y < SHOWN_TOP + shape->height * SHOWN_SCALE)
			code(i, (x - shape->shown_x) / SHOWN_SCALE, (y - SHOWN_TOP) / SHOWN_SCALE, want);
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
			if (near(texel, want))
				continue;
			if (wrong++ < MAX_MISSES_LOGGED)
				log_miss("sampled", x, y, texel, want);
		}
	}
	xsdk_log_write(
		"small targets: sampled: %u of %u pixels wrong", (unsigned)wrong, (unsigned)(SHOWN_WIDTH * SHOWN_HEIGHT));
	return wrong;
}

static void destroy(const Harness *harness, const Scene *scene)
{
	const VkDevice device = harness->device;
	if (scene->commands)
		vkFreeCommandBuffers(device, harness->pool, 1, &scene->commands);
	vkDestroyBuffer(device, scene->readback, NULL);
	vkFreeMemory(device, scene->readback_memory, NULL);
	vkDestroyBuffer(device, scene->staging, NULL);
	vkFreeMemory(device, scene->staging_memory, NULL);
	vkDestroyBuffer(device, scene->vertices, NULL);
	vkFreeMemory(device, scene->vertex_memory, NULL);
	vkDestroyDescriptorPool(device, scene->pool, NULL);
	vkDestroySampler(device, scene->sampler, NULL);
	vkDestroyPipeline(device, scene->pipeline, NULL);
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	vkDestroyDescriptorSetLayout(device, scene->set_layout, NULL);
	for (uint32_t i = 0; i <= TARGETS; i++)
		vkDestroyFramebuffer(device, scene->framebuffers[i], NULL);
	vkDestroyRenderPass(device, scene->render_pass, NULL);
	for (uint32_t i = 0; i < IMAGES; i++) {
		vkDestroyImageView(device, scene->views[i], NULL);
		vkDestroyImage(device, scene->images[i], NULL);
		vkFreeMemory(device, scene->memories[i], NULL);
	}
}

void test_small_targets(const Harness *harness)
{
	static Scene scene;
	memset(&scene, 0, sizeof scene);
	const bool ok = build_images(harness, &scene) && build_drawing(harness, &scene);
	check("small targets: 20x11, 11x20, 16x16 and 2x1 colour attachments, their sources and passes made", ok);
	const bool ran = ok && run(harness, &scene);
	check(
		"small targets: each drawn from a source of its own, read back by vkCmdCopyImageToBuffer, every texel its "
		"own",
		ran && read_back_wrong(&scene) == 0);
	check(
		"small targets: each sampled 2 by 2 pixels a texel into a 128x64 attachment, read back, every pixel its texel",
		ran && shown_wrong(&scene) == 0);
	destroy(harness, &scene);
}
