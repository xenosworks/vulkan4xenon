#ifndef V4X_OBJECTS_H
#define V4X_OBJECTS_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#include "format.h"
#include "vulkan4xenon.h"
#include "xsdk/gpu.h"
#include "xsdk/shader.h"
#include "xsdk/texture.h"

// Every object the layer hands out, and the limits they're built to.

enum {
	V4X_MIN_SWAPCHAIN_IMAGES = 2,
	// xsdk_present's limit (xsdk:include/xsdk/present.h).
	V4X_MAX_SWAPCHAIN_IMAGES = 3,
	V4X_DISPLAY_WIDTH = 1280,
	V4X_DISPLAY_HEIGHT = 720,
	// Each binding takes one of xsdk's vertex streams.
	V4X_MAX_VERTEX_BINDINGS = XSDK_VERTEX_STREAMS,
	V4X_MAX_VERTEX_ATTRIBUTES = 16,
	// A fetch's stride is 8 bits of dwords (xsdk:src/shader.c, FETCH_STRIDE_MASK).
	V4X_MAX_VERTEX_STRIDE_BYTES = 255 * 4,
	// Room a command buffer starts with; it doubles when full.
	V4X_FIRST_COMMANDS = 4096,
	// DRAW_INDX_2 and DRAW_INDX carry the count in 16 bits (xenia:src/xenia/gpu/registers.h:327).
	V4X_MAX_DRAW_VERTICES = 65535,
	// Push constants, dynamic offsets and copy regions, which vary in size, go in a command buffer's data in recording
	// order; it starts with this much and doubles when full.
	V4X_FIRST_COMMAND_DATA_BYTES = 16384,
	V4X_MAX_DESCRIPTOR_SETS = 4,
	// Binding numbers run from 0 to this less one.
	V4X_MAX_DESCRIPTOR_BINDINGS = 32,
	V4X_MAX_PUSH_CONSTANT_BYTES = 128,
	// Float constant registers a stage has, each four floats (xsdk:include/xsdk/gpu.h).
	V4X_FLOAT_CONSTANTS = 256,
	// A stage's samplers, Vulkan's minimum; both stages' together share the fetch constants the streams leave
	// (pipeline.c).
	V4X_MAX_SAMPLERS = 16,
	// Hardware loops' counts a stage's shader has, a loop constant each (xost:docs/xsc.md, "Loops a block counts"), and
	// the passes a loop constant runs at most (xenia:src/xenia/gpu/xenos.h:1089).
	V4X_MAX_LOOPS = 16,
	V4X_MAX_LOOP_PASSES = 255,
	// Fetch constant slots a texture can take (xsdk:include/xsdk/gpu.h, xsdk_gpu_set_texture).
	V4X_TEXTURE_SLOTS = 32,
	// A stage's push constants, every binding, a comparison for each sampler, and its loops' counts.
	V4X_MAX_SHADER_RESOURCES =
		2 * (1 + V4X_MAX_DESCRIPTOR_SETS * V4X_MAX_DESCRIPTOR_BINDINGS + V4X_MAX_SAMPLERS + V4X_MAX_LOOPS),
	// A render pass's subpasses and attachments: a budget, not a limit of Xenos's.
	V4X_MAX_SUBPASSES = 8,
	V4X_MAX_ATTACHMENTS = 8,
	// A subpass's colour attachments, Xenos's colour targets (xsdk:include/xsdk/gpu.h, XSDK_COLOR_TARGETS).
	V4X_MAX_COLOR_ATTACHMENTS = XSDK_COLOR_TARGETS,
	// Vulkan 1.0's least maxFramebufferWidth and Height, and the viewport's; past eDRAM a subpass is a tiled pass.
	V4X_MAX_FRAMEBUFFER_SIDE = 4096,
	// shortcut: a tiled pass's commands fit 1 MB of packets, some thousand of the layer's draws; grow it when a port's
	// pass outgrows it.
	V4X_TILE_RECORDING_DWORDS = 1 << 18,
};

struct VkPhysicalDevice_T {
	VkInstance instance;
};

struct VkInstance_T {
	VkAllocationCallbacks allocator;
	bool has_allocator;
	struct VkPhysicalDevice_T physical_device;
};

typedef enum {
	EDRAM_UNKNOWN,
	// A resolve cleared it to known values, over a framebuffer of a size.
	EDRAM_CLEARED,
	// It holds an image's contents.
	EDRAM_IMAGE,
} EdramContents;

// What a colour target's tiles of eDRAM, or a depth and stencil's, hold between render passes (docs/ROADMAP.md,
// milestones 4, 25 and 27), and over which rectangle of the framebuffer, as a pass clears, loads and stores its render
// area alone; its first tile, and a colour's xsdk target format.
typedef struct {
	EdramContents contents;
	uint32_t width;
	uint32_t height;
	float rgba[4];
	float depth;
	uint32_t stencil;
	const struct Image *image;
	uint32_t tile;
	uint32_t format;
	VkRect2D area;
} EdramPart;

// The layer's own shaders and state for drawing an image back into eDRAM, for vkCmdClearAttachments's rectangles, and
// for blits drawn on the GPU.
typedef struct {
	XsdkShader vertex;
	XsdkShader pixel;
	// For a tiled pass, which can't set one colour target alone, the image's colour to all four targets, the write
	// masks keeping all but one; and depth drawn back from a depth image's texture.
	XsdkShader pixel_four;
	XsdkShader depth;
	// The clear's pixel shader, and the float constant its colour goes in; and the one for several colour attachments,
	// which writes four colours from four constants.
	XsdkShader clear;
	uint32_t clear_constant;
	XsdkShader clear_four;
	uint32_t clear_four_constant;
	// The drawn blit's vertex shader, and the float constant its source region goes in.
	XsdkShader region;
	uint32_t region_constant;
	volatile uint32_t *vertices;
	uint32_t vertices_address;
	uint32_t texture_slot;
	// Nearest, as images are drawn back, and linear, for blits that ask for it; both level 0 alone.
	XsdkSamplerState sampler;
	XsdkSamplerState linear_sampler;
	XsdkBlendState blend;
	XsdkRasterState raster;
	XsdkDepthStencilState depth_stencil;
	// Depth always written, for the depth drawn back.
	XsdkDepthStencilState depth_write;
} Blit;

// What vkCmdClearAttachments clears in one rectangle, kept in the command buffer's data: a colour for each colour
// attachment it clears, bit i of colors_cleared for attachment i.
typedef struct {
	VkRect2D rect;
	float colors[V4X_MAX_COLOR_ATTACHMENTS][4];
	float depth;
	uint32_t stencil;
	uint32_t colors_cleared;
	bool clears_depth;
	bool clears_stencil;
} ClearRect;

// A blit region the GPU draws, kept in the command buffer's data: the source's level 0 drawn over the destination's
// rectangle, its texture coordinates at the rectangle's top left and then across and down it, into an 8_8_8_8 target,
// then that rectangle resolved into the destination's level 0.
typedef struct {
	struct Image *source;
	struct Image *destination;
	VkRect2D rect;
	float uv[4];
	bool linear;
} DrawnBlit;

enum {
	// The fences of this many submissions are kept, for copies into images they used.
	V4X_SUBMIT_FENCES = 64,
	// The depths eDRAM keeps track of at once.
	V4X_EDRAM_DEPTHS = 4,
	// GPU memory for indexed instanced draws' repeated indices (transfer.c), in parts filled in turn; a part holds the
	// most a draw repeats, a packet's 65535 indices of 32 bits.
	V4X_INDEX_COPY_PARTS = 4,
	V4X_INDEX_COPY_PART_BYTES = 256 * 1024,
};

struct VkQueue_T {
	VkDevice device;
	// The last pass's colour targets, from tile 0 (render_pass.c).
	EdramPart colors[V4X_MAX_COLOR_ATTACHMENTS];
	// Depths at tiles of their own, so a pass with depth of its own needn't take an earlier pass's (milestone 14).
	EdramPart depths[V4X_EDRAM_DEPTHS];
	// The depth and stencil resolves clear to, as last given to xsdk_gpu_set_depth_clear.
	float depth_clear;
	uint32_t stencil_clear;
	Blit blit;
	// xsdk's count of frames XAM drew the Guide in, as last seen; XAM's drawing leaves eDRAM holding none of the parts.
	uint32_t system_draws;
	// The submission being replayed, numbered from 1, and the fence each recent one ended with, at serial % the count.
	uint32_t serial;
	uint32_t submit_fences[V4X_SUBMIT_FENCES];
	// A draw went into the ring since the CPU last waited for the GPU, so a copy into a buffer waits first
	// (transfer.c).
	bool drawn;
	// The loop constants as last written, once one has been: the GPU keeps them from one submission to the next.
	uint32_t loop_constants[XSDK_LOOP_CONSTANTS];
	bool loops_written;
	// The repeated indices' memory, the part being filled and the bytes it holds, and the submission that last wrote
	// each part, 0 for none (transfer.c).
	volatile uint32_t *index_copies;
	uint32_t copy_part;
	uint32_t copy_part_bytes;
	uint32_t copy_serials[V4X_INDEX_COPY_PARTS];
};

struct VkDevice_T {
	VkAllocationCallbacks allocator;
	bool has_allocator;
	struct VkQueue_T queue;
	// Shaders compiled on the console (shader.c), kept till the device goes; shaders_lock guards them, as Vulkan lets
	// modules be made on several threads.
	pthread_mutex_t shaders_lock;
	V4xShader **shaders;
	uint32_t shader_count;
	uint32_t shader_capacity;
	// The samplerAnisotropy feature, and VK_KHR_sampler_mirror_clamp_to_edge, enabled when the device was made.
	bool sampler_anisotropy;
	bool mirror_clamp_to_edge;
};

typedef struct {
	volatile uint32_t *pointer;
	uint32_t address;
	uint32_t size_bytes;
	// The CPU's cached memory, which the GPU never reads: transfer-source buffers' alone (docs/SUPPORT.md).
	bool cached;
} DeviceMemory;

typedef struct {
	VkDeviceSize size;
	VkBufferUsageFlags usage;
	const DeviceMemory *memory;
	uint32_t offset_bytes;
} Buffer;

// A swapchain image, a front buffer of xsdk_present at address, or a texture in memory bound to it. Both have a
// texture, to sample or to draw back into eDRAM.
typedef struct Image {
	uint32_t address;
	uint32_t width;
	uint32_t height;
	VkFormat format;
	bool is_texture;
	// Render passes may resolve into it.
	bool is_attachment;
	// A depth attachment lives in eDRAM, its memory never used unless it's made to be sampled: then a render pass that
	// stores it and leaves it to be read resolves it into its texture, D24S8.
	bool is_depth;
	bool is_sampled_depth;
	XsdkTexture texture;
	// The submission that last sampled it, resolved into it or drew it back into eDRAM, so a copy waits for that
	// one alone; 0 for none.
	uint32_t used_serial;
	// A depth image's depth lives in eDRAM, so vkCmdClearDepthStencilImage's values wait here for the next pass that
	// loads it, which clears to them instead.
	bool clear_pending;
	float pending_depth;
	uint8_t pending_stencil;
} Image;

typedef struct {
	Image *image;
	// The image's texture as the view samples it, its components mapped.
	XsdkTexture texture;
} ImageView;

// A module's shader, and its SPIR-V, copied after it, for an instanced variant compiled when a pipeline wants it.
typedef struct {
	const V4xShader *shader;
	uint64_t spirv_hash;
	uint32_t spirv_size_bytes;
} ShaderModule;

typedef struct {
	XsdkSamplerState state;
	// A comparison sampler's weights for xost's comparison: 1 for each of the reference below, equal to and above the
	// depth that its op passes, then 0 (xost:docs/xsc.md, "Interface conventions").
	float compare_weights[4];
} Sampler;

typedef struct {
	VkDescriptorType type;
	VkShaderStageFlags stages;
	// Where this binding's offset sits among the set's dynamic offsets.
	uint32_t dynamic_index;
	bool used;
} LayoutBinding;

typedef struct {
	LayoutBinding bindings[V4X_MAX_DESCRIPTOR_BINDINGS];
	uint32_t dynamic_count;
} DescriptorSetLayout;

typedef struct {
	VkDescriptorType type;
	// A uniform buffer's bytes, mapped and as the GPU addresses them, and how many of them the descriptor covers.
	volatile const uint32_t *buffer;
	uint32_t buffer_address;
	uint32_t range_bytes;
	Image *image;
	// The view's texture.
	const XsdkTexture *texture;
	const Sampler *sampler;
} Descriptor;

typedef struct DescriptorSet {
	const DescriptorSetLayout *layout;
	struct DescriptorSet *next;
	Descriptor descriptors[V4X_MAX_DESCRIPTOR_BINDINGS];
} DescriptorSet;

typedef struct {
	DescriptorSet *sets;
} DescriptorPool;

typedef struct {
	const DescriptorSetLayout *sets[V4X_MAX_DESCRIPTOR_SETS];
	uint32_t set_count;
	VkShaderStageFlags push_stages;
} PipelineLayout;

// A shader resource with the stage it's for.
typedef struct {
	V4xShaderResource resource;
	bool vertex;
} StageResource;

typedef struct {
	uint32_t binding;
	uint32_t stride_bytes;
	// At the instance's rate: the instanced shader fetches each instance's element, so the stream stays at the
	// binding's start.
	bool per_instance;
} VertexStream;

// A clip distance's plane, which each draw reads from where the vertex shader's push constants or uniform block hold
// it: one of the pipeline's resources, and the bytes into it.
typedef struct {
	uint32_t plane;
	uint32_t resource;
	uint32_t offset_bytes;
} ClipPlane;

typedef struct {
	XsdkShader vertex;
	XsdkShader pixel;
	const V4xShader *vertex_source;
	const V4xShader *pixel_source;
	VertexStream streams[V4X_MAX_VERTEX_BINDINGS];
	uint32_t stream_count;
	VkViewport viewport;
	VkRect2D scissor;
	bool dynamic_viewport;
	bool dynamic_scissor;
	uint32_t primitive;
	// A list's vertices a primitive, 1 to 3; 0 for a strip or a fan.
	uint32_t list_vertices;
	// A draw past what a draw packet carries goes in pieces of up to piece_vertices, each piece_step on from the last:
	// lists split on whole primitives, and strips share the vertices where pieces meet. Fans don't split: step 0.
	uint32_t piece_vertices;
	uint32_t piece_step;
	// A blend for each of the subpass's colour attachments.
	XsdkBlendState blends[V4X_MAX_COLOR_ATTACHMENTS];
	uint32_t blend_count;
	XsdkRasterState raster;
	XsdkDepthStencilState depth_stencil;
	// The state past the viewport and scissor it takes from commands, a bit 1 << VK_DYNAMIC_STATE_* each, and the
	// settings its raster and depth and stencil blocks came from, which that state changes at replay.
	uint32_t dynamic;
	XsdkRaster raster_setting;
	bool depth_bias_enabled;
	XsdkDepthStencil depth_stencil_setting;
	// Its blend takes a constant factor, so the colour is set with the pipeline: its own or vkCmdSetBlendConstants's.
	bool blends_by_constants;
	float blend_constants[4];
	StageResource resources[V4X_MAX_SHADER_RESOURCES];
	uint32_t resource_count;
	// Its resources that are hardware loops' counts, so a pipeline with none spends nothing on them.
	uint32_t loop_count;
	// The user clip planes its vertex shader's clip distances take, a bit each in clip_plane_mask (milestone 26).
	ClipPlane clip_planes[XSDK_CLIP_PLANES];
	uint32_t clip_plane_count;
	uint32_t clip_plane_mask;
} Pipeline;

// One subpass as the replay draws it, a render pass of its own: its colour attachments and its depth if it has one,
// with the load and store ops they take there, an attachment's own at its first and last subpass and kept between.
typedef struct {
	uint32_t color_count;
	VkFormat formats[V4X_MAX_COLOR_ATTACHMENTS];
	// The xsdk target format each draws into (image.c, v4x_target_format).
	uint32_t targets[V4X_MAX_COLOR_ATTACHMENTS];
	VkAttachmentLoadOp load_ops[V4X_MAX_COLOR_ATTACHMENTS];
	VkAttachmentStoreOp store_ops[V4X_MAX_COLOR_ATTACHMENTS];
	uint32_t color_indices[V4X_MAX_COLOR_ATTACHMENTS];
	bool has_depth;
	uint32_t depth_index;
	bool clears_depth;
	bool loads_depth;
	// It stores depth or stencil and leaves the attachment in a layout to sample it in, so a sampled depth image is
	// resolved into its texture at its end; one kept as an attachment stays in eDRAM alone.
	bool leaves_depth_to_read;
	// It stores depth or stencil, or a later subpass uses them: a tiled pass whose depth is in bands resolves it.
	bool stores_depth;
} RenderPass;

// A VkRenderPass: its subpasses in order.
typedef struct {
	RenderPass subpasses[V4X_MAX_SUBPASSES];
	uint32_t subpass_count;
	uint32_t attachment_count;
} RenderPassSubpasses;

// A tiled pass's recording, which each band runs; the GPU may run it until fence lands.
typedef struct {
	XsdkCommandBuffer buffer;
	uint32_t fence;
} TileRecording;

// One subpass's attachments' images, and the colour targets they draw into, one after another in eDRAM from tile 0,
// with the tiles they take all together; or past eDRAM's tiles, the tiled pass they draw in and its recording.
typedef struct {
	Image *attachments[V4X_MAX_COLOR_ATTACHMENTS];
	Image *depth;
	uint32_t width;
	uint32_t height;
	XsdkColorTarget targets[V4X_MAX_COLOR_ATTACHMENTS];
	uint32_t color_tiles;
	bool tiled;
	XsdkTiling tiling;
	TileRecording *recording;
} Framebuffer;

// A VkFramebuffer: each subpass's images, as the render pass it was made with places its attachments; a compatible pass
// places them the same.
typedef struct {
	Framebuffer subpasses[V4X_MAX_SUBPASSES];
	uint32_t width;
	uint32_t height;
	TileRecording recordings[V4X_MAX_SUBPASSES];
} FramebufferSubpasses;

// A copy between a buffer and the whole of an image's level, in some of its layers or 3D slices, kept in the command
// buffer's data.
typedef struct {
	// The buffer's bytes at the copy's offset, and their GPU address, 0 in cached memory.
	volatile uint8_t *buffer;
	uint32_t address;
	Image *image;
	uint32_t level;
	uint32_t row_bytes;
	// The image's first layer or slice and how many, each one's rows slice_bytes after the last's in the buffer.
	uint32_t first_slice;
	uint32_t slice_count;
	uint32_t slice_bytes;
} LevelCopy;

typedef enum {
	COMMAND_BEGIN_RENDER_PASS,
	COMMAND_END_RENDER_PASS,
	COMMAND_BIND_PIPELINE,
	COMMAND_BIND_VERTEX_BUFFER,
	COMMAND_DRAW,
	COMMAND_BIND_DESCRIPTOR_SET,
	COMMAND_PUSH_CONSTANTS,
	COMMAND_COPY_BUFFER_TO_IMAGE,
	COMMAND_SET_VIEWPORT,
	COMMAND_SET_SCISSOR,
	COMMAND_BIND_INDEX_BUFFER,
	COMMAND_DRAW_INDEXED,
	COMMAND_COPY_IMAGE_TO_BUFFER,
	COMMAND_COPY_IMAGE,
	COMMAND_BLIT_IMAGE,
	COMMAND_DRAW_BLIT,
	COMMAND_CLEAR_COLOR_IMAGE,
	COMMAND_COPY_BUFFER,
	COMMAND_UPDATE_BUFFER,
	COMMAND_FILL_BUFFER,
	COMMAND_DRAW_INDIRECT,
	COMMAND_DRAW_INDEXED_INDIRECT,
	COMMAND_SET_STENCIL,
	COMMAND_SET_DEPTH_BIAS,
	COMMAND_SET_BLEND_CONSTANTS,
	COMMAND_CLEAR_ATTACHMENTS,
	COMMAND_CLEAR_DEPTH_IMAGE,
	COMMAND_EXECUTE_COMMANDS,
	COMMAND_WRITE_EVENT,
	COMMAND_WAIT_EVENTS,
	COMMAND_RESET_QUERIES,
	COMMAND_WRITE_TIMESTAMP,
	COMMAND_COPY_QUERY_RESULTS,
	COMMAND_BEGIN_QUERY,
	COMMAND_END_QUERY,
} CommandType;

// The values a subpass's attachments clear to: each colour attachment's, by its place in the subpass, then depth and
// stencil.
typedef struct {
	float colors[V4X_MAX_COLOR_ATTACHMENTS][4];
	float depth;
	uint32_t stencil;
} PassClears;

enum {
	// Stencil state's faces, as VkStencilFaceFlags number them: front, then back.
	V4X_STENCIL_FACES = 2,
};

// The dynamic state a command buffer sets past the viewport and scissor, as it last set it in a replay; the stencil
// masks and references a face each.
typedef struct {
	uint8_t stencil_compare_mask[V4X_STENCIL_FACES];
	uint8_t stencil_write_mask[V4X_STENCIL_FACES];
	uint8_t stencil_reference[V4X_STENCIL_FACES];
	float depth_bias_constant;
	float depth_bias_slope;
	float blend_constants[4];
} DynamicState;

typedef struct {
	CommandType type;
	union {
		// The pass's clears, a PassClears in the command buffer's data.
		struct {
			const RenderPass *render_pass;
			const Framebuffer *framebuffer;
			uint32_t clears_at;
			// The render area, a VkRect2D in the command buffer's data.
			uint32_t area_at;
		} begin;
		const Pipeline *pipeline;
		struct {
			uint32_t binding;
			uint32_t address;
			uint32_t size_bytes;
		} vertex_buffer;
		struct {
			uint32_t vertex_count;
			uint32_t first_vertex;
			uint32_t instance_count;
			uint32_t first_instance;
		} draw;
		struct {
			uint32_t index;
			const DescriptorSet *set;
			// The set's dynamic offsets, in the command buffer's data.
			uint32_t offsets_at;
		} descriptor_set;
		struct {
			uint32_t offset_bytes;
			uint32_t size_bytes;
			uint32_t data_at;
		} push_constants;
		// The whole of a level into the whole of another, copied, or blitted to its size, in as many of each one's
		// layers or 3D slices from the first given; small, so a command stays 24 bytes.
		struct {
			Image *source;
			Image *destination;
			uint8_t source_level;
			uint8_t destination_level;
			bool linear;
			uint16_t source_slice;
			uint16_t destination_slice;
			uint16_t slice_count;
		} image_copy;
		// Into a buffer, from another's bytes, the command buffer's data or one word over and over.
		struct {
			volatile const uint8_t *source;
			volatile uint8_t *destination;
			// The destination's GPU address, 0 in cached memory.
			uint32_t address;
			uint32_t size_bytes;
			// vkCmdUpdateBuffer's bytes in the command buffer's data, or vkCmdFillBuffer's word.
			uint32_t data;
		} buffer_copy;
		struct {
			Image *image;
			uint8_t first_level;
			uint8_t level_count;
			uint16_t first_slice;
			uint16_t slice_count;
			// A texel's bytes as the image holds them.
			uint8_t texel[V4X_MAX_TEXEL_BYTES];
		} clear;
		// An indirect draw's parameters, read at replay.
		volatile const uint32_t *parameters;
		// A stencil mask or reference: its VK_DYNAMIC_STATE_STENCIL_*, the faces it's for and its value.
		struct {
			uint32_t state;
			VkStencilFaceFlags faces;
			uint8_t value;
		} stencil;
		struct {
			float constant;
			float slope;
		} depth_bias;
		float blend_constants[4];
		struct {
			Image *image;
			float depth;
			uint8_t stencil;
		} depth_clear;
		// A secondary command buffer, replayed in place.
		struct VkCommandBuffer_T *secondary;
		// vkCmdSetEvent's or vkCmdResetEvent's event word, as the GPU addresses it, and the value it takes.
		struct {
			uint32_t address;
			uint32_t value;
		} event;
		// vkCmdWaitEvents' events, in the command buffer's data.
		struct {
			uint32_t events_at;
			uint32_t count;
		} wait_events;
		// Queries reset, or an occlusion query begun or ended, the first alone; or a time stamp written once the work
		// before it is done or at once.
		struct {
			struct QueryPool *pool;
			uint32_t first;
			uint32_t count;
		} queries;
		struct {
			struct QueryPool *pool;
			uint32_t query;
			bool after_work;
		} timestamp;
		// A VkViewport, VkRect2D, ClearRect, QueryCopy, LevelCopy or DrawnBlit in the command buffer's data.
		uint32_t state_at;
		// The index buffer's bytes at its offset, and their GPU address.
		struct {
			volatile const uint8_t *bytes;
			uint32_t address;
			bool is_32_bit;
		} index_buffer;
		struct {
			uint32_t index_count;
			uint32_t first_index;
			int32_t vertex_offset;
			uint32_t instance_count;
			uint32_t first_instance;
		} draw_indexed;
	};
} Command;

typedef enum {
	COMMAND_BUFFER_INITIAL,
	COMMAND_BUFFER_RECORDING,
	COMMAND_BUFFER_EXECUTABLE,
} CommandBufferState;

struct VkCommandBuffer_T {
	VkDevice device;
	struct CommandPool *pool;
	struct VkCommandBuffer_T *next;
	CommandBufferState state;
	VkResult result;
	Command *commands;
	uint32_t count;
	uint32_t capacity;
	uint8_t *data;
	uint32_t data_bytes;
	uint32_t data_capacity_bytes;
	// The render pass being recorded, its framebuffer, the subpass it's in and the clear values and render area it
	// began with, for vkCmdNextSubpass to begin the next subpass as a pass of its own.
	const RenderPassSubpasses *render_pass;
	const FramebufferSubpasses *framebuffer;
	uint32_t subpass;
	VkClearValue clears[V4X_MAX_ATTACHMENTS];
	uint32_t area_at;
	// The pipeline bound last while recording, to check draws against its topology.
	const Pipeline *pipeline;
	// Allocated as a secondary command buffer, which vkCmdExecuteCommands replays in place.
	bool secondary;
	// A secondary command buffer recorded to continue a render pass, so its queries begin inside one.
	bool continues_pass;
};

typedef struct CommandPool {
	struct VkCommandBuffer_T *buffers;
} CommandPool;

typedef struct {
	bool signaled;
	bool pending;
	uint32_t number;
} Fence;

typedef struct {
	uint32_t unused;
} Semaphore;

// A word in GPU memory the host and the GPU set to 1 and reset to 0, which the GPU can wait on (sync.c).
// shortcut: each event takes a 64 KB page of its own; share pages if a port makes many events.
typedef struct {
	volatile uint32_t *word;
	uint32_t address;
} Event;

enum {
	// A query's slot: the GPU clock's words (xsdk:include/xsdk/gpu.h, xsdk_gpu_write_clock), padded; or an occlusion
	// query's status word, then the last visibility id it took in the clock's last word (queue.c).
	V4X_QUERY_SLOT_WORDS = 4,
	// The word written last, so a slot holding it holds a whole result.
	V4X_QUERY_LAST_WORD = XSDK_GPU_CLOCK_WORDS - 1,
	// That word in a slot nothing has been written into since its reset: a high word the clock would take 2^32 times
	// 8.6 s to reach, and no visibility id.
	V4X_QUERY_UNWRITTEN = 0xFFFFFFFF,
};

// Time stamp or occlusion queries, a slot each in GPU memory (query.c).
typedef struct QueryPool {
	volatile uint32_t *slots;
	uint32_t address;
	uint32_t count;
	bool occlusion;
	// The submission that last wrote it (VkQueue_T's serial), 0 if none, which a reset or a copy waits for.
	uint32_t used_serial;
} QueryPool;

// vkCmdCopyQueryPoolResults' queries and where they go, in the command buffer's data.
typedef struct {
	QueryPool *pool;
	uint32_t first;
	uint32_t count;
	volatile uint8_t *destination;
	// The destination's GPU address, 0 in cached memory.
	uint32_t address;
	uint32_t stride_bytes;
	VkQueryResultFlags flags;
} QueryCopy;

typedef struct {
	uint32_t unused;
} PipelineCache;

typedef struct {
	uint32_t width;
	uint32_t height;
} Surface;

typedef struct {
	Image images[V4X_MAX_SWAPCHAIN_IMAGES];
	uint32_t image_count;
	bool acquired;
	uint32_t acquired_index;
} Swapchain;

// Non-dispatchable handles are 64-bit integers on 32-bit targets, so they carry the object's pointer.
static inline uint64_t v4x_handle(const void *object)
{
	return (uint64_t)(uintptr_t)object;
}

static inline void *v4x_object(uint64_t handle)
{
	return (void *)(uintptr_t)handle;
}

#endif
