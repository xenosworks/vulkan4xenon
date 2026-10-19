#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"

// Milestone 24's instanced draws and milestone 47's: a row of a grid each, every instance a quad in its own cell and
// colour. instanced.vert takes its column from gl_InstanceIndex and its row and colour from the instance's attributes;
// instanced_places.vert takes column and row from its attributes alone. Each row's draw binds the instance buffer at
// its own 8 entries, so an entry's colour shows where gl_InstanceIndex and the attributes agree on the instance. Then
// 1,000 instances of instance_grid.vert in one draw, the packets it adds counted.

enum {
	FRAMES = 30,
	TOLERANCE = 2,
	FENCE_TIMEOUT_NS = 1000000000,
	// instanced.vert's grid: cells 160 by 120 pixels, each quad 96 by 72 of its cell from 32 by 24 in.
	COLUMNS = 8,
	CELL_WIDTH = 160,
	CELL_HEIGHT = 120,
	// Two vertices nothing draws, then the corners for indexed draws, then a list of six and a seventh, which a list of
	// 7 vertices an instance never draws, then a strip of four.
	CORNERS_AT = 2,
	LIST_AT = 6,
	STRIP_AT = LIST_AT + 7,
	VERTICES = STRIP_AT + 4,
	ENTRIES = COLUMNS,
	ENTRY_BYTES = 24,
	// The indexed draws' 12 indices, the quad twice, after 8 that draw nothing: from byte 16, so the layer reads 8 in
	// one 16-byte load and 4 a load each.
	FIRST_INDEX = 8,
	INDEX_COUNT = 12,
	INDICES = FIRST_INDEX + INDEX_COUNT,
	// A VkDrawIndirectCommand, then a VkDrawIndexedIndirectCommand.
	INDEXED_AT = 16,
	INDIRECT_BYTES = INDEXED_AT + 20,
	// instance_grid.vert's instances, each a cell of 40 by 25 over the screen, coloured by a float4 of its own.
	MANY = 1000,
	MANY_COLUMNS = 40,
	MANY_ROWS = 25,
	COLOR_BYTES = 16,
	// Their draw adds a group and a packet; a draw an instance adds at least that for each instance.
	MANY_MOST_DWORDS = 200,
	// A shader the bundle doesn't hold: its SPIR-V's generator word, its third, changed (test_triangle.c).
	GENERATOR_WORD = 2,
	GENERATOR_CHANGE = 0x00010000,
	// That shader's plain container and its instanced variant.
	COMPILED_HERE = 2,
};

typedef enum {
	ROW_DRAW,
	ROW_INDEXED,
	ROW_INDIRECT,
	ROW_INDEXED_INDIRECT,
	ROW_PLACES,
	// The grid's last row: a strip in its left half, a list of 7 vertices an instance in its right.
	ROW_STRIP,
	ROW_SEVEN,
	DRAWS,
} Row;

typedef enum {
	PIPELINE_INSTANCED,
	PIPELINE_PLACES,
	// instanced_places.vert compiled on the console, as a list; instanced.vert as a strip; instance_grid.vert.
	PIPELINE_SEVEN,
	PIPELINE_STRIP,
	PIPELINE_GRID,
	PIPELINES,
} PipelineIndex;

typedef struct {
	const char *what;
	uint32_t instances;
	uint32_t first_instance;
	// The grid's row it draws in, from its first column on, and whether instanced_places.vert places it.
	uint32_t row;
	uint32_t first_column;
	uint32_t columns;
	bool places;
} RowDraw;

static const RowDraw rows[DRAWS] = {
	{ "vkCmdDraw", 4, 0, 0, 0, COLUMNS, false },
	{ "vkCmdDrawIndexed", 3, 2, 1, 0, COLUMNS, false },
	{ "vkCmdDrawIndirect", 4, 3, 2, 0, COLUMNS, false },
	{ "vkCmdDrawIndexedIndirect", 2, 5, 3, 0, COLUMNS, false },
	{ "vkCmdDraw without gl_InstanceIndex", 4, 1, 4, 0, COLUMNS, true },
	{ "a triangle strip", 3, 1, 5, 0, COLUMNS / 2, false },
	{ "a list of 7 vertices an instance", 3, 1, 5, COLUMNS / 2, COLUMNS / 2, true },
};

// The quad's corners in the unit square: top left, top right, bottom left; top right, bottom right, bottom left; then
// the seventh, bottom right, which would join an instance's quad to the next where 7 vertices made an instance.
static const float list[7][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 1 }, { 1, 1 } };
static const float corners[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
static const float strip[4][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
// Into the corners, at a vertex offset of 2: 8 at the first corner, then the quad's two triangles twice.
static const uint16_t indices[INDICES] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 3, 1, 2, 3, 0, 1, 3, 1, 2, 3 };

static const VkVertexInputBindingDescription bindings[2] = {
	{ 0, 8, VK_VERTEX_INPUT_RATE_VERTEX },
	{ 1, ENTRY_BYTES, VK_VERTEX_INPUT_RATE_INSTANCE },
};
// The corner per vertex; the colour and the place per instance, the place first in each entry.
static const VkVertexInputAttributeDescription attributes[3] = {
	{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 0 },
	{ .location = 1, .binding = 1, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 8 },
	{ .location = 2, .binding = 1, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 0 },
};
// instance_grid.vert's: the corner per vertex, the colour per instance.
static const VkVertexInputBindingDescription grid_bindings[2] = {
	{ 0, 8, VK_VERTEX_INPUT_RATE_VERTEX },
	{ 1, COLOR_BYTES, VK_VERTEX_INPUT_RATE_INSTANCE },
};
static const VkVertexInputAttributeDescription grid_attributes[2] = {
	{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 0 },
	{ .location = 1, .binding = 1, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0 },
};

static const VkClearValue clear = { .color = { .float32 = { 16.0f / 255, 24.0f / 255, 40.0f / 255, 1.0f } } };
static const Rgb clear_rgb = { 16, 24, 40 };

static uint32_t compiled_places[INSTANCED_PLACES_VERT_SPIRV_BYTES / 4];
// Large, so not on the stack.
static float colors[MANY][COLOR_BYTES / 4];
static HarnessProbe many_probes[MANY];

typedef struct {
	VkRenderPass render_pass;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES];
	VkPipelineLayout layout;
	VkPipeline pipelines[PIPELINES];
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkBuffer instances;
	VkDeviceMemory instance_memory;
	VkBuffer indices;
	VkDeviceMemory index_memory;
	VkBuffer indirect;
	VkDeviceMemory indirect_memory;
	VkBuffer colors;
	VkDeviceMemory color_memory;
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	// The 1,000 instances' pass, without their draw and with it.
	VkCommandBuffer many[2];
	HarnessProbe probes[DRAWS][COLUMNS];
	HarnessProbes checks[DRAWS];
	uint32_t misses[DRAWS];
	HarnessProbes many_check;
} Scene;

// Entry e of a draw's 8: its own colour, and its place, column 7 - e, which only instanced_places.vert reads.
static Rgb entry_rgb(uint32_t draw, uint32_t entry)
{
	return (Rgb){ 40 + 26 * entry, 40 + 30 * draw, 220 - 24 * entry };
}

static uint32_t places_column(uint32_t entry)
{
	return COLUMNS - 1 - entry;
}

static Rgb many_rgb(uint32_t instance)
{
	return (Rgb){ 40 + 5 * (instance % MANY_COLUMNS), 30 + 9 * (instance / MANY_COLUMNS), 200 - 120 * (instance % 2) };
}

// Each cell a draw owns holds its instance's entry colour or the clear colour; each of the 1,000 its own colour.
static void make_probes(Scene *scene)
{
	for (uint32_t d = 0; d < DRAWS; d++) {
		const RowDraw *const draw = &rows[d];
		for (uint32_t c = 0; c < draw->columns; c++) {
			const uint32_t column = draw->first_column + c;
			Rgb want = clear_rgb;
			for (uint32_t i = 0; i < draw->instances; i++) {
				const uint32_t entry = draw->first_instance + i;
				if ((draw->places ? places_column(entry) : entry) == column)
					want = entry_rgb(d, entry);
			}
			scene->probes[d][c] = (HarnessProbe){ draw->what, column * CELL_WIDTH + CELL_WIDTH / 2,
				draw->row * CELL_HEIGHT + CELL_HEIGHT / 2, want, 0 };
		}
		scene->checks[d] = (HarnessProbes){ "instancing", scene->probes[d], draw->columns, TOLERANCE, 0 };
	}
	for (uint32_t i = 0; i < MANY; i++) {
		const uint32_t row = i / MANY_COLUMNS;
		many_probes[i] =
			(HarnessProbe){ "1,000 instances", (2 * (i % MANY_COLUMNS) + 1) * HARNESS_WIDTH / MANY_COLUMNS / 2,
				(2 * row + 1) * HARNESS_HEIGHT / MANY_ROWS / 2, many_rgb(i), 0 };
	}
	scene->many_check = (HarnessProbes){ "instancing", many_probes, MANY, TOLERANCE, 0 };
}

static VkResult create_pipeline(const Harness *harness, Scene *scene, PipelineIndex which)
{
	static const VkPipelineInputAssemblyStateCreateInfo strip_assembly = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
	};
	const uint32_t *const spirv[PIPELINES] = { instanced_vert_spirv, instanced_places_vert_spirv, compiled_places,
		instanced_vert_spirv, instance_grid_vert_spirv };
	const uint32_t bytes[PIPELINES] = { INSTANCED_VERT_SPIRV_BYTES, INSTANCED_PLACES_VERT_SPIRV_BYTES,
		INSTANCED_PLACES_VERT_SPIRV_BYTES, INSTANCED_VERT_SPIRV_BYTES, INSTANCE_GRID_VERT_SPIRV_BYTES };
	const bool grid = which == PIPELINE_GRID;
	const HarnessPipeline spec = {
		.vertex_spirv = spirv[which],
		.vertex_bytes = bytes[which],
		.fragment_spirv = triangle_frag_spirv,
		.fragment_bytes = TRIANGLE_FRAG_SPIRV_BYTES,
		.bindings = grid ? grid_bindings : bindings,
		.binding_count = 2,
		.attributes = grid ? grid_attributes : attributes,
		.attribute_count = grid ? 2 : 3,
		.layout = scene->layout,
		.assembly = which == PIPELINE_STRIP ? &strip_assembly : NULL,
	};
	return harness_pipeline(harness, &spec, scene->render_pass, &scene->pipelines[which]);
}

static bool make_buffers(const Harness *harness, Scene *scene)
{
	float vertices[VERTICES][2] = { { 0 } };
	for (uint32_t i = 0; i < 4; i++) {
		memcpy(vertices[CORNERS_AT + i], corners[i], sizeof corners[i]);
		memcpy(vertices[STRIP_AT + i], strip[i], sizeof strip[i]);
	}
	memcpy(vertices[LIST_AT], list, sizeof list);
	float entries[DRAWS * ENTRIES][ENTRY_BYTES / 4];
	for (uint32_t d = 0; d < DRAWS; d++) {
		for (uint32_t entry = 0; entry < ENTRIES; entry++) {
			const Rgb rgb = entry_rgb(d, entry);
			float *const e = entries[d * ENTRIES + entry];
			e[0] = (float)places_column(entry), e[1] = (float)rows[d].row;
			e[2] = rgb.red / 255.0f, e[3] = rgb.green / 255.0f, e[4] = rgb.blue / 255.0f, e[5] = 1.0f;
		}
	}
	// The indirect draws' own instances and first instances, as rows gives them.
	const uint32_t draw[4] = { 6, rows[ROW_INDIRECT].instances, LIST_AT, rows[ROW_INDIRECT].first_instance };
	const uint32_t indexed[5] = { INDEX_COUNT, rows[ROW_INDEXED_INDIRECT].instances, FIRST_INDEX, CORNERS_AT,
		rows[ROW_INDEXED_INDIRECT].first_instance };
	uint32_t indirect[INDIRECT_BYTES / 4];
	for (uint32_t i = 0; i < 4; i++)
		indirect[i] = draw[i];
	for (uint32_t i = 0; i < 5; i++)
		indirect[INDEXED_AT / 4 + i] = indexed[i];
	return harness_ok("vertices",
			   harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices, sizeof vertices, &scene->vertices,
				   &scene->vertex_memory)) &&
		harness_ok("instances",
			harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, entries, sizeof entries, &scene->instances,
				&scene->instance_memory)) &&
		harness_ok("indices",
			harness_buffer(harness, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices, sizeof indices, &scene->indices,
				&scene->index_memory)) &&
		harness_ok("indirect",
			harness_buffer(harness, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, indirect, sizeof indirect, &scene->indirect,
				&scene->indirect_memory));
}

static bool build(const Harness *harness, Scene *scene)
{
	const VkPipelineLayoutCreateInfo layout_info = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	bool ok = harness_ok("layout", vkCreatePipelineLayout(harness->device, &layout_info, NULL, &scene->layout));
	ok = ok && harness_ok("render pass", harness_render_pass(harness, &scene->render_pass, scene->framebuffers));
	ok = ok && harness_ok("instanced pipeline", create_pipeline(harness, scene, PIPELINE_INSTANCED));
	ok = ok && harness_ok("places pipeline", create_pipeline(harness, scene, PIPELINE_PLACES));
	return ok && make_buffers(harness, scene);
}

// Milestone 47's: instanced_places.vert's plain shader and its instanced variant compiled on the console, the strip's
// and the 1,000 instances' pipelines, and their colours.
static bool build_more(const Harness *harness, Scene *scene)
{
	memcpy(compiled_places, instanced_places_vert_spirv, sizeof compiled_places);
	compiled_places[GENERATOR_WORD] ^= GENERATOR_CHANGE;
	for (uint32_t i = 0; i < MANY; i++) {
		const Rgb rgb = many_rgb(i);
		const float color[4] = { rgb.red / 255.0f, rgb.green / 255.0f, rgb.blue / 255.0f, 1.0f };
		memcpy(colors[i], color, sizeof color);
	}
	const uint32_t compiled = v4x_shader_counts().compiled;
	return harness_ok("compiled places pipeline", create_pipeline(harness, scene, PIPELINE_SEVEN)) &&
		v4x_shader_counts().compiled - compiled == COMPILED_HERE &&
		harness_ok("strip pipeline", create_pipeline(harness, scene, PIPELINE_STRIP)) &&
		harness_ok("grid pipeline", create_pipeline(harness, scene, PIPELINE_GRID)) &&
		harness_ok("colours",
			harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, colors, sizeof colors, &scene->colors,
				&scene->color_memory));
}

// Binds the row's 8 entries as the instance buffer, then draws its instances.
static void draw_row(VkCommandBuffer commands, const Scene *scene, Row row)
{
	const VkDeviceSize offset = (VkDeviceSize)row * ENTRIES * ENTRY_BYTES;
	vkCmdBindVertexBuffers(commands, 1, 1, &scene->instances, &offset);
	const RowDraw *const draw = &rows[row];
	switch (row) {
	case ROW_DRAW:
	case ROW_PLACES:
		vkCmdDraw(commands, 6, draw->instances, LIST_AT, draw->first_instance);
		break;
	case ROW_INDEXED:
		vkCmdDrawIndexed(commands, INDEX_COUNT, draw->instances, FIRST_INDEX, CORNERS_AT, draw->first_instance);
		break;
	case ROW_INDIRECT:
		vkCmdDrawIndirect(commands, scene->indirect, 0, 1, INDEXED_AT);
		break;
	case ROW_INDEXED_INDIRECT:
		vkCmdDrawIndexedIndirect(commands, scene->indirect, INDEXED_AT, 1, INDIRECT_BYTES - INDEXED_AT);
		break;
	case ROW_STRIP:
		vkCmdDraw(commands, 4, draw->instances, STRIP_AT, draw->first_instance);
		break;
	case ROW_SEVEN:
		vkCmdDraw(commands, 7, draw->instances, LIST_AT, draw->first_instance);
		break;
	case DRAWS:
		break;
	}
}

static void begin_pass(const Scene *scene, VkCommandBuffer commands, uint32_t image)
{
	const VkRenderPassBeginInfo pass = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = scene->render_pass,
		.framebuffer = scene->framebuffers[image],
		.renderArea = { { 0, 0 }, { HARNESS_WIDTH, HARNESS_HEIGHT } },
		.clearValueCount = 1,
		.pClearValues = &clear,
	};
	vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
}

static VkResult record(const Harness *harness, Scene *scene)
{
	// The pipeline each draw takes.
	static const PipelineIndex takes[DRAWS] = { PIPELINE_INSTANCED, PIPELINE_INSTANCED, PIPELINE_INSTANCED,
		PIPELINE_INSTANCED, PIPELINE_PLACES, PIPELINE_STRIP, PIPELINE_SEVEN };
	VkResult result = harness_command_buffers(harness, scene->commands);
	for (uint32_t i = 0; i < harness->image_count && result == VK_SUCCESS; i++) {
		const VkDeviceSize offset = 0;
		const VkCommandBuffer commands = scene->commands[i];
		begin_pass(scene, commands, i);
		vkCmdBindVertexBuffers(commands, 0, 1, &scene->vertices, &offset);
		vkCmdBindIndexBuffer(commands, scene->indices, 0, VK_INDEX_TYPE_UINT16);
		for (uint32_t row = 0; row < DRAWS; row++) {
			if (!row || takes[row] != takes[row - 1])
				vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipelines[takes[row]]);
			draw_row(commands, scene, (Row)row);
		}
		vkCmdEndRenderPass(commands);
		result = vkEndCommandBuffer(commands);
	}
	return result;
}

// The 1,000 instances' pass into the first image: an instance drawn alone, so the pipeline and streams are set before
// them, then, in the second, the 1,000.
static VkResult record_many(const Harness *harness, Scene *scene)
{
	const VkCommandBufferAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = harness->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 2,
	};
	VkResult result = vkAllocateCommandBuffers(harness->device, &allocate_info, scene->many);
	const VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	const VkBuffer buffers[2] = { scene->vertices, scene->colors };
	const VkDeviceSize offsets[2] = { 0, 0 };
	for (uint32_t i = 0; i < 2 && result == VK_SUCCESS; i++) {
		const VkCommandBuffer commands = scene->many[i];
		result = vkBeginCommandBuffer(commands, &begin);
		if (result != VK_SUCCESS)
			break;
		begin_pass(scene, commands, 0);
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipelines[PIPELINE_GRID]);
		vkCmdBindVertexBuffers(commands, 0, 2, buffers, offsets);
		vkCmdDraw(commands, 6, 1, LIST_AT, 0);
		if (i)
			vkCmdDraw(commands, 6, MANY, LIST_AT, 0);
		vkCmdEndRenderPass(commands);
		result = vkEndCommandBuffer(commands);
	}
	return result;
}

// Submits the commands and waits for them; dwords is what the submission wrote into xsdk's stream.
static bool submit_counted(const Harness *harness, VkCommandBuffer commands, uint64_t *dwords)
{
	XsdkGpuStats before;
	xsdk_gpu_stats(&before);
	const VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &commands,
	};
	const bool done = harness_ok("vkQueueSubmit", vkQueueSubmit(harness->queue, 1, &submit, harness->fence)) &&
		harness_ok("vkWaitForFences", vkWaitForFences(harness->device, 1, &harness->fence, VK_TRUE, FENCE_TIMEOUT_NS));
	(void)vkResetFences(harness->device, 1, &harness->fence);
	XsdkGpuStats after;
	xsdk_gpu_stats(&after);
	*dwords = after.dwords - before.dwords;
	return done;
}

// The 1,000 instances' draw's own dwords: the pass with it after the pass without, each after the pass without, so
// each begins with eDRAM and the shaders as the other does.
static bool count_many(const Harness *harness, const Scene *scene, uint64_t *dwords)
{
	uint64_t first = 0;
	uint64_t without = 0;
	uint64_t with = 0;
	const bool done = submit_counted(harness, scene->many[0], &first) &&
		submit_counted(harness, scene->many[0], &without) && submit_counted(harness, scene->many[1], &with);
	*dwords = with - without;
	xsdk_log_write(
		"instancing: the pass took %u dwords, %u without the 1,000 instances' draw", (unsigned)with, (unsigned)without);
	return done && with >= without;
}

// Every draw's probes, each draw's misses counted apart.
static bool rows_hold(uint32_t address, const char *what, void *context)
{
	Scene *const scene = context;
	bool all = true;
	for (uint32_t d = 0; d < DRAWS; d++) {
		if (!harness_probes_hold(address, what, &scene->checks[d])) {
			scene->misses[d]++;
			all = false;
		}
	}
	return all;
}

static void destroy(const Harness *harness, Scene *scene)
{
	const VkDevice device = harness->device;
	vkFreeCommandBuffers(device, harness->pool, harness->image_count, scene->commands);
	vkFreeCommandBuffers(device, harness->pool, 2, scene->many);
	const VkBuffer buffers[] = { scene->vertices, scene->instances, scene->indices, scene->indirect, scene->colors };
	const VkDeviceMemory memories[] = { scene->vertex_memory, scene->instance_memory, scene->index_memory,
		scene->indirect_memory, scene->color_memory };
	for (uint32_t i = 0; i < sizeof buffers / sizeof buffers[0]; i++) {
		vkDestroyBuffer(device, buffers[i], NULL);
		vkFreeMemory(device, memories[i], NULL);
	}
	for (uint32_t i = 0; i < PIPELINES; i++)
		vkDestroyPipeline(device, scene->pipelines[i], NULL);
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	harness_destroy_render_pass(harness, scene->render_pass, scene->framebuffers);
}

// The 1,000 instances drawn, then checked and counted.
static void check_many(const Harness *harness, Scene *scene, bool built)
{
	uint64_t dwords = 0;
	const bool counted = built && harness->swapchain &&
		harness_ok("1,000 instances' recording", record_many(harness, scene)) && count_many(harness, scene, &dwords);
	xsdk_log_write("instancing: 1,000 instances in one draw added %u dwords to the stream", (unsigned)dwords);
	check(
		"instancing: 1,000 instances in one vkCmdDraw, each in the cell gl_InstanceIndex gives and the colour its "
		"attribute gives",
		counted && harness_probes_hold(harness_image_address(0), "1,000 instances", &scene->many_check));
	check(
		"instancing: the 1,000 instances' draw adds fewer than 200 dwords to the stream, which a draw an instance "
		"can't",
		counted && dwords < MANY_MOST_DWORDS);
}

void test_instancing(const Harness *harness)
{
	VkPhysicalDeviceFeatures features;
	vkGetPhysicalDeviceFeatures(harness->physical_device, &features);
	check("instancing: the device has drawIndirectFirstInstance, and took it when made",
		features.drawIndirectFirstInstance);
	Scene scene = { .render_pass = VK_NULL_HANDLE };
	make_probes(&scene);
	const bool built = build(harness, &scene);
	check("instancing: pipelines with a binding at the instance's rate made, with and without gl_InstanceIndex", built);
	const bool more = built && build_more(harness, &scene);
	check(
		"instancing: a list's pipeline from instanced_places.vert compiled on the console, its instanced variant "
		"compiled there too, a strip's and instance_grid.vert's made",
		more);
	const bool recorded = more && harness->swapchain && harness_ok("recording", record(harness, &scene));
	uint32_t misses = 0;
	const uint32_t frames = recorded ? harness_frames(harness, scene.commands, FRAMES, rows_hold, &scene, &misses) : 0;
	check("instancing: 30 frames drawn and presented", frames == FRAMES);
	check(
		"instancing: every frame, vkCmdDraw draws 4 instances, each in the column gl_InstanceIndex gives and the "
		"colour its attributes give",
		frames == FRAMES && scene.misses[ROW_DRAW] == 0);
	check(
		"instancing: every frame, vkCmdDrawIndexed draws 3 from instance 2 and index 8, gl_InstanceIndex and the "
		"attributes both starting there",
		frames == FRAMES && scene.misses[ROW_INDEXED] == 0);
	check("instancing: every frame, vkCmdDrawIndirect draws the 4 instances from instance 3 it reads at replay",
		frames == FRAMES && scene.misses[ROW_INDIRECT] == 0);
	check(
		"instancing: every frame, vkCmdDrawIndexedIndirect draws the 2 instances from instance 5 and index 8 it reads "
		"at replay",
		frames == FRAMES && scene.misses[ROW_INDEXED_INDIRECT] == 0);
	check(
		"instancing: every frame, a shader without gl_InstanceIndex places 4 instances from instance 1 by their "
		"attributes alone",
		frames == FRAMES && scene.misses[ROW_PLACES] == 0);
	check(
		"instancing: every frame, a triangle strip draws 3 instances from instance 1, each a strip of its own in the "
		"column gl_InstanceIndex gives",
		frames == FRAMES && scene.misses[ROW_STRIP] == 0);
	check(
		"instancing: every frame, a list of 7 vertices an instance draws 3 instances from instance 1, each 2 "
		"triangles, the 7th vertex never drawn",
		frames == FRAMES && scene.misses[ROW_SEVEN] == 0);
	check("instancing: the image on screen shows them too", harness_screen(harness, rows_hold, &scene));
	check_many(harness, &scene, more);
	destroy(harness, &scene);
}
