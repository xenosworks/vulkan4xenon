#include "pipeline_state.h"

#include <vulkan/vulkan.h>

#include "host.h"
#include "image.h"
#include "objects.h"
#include "xsdk/gpu.h"

// A pipeline's fixed-function state as xsdk's state blocks, worked out once (docs/ROADMAP.md, milestone 3).

enum {
	// D24's resolution, the unit of Vulkan's constant depth bias; the hardware's offset is in depth units, which Xenia
	// scales by 2^24 - 1 (xenia:src/xenia/gpu/draw_util.h:165-172).
	DEPTH_BIAS_UNITS = 1 << 24,
	NO_BLEND_FACTOR = 0xFF,
};

// Whether the pipeline takes a state from commands, so its create info's value is ignored.
static bool takes(const Pipeline *pipeline, VkDynamicState state)
{
	return pipeline->dynamic & 1u << state;
}

// A list splits on whole primitives.
static void split_list(Pipeline *pipeline, uint32_t primitive_vertices)
{
	pipeline->list_vertices = primitive_vertices;
	pipeline->piece_vertices = V4X_MAX_DRAW_VERTICES - V4X_MAX_DRAW_VERTICES % primitive_vertices;
	pipeline->piece_step = pipeline->piece_vertices;
}

static VkResult topology(const VkPipelineInputAssemblyStateCreateInfo *assembly, Pipeline *pipeline)
{
	if (assembly->primitiveRestartEnable)
		return v4x_host_unsupported("primitive restart");
	switch (assembly->topology) {
	case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
		pipeline->primitive = XSDK_PRIMITIVE_POINT_LIST;
		split_list(pipeline, 1);
		return VK_SUCCESS;
	case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
		pipeline->primitive = XSDK_PRIMITIVE_LINE_LIST;
		split_list(pipeline, 2);
		return VK_SUCCESS;
	case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
		pipeline->primitive = XSDK_PRIMITIVE_LINE_STRIP;
		// Each piece starts at the last one's last vertex.
		pipeline->piece_vertices = V4X_MAX_DRAW_VERTICES;
		pipeline->piece_step = V4X_MAX_DRAW_VERTICES - 1;
		return VK_SUCCESS;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
		pipeline->primitive = XSDK_PRIMITIVE_TRIANGLE_LIST;
		split_list(pipeline, 3);
		return VK_SUCCESS;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
		pipeline->primitive = XSDK_PRIMITIVE_TRIANGLE_STRIP;
		// Each piece starts at the last one's last two vertices, an even number on, so every triangle keeps the parity
		// its facing goes by (Vulkan 1.0 spec, "Triangle Strips").
		pipeline->piece_vertices = V4X_MAX_DRAW_VERTICES - 1;
		pipeline->piece_step = V4X_MAX_DRAW_VERTICES - 3;
		return VK_SUCCESS;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
		pipeline->primitive = XSDK_PRIMITIVE_TRIANGLE_FAN;
		// Every piece would need the first vertex again, which only indices made at replay could give it.
		pipeline->piece_vertices = V4X_MAX_DRAW_VERTICES;
		return VK_SUCCESS;
	default:
		return v4x_host_unsupported("adjacency and patch topologies");
	}
}

// Every Vulkan 1.0 dynamic state but depth bounds, which isn't a feature (docs/SUPPORT.md, Pipelines).
static VkResult dynamic_state(const VkPipelineDynamicStateCreateInfo *dynamic, Pipeline *pipeline)
{
	for (uint32_t i = 0; dynamic && i < dynamic->dynamicStateCount; i++) {
		const VkDynamicState state = dynamic->pDynamicStates[i];
		if (state > VK_DYNAMIC_STATE_STENCIL_REFERENCE || state == VK_DYNAMIC_STATE_DEPTH_BOUNDS)
			return v4x_host_unsupported("dynamic depth bounds, and dynamic state past Vulkan 1.0's");
		pipeline->dynamic |= 1u << state;
	}
	pipeline->dynamic_viewport = takes(pipeline, VK_DYNAMIC_STATE_VIEWPORT);
	pipeline->dynamic_scissor = takes(pipeline, VK_DYNAMIC_STATE_SCISSOR);
	return VK_SUCCESS;
}

static VkResult viewport_state(const VkPipelineViewportStateCreateInfo *state, Pipeline *pipeline)
{
	if (state->viewportCount != 1 || state->scissorCount != 1)
		return v4x_host_unsupported("more than one viewport");
	if (!pipeline->dynamic_viewport)
		pipeline->viewport = state->pViewports[0];
	if (!pipeline->dynamic_scissor)
		pipeline->scissor = state->pScissors[0];
	return VK_SUCCESS;
}

// Whether depthBiasEnable biases is the pipeline's own; with the bias dynamic, its factors are vkCmdSetDepthBias's.
static VkResult raster_state(const VkPipelineRasterizationStateCreateInfo *raster, Pipeline *pipeline)
{
	const bool dynamic_bias = takes(pipeline, VK_DYNAMIC_STATE_DEPTH_BIAS);
	if (raster->depthClampEnable || raster->rasterizerDiscardEnable || raster->polygonMode > VK_POLYGON_MODE_POINT ||
		raster->cullMode == VK_CULL_MODE_FRONT_AND_BACK ||
		(!takes(pipeline, VK_DYNAMIC_STATE_LINE_WIDTH) && raster->lineWidth != 1.0f) ||
		(raster->depthBiasEnable && !dynamic_bias && raster->depthBiasClamp != 0.0f))
		return v4x_host_unsupported(
			"rasterization other than filled, lines or points, culling one face or none, unclamped bias");
	const bool biased = raster->depthBiasEnable && !dynamic_bias;
	pipeline->raster_setting = (XsdkRaster){
		.cull = raster->cullMode == VK_CULL_MODE_FRONT_BIT ? XSDK_CULL_FRONT
			: raster->cullMode == VK_CULL_MODE_BACK_BIT    ? XSDK_CULL_BACK
														   : XSDK_CULL_NONE,
		// Clockwise as it looks in Vulkan's framebuffer, as xsdk measures it (xsdk:tests/test_raster.c:207).
		.front_clockwise = raster->frontFace == VK_FRONT_FACE_CLOCKWISE,
		.depth_bias = biased ? raster->depthBiasConstantFactor / DEPTH_BIAS_UNITS : 0.0f,
		.depth_bias_slope = biased ? raster->depthBiasSlopeFactor : 0.0f,
		// Xenos's polygon types, its triangles' edges or corners (milestone 26).
		.fill = raster->polygonMode == VK_POLYGON_MODE_LINE ? XSDK_FILL_WIREFRAME
			: raster->polygonMode == VK_POLYGON_MODE_POINT  ? XSDK_FILL_POINTS
															: XSDK_FILL_SOLID,
	};
	pipeline->depth_bias_enabled = raster->depthBiasEnable;
	xsdk_gpu_raster_state(&pipeline->raster, &pipeline->raster_setting);
	return VK_SUCCESS;
}

// Back faces need a setting of their own only where it differs from the front's, so one-sided stencil stays as it was.
static bool faces_differ(const XsdkDepthStencil *setting)
{
	const XsdkStencilFace *const back = &setting->back;
	return setting->stencil_test &&
		(setting->stencil_compare != back->compare || setting->stencil_fail != back->fail ||
			setting->stencil_depth_fail != back->depth_fail || setting->stencil_pass != back->pass ||
			setting->stencil_reference != back->reference || setting->stencil_read_mask != back->read_mask ||
			setting->stencil_write_mask != back->write_mask);
}

// Vulkan's compare and stencil ops are xsdk's value for value (xsdk:include/xsdk/gpu.h).
static VkResult depth_stencil_state(const VkPipelineDepthStencilStateCreateInfo *state, Pipeline *pipeline)
{
	XsdkDepthStencil setting = { .depth_compare = XSDK_COMPARE_ALWAYS, .stencil_compare = XSDK_COMPARE_ALWAYS };
	if (state) {
		if (state->depthBoundsTestEnable)
			return v4x_host_unsupported("depth bounds");
		const VkStencilOpState *const front = &state->front;
		const VkStencilOpState *const back = &state->back;
		setting = (XsdkDepthStencil){
			.depth_test = state->depthTestEnable,
			.depth_write = state->depthTestEnable && state->depthWriteEnable,
			.depth_compare = (uint32_t)state->depthCompareOp,
			.stencil_test = state->stencilTestEnable,
			.stencil_compare = (uint32_t)front->compareOp,
			.stencil_fail = (uint32_t)front->failOp,
			.stencil_depth_fail = (uint32_t)front->depthFailOp,
			.stencil_pass = (uint32_t)front->passOp,
			.stencil_reference = (uint8_t)front->reference,
			.stencil_read_mask = (uint8_t)front->compareMask,
			.stencil_write_mask = (uint8_t)front->writeMask,
			.back = { (uint32_t)back->compareOp, (uint32_t)back->failOp, (uint32_t)back->depthFailOp,
				(uint32_t)back->passOp, (uint8_t)back->reference, (uint8_t)back->compareMask,
				(uint8_t)back->writeMask },
		};
		setting.two_sided = faces_differ(&setting);
	}
	pipeline->depth_stencil_setting = setting;
	xsdk_gpu_depth_stencil_state(&pipeline->depth_stencil, &setting);
	return VK_SUCCESS;
}

static uint32_t blend_factor(VkBlendFactor factor)
{
	switch (factor) {
	case VK_BLEND_FACTOR_ZERO:
		return XSDK_BLEND_ZERO;
	case VK_BLEND_FACTOR_ONE:
		return XSDK_BLEND_ONE;
	case VK_BLEND_FACTOR_SRC_COLOR:
		return XSDK_BLEND_SRC_COLOR;
	case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:
		return XSDK_BLEND_ONE_MINUS_SRC_COLOR;
	case VK_BLEND_FACTOR_DST_COLOR:
		return XSDK_BLEND_DST_COLOR;
	case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR:
		return XSDK_BLEND_ONE_MINUS_DST_COLOR;
	case VK_BLEND_FACTOR_SRC_ALPHA:
		return XSDK_BLEND_SRC_ALPHA;
	case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:
		return XSDK_BLEND_ONE_MINUS_SRC_ALPHA;
	case VK_BLEND_FACTOR_DST_ALPHA:
		return XSDK_BLEND_DST_ALPHA;
	case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:
		return XSDK_BLEND_ONE_MINUS_DST_ALPHA;
	case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE:
		return XSDK_BLEND_SRC_ALPHA_SATURATE;
	case VK_BLEND_FACTOR_CONSTANT_COLOR:
		return XSDK_BLEND_CONSTANT_COLOR;
	case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR:
		return XSDK_BLEND_ONE_MINUS_CONSTANT_COLOR;
	case VK_BLEND_FACTOR_CONSTANT_ALPHA:
		return XSDK_BLEND_CONSTANT_ALPHA;
	case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA:
		return XSDK_BLEND_ONE_MINUS_CONSTANT_ALPHA;
	default:
		return NO_BLEND_FACTOR;
	}
}

static bool constant_factor(uint32_t factor)
{
	return factor >= XSDK_BLEND_CONSTANT_COLOR && factor <= XSDK_BLEND_ONE_MINUS_CONSTANT_ALPHA;
}

static uint32_t blend_op(VkBlendOp op)
{
	switch (op) {
	case VK_BLEND_OP_SUBTRACT:
		return XSDK_BLEND_OP_SUBTRACT;
	case VK_BLEND_OP_REVERSE_SUBTRACT:
		return XSDK_BLEND_OP_REVERSE_SUBTRACT;
	case VK_BLEND_OP_MIN:
		return XSDK_BLEND_OP_MIN;
	case VK_BLEND_OP_MAX:
		return XSDK_BLEND_OP_MAX;
	default:
		return XSDK_BLEND_OP_ADD;
	}
}

// Min and max ignore their factors in Vulkan and in Xenia (xenia:src/xenia/gpu/spirv_shader_translator_rb.cc:3324);
// one each makes the result the same either way.
static bool factors_matter(VkBlendOp op)
{
	return op != VK_BLEND_OP_MIN && op != VK_BLEND_OP_MAX;
}

// One colour attachment's blend; float attachments are refused it, as their format reports no blending (image.c).
static VkResult attachment_blend(const VkPipelineColorBlendAttachmentState *attachment, VkFormat format, XsdkBlend *out)
{
	if (attachment->blendEnable && !v4x_blend_format(format))
		return v4x_host_unsupported("blending into float colour attachments, which Xenos doesn't blend");
	XsdkBlend setting = {
		.color_source = XSDK_BLEND_ONE,
		.color_dest = XSDK_BLEND_ZERO,
		.alpha_source = XSDK_BLEND_ONE,
		.alpha_dest = XSDK_BLEND_ZERO,
		.write_mask = attachment->colorWriteMask & XSDK_WRITE_ALL,
	};
	if (attachment->blendEnable) {
		const bool color = factors_matter(attachment->colorBlendOp);
		const bool alpha = factors_matter(attachment->alphaBlendOp);
		setting.color_source = color ? blend_factor(attachment->srcColorBlendFactor) : XSDK_BLEND_ONE;
		setting.color_dest = color ? blend_factor(attachment->dstColorBlendFactor) : XSDK_BLEND_ONE;
		setting.color_op = blend_op(attachment->colorBlendOp);
		setting.alpha_source = alpha ? blend_factor(attachment->srcAlphaBlendFactor) : XSDK_BLEND_ONE;
		setting.alpha_dest = alpha ? blend_factor(attachment->dstAlphaBlendFactor) : XSDK_BLEND_ONE;
		setting.alpha_op = blend_op(attachment->alphaBlendOp);
	}
	if (setting.color_source == NO_BLEND_FACTOR || setting.color_dest == NO_BLEND_FACTOR ||
		setting.alpha_source == NO_BLEND_FACTOR || setting.alpha_dest == NO_BLEND_FACTOR)
		return v4x_host_unsupported("dual-source blend factors");
	*out = setting;
	return VK_SUCCESS;
}

// Each of the subpass's colour attachments blends on its own, as Xenos's targets do (xsdk:include/xsdk/gpu.h,
// xsdk_gpu_set_blends), which is independentBlend; the first's blend state takes alpha to coverage, as xsdk does.
static VkResult blend_state(const VkPipelineColorBlendStateCreateInfo *state, const RenderPass *render_pass,
	bool alpha_to_coverage, Pipeline *pipeline)
{
	if (state->logicOpEnable || state->attachmentCount != render_pass->color_count)
		return v4x_host_unsupported("logic ops, and blend states other than one for each colour attachment");
	pipeline->blends_by_constants = false;
	for (uint32_t i = 0; i < state->attachmentCount; i++) {
		XsdkBlend setting;
		const VkResult result = attachment_blend(&state->pAttachments[i], render_pass->formats[i], &setting);
		if (result != VK_SUCCESS)
			return result;
		setting.alpha_to_coverage = i == 0 && alpha_to_coverage;
		pipeline->blends_by_constants = pipeline->blends_by_constants || constant_factor(setting.color_source) ||
			constant_factor(setting.color_dest) || constant_factor(setting.alpha_source) ||
			constant_factor(setting.alpha_dest);
		xsdk_gpu_blend_state(&pipeline->blends[i], &setting);
	}
	pipeline->blend_count = state->attachmentCount;
	for (uint32_t i = 0; i < 4; i++)
		pipeline->blend_constants[i] = state->blendConstants[i];
	return VK_SUCCESS;
}

const XsdkRasterState *v4x_pipeline_raster(
	const Pipeline *pipeline, const DynamicState *dynamic, XsdkRasterState *scratch)
{
	if (!takes(pipeline, VK_DYNAMIC_STATE_DEPTH_BIAS) || !pipeline->depth_bias_enabled)
		return &pipeline->raster;
	XsdkRaster setting = pipeline->raster_setting;
	setting.depth_bias = dynamic->depth_bias_constant / DEPTH_BIAS_UNITS;
	setting.depth_bias_slope = dynamic->depth_bias_slope;
	xsdk_gpu_raster_state(scratch, &setting);
	return scratch;
}

const XsdkDepthStencilState *v4x_pipeline_depth_stencil(
	const Pipeline *pipeline, const DynamicState *dynamic, XsdkDepthStencilState *scratch)
{
	const uint32_t stencil = 1u << VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK | 1u << VK_DYNAMIC_STATE_STENCIL_WRITE_MASK |
		1u << VK_DYNAMIC_STATE_STENCIL_REFERENCE;
	if (!(pipeline->dynamic & stencil))
		return &pipeline->depth_stencil;
	XsdkDepthStencil setting = pipeline->depth_stencil_setting;
	if (takes(pipeline, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK)) {
		setting.stencil_read_mask = dynamic->stencil_compare_mask[0];
		setting.back.read_mask = dynamic->stencil_compare_mask[1];
	}
	if (takes(pipeline, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK)) {
		setting.stencil_write_mask = dynamic->stencil_write_mask[0];
		setting.back.write_mask = dynamic->stencil_write_mask[1];
	}
	if (takes(pipeline, VK_DYNAMIC_STATE_STENCIL_REFERENCE)) {
		setting.stencil_reference = dynamic->stencil_reference[0];
		setting.back.reference = dynamic->stencil_reference[1];
	}
	setting.two_sided = faces_differ(&setting);
	xsdk_gpu_depth_stencil_state(scratch, &setting);
	return scratch;
}

const float *v4x_pipeline_blend_constants(const Pipeline *pipeline, const DynamicState *dynamic)
{
	if (!pipeline->blends_by_constants)
		return NULL;
	return takes(pipeline, VK_DYNAMIC_STATE_BLEND_CONSTANTS) ? dynamic->blend_constants : pipeline->blend_constants;
}

// The subpass's samples, every one of them drawn; sample masks, sample shading and alpha to one aren't built
// (docs/SUPPORT.md, Pipelines).
static VkResult multisample_state(const VkPipelineMultisampleStateCreateInfo *state, const RenderPass *render_pass)
{
	const VkSampleMask all = (1u << state->rasterizationSamples) - 1;
	if (state->rasterizationSamples != render_pass->samples || state->sampleShadingEnable || state->alphaToOneEnable ||
		(state->pSampleMask && (state->pSampleMask[0] & all) != all))
		return v4x_host_unsupported(
			"multisampling other than the subpass's samples, all of them: sample masks, sample shading or alpha to "
			"one");
	return VK_SUCCESS;
}

VkResult v4x_pipeline_state(const VkGraphicsPipelineCreateInfo *info, const RenderPass *render_pass, Pipeline *pipeline)
{
	const VkPipelineMultisampleStateCreateInfo *const multisample = info->pMultisampleState;
	VkResult result = multisample_state(multisample, render_pass);
	if (result == VK_SUCCESS)
		result = topology(info->pInputAssemblyState, pipeline);
	if (result == VK_SUCCESS)
		result = dynamic_state(info->pDynamicState, pipeline);
	if (result == VK_SUCCESS)
		result = viewport_state(info->pViewportState, pipeline);
	if (result == VK_SUCCESS)
		result = raster_state(info->pRasterizationState, pipeline);
	// Without a depth attachment the depth and stencil state is ignored (Vulkan 1.0 spec,
	// VkGraphicsPipelineCreateInfo).
	if (result == VK_SUCCESS)
		result = depth_stencil_state(render_pass->has_depth ? info->pDepthStencilState : NULL, pipeline);
	if (result == VK_SUCCESS)
		result = blend_state(info->pColorBlendState, render_pass, multisample->alphaToCoverageEnable, pipeline);
	return result;
}
