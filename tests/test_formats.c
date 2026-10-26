#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "xsdk/log.h"

// Milestone 20: the image formats on xsdk's, each sampled through the layer and checked by pixel, nearest and filtered,
// its alpha through a view that shows it in red, green and blue; then copied back out, blitted and cleared on the CPU,
// and read back texel by texel. A texel's channels all differ, so a channel misplaced or a byte order wrong shows.

enum {
	FRAMES = 30,
	FENCE_TIMEOUT_NS = 1000000000,
	COLOR = VK_IMAGE_ASPECT_COLOR_BIT,
	// The memory type the CPU caches, which buffers only copied to and from may take.
	CACHED_TYPE = 1,
	// An uncompressed image is 4x4 texels, a compressed one 2x2 blocks of 4x4; each is drawn 48 pixels a side, from
	// the left in columns of three quads.
	TEXELS = 4,
	BLOCK_SIDE = 4,
	BLOCK_TEXELS = 8,
	QUAD_SIDE = 48,
	STEP = 56,
	COLUMNS = 22,
	LEFT = 24,
	TOP = 24,
	BAND_HEIGHT = 3 * STEP + 16,
	// Each format's room in the staging buffer, and in the read-back buffer: its image, then its blit and its clear.
	STAGED_BYTES = 256,
	BLIT_AT = STAGED_BYTES,
	CLEAR_AT = BLIT_AT + 64,
	READ_BYTES = CLEAR_AT + 64,
	// A blit takes the 4x4 image to 2x2.
	BLITTED = 2,
	TOLERANCE = 1,
	// Filtering rounds its weights its own way (xsdk:docs/HARDWARE.md, "Sampling and mipmaps, milestone 15").
	FILTER_TOLERANCE = 4,
	// Decoders round BC's blends their own way, within 12 for xsdk's DXT (xsdk:tests/test_formats.c).
	BLEND_TOLERANCE = 12,
	MAX_PROBES = 4096,
	MAX_MISSES_LOGGED = 12,
	MAX_SPECS = 48,
};

// The quads drawn of each format.
enum {
	QUAD_NEAREST,
	QUAD_ALPHA,
	QUAD_LINEAR,
	QUADS,
};

// What a probe checks, each a check of its own.
enum {
	GROUP_SAMPLED,
	GROUP_FILTERED,
	// BC2 and BC3 colour blocks whose first endpoint isn't above the second.
	GROUP_FOUR_COLOUR,
	// sRGB images filtered, which Vulkan blends after the curve, and Xenos might before.
	GROUP_SRGB_FILTERED,
	GROUPS,
};

enum {
	KIND_UNITS,
	KIND_BC1,
	KIND_BC1_RGB,
	KIND_BC2,
	KIND_BC3,
	KIND_BC4,
	KIND_BC5,
};

enum {
	UNORM,
	HALF,
	SNORM,
	SRGB,
};

// A format as Vulkan defines it: a texel one unit of its bytes in the CPU's order, each channel's bits from its shift
// up, or a block of the Khronos Data Format Specification's.
typedef struct {
	VkFormat format;
	const char *name;
	uint8_t kind;
	uint8_t texel_bytes;
	uint8_t bits[4];
	uint8_t shifts[4];
	uint8_t number;
} Spec;

static const Spec specs[] = {
	{ VK_FORMAT_BC1_RGB_UNORM_BLOCK, "BC1_RGB_UNORM_BLOCK", KIND_BC1_RGB, 8, { 0 }, { 0 }, UNORM },
	{ VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1_RGBA_UNORM_BLOCK", KIND_BC1, 8, { 0 }, { 0 }, UNORM },
	{ VK_FORMAT_BC2_UNORM_BLOCK, "BC2_UNORM_BLOCK", KIND_BC2, 16, { 0 }, { 0 }, UNORM },
	{ VK_FORMAT_BC3_UNORM_BLOCK, "BC3_UNORM_BLOCK", KIND_BC3, 16, { 0 }, { 0 }, UNORM },
	{ VK_FORMAT_R5G6B5_UNORM_PACK16, "R5G6B5_UNORM_PACK16", KIND_UNITS, 2, { 5, 6, 5, 0 }, { 11, 5, 0, 0 }, UNORM },
	{ VK_FORMAT_B5G6R5_UNORM_PACK16, "B5G6R5_UNORM_PACK16", KIND_UNITS, 2, { 5, 6, 5, 0 }, { 0, 5, 11, 0 }, UNORM },
	{ VK_FORMAT_A1R5G5B5_UNORM_PACK16, "A1R5G5B5_UNORM_PACK16", KIND_UNITS, 2, { 5, 5, 5, 1 }, { 10, 5, 0, 15 },
		UNORM },
	{ VK_FORMAT_R4G4B4A4_UNORM_PACK16, "R4G4B4A4_UNORM_PACK16", KIND_UNITS, 2, { 4, 4, 4, 4 }, { 12, 8, 4, 0 }, UNORM },
	{ VK_FORMAT_B4G4R4A4_UNORM_PACK16, "B4G4R4A4_UNORM_PACK16", KIND_UNITS, 2, { 4, 4, 4, 4 }, { 4, 8, 12, 0 }, UNORM },
	{ VK_FORMAT_A8B8G8R8_UNORM_PACK32, "A8B8G8R8_UNORM_PACK32", KIND_UNITS, 4, { 8, 8, 8, 8 }, { 0, 8, 16, 24 },
		UNORM },
	{ VK_FORMAT_A2B10G10R10_UNORM_PACK32, "A2B10G10R10_UNORM_PACK32", KIND_UNITS, 4, { 10, 10, 10, 2 },
		{ 0, 10, 20, 30 }, UNORM },
	{ VK_FORMAT_A2R10G10B10_UNORM_PACK32, "A2R10G10B10_UNORM_PACK32", KIND_UNITS, 4, { 10, 10, 10, 2 },
		{ 20, 10, 0, 30 }, UNORM },
	{ VK_FORMAT_R16_SFLOAT, "R16_SFLOAT", KIND_UNITS, 2, { 16, 0, 0, 0 }, { 0, 0, 0, 0 }, HALF },
	{ VK_FORMAT_R16G16_SFLOAT, "R16G16_SFLOAT", KIND_UNITS, 4, { 16, 16, 0, 0 }, { 16, 0, 0, 0 }, HALF },
	{ VK_FORMAT_R16G16B16A16_SFLOAT, "R16G16B16A16_SFLOAT", KIND_UNITS, 8, { 16, 16, 16, 16 }, { 48, 32, 16, 0 },
		HALF },
	{ VK_FORMAT_BC4_UNORM_BLOCK, "BC4_UNORM_BLOCK", KIND_BC4, 8, { 0 }, { 0 }, UNORM },
	{ VK_FORMAT_BC5_UNORM_BLOCK, "BC5_UNORM_BLOCK", KIND_BC5, 16, { 0 }, { 0 }, UNORM },
	{ VK_FORMAT_R16_UNORM, "R16_UNORM", KIND_UNITS, 2, { 16, 0, 0, 0 }, { 0, 0, 0, 0 }, UNORM },
	{ VK_FORMAT_R16G16_UNORM, "R16G16_UNORM", KIND_UNITS, 4, { 16, 16, 0, 0 }, { 16, 0, 0, 0 }, UNORM },
	{ VK_FORMAT_R16G16B16A16_UNORM, "R16G16B16A16_UNORM", KIND_UNITS, 8, { 16, 16, 16, 16 }, { 48, 32, 16, 0 }, UNORM },
	{ VK_FORMAT_R16_SNORM, "R16_SNORM", KIND_UNITS, 2, { 16, 0, 0, 0 }, { 0, 0, 0, 0 }, SNORM },
	{ VK_FORMAT_R16G16_SNORM, "R16G16_SNORM", KIND_UNITS, 4, { 16, 16, 0, 0 }, { 16, 0, 0, 0 }, SNORM },
	{ VK_FORMAT_R16G16B16A16_SNORM, "R16G16B16A16_SNORM", KIND_UNITS, 8, { 16, 16, 16, 16 }, { 48, 32, 16, 0 }, SNORM },
	{ VK_FORMAT_R8_SNORM, "R8_SNORM", KIND_UNITS, 1, { 8, 0, 0, 0 }, { 0, 0, 0, 0 }, SNORM },
	{ VK_FORMAT_R8G8_SNORM, "R8G8_SNORM", KIND_UNITS, 2, { 8, 8, 0, 0 }, { 8, 0, 0, 0 }, SNORM },
	{ VK_FORMAT_R8G8B8A8_SNORM, "R8G8B8A8_SNORM", KIND_UNITS, 4, { 8, 8, 8, 8 }, { 24, 16, 8, 0 }, SNORM },
	{ VK_FORMAT_B8G8R8A8_SNORM, "B8G8R8A8_SNORM", KIND_UNITS, 4, { 8, 8, 8, 8 }, { 8, 16, 24, 0 }, SNORM },
	{ VK_FORMAT_A2B10G10R10_SNORM_PACK32, "A2B10G10R10_SNORM_PACK32", KIND_UNITS, 4, { 10, 10, 10, 2 },
		{ 0, 10, 20, 30 }, SNORM },
	{ VK_FORMAT_A2R10G10B10_SNORM_PACK32, "A2R10G10B10_SNORM_PACK32", KIND_UNITS, 4, { 10, 10, 10, 2 },
		{ 20, 10, 0, 30 }, SNORM },
	{ VK_FORMAT_R8_SRGB, "R8_SRGB", KIND_UNITS, 1, { 8, 0, 0, 0 }, { 0, 0, 0, 0 }, SRGB },
	{ VK_FORMAT_R8G8_SRGB, "R8G8_SRGB", KIND_UNITS, 2, { 8, 8, 0, 0 }, { 8, 0, 0, 0 }, SRGB },
	{ VK_FORMAT_R8G8B8A8_SRGB, "R8G8B8A8_SRGB", KIND_UNITS, 4, { 8, 8, 8, 8 }, { 24, 16, 8, 0 }, SRGB },
	{ VK_FORMAT_B8G8R8A8_SRGB, "B8G8R8A8_SRGB", KIND_UNITS, 4, { 8, 8, 8, 8 }, { 8, 16, 24, 0 }, SRGB },
	{ VK_FORMAT_BC1_RGB_SRGB_BLOCK, "BC1_RGB_SRGB_BLOCK", KIND_BC1_RGB, 8, { 0 }, { 0 }, SRGB },
	{ VK_FORMAT_BC1_RGBA_SRGB_BLOCK, "BC1_RGBA_SRGB_BLOCK", KIND_BC1, 8, { 0 }, { 0 }, SRGB },
	{ VK_FORMAT_BC2_SRGB_BLOCK, "BC2_SRGB_BLOCK", KIND_BC2, 16, { 0 }, { 0 }, SRGB },
	{ VK_FORMAT_BC3_SRGB_BLOCK, "BC3_SRGB_BLOCK", KIND_BC3, 16, { 0 }, { 0 }, SRGB },
};

enum {
	SPECS = sizeof specs / sizeof specs[0],
};

// 5-6-5 endpoints whose red, green and blue all differ.
static const uint16_t ends[6] = { 31 << 11 | 10 << 5 | 4, 2 << 11 | 60 << 5 | 20, 16 << 11 | 0 << 5 | 31,
	25 << 11 | 45 << 5 | 0, 0 << 11 | 63 << 5 | 12, 8 << 11 | 20 << 5 | 28 };
// Each block's first and second endpoint: in BC1, two blocks of four colours, then two of three and transparent black;
// in BC2 and BC3 the last is four colours with the first below the second, as those formats take every block.
static const uint8_t bc1_ends[4][2] = { { 0, 1 }, { 3, 2 }, { 4, 5 }, { 1, 0 } };
static const uint8_t bc23_ends[4][2] = { { 0, 1 }, { 3, 2 }, { 5, 4 }, { 4, 5 } };

typedef struct {
	uint32_t spec;
	uint32_t group;
	uint32_t x;
	uint32_t y;
	Rgb want;
	uint32_t tolerance;
} Probe;

typedef struct {
	VkRenderPass render_pass;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES];
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkPipeline pipeline;
	VkSampler samplers[2];
	VkDescriptorPool pool;
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkBuffer staging;
	VkDeviceMemory staging_memory;
	VkBuffer readback;
	VkDeviceMemory readback_memory;
	uint8_t *staged;
	const uint8_t *read;
	VkImage images[MAX_SPECS];
	VkDeviceMemory image_memory[MAX_SPECS];
	VkImageView views[MAX_SPECS][2];
	VkImage blitted[MAX_SPECS];
	VkDeviceMemory blitted_memory[MAX_SPECS];
	VkDescriptorSet sets[MAX_SPECS][QUADS];
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	Probe probes[MAX_PROBES];
	uint32_t probe_count;
	uint32_t missed[MAX_SPECS][GROUPS];
	uint32_t logged;
} Scene;

static bool compressed(const Spec *spec)
{
	return spec->kind != KIND_UNITS;
}

// Xenos filtered the half floats and 10-10-10-2 wrong on the console, so the layer samples them nearest
// (results/2026-10-16-milestone-20/).
static bool filters(const Spec *spec)
{
	return spec->number != HALF && spec->bits[0] != 10;
}

static uint32_t side(const Spec *spec)
{
	return compressed(spec) ? BLOCK_TEXELS : TEXELS;
}

static uint32_t field_max(uint32_t bits)
{
	return bits == 32 ? 0xFFFFFFFF : (1u << bits) - 1;
}

// A field's bits, a bit at a time, counted from the unit's least significant end, its last byte.
static void put_field(uint8_t *texel, uint32_t texel_bytes, uint32_t shift, uint32_t bits, uint32_t value)
{
	for (uint32_t k = 0; k < bits; k++) {
		const uint32_t at = shift + k;
		uint8_t *const byte = &texel[texel_bytes - 1 - at / 8];
		*byte = (uint8_t)((*byte & ~(1u << at % 8)) | (value >> k & 1) << at % 8);
	}
}

static uint32_t get_field(const uint8_t *texel, uint32_t texel_bytes, uint32_t shift, uint32_t bits)
{
	uint32_t value = 0;
	for (uint32_t k = 0; k < bits; k++) {
		const uint32_t at = shift + k;
		value |= (uint32_t)(texel[texel_bytes - 1 - at / 8] >> at % 8 & 1) << k;
	}
	return value;
}

static uint32_t float_bits(float value)
{
	uint32_t bits;
	memcpy(&bits, &value, sizeof bits);
	return bits;
}

// The nearest half to a value a half holds within its normal range, or 0.
static uint16_t to_half(float value)
{
	const uint32_t bits = float_bits(value);
	const uint32_t sign = bits >> 16 & 0x8000;
	const int32_t exponent = (int32_t)(bits >> 23 & 0xFF) - 127 + 15;
	if (exponent <= 0)
		return (uint16_t)sign;
	return (uint16_t)(sign | (((uint32_t)exponent << 10 | (bits & 0x7FFFFF) >> 13) + (bits >> 12 & 1)));
}

static float from_half(uint32_t half)
{
	const uint32_t exponent = half >> 10 & 0x1F;
	const float magnitude =
		exponent ? (float)(0x400 | (half & 0x3FF)) * (float)(1u << exponent) / (float)(1u << 25) : 0.0f;
	return half & 0x8000 ? -magnitude : magnitude;
}

// A channel's share of its range at a texel, in fifteenths, apart for each channel.
static float fraction(uint32_t texel, uint32_t channel)
{
	return (float)((texel * 7 + channel * 5) % 16) / 15.0f;
}

// A half texel's channel: the fraction, but past 1 and below 0 at one texel each, which the target clamps.
static float half_value(uint32_t texel, uint32_t channel)
{
	if (texel == 5 && channel == 0)
		return 2.0f;
	if (texel == 10 && channel == 1)
		return -1.0f;
	return fraction(texel, channel);
}

// A signed texel's channel: the fraction spread from -1 to 1, and the most negative at one texel.
static uint32_t signed_field(uint32_t texel, uint32_t channel, uint32_t bits)
{
	if (texel == 9 && channel == 0)
		return 1u << (bits - 1);
	const float value = fraction(texel, channel) * 2.0f - 1.0f;
	return (uint32_t)lrintf(value * (float)field_max(bits - 1)) & field_max(bits);
}

static void encode_units(const Spec *spec, uint32_t texel, uint8_t *out)
{
	memset(out, 0, spec->texel_bytes);
	for (uint32_t c = 0; c < 4; c++) {
		const uint32_t bits = spec->bits[c];
		if (!bits)
			continue;
		uint32_t value = (uint32_t)lrintf(fraction(texel, c) * (float)field_max(bits));
		if (spec->number == HALF)
			value = to_half(half_value(texel, c));
		else if (spec->number == SNORM)
			value = signed_field(texel, c, bits);
		put_field(out, spec->texel_bytes, spec->shifts[c], bits, value);
	}
}

static float clamp(float value, float low, float high)
{
	return !(value > low) ? low : value > high ? high : value;
}

// A field read signed: two's complement, the most negative read as -1.
static float signed_value(uint32_t field, uint32_t bits)
{
	const int32_t number = field >> (bits - 1) ? (int32_t)field - (int32_t)(1u << bits) : (int32_t)field;
	return clamp((float)number / (float)field_max(bits - 1), -1.0f, 1.0f);
}

// Xenos's gamma, which sampling an sRGB image takes: 255ths to 1023ths in four straight pieces
// (xenia:src/xenia/gpu/xenos.cc:27-65; xsdk:tests/test_formats.c, gamma_linear).
static float gamma_linear(float gamma)
{
	const float g = gamma * 255.0f;
	const float slope = g >= 192.0f ? 8.0f : g >= 96.0f ? 4.0f : g >= 64.0f ? 2.0f : 1.0f;
	const float offset = g >= 192.0f ? -1024.0f : g >= 96.0f ? -256.0f : g >= 64.0f ? -64.0f : 0.0f;
	const float base = g * slope + offset;
	return (base + truncf(base * slope / 1024.0f)) / 1023.0f;
}

// sRGB's own curve, which Vulkan's blits and clears convert by.
static float srgb_linear(float value)
{
	return value <= 0.04045f ? value / 12.92f : powf((value + 0.055f) / 1.055f, 2.4f);
}

static float srgb_encoded(float value)
{
	return value <= 0.0031308f ? value * 12.92f : 1.055f * powf(value, 1.0f / 2.4f) - 0.055f;
}

// An sRGB format's colours as sampled, through Xenos's gamma, or as Vulkan decodes them, through sRGB's.
static void linearise(const Spec *spec, float rgba[4], bool sampled)
{
	for (uint32_t c = 0; c < 3 && spec->number == SRGB; c++)
		rgba[c] = sampled ? gamma_linear(rgba[c]) : srgb_linear(rgba[c]);
}

static void decode_units(const Spec *spec, const uint8_t *texel, float rgba[4])
{
	for (uint32_t c = 0; c < 4; c++) {
		const uint32_t bits = spec->bits[c];
		const uint32_t value = get_field(texel, spec->texel_bytes, spec->shifts[c], bits);
		if (!bits)
			rgba[c] = c == 3 ? 1.0f : 0.0f;
		else if (spec->number == HALF)
			rgba[c] = from_half(value);
		else if (spec->number == SNORM)
			rgba[c] = signed_value(value, bits);
		else
			rgba[c] = (float)value / (float)field_max(bits);
	}
}

static void put_le16(uint8_t *out, uint32_t value)
{
	out[0] = (uint8_t)value;
	out[1] = (uint8_t)(value >> 8);
}

static uint32_t get_le16(const uint8_t *in)
{
	return (uint32_t)in[0] | (uint32_t)in[1] << 8;
}

// A colour block, as the Khronos Data Format Specification lays it out: the endpoints, then two index bits a texel.
static void colour_block(uint32_t block, const uint8_t pair[2], uint8_t *out)
{
	put_le16(out, ends[pair[0]]);
	put_le16(out + 2, ends[pair[1]]);
	memset(out + 4, 0, 4);
	for (uint32_t i = 0; i < 16; i++)
		out[4 + i / 4] |= (uint8_t)(((i + block + i / 4) & 3) << (i % 4 * 2));
}

// BC3's alpha ends: eight alphas when the first is above the second, else six and 0 and 1.
static void bc3_alpha(uint32_t block, uint8_t *out)
{
	out[0] = block % 2 ? 32 : 240;
	out[1] = block % 2 ? 224 : 16;
	memset(out + 2, 0, 6);
	for (uint32_t i = 0; i < 16; i++) {
		const uint32_t index = (i + block) & 7;
		for (uint32_t k = 0; k < 3; k++)
			out[2 + (3 * i + k) / 8] |= (uint8_t)((index >> k & 1) << ((3 * i + k) % 8));
	}
}

static void encode_block(const Spec *spec, uint32_t block, uint8_t *out)
{
	if (spec->kind == KIND_BC4 || spec->kind == KIND_BC5) {
		bc3_alpha(block, out);
		if (spec->kind == KIND_BC5)
			bc3_alpha(block + 1, out + 8);
		return;
	}
	if (spec->kind == KIND_BC1 || spec->kind == KIND_BC1_RGB) {
		colour_block(block, bc1_ends[block], out);
		return;
	}
	if (spec->kind == KIND_BC2) {
		for (uint32_t i = 0; i < 8; i++)
			out[i] = (uint8_t)(((2 * i * 7 + block * 3) & 15) | (((2 * i + 1) * 7 + block * 3) & 15) << 4);
	} else {
		bc3_alpha(block, out);
	}
	colour_block(block, bc23_ends[block], out + 8);
}

static void expand_565(uint32_t colour, float out[3])
{
	out[0] = (float)(colour >> 11) / 31.0f;
	out[1] = (float)(colour >> 5 & 0x3F) / 63.0f;
	out[2] = (float)(colour & 0x1F) / 31.0f;
}

// A texel of a colour block, alpha 0 for BC1's transparent black; returns whether it's a blend of the endpoints.
static bool decode_colour(const uint8_t *block, uint32_t texel, bool bc1, float rgba[4])
{
	const uint32_t first = get_le16(block);
	const uint32_t second = get_le16(block + 2);
	const uint32_t index = block[4 + texel / 4] >> (texel % 4 * 2) & 3;
	const bool three = bc1 && first <= second;
	float a[3];
	float b[3];
	expand_565(first, a);
	expand_565(second, b);
	for (uint32_t c = 0; c < 3; c++) {
		const float blends[4] = { a[c], b[c], three ? (a[c] + b[c]) / 2.0f : (2.0f * a[c] + b[c]) / 3.0f,
			three ? 0.0f : (a[c] + 2.0f * b[c]) / 3.0f };
		rgba[c] = blends[index];
	}
	rgba[3] = three && index == 3 ? 0.0f : 1.0f;
	return index == 2 || (index == 3 && !three);
}

static float bc3_alpha_value(const uint8_t *block, uint32_t texel, bool *blended)
{
	const float first = block[0];
	const float second = block[1];
	uint32_t index = 0;
	for (uint32_t k = 0; k < 3; k++)
		index |= (uint32_t)(block[2 + (3 * texel + k) / 8] >> ((3 * texel + k) % 8) & 1) << k;
	*blended = index >= 2 && (first > second || index < 6);
	if (index < 2)
		return (index ? second : first) / 255.0f;
	if (first > second)
		return ((float)(8 - index) * first + (float)(index - 1) * second) / 7.0f / 255.0f;
	if (index >= 6)
		return index == 6 ? 0.0f : 1.0f;
	return ((float)(6 - index) * first + (float)(index - 1) * second) / 5.0f / 255.0f;
}

// A texel of a compressed image; returns whether a decoder may round it its own way.
static bool block_value(const Spec *spec, const uint8_t *staged, uint32_t x, uint32_t y, float rgba[4])
{
	const uint8_t *const block = staged + (y / BLOCK_SIDE * 2 + x / BLOCK_SIDE) * spec->texel_bytes;
	const uint32_t texel = y % BLOCK_SIDE * BLOCK_SIDE + x % BLOCK_SIDE;
	if (spec->kind == KIND_BC4 || spec->kind == KIND_BC5) {
		bool red_blended = false;
		bool green_blended = false;
		rgba[0] = bc3_alpha_value(block, texel, &red_blended);
		rgba[1] = spec->kind == KIND_BC5 ? bc3_alpha_value(block + 8, texel, &green_blended) : 0.0f;
		rgba[2] = 0.0f;
		rgba[3] = 1.0f;
		return red_blended || green_blended;
	}
	const bool bc1 = spec->kind == KIND_BC1 || spec->kind == KIND_BC1_RGB;
	bool blended = decode_colour(bc1 ? block : block + 8, texel, bc1, rgba);
	if (spec->kind == KIND_BC1_RGB)
		rgba[3] = 1.0f;
	else if (spec->kind == KIND_BC2)
		rgba[3] = (float)(block[texel / 2] >> (texel % 2 * 4) & 15) / 15.0f;
	else if (spec->kind == KIND_BC3) {
		bool alpha_blended = false;
		rgba[3] = bc3_alpha_value(block, texel, &alpha_blended);
		blended = blended || alpha_blended;
	}
	return blended;
}

// A texel of the image as written, an sRGB one's colours as sampled or as Vulkan decodes them; returns whether a
// decoder may round it its own way.
static bool texel_value(const Spec *spec, const uint8_t *staged, uint32_t x, uint32_t y, float rgba[4], bool sampled)
{
	bool blended = false;
	if (compressed(spec))
		blended = block_value(spec, staged, x, y, rgba);
	else
		decode_units(spec, staged + (y * TEXELS + x) * spec->texel_bytes, rgba);
	linearise(spec, rgba, sampled);
	return blended;
}

static uint32_t to_byte(float value)
{
	const float clamped = !(value > 0.0f) ? 0.0f : value > 1.0f ? 1.0f : value;
	return (uint32_t)lrintf(clamped * 255.0f);
}

// The shader shows a signed format's -1 to 1 as 0 to 1 (push_quad).
static float shown(const Spec *spec, float value)
{
	return spec->number == SNORM ? value * 0.5f + 0.5f : value;
}

static Rgb rgb_of(const Spec *spec, const float rgba[4])
{
	return (Rgb){ to_byte(shown(spec, rgba[0])), to_byte(shown(spec, rgba[1])), to_byte(shown(spec, rgba[2])) };
}

static Rgb alpha_of(const Spec *spec, const float rgba[4])
{
	const uint32_t alpha = to_byte(shown(spec, rgba[3]));
	return (Rgb){ alpha, alpha, alpha };
}

static void stage(const Spec *spec, uint8_t *out)
{
	if (compressed(spec)) {
		for (uint32_t block = 0; block < 4; block++)
			encode_block(spec, block, out + block * spec->texel_bytes);
		return;
	}
	for (uint32_t texel = 0; texel < TEXELS * TEXELS; texel++)
		encode_units(spec, texel, out + texel * spec->texel_bytes);
}

static uint32_t quad_x(uint32_t index)
{
	return LEFT + index % COLUMNS * STEP;
}

static uint32_t quad_y(uint32_t index, uint32_t quad)
{
	return TOP + index / COLUMNS * BAND_HEIGHT + quad * STEP;
}

// The colour block's texels in BC2 and BC3's fourth block are those whose first endpoint is below the second; an sRGB
// image's filtered texels have a check of their own.
static uint32_t group_of(const Spec *spec, uint32_t x, uint32_t y, uint32_t group)
{
	const bool four = (spec->kind == KIND_BC2 || spec->kind == KIND_BC3) && x >= BLOCK_SIDE && y >= BLOCK_SIDE;
	if (four)
		return GROUP_FOUR_COLOUR;
	return spec->number == SRGB && group == GROUP_FILTERED ? GROUP_SRGB_FILTERED : group;
}

static void add_probe(
	Scene *scene, uint32_t index, uint32_t group, uint32_t x, uint32_t y, Rgb want, uint32_t tolerance)
{
	if (scene->probe_count < MAX_PROBES)
		scene->probes[scene->probe_count++] = (Probe){ index, group, x, y, want, tolerance };
}

// Every texel's centre in the nearest quad and the alpha one; between texels on the filtered one's edge rows and
// columns, where it samples two texels half and half.
static void make_probes(Scene *scene, uint32_t index)
{
	const Spec *const spec = &specs[index];
	const uint32_t n = side(spec);
	const uint32_t texel = QUAD_SIDE / n;
	const uint32_t left = quad_x(index);
	for (uint32_t y = 0; y < n; y++) {
		for (uint32_t x = 0; x < n; x++) {
			float rgba[4];
			const bool blended = texel_value(spec, scene->staged + index * STAGED_BYTES, x, y, rgba, true);
			const uint32_t px = left + x * texel + texel / 2;
			const uint32_t tolerance = blended ? BLEND_TOLERANCE : TOLERANCE;
			add_probe(scene, index, group_of(spec, x, y, GROUP_SAMPLED), px,
				quad_y(index, QUAD_NEAREST) + y * texel + texel / 2, rgb_of(spec, rgba), tolerance);
			add_probe(scene, index, GROUP_SAMPLED, px, quad_y(index, QUAD_ALPHA) + y * texel + texel / 2,
				alpha_of(spec, rgba), tolerance);
		}
	}
	const uint32_t tolerance = compressed(spec) ? BLEND_TOLERANCE : FILTER_TOLERANCE;
	for (uint32_t i = 0; filters(spec) && i + 1 < n; i++) {
		for (uint32_t edge = 0; edge < 2; edge++) {
			const uint32_t far = edge ? n - 1 : 0;
			float a[4];
			float b[4];
			float mean[4];
			(void)texel_value(spec, scene->staged + index * STAGED_BYTES, i, far, a, true);
			(void)texel_value(spec, scene->staged + index * STAGED_BYTES, i + 1, far, b, true);
			for (uint32_t c = 0; c < 4; c++)
				mean[c] = (a[c] + b[c]) / 2.0f;
			add_probe(scene, index, group_of(spec, i + 1, far, GROUP_FILTERED), left + (i + 1) * texel,
				quad_y(index, QUAD_LINEAR) + far * texel + texel / 2, rgb_of(spec, mean), tolerance);
			(void)texel_value(spec, scene->staged + index * STAGED_BYTES, far, i, a, true);
			(void)texel_value(spec, scene->staged + index * STAGED_BYTES, far, i + 1, b, true);
			for (uint32_t c = 0; c < 4; c++)
				mean[c] = (a[c] + b[c]) / 2.0f;
			add_probe(scene, index, group_of(spec, far, i + 1, GROUP_FILTERED), left + far * texel + texel / 2,
				quad_y(index, QUAD_LINEAR) + (i + 1) * texel, rgb_of(spec, mean), tolerance);
		}
	}
}

static const char *const group_names[GROUPS] = { "sampled", "filtered", "four-colour", "sRGB filtered" };

// A HarnessCheck: every probe; misses are counted for their format and group, and the first few logged.
static bool probes_hold(uint32_t address, const char *what, void *context)
{
	Scene *const scene = context;
	bool holds = true;
	for (uint32_t i = 0; i < scene->probe_count; i++) {
		const Probe *const probe = &scene->probes[i];
		const Rgb pixel = harness_pixel(address, probe->x, probe->y);
		if (harness_near(pixel, probe->want, probe->tolerance))
			continue;
		scene->missed[probe->spec][probe->group]++;
		if (scene->logged++ < MAX_MISSES_LOGGED)
			xsdk_log_write("formats: %s: %s %s at %u, %u held %02X %02X %02X, not %02X %02X %02X", what,
				specs[probe->spec].name, group_names[probe->group], (unsigned)probe->x, (unsigned)probe->y,
				(unsigned)pixel.red, (unsigned)pixel.green, (unsigned)pixel.blue, (unsigned)probe->want.red,
				(unsigned)probe->want.green, (unsigned)probe->want.blue);
		holds = false;
	}
	return holds;
}

static bool cached_buffer(const Harness *harness, VkBufferUsageFlags usage, uint32_t size_bytes, VkBuffer *buffer,
	VkDeviceMemory *memory, void **mapped)
{
	const VkBufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size_bytes, .usage = usage
	};
	if (!harness_ok("vkCreateBuffer", vkCreateBuffer(harness->device, &info, NULL, buffer)))
		return false;
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(harness->device, *buffer, &requirements);
	const VkMemoryAllocateInfo allocate = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
		.memoryTypeIndex = CACHED_TYPE,
	};
	return harness_ok("vkAllocateMemory", vkAllocateMemory(harness->device, &allocate, NULL, memory)) &&
		harness_ok("vkBindBufferMemory", vkBindBufferMemory(harness->device, *buffer, *memory, 0)) &&
		harness_ok("vkMapMemory", vkMapMemory(harness->device, *memory, 0, VK_WHOLE_SIZE, 0, mapped));
}

static bool make_image(const Harness *harness, VkFormat format, uint32_t texels, VkImageUsageFlags usage,
	VkImage *image, VkDeviceMemory *memory)
{
	const VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = format,
		.extent = { texels, texels, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = usage,
	};
	if (!harness_ok("vkCreateImage", vkCreateImage(harness->device, &info, NULL, image)))
		return false;
	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(harness->device, *image, &requirements);
	const VkMemoryAllocateInfo allocate = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
	};
	return harness_ok("vkAllocateMemory", vkAllocateMemory(harness->device, &allocate, NULL, memory)) &&
		harness_ok("vkBindImageMemory", vkBindImageMemory(harness->device, *image, *memory, 0));
}

// The image as it is, and with its alpha in red, green and blue.
static bool make_views(const Harness *harness, Scene *scene, uint32_t index)
{
	for (uint32_t i = 0; i < 2; i++) {
		const VkComponentSwizzle alpha = VK_COMPONENT_SWIZZLE_A;
		const VkImageViewCreateInfo info = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = scene->images[index],
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = specs[index].format,
			.components =
				i ? (VkComponentMapping){ alpha, alpha, alpha, VK_COMPONENT_SWIZZLE_ONE } : (VkComponentMapping){ 0 },
			.subresourceRange = { COLOR, 0, 1, 0, 1 },
		};
		if (!harness_ok("vkCreateImageView", vkCreateImageView(harness->device, &info, NULL, &scene->views[index][i])))
			return false;
	}
	return true;
}

static bool make_images(const Harness *harness, Scene *scene)
{
	const VkImageUsageFlags usage =
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	for (uint32_t i = 0; i < SPECS; i++) {
		if (!make_image(harness, specs[i].format, side(&specs[i]), usage, &scene->images[i], &scene->image_memory[i]) ||
			!make_views(harness, scene, i))
			return false;
		if (!compressed(&specs[i]) &&
			!make_image(harness, specs[i].format, BLITTED,
				VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &scene->blitted[i],
				&scene->blitted_memory[i]))
			return false;
	}
	return true;
}

static bool make_pipeline(const Harness *harness, Scene *scene)
{
	const VkDescriptorSetLayoutBinding binding = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	};
	const VkDescriptorSetLayoutCreateInfo set_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &binding,
	};
	const VkPushConstantRange ranges[] = {
		{ VK_SHADER_STAGE_VERTEX_BIT, 0, 48 },
		{ VK_SHADER_STAGE_FRAGMENT_BIT, 48, 32 },
	};
	const VkPipelineLayoutCreateInfo layout_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &scene->set_layout,
		.pushConstantRangeCount = 2,
		.pPushConstantRanges = ranges,
	};
	if (!harness_ok("vkCreateDescriptorSetLayout",
			vkCreateDescriptorSetLayout(harness->device, &set_info, NULL, &scene->set_layout)) ||
		!harness_ok(
			"vkCreatePipelineLayout", vkCreatePipelineLayout(harness->device, &layout_info, NULL, &scene->layout)))
		return false;
	const HarnessPipeline spec = {
		.vertex_spirv = format_vert_spirv,
		.vertex_bytes = FORMAT_VERT_SPIRV_BYTES,
		.fragment_spirv = format_frag_spirv,
		.fragment_bytes = FORMAT_FRAG_SPIRV_BYTES,
		.bindings = &harness_quad_binding,
		.binding_count = 1,
		.attributes = &harness_quad_attribute,
		.attribute_count = 1,
		.layout = scene->layout,
	};
	return harness_ok("render pass", harness_render_pass(harness, &scene->render_pass, scene->framebuffers)) &&
		harness_ok("pipeline", harness_pipeline(harness, &spec, scene->render_pass, &scene->pipeline));
}

static bool make_descriptors(const Harness *harness, Scene *scene)
{
	for (uint32_t i = 0; i < 2; i++) {
		const VkFilter filter = i ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
		const VkSamplerCreateInfo info = {
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.magFilter = filter,
			.minFilter = filter,
			.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
			.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		};
		if (!harness_ok("vkCreateSampler", vkCreateSampler(harness->device, &info, NULL, &scene->samplers[i])))
			return false;
	}
	const VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SPECS * QUADS };
	const VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = SPECS * QUADS,
		.poolSizeCount = 1,
		.pPoolSizes = &size,
	};
	if (!harness_ok("vkCreateDescriptorPool", vkCreateDescriptorPool(harness->device, &pool_info, NULL, &scene->pool)))
		return false;
	for (uint32_t i = 0; i < SPECS; i++) {
		for (uint32_t quad = 0; quad < QUADS; quad++) {
			const VkDescriptorSetAllocateInfo allocate = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				.descriptorPool = scene->pool,
				.descriptorSetCount = 1,
				.pSetLayouts = &scene->set_layout,
			};
			if (!harness_ok("vkAllocateDescriptorSets",
					vkAllocateDescriptorSets(harness->device, &allocate, &scene->sets[i][quad])))
				return false;
			const VkDescriptorImageInfo image = { scene->samplers[quad == QUAD_LINEAR],
				scene->views[i][quad == QUAD_ALPHA], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
			const VkWriteDescriptorSet write = {
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = scene->sets[i][quad],
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.pImageInfo = &image,
			};
			vkUpdateDescriptorSets(harness->device, 1, &write, 0, NULL);
		}
	}
	return true;
}

static bool build(const Harness *harness, Scene *scene)
{
	void *staged = NULL;
	void *read = NULL;
	const bool buffers = make_pipeline(harness, scene) &&
		harness_ok("vertices", harness_quad_buffer(harness, &scene->vertices, &scene->vertex_memory)) &&
		cached_buffer(harness, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, SPECS * STAGED_BYTES, &scene->staging,
			&scene->staging_memory, &staged) &&
		cached_buffer(harness, VK_BUFFER_USAGE_TRANSFER_DST_BIT, SPECS * READ_BYTES, &scene->readback,
			&scene->readback_memory, &read);
	if (!buffers)
		return false;
	scene->staged = staged;
	scene->read = read;
	for (uint32_t i = 0; i < SPECS; i++)
		stage(&specs[i], scene->staged + i * STAGED_BYTES);
	for (uint32_t i = 0; i < SPECS; i++)
		make_probes(scene, i);
	return make_images(harness, scene) && make_descriptors(harness, scene);
}

static void barrier(VkCommandBuffer commands, VkImage image, VkImageLayout from, VkImageLayout to)
{
	const VkImageMemoryBarrier info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
		.oldLayout = from,
		.newLayout = to,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = image,
		.subresourceRange = { COLOR, 0, 1, 0, 1 },
	};
	vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &info);
}

static VkBufferImageCopy whole(uint32_t at, uint32_t texels)
{
	return (VkBufferImageCopy){
		.bufferOffset = at,
		.imageSubresource = { COLOR, 0, 0, 1 },
		.imageExtent = { texels, texels, 1 },
	};
}

// The clear's colour, its channels apart and none halfway between two of any format's steps.
static const float clear_colour[4] = { 0.2f, 0.4f, 0.6f, 0.8f };

// An uncompressed image blitted 4x4 to 2x2, read back, then cleared and read back again.
static void record_blit(const Scene *scene, VkCommandBuffer commands, uint32_t index)
{
	const VkImage image = scene->blitted[index];
	const VkImageBlit region = {
		.srcSubresource = { COLOR, 0, 0, 1 },
		.srcOffsets = { { 0, 0, 0 }, { TEXELS, TEXELS, 1 } },
		.dstSubresource = { COLOR, 0, 0, 1 },
		.dstOffsets = { { 0, 0, 0 }, { BLITTED, BLITTED, 1 } },
	};
	barrier(commands, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	vkCmdBlitImage(commands, scene->images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
		filters(&specs[index]) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
	const VkBufferImageCopy blitted = whole(index * READ_BYTES + BLIT_AT, BLITTED);
	vkCmdCopyImageToBuffer(commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, scene->readback, 1, &blitted);
	const VkClearColorValue colour = { .float32 = {
										   clear_colour[0], clear_colour[1], clear_colour[2], clear_colour[3] } };
	const VkImageSubresourceRange range = { COLOR, 0, 1, 0, 1 };
	vkCmdClearColorImage(commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &range);
	const VkBufferImageCopy cleared = whole(index * READ_BYTES + CLEAR_AT, BLITTED);
	vkCmdCopyImageToBuffer(commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, scene->readback, 1, &cleared);
}

static bool begin(VkCommandBuffer commands)
{
	const VkCommandBufferBeginInfo info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	return harness_ok("vkBeginCommandBuffer", vkBeginCommandBuffer(commands, &info));
}

static bool submit(const Harness *harness, VkCommandBuffer commands)
{
	const VkSubmitInfo info = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &commands,
	};
	const bool done = harness_ok("vkEndCommandBuffer", vkEndCommandBuffer(commands)) &&
		harness_ok("vkQueueSubmit", vkQueueSubmit(harness->queue, 1, &info, harness->fence)) &&
		harness_ok("vkWaitForFences", vkWaitForFences(harness->device, 1, &harness->fence, VK_TRUE, FENCE_TIMEOUT_NS));
	// The tests after submit with the fence and wait on it, so it's left unsignaled, as they leave it.
	(void)vkResetFences(harness->device, 1, &harness->fence);
	return done;
}

// Each image copied in and straight back out, and the uncompressed ones blitted and cleared, in one submission.
static bool transfer(const Harness *harness, const Scene *scene, VkCommandBuffer commands)
{
	if (!begin(commands))
		return false;
	for (uint32_t i = 0; i < SPECS; i++) {
		const VkImage image = scene->images[i];
		const VkBufferImageCopy in = whole(i * STAGED_BYTES, side(&specs[i]));
		const VkBufferImageCopy out = whole(i * READ_BYTES, side(&specs[i]));
		barrier(commands, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		vkCmdCopyBufferToImage(commands, scene->staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &in);
		barrier(commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		vkCmdCopyImageToBuffer(commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scene->readback, 1, &out);
		if (!compressed(&specs[i]))
			record_blit(scene, commands, i);
		barrier(commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
	return submit(harness, commands);
}

// Whether two encodings of a channel are a step apart at most, as rounding may take either; a signed one's as numbers.
static bool near_field(const Spec *spec, uint32_t bits, uint32_t got, uint32_t want)
{
	const int32_t sign = spec->number == SNORM ? (int32_t)(1u << bits) : 0;
	const int32_t a = sign && got >> (bits - 1) ? (int32_t)got - sign : (int32_t)got;
	const int32_t b = sign && want >> (bits - 1) ? (int32_t)want - sign : (int32_t)want;
	return a + 1 >= b && a <= b + 1;
}

// A channel's encoding of a value, as Vulkan converts it: an sRGB colour through sRGB's curve.
static uint32_t field_of(const Spec *spec, uint32_t channel, float value)
{
	const uint32_t bits = spec->bits[channel];
	if (spec->number == HALF)
		return to_half(value);
	if (spec->number == SNORM)
		return (uint32_t)lrintf(clamp(value, -1.0f, 1.0f) * (float)field_max(bits - 1)) & field_max(bits);
	const float unorm = spec->number == SRGB && channel < 3 ? srgb_encoded(clamp(value, 0.0f, 1.0f)) : value;
	return (uint32_t)lrintf(clamp(unorm, 0.0f, 1.0f) * (float)field_max(bits));
}

// Each channel of a texel read back against the value it should hold.
static bool texel_holds(const Spec *spec, const uint8_t *texel, const float want[4])
{
	bool holds = true;
	for (uint32_t c = 0; c < 4; c++) {
		if (!spec->bits[c])
			continue;
		const uint32_t got = get_field(texel, spec->texel_bytes, spec->shifts[c], spec->bits[c]);
		holds = holds && near_field(spec, spec->bits[c], got, field_of(spec, c, want[c]));
	}
	return holds;
}

static bool copied_back(const Scene *scene, uint32_t index)
{
	const Spec *const spec = &specs[index];
	const uint32_t bytes = compressed(spec) ? 4 * spec->texel_bytes : TEXELS * TEXELS * spec->texel_bytes;
	return !memcmp(scene->read + index * READ_BYTES, scene->staged + index * STAGED_BYTES, bytes);
}

// Each texel of the 2x2 the mean of the four under it, as the blit samples its centre between them.
static bool blitted_back(const Scene *scene, uint32_t index)
{
	const Spec *const spec = &specs[index];
	const uint8_t *const staged = scene->staged + index * STAGED_BYTES;
	bool holds = true;
	for (uint32_t y = 0; y < BLITTED; y++) {
		for (uint32_t x = 0; x < BLITTED; x++) {
			float mean[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			for (uint32_t i = 0; i < 4; i++) {
				float rgba[4];
				(void)texel_value(spec, staged, 2 * x + i % 2, 2 * y + i / 2, rgba, false);
				for (uint32_t c = 0; c < 4; c++)
					mean[c] += rgba[c] / 4.0f;
			}
			// A nearest blit takes the texel at the destination texel's centre (src/transfer.c).
			if (!filters(spec))
				(void)texel_value(spec, staged, 2 * x + 1, 2 * y + 1, mean, false);
			const uint8_t *const texel =
				scene->read + index * READ_BYTES + BLIT_AT + (y * BLITTED + x) * spec->texel_bytes;
			holds = holds && texel_holds(spec, texel, mean);
		}
	}
	return holds;
}

static bool cleared_back(const Scene *scene, uint32_t index)
{
	const Spec *const spec = &specs[index];
	bool holds = true;
	for (uint32_t i = 0; i < BLITTED * BLITTED; i++)
		holds = holds &&
			texel_holds(spec, scene->read + index * READ_BYTES + CLEAR_AT + i * spec->texel_bytes, clear_colour);
	return holds;
}

static void check_transfers(const Scene *scene, bool transferred)
{
	bool copied = transferred;
	bool blits = transferred;
	bool clears = transferred;
	for (uint32_t i = 0; i < SPECS && transferred; i++) {
		const bool this_copied = copied_back(scene, i);
		const bool this_blitted = compressed(&specs[i]) || blitted_back(scene, i);
		const bool this_cleared = compressed(&specs[i]) || cleared_back(scene, i);
		if (!this_copied || !this_blitted || !this_cleared)
			xsdk_log_write("formats: %s read back %s, blitted %s, cleared %s", specs[i].name,
				this_copied ? "right" : "wrong", this_blitted ? "right" : "wrong", this_cleared ? "right" : "wrong");
		copied = copied && this_copied;
		blits = blits && this_blitted;
		clears = clears && this_cleared;
	}
	check("formats: each image copied back out to a buffer holds the bytes it was copied in from", copied);
	check(
		"formats: each uncompressed image blitted from 4x4 to 2x2 holds the means of its texels, or the texels at its "
		"centres where it's sampled nearest",
		blits);
	check("formats: each uncompressed image cleared holds the colour in every texel, as its format packs it", clears);
}

static void push_quad(VkCommandBuffer commands, VkPipelineLayout layout, uint32_t index, uint32_t quad)
{
	const float half_width = HARNESS_WIDTH / 2.0f;
	const float half_height = HARNESS_HEIGHT / 2.0f;
	// A pixel's centre samples at its own left and top edges' coordinates, so texels' centres and the midpoints
	// between them land on pixels' centres.
	const float shift = -0.5f / QUAD_SIDE;
	const float place[12] = {
		QUAD_SIDE / half_width,
		QUAD_SIDE / half_height,
		0.0f,
		1.0f,
		(float)quad_x(index) / half_width - 1.0f,
		(float)quad_y(index, quad) / half_height - 1.0f,
		0.0f,
		0.0f,
		shift,
		shift,
		0.0f,
		0.0f,
	};
	// A signed format's -1 to 1 shows as 0 to 1.
	const float scale = specs[index].number == SNORM ? 0.5f : 1.0f;
	const float bias = specs[index].number == SNORM ? 0.5f : 0.0f;
	const float scale_bias[8] = { scale, scale, scale, scale, bias, bias, bias, bias };
	vkCmdPushConstants(commands, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof place, place);
	vkCmdPushConstants(commands, layout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof place, sizeof scale_bias, scale_bias);
	vkCmdDraw(commands, 6, 1, 0, 0);
}

// A dark grey around the quads.
static const VkClearValue background = { .color = { .float32 = { 0.125f, 0.125f, 0.125f, 1.0f } } };

static VkResult record(const Harness *harness, Scene *scene)
{
	VkResult result = harness_command_buffers(harness, scene->commands);
	for (uint32_t i = 0; i < harness->image_count && result == VK_SUCCESS; i++) {
		const VkCommandBuffer commands = scene->commands[i];
		const VkRenderPassBeginInfo pass = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = scene->render_pass,
			.framebuffer = scene->framebuffers[i],
			.renderArea = { { 0, 0 }, { HARNESS_WIDTH, HARNESS_HEIGHT } },
			.clearValueCount = 1,
			.pClearValues = &background,
		};
		const VkDeviceSize offset = 0;
		vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipeline);
		vkCmdBindVertexBuffers(commands, 0, 1, &scene->vertices, &offset);
		for (uint32_t index = 0; index < SPECS; index++) {
			for (uint32_t quad = 0; quad < QUADS; quad++) {
				vkCmdBindDescriptorSets(
					commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->layout, 0, 1, &scene->sets[index][quad], 0, NULL);
				push_quad(commands, scene->layout, index, quad);
			}
		}
		vkCmdEndRenderPass(commands);
		result = vkEndCommandBuffer(commands);
	}
	return result;
}

static void check_sampling(const Scene *scene, uint32_t frames)
{
	check("formats: 30 frames drawn and presented", frames == FRAMES);
	char name[160];
	bool filtered = frames == FRAMES;
	bool four_colours = frames == FRAMES;
	bool srgb_filtered = frames == FRAMES;
	for (uint32_t i = 0; i < SPECS; i++) {
		(void)snprintf(name, sizeof name,
			"formats: %s sampled nearest, every texel's red, green, blue and alpha as written%s", specs[i].name,
			specs[i].number == SRGB ? ", its colours through Xenos's gamma curve" : "");
		check(name, frames == FRAMES && !scene->missed[i][GROUP_SAMPLED]);
		filtered = filtered && !scene->missed[i][GROUP_FILTERED];
		four_colours = four_colours && !scene->missed[i][GROUP_FOUR_COLOUR];
		srgb_filtered = srgb_filtered && !scene->missed[i][GROUP_SRGB_FILTERED];
	}
	check(
		"formats: every format but the half floats and 10-10-10-2 filtered linearly halfway between two texels "
		"gives their mean",
		filtered);
	check(
		"formats: BC2 and BC3 colour blocks whose first endpoint is below the second take four colours", four_colours);
	check(
		"formats: sRGB images filtered linearly blend their texels after Xenos's curve, as Vulkan's do after decoding",
		srgb_filtered);
}

// The three of them milestone 25 draws into, 10-10-10-2 blending and the half floats not (test_targets.c).
static VkFormatFeatureFlags attachment_features(VkFormat format)
{
	switch (format) {
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
		return VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
	case VK_FORMAT_R16G16_SFLOAT:
	case VK_FORMAT_R16G16B16A16_SFLOAT:
		return VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
	default:
		return 0;
	}
}

// The features each format reports, as docs/SUPPORT.md lists them.
static void check_properties(const Harness *harness)
{
	const VkFormatFeatureFlags filtered =
		VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
	const VkFormatFeatureFlags blits = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
	bool reported = true;
	for (uint32_t i = 0; i < SPECS; i++) {
		const Spec *const spec = &specs[i];
		VkFormatProperties properties;
		vkGetPhysicalDeviceFormatProperties(harness->physical_device, spec->format, &properties);
		VkImageFormatProperties image;
		const bool attachment =
			vkGetPhysicalDeviceImageFormatProperties(harness->physical_device, spec->format, VK_IMAGE_TYPE_2D,
				VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, &image) == VK_SUCCESS;
		const bool linear =
			vkGetPhysicalDeviceImageFormatProperties(harness->physical_device, spec->format, VK_IMAGE_TYPE_2D,
				VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, &image) == VK_SUCCESS;
		const VkFormatFeatureFlags sampled = filters(spec) ? filtered : VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
		const VkFormatFeatureFlags drawn = attachment_features(spec->format);
		const VkFormatFeatureFlags wanted = (compressed(spec) ? sampled : sampled | blits) | drawn;
		const bool this_reported = properties.optimalTilingFeatures == wanted && !properties.linearTilingFeatures &&
			attachment == (drawn != 0) && linear != compressed(spec);
		if (!this_reported)
			xsdk_log_write("formats: %s reports optimal features %08X, linear %08X", spec->name,
				(unsigned)properties.optimalTilingFeatures, (unsigned)properties.linearTilingFeatures);
		reported = reported && this_reported;
	}
	VkPhysicalDeviceFeatures features;
	vkGetPhysicalDeviceFeatures(harness->physical_device, &features);
	check(
		"formats: each reports sampling, and filtering but the half floats and 10-10-10-2, the uncompressed blits and "
		"linear images too, colour attachments only A2B10G10R10_UNORM, R16G16_SFLOAT and R16G16B16A16_SFLOAT "
		"(milestone 25), and textureCompressionBC is off",
		reported && !features.textureCompressionBC);
}

// One refused command, recorded alone: a BC1 image's clear, its blit, or its copy to an uncompressed image of its
// blocks' size.
static void record_refused(VkCommandBuffer commands, VkImage bc1, VkImage other, uint32_t which)
{
	const VkImageSubresourceRange range = { COLOR, 0, 1, 0, 1 };
	const VkClearColorValue colour = { .float32 = { 0.0f, 0.0f, 0.0f, 0.0f } };
	const VkImageBlit blit = {
		.srcSubresource = { COLOR, 0, 0, 1 },
		.srcOffsets = { { 0, 0, 0 }, { BLOCK_TEXELS, BLOCK_TEXELS, 1 } },
		.dstSubresource = { COLOR, 0, 0, 1 },
		.dstOffsets = { { 0, 0, 0 }, { BLOCK_TEXELS, BLOCK_TEXELS, 1 } },
	};
	const VkImageCopy copy = {
		.srcSubresource = { COLOR, 0, 0, 1 },
		.dstSubresource = { COLOR, 0, 0, 1 },
		.extent = { BLOCK_TEXELS, BLOCK_TEXELS, 1 },
	};
	if (which == 0)
		vkCmdClearColorImage(commands, bc1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &range);
	else if (which == 1)
		vkCmdBlitImage(commands, bc1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, bc1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &blit, VK_FILTER_NEAREST);
	else
		vkCmdCopyImage(
			commands, bc1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, other, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
}

static void check_refusals(const Harness *harness, const Scene *scene, VkCommandBuffer commands)
{
	VkImage other = VK_NULL_HANDLE;
	VkDeviceMemory other_memory = VK_NULL_HANDLE;
	bool refused = make_image(
		harness, VK_FORMAT_R16G16B16A16_SFLOAT, BLOCK_TEXELS, VK_IMAGE_USAGE_TRANSFER_DST_BIT, &other, &other_memory);
	for (uint32_t i = 0; i < 3 && refused; i++) {
		refused = begin(commands);
		record_refused(commands, scene->images[1], other, i);
		refused = refused && vkEndCommandBuffer(commands) == VK_ERROR_FEATURE_NOT_PRESENT;
	}
	vkDestroyImage(harness->device, other, NULL);
	vkFreeMemory(harness->device, other_memory, NULL);
	check("formats: a BC1 image's clear, blit, and copy to a 64-bit uncompressed image are refused", refused);
}

static void destroy(const Harness *harness, Scene *scene)
{
	const VkDevice device = harness->device;
	vkFreeCommandBuffers(device, harness->pool, harness->image_count, scene->commands);
	vkDestroyDescriptorPool(device, scene->pool, NULL);
	for (uint32_t i = 0; i < 2; i++)
		vkDestroySampler(device, scene->samplers[i], NULL);
	for (uint32_t i = 0; i < SPECS; i++) {
		vkDestroyImageView(device, scene->views[i][0], NULL);
		vkDestroyImageView(device, scene->views[i][1], NULL);
		vkDestroyImage(device, scene->images[i], NULL);
		vkFreeMemory(device, scene->image_memory[i], NULL);
		vkDestroyImage(device, scene->blitted[i], NULL);
		vkFreeMemory(device, scene->blitted_memory[i], NULL);
	}
	const VkBuffer buffers[] = { scene->vertices, scene->staging, scene->readback };
	const VkDeviceMemory memories[] = { scene->vertex_memory, scene->staging_memory, scene->readback_memory };
	for (uint32_t i = 0; i < sizeof buffers / sizeof buffers[0]; i++) {
		vkDestroyBuffer(device, buffers[i], NULL);
		vkFreeMemory(device, memories[i], NULL);
	}
	vkDestroyPipeline(device, scene->pipeline, NULL);
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	vkDestroyDescriptorSetLayout(device, scene->set_layout, NULL);
	harness_destroy_render_pass(harness, scene->render_pass, scene->framebuffers);
}

void test_formats(const Harness *harness)
{
	// Static, as its probes are too many for the stack.
	static Scene scene;
	scene = (Scene){ .render_pass = VK_NULL_HANDLE };
	const VkCommandBufferAllocateInfo allocate = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = harness->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBuffer commands = VK_NULL_HANDLE;
	const bool built = build(harness, &scene) &&
		harness_ok("vkAllocateCommandBuffers", vkAllocateCommandBuffers(harness->device, &allocate, &commands));
	const bool transferred = built && transfer(harness, &scene, commands);
	check("formats: an image of each format, its views and the pipeline made, and the images copied in", transferred);
	check_transfers(&scene, transferred);
	const bool recorded = transferred && harness->swapchain && harness_ok("recording", record(harness, &scene));
	uint32_t misses = 0;
	const uint32_t frames =
		recorded ? harness_frames(harness, scene.commands, FRAMES, probes_hold, &scene, &misses) : 0;
	xsdk_log_write(
		"formats: %u probes a frame, %u frames held a wrong one", (unsigned)scene.probe_count, (unsigned)misses);
	for (uint32_t i = 0; i < SPECS; i++) {
		for (uint32_t group = 0; group < GROUPS; group++) {
			if (scene.missed[i][group])
				xsdk_log_write("formats: %s %s: %u probes missed over the frames", specs[i].name, group_names[group],
					(unsigned)scene.missed[i][group]);
		}
	}
	check_sampling(&scene, frames);
	check("formats: the image on screen shows them too", harness_screen(harness, probes_hold, &scene));
	check_properties(harness);
	if (commands) {
		check_refusals(harness, &scene, commands);
		vkFreeCommandBuffers(harness->device, harness->pool, 1, &commands);
	}
	destroy(harness, &scene);
}
