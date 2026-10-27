#include <stdbool.h>
#include <stdint.h>

#include "harness.h"
#include "tests.h"

enum {
	// Odd on purpose, so the requirement's rounding shows.
	BUFFER_BYTES = 100,
	BUFFER_ALIGNMENT = 64,
	ROUNDED_BYTES = 128,
	// A buffer only copied to and from, so a copy can draw its rows as a texture.
	TRANSFER_ALIGNMENT = 4096,
};

static const uint32_t unknown_spirv[] = { 0x07230203, 0x00010000, 0, 1, 0 };

static void check_proc_addresses(const Harness *harness)
{
	check("objects: vkGetInstanceProcAddr finds global functions without an instance",
		vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance") == (PFN_vkVoidFunction)vkCreateInstance);
	check("objects: vkGetInstanceProcAddr finds instance functions only with an instance",
		!vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateDevice") &&
			vkGetInstanceProcAddr(harness->instance, "vkCreateDevice") == (PFN_vkVoidFunction)vkCreateDevice);
	check("objects: vkGetDeviceProcAddr finds device functions and not instance ones",
		vkGetDeviceProcAddr(harness->device, "vkCmdDraw") == (PFN_vkVoidFunction)vkCmdDraw &&
			!vkGetDeviceProcAddr(harness->device, "vkCreateDevice"));
	check("objects: an unknown name finds nothing", !vkGetInstanceProcAddr(harness->instance, "vkCmdDrawMeshTasksEXT"));
}

static void check_memory(const Harness *harness)
{
	VkPhysicalDeviceMemoryProperties memory;
	vkGetPhysicalDeviceMemoryProperties(harness->physical_device, &memory);
	const VkMemoryPropertyFlags gpu = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	const VkMemoryPropertyFlags cached = gpu | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
	check("objects: two memory types in one device-local heap, both device-local, the GPU's, then the CPU's cached",
		memory.memoryTypeCount == 2 && memory.memoryTypes[0].propertyFlags == gpu &&
			memory.memoryTypes[1].propertyFlags == cached && memory.memoryTypes[1].heapIndex == 0 &&
			memory.memoryHeapCount == 1 && memory.memoryHeaps[0].flags == VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);
	const VkBufferUsageFlags usages[] = { VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
	VkMemoryRequirements requirements[3] = { { 0 } };
	bool made = true;
	for (uint32_t i = 0; i < 3; i++) {
		const VkBufferCreateInfo info = {
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = BUFFER_BYTES,
			.usage = usages[i],
		};
		VkBuffer buffer;
		const bool this_made = harness_ok("vkCreateBuffer", vkCreateBuffer(harness->device, &info, NULL, &buffer));
		if (this_made) {
			vkGetBufferMemoryRequirements(harness->device, buffer, &requirements[i]);
			vkDestroyBuffer(harness->device, buffer, NULL);
		}
		made = made && this_made;
	}
	check("objects: a buffer's requirements round its size up to its alignment",
		made && requirements[0].size == ROUNDED_BYTES && requirements[0].alignment == BUFFER_ALIGNMENT);
	check("objects: the cached memory type is offered to buffers only copied to and from",
		made && requirements[0].memoryTypeBits == 1 && requirements[1].memoryTypeBits == 3 &&
			requirements[2].memoryTypeBits == 3);
	check("objects: buffers only copied to and from are 4 KB aligned",
		made && requirements[1].alignment == TRANSFER_ALIGNMENT && requirements[2].alignment == TRANSFER_ALIGNMENT);
}

static void check_refusals(const Harness *harness)
{
	const VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
	const VkInstanceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	VkInstance instance;
	check("objects: an instance for Vulkan 1.1 is refused",
		vkCreateInstance(&info, NULL, &instance) == VK_ERROR_INCOMPATIBLE_DRIVER);
	VkShaderModule module;
	check("objects: SPIR-V the bundle doesn't hold and xost can't compile is refused",
		harness_shader(harness, unknown_spirv, sizeof unknown_spirv, &module) == VK_ERROR_FEATURE_NOT_PRESENT);
}

// A colour attachment 16 texels or less on a side has its base at the corner, where a resolve writes, at one sample
// alone (milestone 31).
static void check_attachments(const Harness *harness)
{
	VkFormatProperties properties;
	vkGetPhysicalDeviceFormatProperties(harness->physical_device, VK_FORMAT_R8G8B8A8_UNORM, &properties);
	const VkFormatFeatureFlags wanted =
		VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
	VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = { 16, 64, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
	};
	VkImage image = VK_NULL_HANDLE;
	const bool narrow = vkCreateImage(harness->device, &info, NULL, &image) == VK_SUCCESS;
	vkDestroyImage(harness->device, image, NULL);
	info.samples = VK_SAMPLE_COUNT_2_BIT;
	const bool narrow_multisampled =
		vkCreateImage(harness->device, &info, NULL, &image) == VK_ERROR_FEATURE_NOT_PRESENT;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.extent = (VkExtent3D){ 64, 64, 1 };
	info.mipLevels = 2;
	const bool mipped = vkCreateImage(harness->device, &info, NULL, &image) == VK_ERROR_FEATURE_NOT_PRESENT;
	check(
		"objects: R8G8B8A8_UNORM is a blendable colour attachment, 16 texels across at one sample, but not at 2 "
		"samples or with mips",
		(properties.optimalTilingFeatures & wanted) == wanted && narrow && narrow_multisampled && mipped);
	const VkAttachmentDescription attachments[2] = {
		{ .format = VK_FORMAT_B8G8R8A8_UNORM,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE },
		{ .format = VK_FORMAT_D24_UNORM_S8_UINT,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD },
	};
	const VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	const VkAttachmentReference depth = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	const VkSubpassDescription subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &color,
		.pDepthStencilAttachment = &depth,
	};
	const VkRenderPassCreateInfo pass_info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 2,
		.pAttachments = attachments,
		.subpassCount = 1,
		.pSubpasses = &subpass,
	};
	VkRenderPass render_pass;
	check("objects: a render pass that clears depth and loads stencil is refused",
		vkCreateRenderPass(harness->device, &pass_info, NULL, &render_pass) == VK_ERROR_FEATURE_NOT_PRESENT);
}

void test_objects(const Harness *harness)
{
	check_proc_addresses(harness);
	check_memory(harness);
	check_refusals(harness);
	check_attachments(harness);
}
