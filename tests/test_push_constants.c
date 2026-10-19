#include <stdbool.h>
#include <stdint.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"

enum {
	FRAMES = 60,
	QUADS = 4,
	// The quad pushed in pieces (push_quad).
	PIECES_QUAD = 3,
	VEC4_BYTES = 16,
	RED_GREEN_BYTES = 8,
	QUAD_VERTICES = 6,
	STRIDE_BYTES = 16,
	// push.vert reads scale and offset from bytes 0 to 31, push.frag the colour from 32 to 47.
	VERTEX_RANGE_BYTES = 32,
	FRAGMENT_OFFSET_BYTES = 32,
	FRAGMENT_RANGE_BYTES = 16,
	TOLERANCE = 1,
};

// A unit quad, which each draw's push constants scale and move.
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

typedef struct {
	float scale[4];
	float offset[4];
	float color[4];
	Rgb rgb;
} Quad;

// Each 0.4 by 0.5 of clip space, 256 by 180 pixels, in a colour of its own.
static const Quad quads[QUADS] = {
	{ { 0.4f, 0.5f, 1.0f, 1.0f }, { -0.9f, -0.6f, 0.0f, 0.0f }, { 1.0f, 128.0f / 255, 0.0f, 1.0f }, { 255, 128, 0 } },
	{ { 0.4f, 0.5f, 1.0f, 1.0f }, { -0.2f, -0.1f, 0.0f, 0.0f }, { 0.0f, 192.0f / 255, 192.0f / 255, 1.0f },
		{ 0, 192, 192 } },
	{ { 0.4f, 0.5f, 1.0f, 1.0f }, { 0.5f, -0.4f, 0.0f, 0.0f }, { 1.0f, 64.0f / 255, 192.0f / 255, 1.0f },
		{ 255, 64, 192 } },
	// Its blue is the quad before's, as only its red and green are pushed.
	{ { 0.4f, 0.5f, 1.0f, 1.0f }, { -0.2f, 0.45f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f, 0.0f }, { 0, 255, 192 } },
};

// A dark green: 16, 40, 16.
static const VkClearValue clear = { .color = { .float32 = { 16.0f / 255, 40.0f / 255, 16.0f / 255, 1.0f } } };
static const Rgb clear_rgb = { 16, 40, 16 };

typedef struct {
	VkRenderPass render_pass;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES];
	VkPipelineLayout layout;
	VkPipeline pipeline;
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	HarnessProbe probes[QUADS + 1];
	HarnessProbes check;
} Scene;

// Clip x and y in -1 to 1 as pixels, y down.
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
		const Quad *const q = &quads[i];
		scene->probes[i] = (HarnessProbe){ "a quad's centre", pixel_x(q->offset[0] + q->scale[0] / 2),
			pixel_y(q->offset[1] + q->scale[1] / 2), q->rgb, 0 };
	}
	scene->probes[QUADS] = (HarnessProbe){ "the clear colour", 20, 20, clear_rgb, 0 };
	scene->check = (HarnessProbes){ "push constants", scene->probes, QUADS + 1, TOLERANCE, 0 };
}

static bool build(const Harness *harness, Scene *scene)
{
	const VkPushConstantRange ranges[] = {
		{ VK_SHADER_STAGE_VERTEX_BIT, 0, VERTEX_RANGE_BYTES },
		{ VK_SHADER_STAGE_FRAGMENT_BIT, FRAGMENT_OFFSET_BYTES, FRAGMENT_RANGE_BYTES },
	};
	const VkPipelineLayoutCreateInfo layout_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.pushConstantRangeCount = 2,
		.pPushConstantRanges = ranges,
	};
	bool ok = harness_ok(
		"vkCreatePipelineLayout", vkCreatePipelineLayout(harness->device, &layout_info, NULL, &scene->layout));
	const HarnessPipeline spec = {
		.vertex_spirv = push_vert_spirv,
		.vertex_bytes = PUSH_VERT_SPIRV_BYTES,
		.fragment_spirv = push_frag_spirv,
		.fragment_bytes = PUSH_FRAG_SPIRV_BYTES,
		.bindings = &binding,
		.binding_count = 1,
		.attributes = &attribute,
		.attribute_count = 1,
		.layout = scene->layout,
	};
	ok = ok && harness_ok("render pass", harness_render_pass(harness, &scene->render_pass, scene->framebuffers));
	ok = ok && harness_ok("pipeline", harness_pipeline(harness, &spec, scene->render_pass, &scene->pipeline));
	return ok &&
		harness_ok("vertex buffer",
			harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, quad, sizeof quad, &scene->vertices,
				&scene->vertex_memory));
}

// A quad pushes its vertex range, then its fragment range. The last pushes its offset then its scale, a range from two
// pushes recorded apart, and its colour's red and green alone, part of 16 bytes: both of which the layer copies
// (docs/SUPPORT.md, Pipelines).
static void push_quad(VkCommandBuffer commands, VkPipelineLayout layout, const Quad *quad, bool in_pieces)
{
	if (!in_pieces) {
		vkCmdPushConstants(commands, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, VERTEX_RANGE_BYTES, quad);
		vkCmdPushConstants(
			commands, layout, VK_SHADER_STAGE_FRAGMENT_BIT, FRAGMENT_OFFSET_BYTES, FRAGMENT_RANGE_BYTES, quad->color);
		return;
	}
	vkCmdPushConstants(commands, layout, VK_SHADER_STAGE_VERTEX_BIT, VEC4_BYTES, VEC4_BYTES, quad->offset);
	vkCmdPushConstants(commands, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, VEC4_BYTES, quad->scale);
	vkCmdPushConstants(
		commands, layout, VK_SHADER_STAGE_FRAGMENT_BIT, FRAGMENT_OFFSET_BYTES, RED_GREEN_BYTES, quad->color);
}

// Each quad pushes, then draws.
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
		for (uint32_t q = 0; q < QUADS; q++) {
			push_quad(commands, scene->layout, &quads[q], q == PIECES_QUAD);
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
	vkDestroyBuffer(device, scene->vertices, NULL);
	vkFreeMemory(device, scene->vertex_memory, NULL);
	vkDestroyPipeline(device, scene->pipeline, NULL);
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	harness_destroy_render_pass(harness, scene->render_pass, scene->framebuffers);
}

void test_push_constants(const Harness *harness)
{
	Scene scene = { .render_pass = VK_NULL_HANDLE };
	make_probes(&scene);
	const bool built = build(harness, &scene);
	check("push constants: a pipeline with a vertex and a fragment range made", built);
	const bool recorded = built && harness->swapchain && harness_ok("recording", record(harness, &scene));
	uint32_t misses = 0;
	const uint32_t frames =
		recorded ? harness_frames(harness, scene.commands, FRAMES, harness_probes_hold, &scene.check, &misses) : 0;
	check("push constants: 60 frames drawn and presented", frames == FRAMES);
	check("push constants: every frame places and colours each quad by its pushes", frames == FRAMES && misses == 0);
	check("push constants: the image on screen shows them too",
		harness_screen(harness, harness_probes_hold, &scene.check));
	destroy(harness, &scene);
}
