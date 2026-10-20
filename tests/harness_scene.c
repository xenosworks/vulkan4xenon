#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "harness.h"
#include "xsdk/display.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/thread.h"

enum {
	ACQUIRE_TIMEOUT_NS = 1000000000,
	FENCE_TIMEOUT_NS = 1000000000,
	MAX_MISSES_LOGGED = 4,
};

bool harness_near(Rgb pixel, Rgb want, uint32_t tolerance)
{
	return pixel.red + tolerance >= want.red && pixel.red <= want.red + tolerance &&
		pixel.green + tolerance >= want.green && pixel.green <= want.green + tolerance &&
		pixel.blue + tolerance >= want.blue && pixel.blue <= want.blue + tolerance;
}

static VkResult create_render_pass(
	const Harness *harness, VkImageView depth, VkRenderPass *render_pass, VkFramebuffer *framebuffers)
{
	const VkAttachmentDescription attachments[2] = {
		{ .format = VK_FORMAT_B8G8R8A8_UNORM,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR },
		{ .format = VK_FORMAT_D24_UNORM_S8_UINT,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL },
	};
	const VkAttachmentReference reference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	const VkAttachmentReference depth_reference = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	const VkSubpassDescription subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &reference,
		.pDepthStencilAttachment = depth ? &depth_reference : NULL,
	};
	const VkRenderPassCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = depth ? 2 : 1,
		.pAttachments = attachments,
		.subpassCount = 1,
		.pSubpasses = &subpass,
	};
	VkResult result = vkCreateRenderPass(harness->device, &info, NULL, render_pass);
	for (uint32_t i = 0; i < harness->image_count && result == VK_SUCCESS && framebuffers; i++) {
		const VkImageView views[2] = { harness->views[i], depth };
		const VkFramebufferCreateInfo framebuffer_info = {
			.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			.renderPass = *render_pass,
			.attachmentCount = depth ? 2 : 1,
			.pAttachments = views,
			.width = HARNESS_WIDTH,
			.height = HARNESS_HEIGHT,
			.layers = 1,
		};
		result = vkCreateFramebuffer(harness->device, &framebuffer_info, NULL, &framebuffers[i]);
	}
	return result;
}

VkResult harness_render_pass(const Harness *harness, VkRenderPass *render_pass, VkFramebuffer *framebuffers)
{
	return create_render_pass(harness, VK_NULL_HANDLE, render_pass, framebuffers);
}

VkResult harness_depth_render_pass(
	const Harness *harness, VkImageView depth, VkRenderPass *render_pass, VkFramebuffer *framebuffers)
{
	return create_render_pass(harness, depth, render_pass, framebuffers);
}

VkResult harness_depth_image(const Harness *harness, VkImage *image, VkDeviceMemory *memory, VkImageView *view)
{
	return harness_image(harness, VK_FORMAT_D24_UNORM_S8_UINT, HARNESS_WIDTH, HARNESS_HEIGHT,
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, image, memory, view);
}

VkResult harness_image(const Harness *harness, VkFormat format, uint32_t width, uint32_t height,
	VkImageUsageFlags usage, VkImage *image, VkDeviceMemory *memory, VkImageView *view)
{
	return harness_multisampled_image(
		harness, format, width, height, VK_SAMPLE_COUNT_1_BIT, usage, image, memory, view);
}

VkResult harness_multisampled_image(const Harness *harness, VkFormat format, uint32_t width, uint32_t height,
	VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImage *image, VkDeviceMemory *memory, VkImageView *view)
{
	const VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = format,
		.extent = { width, height, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = samples,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = usage,
	};
	VkResult result = vkCreateImage(harness->device, &info, NULL, image);
	if (result != VK_SUCCESS)
		return result;
	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(harness->device, *image, &requirements);
	const VkMemoryAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
	};
	result = vkAllocateMemory(harness->device, &allocate_info, NULL, memory);
	if (result == VK_SUCCESS)
		result = vkBindImageMemory(harness->device, *image, *memory, 0);
	const bool depth = format == VK_FORMAT_D24_UNORM_S8_UINT;
	const VkImageViewCreateInfo view_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = *image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = format,
		.subresourceRange = { depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
									: VK_IMAGE_ASPECT_COLOR_BIT,
			0, 1, 0, 1 },
	};
	if (result == VK_SUCCESS)
		result = vkCreateImageView(harness->device, &view_info, NULL, view);
	return result;
}

void harness_destroy_render_pass(const Harness *harness, VkRenderPass render_pass, const VkFramebuffer *framebuffers)
{
	for (uint32_t i = 0; i < harness->image_count && framebuffers; i++)
		vkDestroyFramebuffer(harness->device, framebuffers[i], NULL);
	vkDestroyRenderPass(harness->device, render_pass, NULL);
}

static VkResult create_pipeline(const Harness *harness, const HarnessPipeline *spec, VkRenderPass render_pass,
	const VkShaderModule *modules, VkPipeline *pipeline)
{
	const VkPipelineVertexInputStateCreateInfo input = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount = spec->binding_count,
		.pVertexBindingDescriptions = spec->bindings,
		.vertexAttributeDescriptionCount = spec->attribute_count,
		.pVertexAttributeDescriptions = spec->attributes,
	};
	const VkPipelineInputAssemblyStateCreateInfo triangle_list = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	};
	const VkViewport viewport = { 0.0f, 0.0f, HARNESS_WIDTH, HARNESS_HEIGHT, 0.0f, 1.0f };
	const VkRect2D scissor = { { 0, 0 }, { HARNESS_WIDTH, HARNESS_HEIGHT } };
	const VkPipelineViewportStateCreateInfo viewport_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.pViewports = &viewport,
		.scissorCount = 1,
		.pScissors = &scissor,
	};
	const VkPipelineRasterizationStateCreateInfo plain = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.lineWidth = spec->dynamic_line_width ? 0.0f : 1.0f,
	};
	const VkPipelineMultisampleStateCreateInfo multisample = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};
	const VkPipelineColorBlendAttachmentState opaque = {
		.colorWriteMask =
			VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	};
	VkPipelineColorBlendStateCreateInfo blend = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = spec->blend_count ? spec->blend_count : 1,
		.pAttachments = spec->blend ? spec->blend : &opaque,
	};
	for (uint32_t i = 0; i < 4 && spec->blend_constants; i++)
		blend.blendConstants[i] = spec->blend_constants[i];
	VkDynamicState dynamic_states[HARNESS_MAX_DYNAMIC_STATES + 3];
	uint32_t dynamic_count = 0;
	if (spec->dynamic_line_width)
		dynamic_states[dynamic_count++] = VK_DYNAMIC_STATE_LINE_WIDTH;
	if (spec->dynamic_viewport) {
		dynamic_states[dynamic_count++] = VK_DYNAMIC_STATE_VIEWPORT;
		dynamic_states[dynamic_count++] = VK_DYNAMIC_STATE_SCISSOR;
	}
	for (uint32_t i = 0; i < spec->dynamic_state_count; i++)
		dynamic_states[dynamic_count++] = spec->dynamic_states[i];
	const VkPipelineDynamicStateCreateInfo dynamic = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = dynamic_count,
		.pDynamicStates = dynamic_states,
	};
	const VkPipelineShaderStageCreateInfo stages[] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = modules[0],
			.pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = modules[1],
			.pName = "main" },
	};
	const VkGraphicsPipelineCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2,
		.pStages = stages,
		.pVertexInputState = &input,
		.pInputAssemblyState = spec->assembly ? spec->assembly : &triangle_list,
		.pViewportState = &viewport_state,
		.pRasterizationState = spec->raster ? spec->raster : &plain,
		.pMultisampleState = spec->multisample ? spec->multisample : &multisample,
		.pDepthStencilState = spec->depth_stencil,
		.pColorBlendState = &blend,
		.pDynamicState = dynamic_count ? &dynamic : NULL,
		.layout = spec->layout,
		.renderPass = render_pass,
		.subpass = spec->subpass,
	};
	return vkCreateGraphicsPipelines(harness->device, VK_NULL_HANDLE, 1, &info, NULL, pipeline);
}

// The shader modules live only as long as pipeline creation needs them.
VkResult harness_pipeline(
	const Harness *harness, const HarnessPipeline *spec, VkRenderPass render_pass, VkPipeline *pipeline)
{
	VkShaderModule modules[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
	VkResult result = harness_shader(harness, spec->vertex_spirv, spec->vertex_bytes, &modules[0]);
	if (harness_ok("vertex shader module", result))
		result = harness_shader(harness, spec->fragment_spirv, spec->fragment_bytes, &modules[1]);
	if (harness_ok("fragment shader module", result))
		result = create_pipeline(harness, spec, render_pass, modules, pipeline);
	vkDestroyShaderModule(harness->device, modules[0], NULL);
	vkDestroyShaderModule(harness->device, modules[1], NULL);
	return result;
}

VkResult harness_command_buffers(const Harness *harness, VkCommandBuffer *commands)
{
	const VkCommandBufferAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = harness->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = harness->image_count,
	};
	VkResult result = vkAllocateCommandBuffers(harness->device, &allocate_info, commands);
	const VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	for (uint32_t i = 0; i < harness->image_count && result == VK_SUCCESS; i++)
		result = vkBeginCommandBuffer(commands[i], &begin);
	return result;
}

static bool frame(const Harness *harness, const VkCommandBuffer *commands, uint32_t number, HarnessCheck check,
	void *context, uint32_t *misses)
{
	uint32_t index = 0;
	if (!harness_ok("vkAcquireNextImageKHR",
			vkAcquireNextImageKHR(
				harness->device, harness->swapchain, ACQUIRE_TIMEOUT_NS, VK_NULL_HANDLE, VK_NULL_HANDLE, &index)))
		return false;
	const VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &commands[index],
	};
	if (!harness_ok("vkQueueSubmit", vkQueueSubmit(harness->queue, 1, &submit, harness->fence)) ||
		!harness_ok("vkWaitForFences", vkWaitForFences(harness->device, 1, &harness->fence, VK_TRUE, FENCE_TIMEOUT_NS)))
		return false;
	(void)vkResetFences(harness->device, 1, &harness->fence);
	char what[24];
	(void)snprintf(what, sizeof what, "frame %u", (unsigned)number);
	*misses += !check(harness_image_address(index), what, context);
	const VkPresentInfoKHR present = {
		.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.swapchainCount = 1,
		.pSwapchains = &harness->swapchain,
		.pImageIndices = &index,
	};
	return harness_ok("vkQueuePresentKHR", vkQueuePresentKHR(harness->queue, &present));
}

uint32_t harness_frames(const Harness *harness, const VkCommandBuffer *commands, uint32_t frames, HarnessCheck check,
	void *context, uint32_t *misses)
{
	uint32_t done = 0;
	while (done < frames && frame(harness, commands, done, check, context, misses))
		done++;
	(void)vkDeviceWaitIdle(harness->device);
	return done;
}

bool harness_screen(const Harness *harness, HarnessCheck check, void *context)
{
	(void)xsdk_thread_sleep(HARNESS_SETTLE_MS);
	const uint32_t shown = xsdk_display_shown_address();
	for (uint32_t i = 0; i < harness->image_count; i++) {
		if (shown == harness_image_address(i))
			return check(shown, "on screen", context);
	}
	return false;
}

uint32_t harness_flips(void)
{
	XsdkPresentStats stats;
	xsdk_present_stats(&stats);
	return stats.flips;
}

// Any pixel within the probe's radius holds the colour.
static bool probe_holds(uint32_t address, const HarnessProbe *probe, uint32_t tolerance)
{
	const uint32_t r = probe->radius;
	for (uint32_t y = probe->y - r; y <= probe->y + r; y++) {
		for (uint32_t x = probe->x - r; x <= probe->x + r; x++) {
			if (harness_near(harness_pixel(address, x, y), probe->want, tolerance))
				return true;
		}
	}
	return false;
}

bool harness_probes_hold(uint32_t address, const char *what, void *context)
{
	HarnessProbes *const probes = context;
	bool holds = true;
	for (uint32_t i = 0; i < probes->count; i++) {
		const HarnessProbe *const probe = &probes->probes[i];
		const Rgb pixel = harness_pixel(address, probe->x, probe->y);
		if (probe_holds(address, probe, probes->tolerance))
			continue;
		if (probes->logged++ < MAX_MISSES_LOGGED)
			xsdk_log_write("%s: %s: %s at %u, %u held %02X %02X %02X, not %02X %02X %02X", probes->test, what,
				probe->name, (unsigned)probe->x, (unsigned)probe->y, (unsigned)pixel.red, (unsigned)pixel.green,
				(unsigned)pixel.blue, (unsigned)probe->want.red, (unsigned)probe->want.green,
				(unsigned)probe->want.blue);
		holds = false;
	}
	return holds;
}

const VkVertexInputBindingDescription harness_quad_binding = { 0, 16, VK_VERTEX_INPUT_RATE_VERTEX };
const VkVertexInputAttributeDescription harness_quad_attribute = {
	.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0
};

VkResult harness_push_layout(const Harness *harness, VkPipelineLayout *layout)
{
	const VkPushConstantRange ranges[] = {
		{ VK_SHADER_STAGE_VERTEX_BIT, 0, 32 },
		{ VK_SHADER_STAGE_FRAGMENT_BIT, 32, 16 },
	};
	const VkPipelineLayoutCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.pushConstantRangeCount = 2,
		.pPushConstantRanges = ranges,
	};
	return vkCreatePipelineLayout(harness->device, &info, NULL, layout);
}

// Top left, top right, bottom left; then top right, bottom right, bottom left: clockwise as it looks, y down.
VkResult harness_quad_buffer(const Harness *harness, VkBuffer *buffer, VkDeviceMemory *memory)
{
	static const float quad[6][4] = {
		{ 0.0f, 0.0f, 0.0f, 1.0f },
		{ 1.0f, 0.0f, 0.0f, 1.0f },
		{ 0.0f, 1.0f, 0.0f, 1.0f },
		{ 1.0f, 0.0f, 0.0f, 1.0f },
		{ 1.0f, 1.0f, 0.0f, 1.0f },
		{ 0.0f, 1.0f, 0.0f, 1.0f },
	};
	return harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, quad, sizeof quad, buffer, memory);
}

void harness_push_quad(VkCommandBuffer commands, VkPipelineLayout layout, const HarnessQuad *quad)
{
	harness_push_quad_in(commands, layout, quad, HARNESS_WIDTH, HARNESS_HEIGHT);
}

void harness_push_quad_in(
	VkCommandBuffer commands, VkPipelineLayout layout, const HarnessQuad *quad, uint32_t width, uint32_t height)
{
	harness_push_place_in(commands, layout, quad, width, height);
	vkCmdDraw(commands, 6, 1, 0, 0);
}

// push.vert works out position = unit quad * scale + offset, so the quad's pixels become clip space here.
void harness_push_place_in(
	VkCommandBuffer commands, VkPipelineLayout layout, const HarnessQuad *quad, uint32_t width, uint32_t height)
{
	const float half_width = (float)width / 2.0f;
	const float half_height = (float)height / 2.0f;
	const float left = quad->mirrored ? quad->x + quad->width : quad->x;
	const float place[8] = {
		(quad->mirrored ? -quad->width : quad->width) / half_width,
		quad->height / half_height,
		0.0f,
		1.0f,
		left / half_width - 1.0f,
		quad->y / half_height - 1.0f,
		quad->depth,
		0.0f,
	};
	vkCmdPushConstants(commands, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof place, place);
	vkCmdPushConstants(commands, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 32, 16, quad->color);
}
