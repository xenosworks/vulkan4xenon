#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"

enum {
	FRAMES = 60,
	QUADS = 4,
	// The quads, the clear colour and the cleared corner.
	PROBES = QUADS + 2,
	QUAD_VERTICES = 6,
	STRIDE_BYTES = 16,
	// Each draw's block sits 256 bytes on, minUniformBufferOffsetAlignment.
	DRAW_STRIDE_BYTES = 256,
	DRAW_BYTES = 80,
	TINT_BYTES = 16,
	TOLERANCE = 1,
};

static const float quad[QUAD_VERTICES][4] = {
	{ 0.0f, 0.0f, 0.0f, 1.0f },
	{ 1.0f, 0.0f, 0.0f, 1.0f },
	{ 0.0f, 1.0f, 0.0f, 1.0f },
	{ 1.0f, 0.0f, 0.0f, 1.0f },
	{ 1.0f, 1.0f, 0.0f, 1.0f },
	{ 0.0f, 1.0f, 0.0f, 1.0f },
};

static const VkVertexInputBindingDescription binding = { 0, STRIDE_BYTES, VK_VERTEX_INPUT_RATE_VERTEX };
static const VkVertexInputAttributeDescription attribute = {
	.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0
};

// A matrix scales and moves the unit quad; the colour is tinted by set 1's block, which halves green.
typedef struct {
	float scale_x;
	float scale_y;
	float move_x;
	float move_y;
	Rgb color;
	Rgb shown;
} Draw;

static const Draw draws[QUADS] = {
	{ 0.3f, 0.4f, -0.8f, -0.7f, { 200, 100, 40 }, { 200, 50, 40 } },
	{ 0.5f, 0.3f, -0.2f, 0.0f, { 40, 200, 160 }, { 40, 100, 160 } },
	{ 0.25f, 0.5f, 0.45f, -0.3f, { 120, 80, 220 }, { 120, 40, 220 } },
	// After the clear.
	{ 0.3f, 0.3f, 0.4f, 0.4f, { 200, 160, 40 }, { 200, 80, 40 } },
};

static const float tint[4] = { 1.0f, 0.5f, 1.0f, 1.0f };

// A dark purple: 40, 16, 40.
static const VkClearValue clear = { .color = { .float32 = { 40.0f / 255, 16.0f / 255, 40.0f / 255, 1.0f } } };
static const Rgb clear_rgb = { 40, 16, 40 };

// vkCmdClearAttachments' blue corner, its colour set in the pixel shader's register 0 (src/blit.c), where uniform.frag
// loads its tint: the last quad, tinted by that register again, would show 0, 0, 40 had its tint not been loaded again.
static const VkClearAttachment corner = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	.colorAttachment = 0,
	.clearValue = { .color = { .float32 = { 0.0f, 0.0f, 1.0f, 1.0f } } } };
static const VkClearRect corner_rect = { { { 1152, 24 }, { 96, 64 } }, 0, 1 };
static const Rgb corner_rgb = { 0, 0, 255 };

typedef struct {
	VkRenderPass render_pass;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES];
	VkDescriptorSetLayout set_layouts[2];
	VkPipelineLayout layout;
	VkPipeline pipeline;
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkBuffer uniforms;
	VkDeviceMemory uniform_memory;
	VkDescriptorPool pool;
	VkDescriptorSet sets[2];
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	HarnessProbe probes[PROBES];
	HarnessProbes check;
} Scene;

static uint32_t pixel_x(float x)
{
	return (uint32_t)((x + 1.0f) / 2.0f * HARNESS_WIDTH);
}

static uint32_t pixel_y(float y)
{
	return (uint32_t)((y + 1.0f) / 2.0f * HARNESS_HEIGHT);
}

static void make_probes(Scene *scene)
{
	for (uint32_t i = 0; i < QUADS; i++) {
		const Draw *const d = &draws[i];
		scene->probes[i] = (HarnessProbe){ "a quad's centre", pixel_x(d->move_x + d->scale_x / 2),
			pixel_y(d->move_y + d->scale_y / 2), d->shown, 0 };
	}
	scene->probes[QUADS - 1].name = "the quad after vkCmdClearAttachments' centre";
	scene->probes[QUADS] = (HarnessProbe){ "the clear colour", 20, 20, clear_rgb, 0 };
	scene->probes[QUADS + 1] = (HarnessProbe){ "the cleared corner", 1200, 56, corner_rgb, 0 };
	scene->check = (HarnessProbes){ "uniform buffers", scene->probes, PROBES, TOLERANCE, 0 };
}

// Each draw's block, a column-major matrix then a colour, 256 bytes apart; the tint after them.
static void fill_uniforms(uint8_t *bytes, uint32_t size_bytes)
{
	memset(bytes, 0, size_bytes);
	for (uint32_t i = 0; i < QUADS; i++) {
		const Draw *const d = &draws[i];
		const float block[20] = {
			d->scale_x,
			0.0f,
			0.0f,
			0.0f,
			0.0f,
			d->scale_y,
			0.0f,
			0.0f,
			0.0f,
			0.0f,
			1.0f,
			0.0f,
			d->move_x,
			d->move_y,
			0.0f,
			1.0f,
			d->color.red / 255.0f,
			d->color.green / 255.0f,
			d->color.blue / 255.0f,
			1.0f,
		};
		memcpy(bytes + i * DRAW_STRIDE_BYTES, block, sizeof block);
	}
	memcpy(bytes + QUADS * DRAW_STRIDE_BYTES, tint, sizeof tint);
}

static VkResult create_layouts(const Harness *harness, Scene *scene)
{
	const VkDescriptorSetLayoutBinding bindings[2] = {
		{ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL },
		{ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
	};
	VkResult result = VK_SUCCESS;
	for (uint32_t i = 0; i < 2 && result == VK_SUCCESS; i++) {
		const VkDescriptorSetLayoutCreateInfo info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = 1,
			.pBindings = &bindings[i],
		};
		result = vkCreateDescriptorSetLayout(harness->device, &info, NULL, &scene->set_layouts[i]);
	}
	const VkPipelineLayoutCreateInfo layout_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 2,
		.pSetLayouts = scene->set_layouts,
	};
	if (result == VK_SUCCESS)
		result = vkCreatePipelineLayout(harness->device, &layout_info, NULL, &scene->layout);
	return result;
}

static VkResult create_sets(const Harness *harness, Scene *scene)
{
	const VkDescriptorPoolSize sizes[2] = {
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 },
	};
	const VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 2,
		.poolSizeCount = 2,
		.pPoolSizes = sizes,
	};
	VkResult result = vkCreateDescriptorPool(harness->device, &pool_info, NULL, &scene->pool);
	const VkDescriptorSetAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = scene->pool,
		.descriptorSetCount = 2,
		.pSetLayouts = scene->set_layouts,
	};
	if (result == VK_SUCCESS)
		result = vkAllocateDescriptorSets(harness->device, &allocate_info, scene->sets);
	if (result != VK_SUCCESS)
		return result;
	const VkDescriptorBufferInfo buffers[2] = {
		{ scene->uniforms, 0, DRAW_BYTES },
		{ scene->uniforms, QUADS * DRAW_STRIDE_BYTES, TINT_BYTES },
	};
	const VkWriteDescriptorSet writes[2] = {
		{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = scene->sets[0],
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
			.pBufferInfo = &buffers[0] },
		{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = scene->sets[1],
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.pBufferInfo = &buffers[1] },
	};
	vkUpdateDescriptorSets(harness->device, 2, writes, 0, NULL);
	return VK_SUCCESS;
}

static bool build(const Harness *harness, Scene *scene)
{
	uint8_t uniforms[QUADS * DRAW_STRIDE_BYTES + TINT_BYTES];
	fill_uniforms(uniforms, sizeof uniforms);
	bool ok = harness_ok("layouts", create_layouts(harness, scene));
	const HarnessPipeline spec = {
		.vertex_spirv = uniform_vert_spirv,
		.vertex_bytes = UNIFORM_VERT_SPIRV_BYTES,
		.fragment_spirv = uniform_frag_spirv,
		.fragment_bytes = UNIFORM_FRAG_SPIRV_BYTES,
		.bindings = &binding,
		.binding_count = 1,
		.attributes = &attribute,
		.attribute_count = 1,
		.layout = scene->layout,
	};
	ok = ok && harness_ok("render pass", harness_render_pass(harness, &scene->render_pass, scene->framebuffers));
	ok = ok && harness_ok("pipeline", harness_pipeline(harness, &spec, scene->render_pass, &scene->pipeline));
	ok = ok &&
		harness_ok("vertex buffer",
			harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, quad, sizeof quad, &scene->vertices,
				&scene->vertex_memory));
	ok = ok &&
		harness_ok("uniform buffer",
			harness_buffer(harness, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, uniforms, sizeof uniforms, &scene->uniforms,
				&scene->uniform_memory));
	return ok && harness_ok("descriptor sets", create_sets(harness, scene));
}

// Set 1 once; set 0 again for each draw, at that draw's dynamic offset; the corner cleared before the last.
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
		vkCmdBindDescriptorSets(
			commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->layout, 1, 1, &scene->sets[1], 0, NULL);
		for (uint32_t q = 0; q < QUADS; q++) {
			if (q == QUADS - 1)
				vkCmdClearAttachments(commands, 1, &corner, 1, &corner_rect);
			const uint32_t dynamic_offset = q * DRAW_STRIDE_BYTES;
			vkCmdBindDescriptorSets(
				commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->layout, 0, 1, &scene->sets[0], 1, &dynamic_offset);
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
	vkDestroyBuffer(device, scene->uniforms, NULL);
	vkFreeMemory(device, scene->uniform_memory, NULL);
	vkDestroyBuffer(device, scene->vertices, NULL);
	vkFreeMemory(device, scene->vertex_memory, NULL);
	vkDestroyPipeline(device, scene->pipeline, NULL);
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	for (uint32_t i = 0; i < 2; i++)
		vkDestroyDescriptorSetLayout(device, scene->set_layouts[i], NULL);
	harness_destroy_render_pass(harness, scene->render_pass, scene->framebuffers);
}

void test_uniform_buffers(const Harness *harness)
{
	Scene scene = { .render_pass = VK_NULL_HANDLE };
	make_probes(&scene);
	const bool built = build(harness, &scene);
	check("uniform buffers: two set layouts, a pipeline, buffers and descriptor sets made", built);
	const bool recorded = built && harness->swapchain && harness_ok("recording", record(harness, &scene));
	uint32_t misses = 0;
	const uint32_t frames =
		recorded ? harness_frames(harness, scene.commands, FRAMES, harness_probes_hold, &scene.check, &misses) : 0;
	check("uniform buffers: 60 frames drawn and presented", frames == FRAMES);
	check("uniform buffers: every frame places each quad by its matrix, coloured and tinted, the last after a clear",
		frames == FRAMES && misses == 0);
	check("uniform buffers: the image on screen shows them too",
		harness_screen(harness, harness_probes_hold, &scene.check));
	destroy(harness, &scene);
}
