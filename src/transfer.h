#ifndef V4X_TRANSFER_H
#define V4X_TRANSFER_H

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#include "objects.h"
#include "xsdk/gpu.h"

// Copies, clears and blits, which the CPU runs as it replays them (docs/ROADMAP.md, section 3), each waiting first for
// the GPU to be done with what it reads or writes.

void v4x_transfer_buffer_to_image(VkQueue queue, const LevelCopy *copy);
void v4x_transfer_image_to_buffer(VkQueue queue, const LevelCopy *copy);
void v4x_transfer_copy_image(VkQueue queue, const Command *command);
void v4x_transfer_blit(VkQueue queue, const Command *command);
void v4x_transfer_clear(VkQueue queue, const Command *command);
// A copy into a buffer from source: another buffer's bytes, or vkCmdUpdateBuffer's in the command buffer's data.
void v4x_transfer_copy_buffer(VkQueue queue, const Command *command, volatile const void *source);
void v4x_transfer_fill_buffer(VkQueue queue, const Command *command);
// A sampled depth image's texture cleared, each texel's word depth's 24 bits above stencil's 8 (docs/SUPPORT.md,
// Images).
void v4x_transfer_fill_depth(VkQueue queue, Image *image, float depth, uint8_t stencil);
// Time stamp queries marked unwritten, and their results copied into a buffer (query.c).
void v4x_transfer_reset_queries(VkQueue queue, const Command *command);
void v4x_transfer_copy_queries(VkQueue queue, const QueryCopy *copy);
// A list's count indices, 32 or 16 bits each, from the app's mapped bytes, repeated in up to 64 copies for an indexed
// instanced draw of instance_count instances, as xsdk_gpu_repeat_indices repeats them, in the queue's memory for them
// (VkQueue_T); false, writing nothing, where this submission has filled it all.
bool v4x_transfer_repeat_indices(VkQueue queue, volatile const uint8_t *from, bool is_32_bit, uint32_t count,
	uint32_t instance_count, XsdkRepeatedIndices *repeated);

#endif
