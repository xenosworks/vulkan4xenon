#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "vulkan4xenon.h"
#include "xsdk/log.h"

// Quads from gl_VertexIndex alone, as PPSSPP's stencil upload draws them: vertex_index.vert puts vertex i at corner
// i & 3 of cell i >> 2, so a quad lands in its cell only where gl_VertexIndex counts the draw's first vertex, or an
// indexed draw's vertex offset, as Vulkan's does. A row from the bundle's shader, a row from the same shader compiled
// on the console, as PPSSPP's are, and a row of indexed lists through its instanced variant, drawn into a 160x64
// attachment read back, every pixel its quad's colour or the clear.

enum {
	WIDTH = 160,
	HEIGHT = 64,
	TEXEL_BYTES = 4,
	CELLS = 7,
	CELL_PITCH = 20,
	QUAD = 16,
	ORIGIN_X = 4,
	TOLERANCE = 2,
	FENCE_TIMEOUT_NS = 1000000000,
	// The memory type the CPU caches, which buffers only copied to and from may take.
	CACHED_TYPE = 1,
	MAX_MISSES_LOGGED = 4,
	// A shader the bundle doesn't hold: its SPIR-V's generator word, its third, changed (test_triangle.c).
	GENERATOR_WORD = 2,
	GENERATOR_CHANGE = 0x00010000,
	// A quad is 4 vertices of a strip, or 6 indices of a list.
	STRIP_VERTICES = 4,
	LIST_INDICES = 6,
	// Where the strips' second quad's indices and the list's start.
	SECOND_QUAD_AT = 4,
	LIST_AT = 8,
};

typedef enum {
	ROW_BUNDLE,
	ROW_COMPILED,
	ROW_INSTANCED,
	ROWS,
} Row;

static const uint32_t row_y[ROWS] = { 4, 24, 44 };
static const char *const row_names[ROWS] = { "the bundle's shader", "compiled on the console",
	"the instanced variant" };

// Strips' indices, one quad's then the next's, and a list's of one quad.
static const uint16_t indices[] = { 0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 2, 1, 3 };

typedef struct {
	uint32_t cell;
	uint8_t rgba[4];
	// vkCmdDraw's first vertex, or an indexed draw's first index and vertex offset.
	bool indexed;
	uint32_t first;
	int32_t vertex_offset;
	uint32_t instances;
} Quad;

// The strips' rows: from vertex 0, from vertex 8, and indexed from vertex offsets 16 and 20, the latter's indices
// from 4.
static const Quad strip_quads[] = {
	{ 0, { 224, 64, 64, 255 }, false, 0, 0, 1 },
	{ 2, { 64, 224, 64, 255 }, false, 8, 0, 1 },
	{ 4, { 64, 96, 240, 255 }, true, 0, 16, 1 },
	{ 6, { 240, 224, 64, 255 }, true, SECOND_QUAD_AT, 20, 1 },
};
// The instanced row: lists of two instances from vertex offset 4, and of one from 12.
static const Quad list_quads[] = {
	{ 1, { 224, 64, 224, 255 }, true, LIST_AT, 4, 2 },
	{ 3, { 64, 224, 224, 255 }, true, LIST_AT, 12, 1 },
};

static const uint8_t clear_rgba[4] = { 16, 24, 40, 255 };

static uint32_t compiled_spirv[VERTEX_INDEX_VERT_SPIRV_BYTES / 4];

typedef struct {
	VkImage image;
	VkDeviceMemory image_memory;
	VkImageView view;
	VkRenderPass render_pass;
	VkFramebuffer framebuffer;
	VkPipelineLayout layout;
	VkPipeline pipelines[ROWS];
	VkBuffer indices;
	VkDeviceMemory index_memory;
	// The instanced pipeline's binding at the instance's rate, which no attribute reads.
	VkBuffer instance;
	VkDeviceMemory instance_memory;
	VkBuffer readback;
	VkDeviceMemory readback_memory;
	const uint8_t *read;
	VkCommandBuffer commands;
	uint32_t compiled_here;
} Scene;

static uint32_t quad_count(Row row)
{
	return row == ROW_INSTANCED ? sizeof list_quads / sizeof *list_quads : sizeof strip_quads / sizeof *strip_quads;
}

static const Quad *row_quads(Row row)
{
	return row == ROW_INSTANCED ? list_quads : strip_quads;
}

static VkResult build_target(const Harness *harness, Scene *scene)
{
	VkResult result = harness_image(harness, VK_FORMAT_R8G8B8A8_UNORM, WIDTH, HEIGHT,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &scene->image, &scene->image_memory,
		&scene->view);
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
	const VkFramebufferCreateInfo framebuffer_info = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = scene->render_pass,
		.attachmentCount = 1,
		.pAttachments = &scene->view,
		.width = WIDTH,
		.height = HEIGHT,
		.layers = 1,
	};
	if (result == VK_SUCCESS)
		result = vkCreateFramebuffer(harness->device, &framebuffer_info, NULL, &scene->framebuffer);
	return result;
}

static VkResult create_pipeline(const Harness *harness, Scene *scene, Row row)
{
	const VkPipelineInputAssemblyStateCreateInfo assembly = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = row == ROW_INSTANCED ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
	};
	static const VkVertexInputBindingDescription instance_binding = { 0, 16, VK_VERTEX_INPUT_RATE_INSTANCE };
	const HarnessPipeline spec = {
		.vertex_spirv = row == ROW_COMPILED ? compiled_spirv : vertex_index_vert_spirv,
		.vertex_bytes = VERTEX_INDEX_VERT_SPIRV_BYTES,
		.fragment_spirv = push_frag_spirv,
		.fragment_bytes = PUSH_FRAG_SPIRV_BYTES,
		.bindings = row == ROW_INSTANCED ? &instance_binding : NULL,
		.binding_count = row == ROW_INSTANCED,
		.layout = scene->layout,
		.assembly = &assembly,
		.dynamic_viewport = true,
	};
	return harness_pipeline(harness, &spec, scene->render_pass, &scene->pipelines[row]);
}

// The read-back buffer, in memory the CPU caches.
static VkResult build_readback(const Harness *harness, Scene *scene)
{
	const VkBufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = WIDTH * HEIGHT * TEXEL_BYTES,
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

static bool build(const Harness *harness, Scene *scene)
{
	memcpy(compiled_spirv, vertex_index_vert_spirv, sizeof compiled_spirv);
	compiled_spirv[GENERATOR_WORD] ^= GENERATOR_CHANGE;
	bool ok = harness_ok("target", build_target(harness, scene)) &&
		harness_ok("push layout", harness_push_layout(harness, &scene->layout)) &&
		harness_ok("bundle pipeline", create_pipeline(harness, scene, ROW_BUNDLE));
	const uint32_t compiled = v4x_shader_counts().compiled;
	ok = ok && harness_ok("compiled pipeline", create_pipeline(harness, scene, ROW_COMPILED));
	scene->compiled_here = v4x_shader_counts().compiled - compiled;
	const float instance[4] = { 0 };
	ok = ok && harness_ok("instanced pipeline", create_pipeline(harness, scene, ROW_INSTANCED)) &&
		harness_ok("index buffer",
			harness_buffer(harness, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices, sizeof indices, &scene->indices,
				&scene->index_memory)) &&
		harness_ok("instance buffer",
			harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, instance, sizeof instance, &scene->instance,
				&scene->instance_memory)) &&
		harness_ok("read-back buffer", build_readback(harness, scene));
	const VkCommandBufferAllocateInfo commands = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = harness->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	return ok &&
		harness_ok("vkAllocateCommandBuffers", vkAllocateCommandBuffers(harness->device, &commands, &scene->commands));
}

// Places the row's cell 0, the step to the next and the quad's size, and colours the quad.
static void place(VkCommandBuffer commands, VkPipelineLayout layout, Row row, const uint8_t rgba[4])
{
	const float half_width = WIDTH / 2.0f;
	const float half_height = HEIGHT / 2.0f;
	const float origin_and_step[8] = { ORIGIN_X / half_width - 1.0f, row_y[row] / half_height - 1.0f, 0.0f, 0.0f,
		CELL_PITCH / half_width, 0.0f, QUAD / half_width, QUAD / half_height };
	const float color[4] = { rgba[0] / 255.0f, rgba[1] / 255.0f, rgba[2] / 255.0f, rgba[3] / 255.0f };
	vkCmdPushConstants(commands, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof origin_and_step, origin_and_step);
	vkCmdPushConstants(commands, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 32, sizeof color, color);
}

static void draw_row(const Scene *scene, Row row)
{
	const VkCommandBuffer commands = scene->commands;
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipelines[row]);
	for (uint32_t i = 0; i < quad_count(row); i++) {
		const Quad *const quad = &row_quads(row)[i];
		place(commands, scene->layout, row, quad->rgba);
		if (!quad->indexed)
			vkCmdDraw(commands, STRIP_VERTICES, quad->instances, quad->first, 0);
		else
			vkCmdDrawIndexed(commands, row == ROW_INSTANCED ? LIST_INDICES : STRIP_VERTICES, quad->instances,
				quad->first, quad->vertex_offset, 0);
	}
}

// The rows drawn in one pass, then the attachment read back; recorded into one command buffer and waited for.
static bool run(const Harness *harness, const Scene *scene)
{
	const VkCommandBuffer commands = scene->commands;
	const VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	if (!harness_ok("vkBeginCommandBuffer", vkBeginCommandBuffer(commands, &begin)))
		return false;
	const VkClearValue clear = { .color = { .float32 = { clear_rgba[0] / 255.0f, clear_rgba[1] / 255.0f,
												clear_rgba[2] / 255.0f, clear_rgba[3] / 255.0f } } };
	const VkRect2D area = { { 0, 0 }, { WIDTH, HEIGHT } };
	const VkRenderPassBeginInfo pass = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = scene->render_pass,
		.framebuffer = scene->framebuffer,
		.renderArea = area,
		.clearValueCount = 1,
		.pClearValues = &clear,
	};
	vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
	const VkViewport viewport = { 0.0f, 0.0f, (float)WIDTH, (float)HEIGHT, 0.0f, 1.0f };
	vkCmdSetViewport(commands, 0, 1, &viewport);
	vkCmdSetScissor(commands, 0, 1, &area);
	const VkDeviceSize offset = 0;
	vkCmdBindIndexBuffer(commands, scene->indices, 0, VK_INDEX_TYPE_UINT16);
	vkCmdBindVertexBuffers(commands, 0, 1, &scene->instance, &offset);
	for (uint32_t row = 0; row < ROWS; row++)
		draw_row(scene, (Row)row);
	vkCmdEndRenderPass(commands);
	const VkBufferImageCopy region = {
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { WIDTH, HEIGHT, 1 },
	};
	vkCmdCopyImageToBuffer(commands, scene->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scene->readback, 1, &region);
	const VkSubmitInfo info = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &scene->commands,
	};
	const bool done = harness_ok("vkEndCommandBuffer", vkEndCommandBuffer(commands)) &&
		harness_ok("vkResetFences", vkResetFences(harness->device, 1, &harness->fence)) &&
		harness_ok("vkQueueSubmit", vkQueueSubmit(harness->queue, 1, &info, harness->fence)) &&
		harness_ok("vkWaitForFences", vkWaitForFences(harness->device, 1, &harness->fence, VK_TRUE, FENCE_TIMEOUT_NS));
	(void)vkResetFences(harness->device, 1, &harness->fence);
	return done;
}

// What a pixel should hold: the colour of the quad over it, or the clear; and the row it's in, ROWS for none.
static const uint8_t *wanted(uint32_t x, uint32_t y, uint32_t *row_of)
{
	*row_of = ROWS;
	for (uint32_t row = 0; row < ROWS; row++) {
		if (y < row_y[row] || y >= row_y[row] + QUAD)
			continue;
		*row_of = row;
		for (uint32_t i = 0; i < quad_count((Row)row); i++) {
			const Quad *const quad = &row_quads((Row)row)[i];
			const uint32_t left = ORIGIN_X + quad->cell * CELL_PITCH;
			if (x >= left && x < left + QUAD)
				return quad->rgba;
		}
	}
	return clear_rgba;
}

static uint32_t difference(uint32_t a, uint32_t b)
{
	return a > b ? a - b : b - a;
}

// Every pixel read back against what it should hold, the wrong ones counted by row and the first few logged.
static bool pixels_right(const Scene *scene)
{
	uint32_t wrong[ROWS + 1] = { 0 };
	uint32_t logged = 0;
	for (uint32_t y = 0; y < HEIGHT; y++) {
		for (uint32_t x = 0; x < WIDTH; x++) {
			uint32_t row;
			const uint8_t *const want = wanted(x, y, &row);
			const uint8_t *const got = scene->read + (y * WIDTH + x) * TEXEL_BYTES;
			bool right = true;
			for (uint32_t c = 0; c < TEXEL_BYTES; c++)
				right = right && difference(got[c], want[c]) <= TOLERANCE;
			if (right)
				continue;
			wrong[row]++;
			if (logged++ < MAX_MISSES_LOGGED)
				xsdk_log_write("vertex index: (%u, %u) is %u %u %u %u, not %u %u %u %u", (unsigned)x, (unsigned)y,
					(unsigned)got[0], (unsigned)got[1], (unsigned)got[2], (unsigned)got[3], (unsigned)want[0],
					(unsigned)want[1], (unsigned)want[2], (unsigned)want[3]);
		}
	}
	for (uint32_t row = 0; row < ROWS; row++)
		xsdk_log_write("vertex index: %s's row, %u pixels wrong", row_names[row], (unsigned)wrong[row]);
	xsdk_log_write("vertex index: between the rows, %u pixels wrong", (unsigned)wrong[ROWS]);
	return wrong[ROW_BUNDLE] + wrong[ROW_COMPILED] + wrong[ROW_INSTANCED] + wrong[ROWS] == 0;
}

static void destroy(const Harness *harness, const Scene *scene)
{
	const VkDevice device = harness->device;
	if (scene->commands)
		vkFreeCommandBuffers(device, harness->pool, 1, &scene->commands);
	vkDestroyBuffer(device, scene->readback, NULL);
	vkFreeMemory(device, scene->readback_memory, NULL);
	vkDestroyBuffer(device, scene->instance, NULL);
	vkFreeMemory(device, scene->instance_memory, NULL);
	vkDestroyBuffer(device, scene->indices, NULL);
	vkFreeMemory(device, scene->index_memory, NULL);
	for (uint32_t i = 0; i < ROWS; i++)
		vkDestroyPipeline(device, scene->pipelines[i], NULL);
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	vkDestroyFramebuffer(device, scene->framebuffer, NULL);
	vkDestroyRenderPass(device, scene->render_pass, NULL);
	vkDestroyImageView(device, scene->view, NULL);
	vkDestroyImage(device, scene->image, NULL);
	vkFreeMemory(device, scene->image_memory, NULL);
}

void test_vertex_index(const Harness *harness)
{
	static Scene scene;
	memset(&scene, 0, sizeof scene);
	const bool built = build(harness, &scene);
	check("vertex index: pipelines from vertex_index.vert, from the bundle, compiled on the console and instanced",
		built);
	check("vertex index: the shader the bundle doesn't hold compiled on the console", scene.compiled_here == 1);
	const bool ran = built && run(harness, &scene);
	check(
		"vertex index: each quad in the cell gl_VertexIndex picks, read back: from vertex 0 and 8, indexed from vertex "
		"offsets 16 and 20, and instanced indexed lists from 4 and 12, every other pixel clear",
		ran && pixels_right(&scene));
	destroy(harness, &scene);
}
