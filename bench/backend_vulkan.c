#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <vulkan/vulkan.h>

#include "assets.h"
#include "bench.h"
#include "shaders.h"
#include "xsdk/log.h"
#include "xsdk/present.h"

// The scenes through vulkan4xenon.

enum {
	MAX_IMAGES = 3,
	MVP_BYTES = MVP_FLOATS * 4,
	TINT_BYTES = TINT_FLOATS * 4,
	// minUniformBufferOffsetAlignment (docs/SUPPORT.md).
	UNIFORM_STRIDE = 256,
	TINT_REGION = MAX_DRAW_ITEMS * UNIFORM_STRIDE,
	UNIFORM_BYTES = 2 * TINT_REGION,
	STREAM_BYTES = MAX_SPRITES * 4 * sizeof(SpriteVertex),
	INSTANCE_BYTES = MAX_DRAW_ITEMS * sizeof(DrawItem),
	QUAD_INDEX_BYTES = MAX_SPRITES * QUAD_INDICES * 2,
	UPLOAD_TIMEOUT_NS = 1000000000,
};

static const char *const instance_extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME };
static const char *const device_extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

// The background, 26 26 31, and depth 1.
static const VkClearValue clears[2] = {
	{ .color = { .float32 = { 26.0f / 255, 26.0f / 255, 31.0f / 255, 1.0f } } },
	{ .depthStencil = { 1.0f, 0 } },
};

typedef enum {
	PIPELINE_MESH,
	PIPELINE_MESH_BLEND,
	PIPELINE_GRAY,
	PIPELINE_GRAY_BLEND,
	PIPELINE_UNIFORM,
	PIPELINE_SPRITE,
	PIPELINE_INSTANCED,
	PIPELINES,
} PipelineKind;

typedef struct {
	VkBuffer buffer;
	VkDeviceMemory memory;
	void *mapped;
} Buffer;

typedef struct {
	VkInstance instance;
	VkPhysicalDevice physical_device;
	VkDevice device;
	VkQueue queue;
	VkSurfaceKHR surface;
	VkSwapchainKHR swapchain;
	uint32_t image_count;
	VkImage images[MAX_IMAGES];
	VkImageView views[MAX_IMAGES];
	// Without a display, frames go into this instead of a swapchain image.
	VkImage offscreen;
	VkDeviceMemory offscreen_memory;
	void *offscreen_pixels;
	VkImage depth;
	VkDeviceMemory depth_memory;
	VkImageView depth_view;
	VkRenderPass render_pass;
	VkFramebuffer framebuffers[MAX_IMAGES];
	VkDescriptorSetLayout texture_set_layout;
	VkDescriptorSetLayout uniform_set_layout;
	VkPipelineLayout push_layout;
	VkPipelineLayout uniform_layout;
	VkPipeline pipelines[PIPELINES];
	Buffer cube_vertices;
	Buffer cube_indices;
	Buffer quad_indices;
	Buffer fullscreen;
	Buffer stream;
	Buffer instances;
	Buffer uniforms;
	VkImage textures[TEXTURE_COUNT];
	VkDeviceMemory texture_memory[TEXTURE_COUNT];
	VkImageView texture_views[TEXTURE_COUNT];
	VkSampler sampler;
	VkDescriptorPool pool;
	VkDescriptorSet texture_sets[TEXTURE_COUNT];
	VkDescriptorSet uniform_set;
	VkCommandPool command_pool;
	VkCommandBuffer commands;
	VkFence fence;
	uint32_t image_index;
	uint32_t presented_index;
	uint32_t errors;
} State;

static State state;

static bool ok(const char *call, VkResult result)
{
	if (result == VK_SUCCESS)
		return true;
	if (!state.errors)
		xsdk_log_write("vkbench: %s returned %d", call, (int)result);
	state.errors++;
	return false;
}

static bool headless(void)
{
	return state.offscreen != VK_NULL_HANDLE;
}

static bool create_device(void)
{
	const VkApplicationInfo app = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "vkbench",
		.apiVersion = VK_API_VERSION_1_0,
	};
	const VkInstanceCreateInfo instance_info = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app,
		.enabledExtensionCount = sizeof instance_extensions / sizeof instance_extensions[0],
		.ppEnabledExtensionNames = instance_extensions,
	};
	if (!ok("vkCreateInstance", vkCreateInstance(&instance_info, NULL, &state.instance)))
		return false;
	uint32_t count = 1;
	if (!ok("vkEnumeratePhysicalDevices", vkEnumeratePhysicalDevices(state.instance, &count, &state.physical_device)))
		return false;
	const float priority = 1.0f;
	const VkDeviceQueueCreateInfo queue_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueCount = 1,
		.pQueuePriorities = &priority,
	};
	const VkDeviceCreateInfo device_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &queue_info,
		.enabledExtensionCount = sizeof device_extensions / sizeof device_extensions[0],
		.ppEnabledExtensionNames = device_extensions,
	};
	if (!ok("vkCreateDevice", vkCreateDevice(state.physical_device, &device_info, NULL, &state.device)))
		return false;
	vkGetDeviceQueue(state.device, 0, 0, &state.queue);
	return true;
}

// The display's one mode on plane 0, as an app finds it through VK_KHR_display.
static bool create_swapchain(void)
{
	uint32_t count = 1;
	VkDisplayPropertiesKHR display;
	VkDisplayModePropertiesKHR mode;
	if (!ok("vkGetPhysicalDeviceDisplayPropertiesKHR",
			vkGetPhysicalDeviceDisplayPropertiesKHR(state.physical_device, &count, &display)) ||
		!ok("vkGetDisplayModePropertiesKHR",
			vkGetDisplayModePropertiesKHR(state.physical_device, display.display, &count, &mode)))
		return false;
	const VkDisplaySurfaceCreateInfoKHR surface_info = {
		.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
		.displayMode = mode.displayMode,
		.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
		.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
		.imageExtent = mode.parameters.visibleRegion,
	};
	if (!ok("vkCreateDisplayPlaneSurfaceKHR",
			vkCreateDisplayPlaneSurfaceKHR(state.instance, &surface_info, NULL, &state.surface)))
		return false;
	const VkSwapchainCreateInfoKHR swapchain_info = {
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.surface = state.surface,
		.minImageCount = MAX_IMAGES,
		.imageFormat = VK_FORMAT_B8G8R8A8_UNORM,
		.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
		.imageExtent = { SCREEN_WIDTH, SCREEN_HEIGHT },
		.imageArrayLayers = 1,
		.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
		.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
		.presentMode = VK_PRESENT_MODE_FIFO_KHR,
		.clipped = VK_TRUE,
	};
	// Without a display, as in Xenia, there's no swapchain; that's not one of the calls that failed.
	if (vkCreateSwapchainKHR(state.device, &swapchain_info, NULL, &state.swapchain) != VK_SUCCESS)
		return false;
	state.image_count = MAX_IMAGES;
	return ok("vkGetSwapchainImagesKHR",
		vkGetSwapchainImagesKHR(state.device, state.swapchain, &state.image_count, state.images));
}

static bool bind_memory(VkImage image, VkDeviceMemory *memory)
{
	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(state.device, image, &requirements);
	const VkMemoryAllocateInfo info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
	};
	return ok("vkAllocateMemory", vkAllocateMemory(state.device, &info, NULL, memory)) &&
		ok("vkBindImageMemory", vkBindImageMemory(state.device, image, *memory, 0));
}

static bool create_image(VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage, VkImage *image,
	VkDeviceMemory *memory, VkImageView *view)
{
	const VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = format,
		.extent = { width, height, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = usage,
	};
	if (!ok("vkCreateImage", vkCreateImage(state.device, &info, NULL, image)) || !bind_memory(*image, memory))
		return false;
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
	return ok("vkCreateImageView", vkCreateImageView(state.device, &view_info, NULL, view));
}

// Without a swapchain, one image the screen's size, in the swapchain's format and layout, takes its place.
static bool create_targets(void)
{
	if (!headless()) {
		for (uint32_t i = 0; i < state.image_count; i++) {
			const VkImageViewCreateInfo info = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
				.image = state.images[i],
				.viewType = VK_IMAGE_VIEW_TYPE_2D,
				.format = VK_FORMAT_B8G8R8A8_UNORM,
				.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
			};
			if (!ok("vkCreateImageView", vkCreateImageView(state.device, &info, NULL, &state.views[i])))
				return false;
		}
	}
	return create_image(VK_FORMAT_D24_UNORM_S8_UINT, SCREEN_WIDTH, SCREEN_HEIGHT,
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, &state.depth, &state.depth_memory, &state.depth_view);
}

static bool create_offscreen(void)
{
	if (!create_image(VK_FORMAT_B8G8R8A8_UNORM, SCREEN_WIDTH, SCREEN_HEIGHT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
			&state.offscreen, &state.offscreen_memory, &state.views[0]))
		return false;
	state.image_count = 1;
	return ok(
		"vkMapMemory", vkMapMemory(state.device, state.offscreen_memory, 0, VK_WHOLE_SIZE, 0, &state.offscreen_pixels));
}

static bool create_render_pass(void)
{
	const VkAttachmentDescription attachments[2] = {
		{ .format = VK_FORMAT_B8G8R8A8_UNORM,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = headless() ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR },
		// Like vkbench: depth starts at 1 every frame and is never kept.
		{ .format = VK_FORMAT_D24_UNORM_S8_UINT,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
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
	if (!ok("vkCreateRenderPass", vkCreateRenderPass(state.device, &info, NULL, &state.render_pass)))
		return false;
	for (uint32_t i = 0; i < state.image_count; i++) {
		const VkImageView views[2] = { state.views[i], state.depth_view };
		const VkFramebufferCreateInfo framebuffer_info = {
			.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			.renderPass = state.render_pass,
			.attachmentCount = 2,
			.pAttachments = views,
			.width = SCREEN_WIDTH,
			.height = SCREEN_HEIGHT,
			.layers = 1,
		};
		if (!ok("vkCreateFramebuffer",
				vkCreateFramebuffer(state.device, &framebuffer_info, NULL, &state.framebuffers[i])))
			return false;
	}
	return true;
}

// Set 0 holds a texture; set 1 the two dynamic uniform buffers uniforms_1000 reads.
static bool create_layouts(void)
{
	const VkDescriptorSetLayoutBinding texture = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	};
	const VkDescriptorSetLayoutBinding uniforms[2] = {
		{ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL },
		{ 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
	};
	const VkDescriptorSetLayoutCreateInfo texture_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &texture,
	};
	const VkDescriptorSetLayoutCreateInfo uniform_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 2,
		.pBindings = uniforms,
	};
	if (!ok("vkCreateDescriptorSetLayout",
			vkCreateDescriptorSetLayout(state.device, &texture_info, NULL, &state.texture_set_layout)) ||
		!ok("vkCreateDescriptorSetLayout",
			vkCreateDescriptorSetLayout(state.device, &uniform_info, NULL, &state.uniform_set_layout)))
		return false;
	// One range a stage, since each covers different bytes.
	const VkPushConstantRange ranges[2] = {
		{ VK_SHADER_STAGE_VERTEX_BIT, 0, MVP_BYTES },
		{ VK_SHADER_STAGE_FRAGMENT_BIT, MVP_BYTES, TINT_BYTES },
	};
	const VkPipelineLayoutCreateInfo push_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &state.texture_set_layout,
		.pushConstantRangeCount = 2,
		.pPushConstantRanges = ranges,
	};
	const VkDescriptorSetLayout sets[2] = { state.texture_set_layout, state.uniform_set_layout };
	const VkPipelineLayoutCreateInfo uniform_layout_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 2,
		.pSetLayouts = sets,
	};
	return ok("vkCreatePipelineLayout", vkCreatePipelineLayout(state.device, &push_info, NULL, &state.push_layout)) &&
		ok("vkCreatePipelineLayout",
			vkCreatePipelineLayout(state.device, &uniform_layout_info, NULL, &state.uniform_layout));
}

static bool create_module(const uint32_t *spirv, uint32_t size_bytes, VkShaderModule *module)
{
	const VkShaderModuleCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = size_bytes,
		.pCode = spirv,
	};
	return ok("vkCreateShaderModule", vkCreateShaderModule(state.device, &info, NULL, module));
}

// A pipeline's vertices: the cube's, the sprites', or the cube's with each instance's DrawItem at the instance's rate.
typedef enum { INPUT_MESH, INPUT_SPRITE, INPUT_INSTANCES } VertexInput;

typedef struct {
	const uint32_t *vertex_spirv;
	uint32_t vertex_bytes;
	const uint32_t *fragment_spirv;
	uint32_t fragment_bytes;
	VkPipelineLayout layout;
	VertexInput input;
	bool blend;
	bool depth;
} PipelineSpec;

// The cube's binding and attributes, then the instances': the matrix's columns and the tint.
static const VkVertexInputBindingDescription mesh_bindings[] = {
	{ 0, sizeof(MeshVertex), VK_VERTEX_INPUT_RATE_VERTEX },
	{ 1, sizeof(DrawItem), VK_VERTEX_INPUT_RATE_INSTANCE },
};
static const VkVertexInputAttributeDescription mesh_attributes[] = {
	{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
	{ 1, 0, VK_FORMAT_R32G32_SFLOAT, 12 },
	{ 2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0 },
	{ 3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16 },
	{ 4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32 },
	{ 5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 48 },
	{ 6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(DrawItem, tint) },
};
static const VkVertexInputBindingDescription sprite_binding = { 0, sizeof(SpriteVertex), VK_VERTEX_INPUT_RATE_VERTEX };
static const VkVertexInputAttributeDescription sprite_attributes[] = {
	{ 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 },
	{ 1, 0, VK_FORMAT_R32G32_SFLOAT, 8 },
	{ 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16 },
};

static VkResult create_pipeline_from(const PipelineSpec *spec, const VkShaderModule modules[2], VkPipeline *pipeline)
{
	const bool sprite = spec->input == INPUT_SPRITE;
	// The cube's own binding and attributes, or those with the instances' after them.
	const bool instances = spec->input == INPUT_INSTANCES;
	const uint32_t mesh_binding_count = instances ? 2 : 1;
	const uint32_t mesh_attribute_count = instances ? 7 : 2;
	const VkPipelineVertexInputStateCreateInfo input = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount = sprite ? 1 : mesh_binding_count,
		.pVertexBindingDescriptions = sprite ? &sprite_binding : mesh_bindings,
		.vertexAttributeDescriptionCount = sprite ? 3 : mesh_attribute_count,
		.pVertexAttributeDescriptions = sprite ? sprite_attributes : mesh_attributes,
	};
	const VkPipelineInputAssemblyStateCreateInfo assembly = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	};
	const VkViewport viewport = { 0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT, 0.0f, 1.0f };
	const VkRect2D scissor = { { 0, 0 }, { SCREEN_WIDTH, SCREEN_HEIGHT } };
	const VkPipelineViewportStateCreateInfo viewport_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.pViewports = &viewport,
		.scissorCount = 1,
		.pScissors = &scissor,
	};
	const VkPipelineRasterizationStateCreateInfo raster = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.lineWidth = 1.0f,
	};
	const VkPipelineMultisampleStateCreateInfo multisample = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};
	const VkPipelineDepthStencilStateCreateInfo depth = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = spec->depth,
		.depthWriteEnable = spec->depth,
		.depthCompareOp = VK_COMPARE_OP_LESS,
	};
	const VkPipelineColorBlendAttachmentState attachment = {
		.blendEnable = spec->blend,
		.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
		.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.colorBlendOp = VK_BLEND_OP_ADD,
		.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
		.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.alphaBlendOp = VK_BLEND_OP_ADD,
		.colorWriteMask =
			VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	};
	const VkPipelineColorBlendStateCreateInfo blend = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &attachment,
	};
	const VkPipelineShaderStageCreateInfo stages[2] = {
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
		.pInputAssemblyState = &assembly,
		.pViewportState = &viewport_state,
		.pRasterizationState = &raster,
		.pMultisampleState = &multisample,
		.pDepthStencilState = &depth,
		.pColorBlendState = &blend,
		.layout = spec->layout,
		.renderPass = state.render_pass,
	};
	return vkCreateGraphicsPipelines(state.device, VK_NULL_HANDLE, 1, &info, NULL, pipeline);
}

static bool create_pipeline(const PipelineSpec *spec, VkPipeline *pipeline)
{
	VkShaderModule modules[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
	bool made = create_module(spec->vertex_spirv, spec->vertex_bytes, &modules[0]) &&
		create_module(spec->fragment_spirv, spec->fragment_bytes, &modules[1]);
	made = made && ok("vkCreateGraphicsPipelines", create_pipeline_from(spec, modules, pipeline));
	vkDestroyShaderModule(state.device, modules[0], NULL);
	vkDestroyShaderModule(state.device, modules[1], NULL);
	return made;
}

static bool create_pipelines(void)
{
	const PipelineSpec specs[PIPELINES] = {
		[PIPELINE_MESH] = { mesh_vert_spirv, MESH_VERT_SPIRV_BYTES, mesh_frag_spirv, MESH_FRAG_SPIRV_BYTES,
			state.push_layout, INPUT_MESH, false, true },
		[PIPELINE_MESH_BLEND] = { mesh_vert_spirv, MESH_VERT_SPIRV_BYTES, mesh_frag_spirv, MESH_FRAG_SPIRV_BYTES,
			state.push_layout, INPUT_MESH, true, true },
		[PIPELINE_GRAY] = { mesh_vert_spirv, MESH_VERT_SPIRV_BYTES, mesh_gray_frag_spirv, MESH_GRAY_FRAG_SPIRV_BYTES,
			state.push_layout, INPUT_MESH, false, true },
		[PIPELINE_GRAY_BLEND] = { mesh_vert_spirv, MESH_VERT_SPIRV_BYTES, mesh_gray_frag_spirv,
			MESH_GRAY_FRAG_SPIRV_BYTES, state.push_layout, INPUT_MESH, true, true },
		[PIPELINE_UNIFORM] = { mesh_uniform_vert_spirv, MESH_UNIFORM_VERT_SPIRV_BYTES, mesh_uniform_frag_spirv,
			MESH_UNIFORM_FRAG_SPIRV_BYTES, state.uniform_layout, INPUT_MESH, false, true },
		[PIPELINE_SPRITE] = { sprite_vert_spirv, SPRITE_VERT_SPIRV_BYTES, sprite_frag_spirv, SPRITE_FRAG_SPIRV_BYTES,
			state.push_layout, INPUT_SPRITE, true, false },
		// draws_4000's state, the texture times each instance's tint through sprite.frag.
		[PIPELINE_INSTANCED] = { mesh_instanced_vert_spirv, MESH_INSTANCED_VERT_SPIRV_BYTES, sprite_frag_spirv,
			SPRITE_FRAG_SPIRV_BYTES, state.push_layout, INPUT_INSTANCES, false, true },
	};
	for (uint32_t i = 0; i < PIPELINES; i++) {
		if (!create_pipeline(&specs[i], &state.pipelines[i]))
			return false;
	}
	return true;
}

static bool create_buffer(VkBufferUsageFlags usage, uint32_t size_bytes, const void *data, Buffer *buffer)
{
	const VkBufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size_bytes,
		.usage = usage,
	};
	if (!ok("vkCreateBuffer", vkCreateBuffer(state.device, &info, NULL, &buffer->buffer)))
		return false;
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(state.device, buffer->buffer, &requirements);
	const VkMemoryAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
	};
	if (!ok("vkAllocateMemory", vkAllocateMemory(state.device, &allocate_info, NULL, &buffer->memory)) ||
		!ok("vkBindBufferMemory", vkBindBufferMemory(state.device, buffer->buffer, buffer->memory, 0)) ||
		!ok("vkMapMemory", vkMapMemory(state.device, buffer->memory, 0, VK_WHOLE_SIZE, 0, &buffer->mapped)))
		return false;
	if (data)
		memcpy(buffer->mapped, data, size_bytes);
	return true;
}

static bool create_buffers(void)
{
	MeshVertex cube[CUBE_VERTEX_COUNT];
	uint16_t cube_indices[CUBE_INDEX_COUNT];
	static uint16_t quad_indices[MAX_SPRITES * QUAD_INDICES];
	SpriteVertex fullscreen[FULLSCREEN_VERTICES];
	assets_build_cube(cube, cube_indices);
	assets_build_quad_indices(quad_indices, MAX_SPRITES);
	assets_build_fullscreen_quad(fullscreen);
	return create_buffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, sizeof cube, cube, &state.cube_vertices) &&
		create_buffer(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, sizeof cube_indices, cube_indices, &state.cube_indices) &&
		create_buffer(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, QUAD_INDEX_BYTES, quad_indices, &state.quad_indices) &&
		create_buffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, sizeof fullscreen, fullscreen, &state.fullscreen) &&
		create_buffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, STREAM_BYTES, NULL, &state.stream) &&
		create_buffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, INSTANCE_BYTES, NULL, &state.instances) &&
		create_buffer(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, UNIFORM_BYTES, NULL, &state.uniforms);
}

static void transition(VkImage image, VkImageLayout from, VkImageLayout to)
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
	vkCmdPipelineBarrier(state.commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
		NULL, 0, NULL, 1, &barrier);
}

// One submission copies each texture in from a staging buffer.
static bool upload_textures(void)
{
	static uint8_t pixels[TEXTURE_COUNT * TEXTURE_BYTES];
	for (int i = 0; i < TEXTURE_COUNT; i++)
		assets_build_texture(&pixels[i * TEXTURE_BYTES], i);
	Buffer staging = { VK_NULL_HANDLE, VK_NULL_HANDLE, NULL };
	bool done = create_buffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, sizeof pixels, pixels, &staging);
	const VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	done = done && ok("vkBeginCommandBuffer", vkBeginCommandBuffer(state.commands, &begin));
	for (int i = 0; i < TEXTURE_COUNT && done; i++) {
		const VkBufferImageCopy region = {
			.bufferOffset = (VkDeviceSize)i * TEXTURE_BYTES,
			.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.imageExtent = { TEXTURE_SIZE, TEXTURE_SIZE, 1 },
		};
		transition(state.textures[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		vkCmdCopyBufferToImage(
			state.commands, staging.buffer, state.textures[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		transition(state.textures[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
	done = done && ok("vkEndCommandBuffer", vkEndCommandBuffer(state.commands));
	const VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &state.commands
	};
	done = done && ok("vkQueueSubmit", vkQueueSubmit(state.queue, 1, &submit, state.fence)) &&
		ok("vkWaitForFences", vkWaitForFences(state.device, 1, &state.fence, VK_TRUE, UPLOAD_TIMEOUT_NS)) &&
		ok("vkResetFences", vkResetFences(state.device, 1, &state.fence));
	vkDestroyBuffer(state.device, staging.buffer, NULL);
	vkFreeMemory(state.device, staging.memory, NULL);
	return done;
}

static bool create_textures(void)
{
	for (int i = 0; i < TEXTURE_COUNT; i++) {
		if (!create_image(VK_FORMAT_R8G8B8A8_UNORM, TEXTURE_SIZE, TEXTURE_SIZE,
				VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &state.textures[i],
				&state.texture_memory[i], &state.texture_views[i]))
			return false;
	}
	const VkSamplerCreateInfo sampler_info = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_LINEAR,
		.minFilter = VK_FILTER_LINEAR,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
	};
	return ok("vkCreateSampler", vkCreateSampler(state.device, &sampler_info, NULL, &state.sampler));
}

static bool create_descriptors(void)
{
	const VkDescriptorPoolSize sizes[2] = {
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, TEXTURE_COUNT },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2 },
	};
	const VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = TEXTURE_COUNT + 1,
		.poolSizeCount = 2,
		.pPoolSizes = sizes,
	};
	if (!ok("vkCreateDescriptorPool", vkCreateDescriptorPool(state.device, &pool_info, NULL, &state.pool)))
		return false;
	const VkDescriptorSetLayout layouts[TEXTURE_COUNT + 1] = { state.texture_set_layout, state.texture_set_layout,
		state.texture_set_layout, state.texture_set_layout, state.uniform_set_layout };
	VkDescriptorSet sets[TEXTURE_COUNT + 1];
	const VkDescriptorSetAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = state.pool,
		.descriptorSetCount = TEXTURE_COUNT + 1,
		.pSetLayouts = layouts,
	};
	if (!ok("vkAllocateDescriptorSets", vkAllocateDescriptorSets(state.device, &allocate_info, sets)))
		return false;
	memcpy(state.texture_sets, sets, sizeof state.texture_sets);
	state.uniform_set = sets[TEXTURE_COUNT];
	for (int i = 0; i < TEXTURE_COUNT; i++) {
		const VkDescriptorImageInfo image = { state.sampler, state.texture_views[i],
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		const VkWriteDescriptorSet write = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = state.texture_sets[i],
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &image,
		};
		vkUpdateDescriptorSets(state.device, 1, &write, 0, NULL);
	}
	// The matrix and the tint each read from their own region, at a dynamic offset a draw.
	const VkDescriptorBufferInfo buffers[2] = {
		{ state.uniforms.buffer, 0, MVP_BYTES },
		{ state.uniforms.buffer, 0, TINT_BYTES },
	};
	const VkWriteDescriptorSet write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = state.uniform_set,
		.descriptorCount = 2,
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
		.pBufferInfo = buffers,
	};
	vkUpdateDescriptorSets(state.device, 1, &write, 0, NULL);
	return true;
}

static bool create_commands(void)
{
	const VkCommandPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	};
	if (!ok("vkCreateCommandPool", vkCreateCommandPool(state.device, &pool_info, NULL, &state.command_pool)))
		return false;
	const VkCommandBufferAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = state.command_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	const VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	return ok("vkAllocateCommandBuffers", vkAllocateCommandBuffers(state.device, &allocate_info, &state.commands)) &&
		ok("vkCreateFence", vkCreateFence(state.device, &fence_info, NULL, &state.fence));
}

static bool acquire(void)
{
	return headless() ||
		ok("vkAcquireNextImageKHR",
			vkAcquireNextImageKHR(
				state.device, state.swapchain, UINT64_MAX, VK_NULL_HANDLE, VK_NULL_HANDLE, &state.image_index));
}

static bool vulkan_init(void)
{
	state = (State){ .instance = VK_NULL_HANDLE };
	if (!create_device())
		return false;
	if (!create_swapchain()) {
		xsdk_log_write("vkbench: no swapchain, so frames go into an image and show nowhere");
		if (!create_offscreen())
			return false;
	}
	return create_targets() && create_render_pass() && create_layouts() && create_pipelines() && create_buffers() &&
		create_commands() && create_textures() && upload_textures() && create_descriptors() && acquire();
}

static void vulkan_begin_frame(void)
{
	const VkCommandBufferBeginInfo begin = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	(void)ok("vkBeginCommandBuffer", vkBeginCommandBuffer(state.commands, &begin));
	const VkRenderPassBeginInfo pass = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = state.render_pass,
		.framebuffer = state.framebuffers[state.image_index],
		.renderArea = { { 0, 0 }, { SCREEN_WIDTH, SCREEN_HEIGHT } },
		.clearValueCount = 2,
		.pClearValues = clears,
	};
	vkCmdBeginRenderPass(state.commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
}

// Each draw's matrix and tint go into the uniform buffer at its own offsets, as Aurora writes its uniforms.
static void write_uniforms(const DrawItem *items, int count)
{
	uint8_t *const base = state.uniforms.mapped;
	for (int i = 0; i < count; i++) {
		memcpy(base + i * UNIFORM_STRIDE, items[i].mvp, MVP_BYTES);
		memcpy(base + TINT_REGION + i * UNIFORM_STRIDE, items[i].tint, TINT_BYTES);
	}
}

static void vulkan_draw_meshes(const DrawItem *items, int count, bool uniform_buffers)
{
	const VkCommandBuffer cmd = state.commands;
	const VkDeviceSize offset = 0;
	const VkPipelineLayout layout = uniform_buffers ? state.uniform_layout : state.push_layout;
	int current_pipeline = -1;
	int current_texture = -1;
	if (uniform_buffers)
		write_uniforms(items, count);
	vkCmdBindVertexBuffers(cmd, 0, 1, &state.cube_vertices.buffer, &offset);
	vkCmdBindIndexBuffer(cmd, state.cube_indices.buffer, 0, VK_INDEX_TYPE_UINT16);
	for (int i = 0; i < count; i++) {
		const DrawItem *const item = &items[i];
		const int pipeline = uniform_buffers ? PIPELINE_UNIFORM : item->grayscale * 2 + item->blend;
		if (pipeline != current_pipeline) {
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state.pipelines[pipeline]);
			current_pipeline = pipeline;
		}
		if (item->texture != current_texture) {
			vkCmdBindDescriptorSets(
				cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &state.texture_sets[item->texture], 0, NULL);
			current_texture = item->texture;
		}
		if (uniform_buffers) {
			const uint32_t offsets[2] = { (uint32_t)i * UNIFORM_STRIDE, TINT_REGION + (uint32_t)i * UNIFORM_STRIDE };
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &state.uniform_set, 2, offsets);
		} else {
			vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, MVP_BYTES, item->mvp);
			vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, MVP_BYTES, TINT_BYTES, item->tint);
		}
		vkCmdDrawIndexed(cmd, CUBE_INDEX_COUNT, 1, 0, 0, 0);
	}
}

// draws_4000's cubes, each item copied whole as its instance's element, in one vkCmdDrawIndexed of them all.
static void vulkan_draw_instances(const DrawItem *items, int count)
{
	const VkCommandBuffer cmd = state.commands;
	const VkBuffer buffers[2] = { state.cube_vertices.buffer, state.instances.buffer };
	const VkDeviceSize offsets[2] = { 0, 0 };
	// The GPU is done with the instances: each frame waits for its fence.
	memcpy(state.instances.mapped, items, (size_t)count * sizeof(DrawItem));
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state.pipelines[PIPELINE_INSTANCED]);
	vkCmdBindDescriptorSets(
		cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state.push_layout, 0, 1, &state.texture_sets[0], 0, NULL);
	vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
	vkCmdBindIndexBuffer(cmd, state.cube_indices.buffer, 0, VK_INDEX_TYPE_UINT16);
	vkCmdDrawIndexed(cmd, CUBE_INDEX_COUNT, (uint32_t)count, 0, 0, 0);
}

static void vulkan_draw_sprites(const SpriteVertex *vertices, int sprite_count)
{
	const VkCommandBuffer cmd = state.commands;
	const VkDeviceSize offset = 0;
	// The GPU is done with the stream: each frame waits for its fence.
	memcpy(state.stream.mapped, vertices, (size_t)sprite_count * 4 * sizeof(SpriteVertex));
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state.pipelines[PIPELINE_SPRITE]);
	vkCmdBindDescriptorSets(
		cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state.push_layout, 0, 1, &state.texture_sets[0], 0, NULL);
	vkCmdBindVertexBuffers(cmd, 0, 1, &state.stream.buffer, &offset);
	vkCmdBindIndexBuffer(cmd, state.quad_indices.buffer, 0, VK_INDEX_TYPE_UINT16);
	vkCmdDrawIndexed(cmd, (uint32_t)sprite_count * QUAD_INDICES, 1, 0, 0, 0);
}

static void vulkan_draw_fullscreen(int layers)
{
	const VkCommandBuffer cmd = state.commands;
	const VkDeviceSize offset = 0;
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state.pipelines[PIPELINE_SPRITE]);
	vkCmdBindDescriptorSets(
		cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, state.push_layout, 0, 1, &state.texture_sets[1], 0, NULL);
	vkCmdBindVertexBuffers(cmd, 0, 1, &state.fullscreen.buffer, &offset);
	vkCmdBindIndexBuffer(cmd, state.quad_indices.buffer, 0, VK_INDEX_TYPE_UINT16);
	for (int i = 0; i < layers; i++)
		vkCmdDrawIndexed(cmd, QUAD_INDICES, 1, 0, 0, 0);
}

// The layer replays the commands into the ring here, and its fence kicks them.
static void vulkan_submit_frame(void)
{
	vkCmdEndRenderPass(state.commands);
	(void)ok("vkEndCommandBuffer", vkEndCommandBuffer(state.commands));
	const VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &state.commands
	};
	(void)ok("vkQueueSubmit", vkQueueSubmit(state.queue, 1, &submit, state.fence));
}

static bool vulkan_wait_frame(void)
{
	const uint64_t start = bench_now_ns();
	VkResult status;
	do
		status = vkGetFenceStatus(state.device, state.fence);
	while (status == VK_NOT_READY && bench_now_ns() - start <= (uint64_t)FRAME_TIMEOUT_MS * 1000000);
	if (status == VK_NOT_READY)
		return false;
	(void)ok("vkGetFenceStatus", status);
	(void)ok("vkResetFences", vkResetFences(state.device, 1, &state.fence));
	return true;
}

static void vulkan_present_frame(void)
{
	state.presented_index = state.image_index;
	if (headless())
		return;
	const VkPresentInfoKHR present = {
		.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.swapchainCount = 1,
		.pSwapchains = &state.swapchain,
		.pImageIndices = &state.image_index,
	};
	(void)ok("vkQueuePresentKHR", vkQueuePresentKHR(state.queue, &present));
	(void)acquire();
}

// The swapchain's images are xsdk_present's front buffers, in its order (docs/SUPPORT.md).
static volatile const uint32_t *vulkan_presented_pixels(void)
{
	if (headless())
		return state.offscreen_pixels;
	return xsdk_present_pixels(xsdk_present_buffer_address(state.presented_index));
}

static uint32_t vulkan_finish(void)
{
	(void)ok("vkDeviceWaitIdle", vkDeviceWaitIdle(state.device));
	const uint32_t errors = state.errors;
	state.errors = 0;
	return errors;
}

static void destroy_buffer(Buffer *buffer)
{
	vkDestroyBuffer(state.device, buffer->buffer, NULL);
	vkFreeMemory(state.device, buffer->memory, NULL);
}

static void vulkan_shutdown(void)
{
	if (!state.device) {
		vkDestroyInstance(state.instance, NULL);
		return;
	}
	(void)vkDeviceWaitIdle(state.device);
	const VkDevice device = state.device;
	vkDestroyFence(device, state.fence, NULL);
	vkDestroyCommandPool(device, state.command_pool, NULL);
	vkDestroyDescriptorPool(device, state.pool, NULL);
	vkDestroySampler(device, state.sampler, NULL);
	for (int i = 0; i < TEXTURE_COUNT; i++) {
		vkDestroyImageView(device, state.texture_views[i], NULL);
		vkDestroyImage(device, state.textures[i], NULL);
		vkFreeMemory(device, state.texture_memory[i], NULL);
	}
	destroy_buffer(&state.uniforms);
	destroy_buffer(&state.instances);
	destroy_buffer(&state.stream);
	destroy_buffer(&state.fullscreen);
	destroy_buffer(&state.quad_indices);
	destroy_buffer(&state.cube_indices);
	destroy_buffer(&state.cube_vertices);
	for (int i = 0; i < PIPELINES; i++)
		vkDestroyPipeline(device, state.pipelines[i], NULL);
	vkDestroyPipelineLayout(device, state.uniform_layout, NULL);
	vkDestroyPipelineLayout(device, state.push_layout, NULL);
	vkDestroyDescriptorSetLayout(device, state.uniform_set_layout, NULL);
	vkDestroyDescriptorSetLayout(device, state.texture_set_layout, NULL);
	for (uint32_t i = 0; i < MAX_IMAGES; i++) {
		vkDestroyFramebuffer(device, state.framebuffers[i], NULL);
		vkDestroyImageView(device, state.views[i], NULL);
	}
	vkDestroyRenderPass(device, state.render_pass, NULL);
	vkDestroyImageView(device, state.depth_view, NULL);
	vkDestroyImage(device, state.depth, NULL);
	vkFreeMemory(device, state.depth_memory, NULL);
	vkDestroyImage(device, state.offscreen, NULL);
	vkFreeMemory(device, state.offscreen_memory, NULL);
	vkDestroySwapchainKHR(device, state.swapchain, NULL);
	vkDestroySurfaceKHR(state.instance, state.surface, NULL);
	vkDestroyDevice(device, NULL);
	vkDestroyInstance(state.instance, NULL);
	state.device = VK_NULL_HANDLE;
}

const BenchBackend bench_vulkan_backend = {
	.name = "vulkan4xenon",
	.init = vulkan_init,
	.begin_frame = vulkan_begin_frame,
	.draw_meshes = vulkan_draw_meshes,
	.draw_instances = vulkan_draw_instances,
	.draw_sprites = vulkan_draw_sprites,
	.draw_fullscreen = vulkan_draw_fullscreen,
	.submit_frame = vulkan_submit_frame,
	.wait_frame = vulkan_wait_frame,
	.present_frame = vulkan_present_frame,
	.presented_pixels = vulkan_presented_pixels,
	.finish = vulkan_finish,
	.shutdown = vulkan_shutdown,
};
