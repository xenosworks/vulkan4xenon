#include <string.h>

#include <vulkan/vulkan.h>

#include "format.h"
#include "host.h"
#include "image.h"
#include "objects.h"
#include "xsdk/gpu.h"
#include "xsdk/texture.h"

enum {
	// A budget for the layer, half the console's 512 MB that the CPU and GPU share; not a measurement.
	HEAP_BYTES = 256u << 20,
	// xsdk_gpu_alloc hands out 64 KB pages, so a mapping starts on one.
	MAP_ALIGNMENT = 64 << 10,
	// The CPU's cache line.
	CACHE_LINE_BYTES = 128,
	VIEWPORT_MAX = V4X_MAX_FRAMEBUFFER_SIDE,
	// Every register of a fragment shader's constants, the 252 below the group xsdk keeps (xsdk:include/xsdk/gpu.h).
	MAX_UNIFORM_BUFFER_BYTES = XSDK_PIXEL_CONSTANTS * 16,
	// Vulkan 1.0's minimums, which descriptor sets in host memory meet.
	MIN_UNIFORM_BUFFERS_PER_STAGE = 12,
	MIN_UNIFORM_BUFFERS_PER_SET = 72,
	MIN_DYNAMIC_UNIFORM_BUFFERS_PER_SET = 8,
	MIN_SAMPLERS_PER_SET = 96,
	MIN_SAMPLER_ALLOCATIONS = 4000,
	// A level for each halving down to 1x1 (xsdk:src/texture.c, level_count).
	MAX_IMAGE_LEVELS = 14,
	// A 3D texture's sides go to 2048 and its depth to 1024 (xsdk:include/xsdk/texture.h); one limit takes all three.
	MAX_IMAGE_3D = XSDK_TEXTURE_3D_MAX_DEPTH,
	MAX_LOD_BIAS = 15,
	// xsdk sets VGT_MAX_VTX_INDX to 24 bits (xsdk:src/gpu_draw.c).
	MAX_INDEX_VALUE = 0x00FFFFFF,
	// The GPU clock's 64 bits, which time stamps copy (xsdk:docs/HARDWARE.md, "Queries and events").
	TIMESTAMP_BITS = 64,
	// A point's radius in PA_SU_POINT_MINMAX: 12.4 fixed point, 16 bits.
	MAX_POINT_RADIUS = 0xFFFF,
	SUBPIXELS = 16,
};

// Nanoseconds a tick of the GPU's clock, which counts 498.75 a microsecond against the CPU's
// (xsdk:docs/HARDWARE.md, "Queries and events").
static const float timestamp_period_ns = 1000.0f / 498.75f;

static const VkExtensionProperties device_extensions[] = {
	{ VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_SWAPCHAIN_SPEC_VERSION },
	{ VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_SPEC_VERSION },
};

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(
	VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties *pProperties)
{
	(void)physicalDevice;
	*pProperties = (VkPhysicalDeviceProperties){
		.apiVersion = VK_MAKE_API_VERSION(0, 1, 0, VK_HEADER_VERSION),
		.driverVersion = 1,
		.deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
		.deviceName = "Xenos (vulkan4xenon)",
		.limits = {
			.maxImageDimension2D = XSDK_TEXTURE_MAX_SIZE,
			.maxImageDimension3D = MAX_IMAGE_3D,
			.maxImageDimensionCube = XSDK_TEXTURE_MAX_SIZE,
			// A cube's; a 2D image has one layer (vkGetPhysicalDeviceImageFormatProperties).
			.maxImageArrayLayers = XSDK_CUBE_FACES,
			.maxUniformBufferRange = MAX_UNIFORM_BUFFER_BYTES,
			.maxPushConstantsSize = V4X_MAX_PUSH_CONSTANT_BYTES,
			.maxMemoryAllocationCount = HEAP_BYTES / MAP_ALIGNMENT,
			.maxSamplerAllocationCount = MIN_SAMPLER_ALLOCATIONS,
			.bufferImageGranularity = 1,
			.maxBoundDescriptorSets = V4X_MAX_DESCRIPTOR_SETS,
			.maxPerStageDescriptorSamplers = V4X_MAX_SAMPLERS,
			.maxPerStageDescriptorUniformBuffers = MIN_UNIFORM_BUFFERS_PER_STAGE,
			.maxPerStageDescriptorSampledImages = V4X_MAX_SAMPLERS,
			.maxPerStageResources = MIN_UNIFORM_BUFFERS_PER_STAGE + V4X_MAX_SAMPLERS,
			.maxDescriptorSetSamplers = MIN_SAMPLERS_PER_SET,
			.maxDescriptorSetUniformBuffers = MIN_UNIFORM_BUFFERS_PER_SET,
			.maxDescriptorSetUniformBuffersDynamic = MIN_DYNAMIC_UNIFORM_BUFFERS_PER_SET,
			.maxDescriptorSetSampledImages = MIN_SAMPLERS_PER_SET,
			.maxVertexInputAttributes = V4X_MAX_VERTEX_ATTRIBUTES,
			.maxVertexInputBindings = V4X_MAX_VERTEX_BINDINGS,
			.maxVertexInputAttributeOffset = V4X_MAX_VERTEX_STRIDE_BYTES - 4,
			.maxVertexInputBindingStride = V4X_MAX_VERTEX_STRIDE_BYTES,
			// 16 interpolators of four floats (xost:docs/xsc.md, "Interface conventions").
			.maxVertexOutputComponents = 64,
			.maxFragmentInputComponents = 64,
			.maxFragmentOutputAttachments = V4X_MAX_COLOR_ATTACHMENTS,
			.maxFragmentCombinedOutputResources = V4X_MAX_COLOR_ATTACHMENTS,
			.maxViewports = 1,
			.maxViewportDimensions = { VIEWPORT_MAX, VIEWPORT_MAX },
			.viewportBoundsRange = { -2.0f * VIEWPORT_MAX, 2.0f * VIEWPORT_MAX - 1.0f },
			// Vertices land on 1/16 pixel under xsdk's PA_SU_VTX_CNTL (xenia:src/xenia/gpu/xenos.h:877-884).
			.subPixelPrecisionBits = 4,
			.maxDrawIndexedIndexValue = MAX_INDEX_VALUE,
			// No multiDrawIndirect: an indirect command draws once.
			.maxDrawIndirectCount = 1,
			.maxSamplerLodBias = MAX_LOD_BIAS,
			.maxSamplerAnisotropy = XSDK_MAX_ANISOTROPY,
			.minMemoryMapAlignment = MAP_ALIGNMENT,
			.minTexelBufferOffsetAlignment = 256,
			.minUniformBufferOffsetAlignment = 256,
			.minStorageBufferOffsetAlignment = 256,
			.maxFramebufferWidth = V4X_MAX_FRAMEBUFFER_SIDE,
			.maxFramebufferHeight = V4X_MAX_FRAMEBUFFER_SIDE,
			.maxFramebufferLayers = 1,
			// The float formats take 1 alone (docs/SUPPORT.md, Physical device); Xenos samples no multisampled texture.
			.framebufferColorSampleCounts = V4X_SAMPLE_COUNTS,
			.framebufferDepthSampleCounts = V4X_SAMPLE_COUNTS,
			.framebufferStencilSampleCounts = V4X_SAMPLE_COUNTS,
			.maxColorAttachments = V4X_MAX_COLOR_ATTACHMENTS,
			.sampledImageColorSampleCounts = VK_SAMPLE_COUNT_1_BIT,
			.discreteQueuePriorities = 2,
			// Xenos's six user clip planes take the clip distances; it has no cull distances (milestone 26).
			.maxClipDistances = XSDK_CLIP_PLANES,
			.maxCombinedClipAndCullDistances = XSDK_CLIP_PLANES,
			// A vertex shader's point size is clamped to xsdk's PA_SU_POINT_MINMAX, radii of 0 to 0xFFFF sixteenths of
			// a pixel (xenia:src/xenia/gpu/registers.h:433-442): widths in eighths up to 8191.875 (milestone 26).
			.pointSizeRange = { 0.0f, MAX_POINT_RADIUS * 2.0f / SUBPIXELS },
			.lineWidthRange = { 1.0f, 1.0f },
			.pointSizeGranularity = 2.0f / SUBPIXELS,
			// Xenos's samples sit elsewhere (xsdk:docs/HARDWARE.md, "Multisampled formats and targets, milestone 44 step 3").
			.standardSampleLocations = VK_FALSE,
			.optimalBufferCopyOffsetAlignment = 1,
			.optimalBufferCopyRowPitchAlignment = 1,
			.nonCoherentAtomSize = CACHE_LINE_BYTES,
			.timestampComputeAndGraphics = VK_TRUE,
			.timestampPeriod = timestamp_period_ns,
		},
	};
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures(
	VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures *pFeatures)
{
	(void)physicalDevice;
	// Indirect draws read firstInstance at replay, as direct ones take it (queue.c); each colour target blends by its
	// own RB_BLENDCONTROL (xsdk:include/xsdk/gpu.h, xsdk_gpu_set_blends); polygons draw as lines or points, points are
	// as wide as gl_PointSize, and Xenos's user clip planes take the clip distances (milestone 26).
	*pFeatures = (VkPhysicalDeviceFeatures){ .drawIndirectFirstInstance = VK_TRUE,
		.independentBlend = VK_TRUE,
		.fillModeNonSolid = VK_TRUE,
		.largePoints = VK_TRUE,
		.samplerAnisotropy = VK_TRUE,
		.shaderClipDistance = VK_TRUE };
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice,
	uint32_t *pQueueFamilyPropertyCount, VkQueueFamilyProperties *pQueueFamilyProperties)
{
	(void)physicalDevice;
	const VkQueueFamilyProperties family = {
		.queueFlags = VK_QUEUE_GRAPHICS_BIT,
		.queueCount = 1,
		.timestampValidBits = TIMESTAMP_BITS,
		.minImageTransferGranularity = { 1, 1, 1 },
	};
	(void)v4x_host_enumerate(pQueueFamilyPropertyCount, pQueueFamilyProperties, &family, 1, sizeof family);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(
	VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties *pMemoryProperties)
{
	(void)physicalDevice;
	// The GPU's write-combined memory, then the CPU's cached memory, which only transfer-source buffers take; both from
	// the one heap of the title's memory.
	*pMemoryProperties = (VkPhysicalDeviceMemoryProperties){
		.memoryTypeCount = 2,
		.memoryTypes = { { VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
								 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
							 0 },
			{ VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
					VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
				0 } },
		.memoryHeapCount = 1,
		.memoryHeaps = { { HEAP_BYTES, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT } },
	};
}

// No format has linear tiling features, which copies don't need. textureCompressionBC stays false, as it promises BC4
// to BC7 too; Vulkan has each BC format's features read here instead.
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(
	VkPhysicalDevice physicalDevice, VkFormat format, VkFormatProperties *pFormatProperties)
{
	(void)physicalDevice;
	*pFormatProperties = (VkFormatProperties){ 0 };
	const V4xFormat *const image = v4x_format(format);
	if (image)
		pFormatProperties->optimalTilingFeatures = image->features;
	// Sampled, its depth in red, filtered linearly; vulkan4xenon_test's shadow check tries the filter on the console.
	if (format == VK_FORMAT_D24_UNORM_S8_UINT)
		pFormatProperties->optimalTilingFeatures = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
			VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
	XsdkVertexElement element;
	if (v4x_format_vertex(format, &element))
		pFormatProperties->bufferFeatures = VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
}

// Cubes of the colour formats with their mips, and 3D images of one level, sampled and copied (image.c).
static VkResult cube_or_3d_properties(VkFormat format, VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage,
	VkImageCreateFlags flags, VkImageFormatProperties *properties)
{
	const bool cube = type == VK_IMAGE_TYPE_2D && flags == VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
	if (!v4x_format(format) || (!cube && (type != VK_IMAGE_TYPE_3D || flags)) || tiling != VK_IMAGE_TILING_OPTIMAL ||
		(usage & ~(VkImageUsageFlags)V4X_CUBE_AND_3D_USAGE)) {
		*properties = (VkImageFormatProperties){ 0 };
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}
	*properties = (VkImageFormatProperties){
		.maxExtent = cube ? (VkExtent3D){ XSDK_TEXTURE_MAX_SIZE, XSDK_TEXTURE_MAX_SIZE, 1 }
						  : (VkExtent3D){ MAX_IMAGE_3D, MAX_IMAGE_3D, MAX_IMAGE_3D },
		.maxMipLevels = cube ? MAX_IMAGE_LEVELS : 1,
		.maxArrayLayers = cube ? XSDK_CUBE_FACES : 1,
		.sampleCounts = VK_SAMPLE_COUNT_1_BIT,
		.maxResourceSize = HEAP_BYTES,
	};
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice physicalDevice,
	VkFormat format, VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags,
	VkImageFormatProperties *pImageFormatProperties)
{
	(void)physicalDevice;
	if (format == VK_FORMAT_D24_UNORM_S8_UINT && type == VK_IMAGE_TYPE_2D && tiling == VK_IMAGE_TILING_OPTIMAL &&
		!(usage & ~(VkImageUsageFlags)V4X_DEPTH_IMAGE_USAGE) && !flags) {
		*pImageFormatProperties = (VkImageFormatProperties){
			.maxExtent = { V4X_MAX_FRAMEBUFFER_SIDE, V4X_MAX_FRAMEBUFFER_SIDE, 1 },
			.maxMipLevels = 1,
			.maxArrayLayers = 1,
			.sampleCounts = (usage & VK_IMAGE_USAGE_SAMPLED_BIT) ? VK_SAMPLE_COUNT_1_BIT : V4X_SAMPLE_COUNTS,
			.maxResourceSize = HEAP_BYTES,
		};
		return VK_SUCCESS;
	}
	if (flags == VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT || type == VK_IMAGE_TYPE_3D)
		return cube_or_3d_properties(format, type, tiling, usage, flags, pImageFormatProperties);
	const V4xFormat *const image = v4x_format(format);
	const bool linear = tiling == VK_IMAGE_TILING_LINEAR;
	const VkImageUsageFlags usages = linear ? V4X_LINEAR_IMAGE_USAGE : V4X_IMAGE_USAGE;
	if (!image || type != VK_IMAGE_TYPE_2D || (usage & ~usages) || flags || (linear && image->compressed) ||
		((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && !v4x_attachment_format(format))) {
		*pImageFormatProperties = (VkImageFormatProperties){ 0 };
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}
	// Multisampled images are optimal attachments, resolved and never sampled (image.c).
	const bool multisampled = !linear && (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) &&
		!(usage & ~(VkImageUsageFlags)V4X_MULTISAMPLED_USAGE);
	*pImageFormatProperties = (VkImageFormatProperties){
		.maxExtent = { XSDK_TEXTURE_MAX_SIZE, XSDK_TEXTURE_MAX_SIZE, 1 },
		.maxMipLevels = linear || (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ? 1 : MAX_IMAGE_LEVELS,
		.maxArrayLayers = 1,
		.sampleCounts = multisampled ? v4x_sample_counts(format) : VK_SAMPLE_COUNT_1_BIT,
		.maxResourceSize = HEAP_BYTES,
	};
	return VK_SUCCESS;
}

// The features report no sparse residency, so no format has sparse properties.
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties(VkPhysicalDevice physicalDevice,
	VkFormat format, VkImageType type, VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageTiling tiling,
	uint32_t *pPropertyCount, VkSparseImageFormatProperties *pProperties)
{
	(void)physicalDevice;
	(void)format;
	(void)type;
	(void)samples;
	(void)usage;
	(void)tiling;
	(void)pProperties;
	*pPropertyCount = 0;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
	const char *pLayerName, uint32_t *pPropertyCount, VkExtensionProperties *pProperties)
{
	(void)physicalDevice;
	if (pLayerName)
		return VK_ERROR_LAYER_NOT_PRESENT;
	return v4x_host_enumerate(pPropertyCount, pProperties, device_extensions,
		sizeof device_extensions / sizeof device_extensions[0], sizeof device_extensions[0]);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(
	VkPhysicalDevice physicalDevice, uint32_t *pPropertyCount, VkLayerProperties *pProperties)
{
	(void)physicalDevice;
	(void)pProperties;
	*pPropertyCount = 0;
	return VK_SUCCESS;
}

bool v4x_physical_device_has_extension(const char *name)
{
	for (uint32_t i = 0; i < sizeof device_extensions / sizeof device_extensions[0]; i++) {
		if (!strcmp(name, device_extensions[i].extensionName))
			return true;
	}
	return false;
}
