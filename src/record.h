#ifndef V4X_RECORD_H
#define V4X_RECORD_H

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#include "objects.h"

// A command buffer's draws as the GPU's packets, built as they're recorded (docs/COMMAND_BUFFERS.md): a run of packets
// between two of the commands the submit replays, which the submit copies into xsdk's stream, working out then the
// hierarchical buffer's control and the uniform buffers' words the GPU can't load.

// A recorder, its lists empty with room to start, 16-byte aligned; NULL where there's no memory.
Recorder *v4x_record_new(const VkAllocationCallbacks *allocator);
void v4x_record_delete(Recorder *recorder);
// Empties the lists, keeping their room, and forgets what the GPU holds.
void v4x_record_reset(Recorder *recorder);
// Ends the run being recorded, giving its index when it holds packets, and begins the next, knowing nothing of what
// the GPU holds, as the command after it may change anything.
bool v4x_record_close(Recorder *recorder, uint32_t *run);
// A draw with what's bound, which must be a pipeline in a render pass. A queue is given at the submit alone, where an
// indexed draw of instances may repeat its indices (VkQueue_T, scratch); while recording, the command buffer keeps
// such draws as snapshots instead.
void v4x_record_draw(Recorder *recorder, const Bound *bound, uint32_t vertex_count, uint32_t first_vertex,
	uint32_t instance_count, uint32_t first_instance);
void v4x_record_draw_indexed(Recorder *recorder, const Bound *bound, VkQueue queue, uint32_t index_count,
	uint32_t first_index, int32_t vertex_offset, uint32_t instance_count, uint32_t first_instance);
// Whether an indexed draw repeats its indices for its instances, which needs them at the submit.
bool v4x_record_repeats_indices(const Bound *bound, uint32_t index_count, uint32_t instance_count);
// The most a draw with the pipeline writes but for its draw packets' pieces; 0 where that's past what a run's piece
// holds, about 8,000 dwords.
uint32_t v4x_record_pipeline_dwords(const Pipeline *pipeline);
// Copies a run into xsdk's stream, or the tiled pass's recording, as it is now: its hierarchical buffer's controls
// worked out from xsdk's, which takes the state they leave, and its uniform buffers read. Marks the images it samples
// as used by submission serial; returns its draws.
uint32_t v4x_record_copy(const Recorder *recorder, const Run *run, uint32_t serial);

#endif
