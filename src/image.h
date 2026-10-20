#ifndef V4X_IMAGE_H
#define V4X_IMAGE_H

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

enum {
	// What colour images may be used for.
	V4X_IMAGE_USAGE = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
	// A multisampled colour image is drawn into and resolved from, as Xenos samples no multisampled texture.
	V4X_MULTISAMPLED_USAGE =
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
	// The samples a target takes (xsdk:include/xsdk/gpu.h, xsdk_gpu_set_targets_samples).
	V4X_SAMPLE_COUNTS = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT,
	// Linear images are only copied to and from.
	V4X_LINEAR_IMAGE_USAGE = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	// Cube and 3D images are sampled and copied to and from, not drawn into (docs/ROADMAP.md, milestone 23).
	V4X_CUBE_AND_3D_USAGE =
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	// What depth images may be used for: attachments, sampled after a pass stores them and leaves them to be read,
	// cleared by vkCmdClearDepthStencilImage, and a multisampled one resolved by vkCmdResolveImage.
	V4X_DEPTH_IMAGE_USAGE = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT |
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	// A base level this many texels or fewer on a side sits in the packed mip tail, where a resolve can't write
	// (xsdk:src/gpu_resolve.c, xsdk_gpu_resolve_texture).
	V4X_PACKED_SIDE = 16,
};

// The xsdk format an image of this Vulkan format is, if images of it are supported.
bool v4x_image_format(VkFormat format, uint32_t *xsdk_format);
// Whether images of this format can be colour attachments: those an xsdk target draws and resolves into.
bool v4x_attachment_format(VkFormat format);
// The XSDK_TARGET_* a colour attachment of this format draws into.
uint32_t v4x_target_format(VkFormat format);
// Whether colour attachments of this format blend.
bool v4x_blend_format(VkFormat format);
// The sample counts attachments of this format take: V4X_SAMPLE_COUNTS, or 1 alone.
VkSampleCountFlags v4x_sample_counts(VkFormat format);
// The bytes in a texel of an image of this format, or in a block of a compressed one, which copies take.
uint32_t v4x_texel_bytes(VkFormat format);
// The texels or blocks a row or column of this many texels takes: a compressed format's blocks are 4 texels a side.
uint32_t v4x_texel_blocks(VkFormat format, uint32_t texels);
// Whether images of this format can be blitted: the CPU unpacks their texels to filter them (src/format.c).
bool v4x_blit_format(VkFormat format);
// Whether a shader may sample an image in this layout: a render pass that leaves a sampled depth image in one resolves
// it into its texture.
bool v4x_read_layout(VkImageLayout layout);

#endif
