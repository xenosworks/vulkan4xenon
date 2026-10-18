#include "shader.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <vulkan/vulkan.h>

#include "host.h"
#include "objects.h"
#include "vulkan4xenon.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"
#include "xsdk/xsc.h"

enum {
	FNV_OFFSET_BASIS_HIGH = 0xCBF29CE4,
	FNV_OFFSET_BASIS_LOW = 0x84222325,
	FNV_PRIME_HIGH = 0x00000100,
	FNV_PRIME_LOW = 0x000001B3,
	// The widths of V4xShaderResource's fields.
	MAX_RESOURCE_INDEX = 0xFF,
	MAX_RESOURCE_REGISTER = 0xFFFF,
	NANOSECONDS_PER_MICROSECOND = 1000,
	MICROSECONDS_PER_SECOND = 1000000,
	CACHE_PATH_BYTES = 256,
	// "V4XS", and the layout of what follows it, which changes as V4xShader's parts do: 2 since clip planes.
	CACHE_MAGIC = 0x56345853,
	CACHE_LAYOUT = 2,
	// No container xost writes is near this; a file that says so is damaged.
	MAX_CACHED_BYTES = 1024 * 1024,
	FOLDER_MODE = 0777,
};

// A kept shader's file: this, then the shader's container, literals, resources and clip planes as its allocation holds
// them.
typedef struct {
	uint32_t magic;
	uint32_t layout;
	uint64_t spirv_hash;
	uint32_t spirv_size_bytes;
	uint32_t container_size_bytes;
	uint32_t literal_count;
	uint32_t resource_count;
	uint32_t clip_plane_count;
} CacheHeader;

// Named before shader modules are made, and only read after.
static char cache_directory[CACHE_PATH_BYTES];
// Added to and read atomically, since each device's shaders_lock covers its own shaders only.
static uint32_t compiled_count;
static uint32_t read_count;

// Weak, so an app that links no bundle still links: the address is then NULL. A weak definition instead gets folded.
extern const V4xShaderBundle v4x_shader_bundle __attribute__((weak));

// 64-bit FNV-1a over each word's bytes, low first, so the tool and the console agree whatever their byte order.
static uint64_t spirv_hash(const uint32_t *code, uint32_t size_bytes)
{
	const uint64_t prime = (uint64_t)FNV_PRIME_HIGH << 32 | FNV_PRIME_LOW;
	uint64_t hash = (uint64_t)FNV_OFFSET_BASIS_HIGH << 32 | FNV_OFFSET_BASIS_LOW;
	for (uint32_t i = 0; i < size_bytes / 4; i++) {
		for (uint32_t shift = 0; shift < 32; shift += 8) {
			hash ^= code[i] >> shift & 0xFF;
			hash *= prime;
		}
	}
	return hash;
}

static const V4xShader *in_bundle(const V4xShaderBundle *bundle, uint64_t hash, uint32_t size_bytes)
{
	for (uint32_t i = 0; bundle && i < bundle->count; i++) {
		const V4xShader *const shader = &bundle->shaders[i];
		if (shader->spirv_hash == hash && shader->spirv_size_bytes == size_bytes)
			return shader;
	}
	return NULL;
}

const V4xShader *v4x_shader_find(const V4xShaderBundle *bundle, const uint32_t *code, uint32_t size_bytes)
{
	const uint64_t hash = spirv_hash(code, size_bytes);
	const V4xShader *const found = in_bundle(bundle, hash, size_bytes);
	if (!found)
		xsdk_log_write("vulkan4xenon: no compiled shader for SPIR-V %08X%08X, %u bytes", (unsigned)(hash >> 32),
			(unsigned)hash, (unsigned)size_bytes);
	return found;
}

static uint64_t now_us(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * MICROSECONDS_PER_SECOND + (uint64_t)now.tv_nsec / NANOSECONDS_PER_MICROSECOND;
}

static uint8_t resource_kind(XsdkXscResourceKind kind)
{
	switch (kind) {
	case XSDK_XSC_PUSH_CONSTANTS:
		return V4X_RESOURCE_PUSH_CONSTANTS;
	case XSDK_XSC_UNIFORM_BUFFER:
		return V4X_RESOURCE_UNIFORM_BUFFER;
	case XSDK_XSC_COMPARISON:
		return V4X_RESOURCE_COMPARISON;
	case XSDK_XSC_INSTANCE_INDEX:
		return V4X_RESOURCE_INSTANCE_INDEX;
	default:
		return V4X_RESOURCE_SAMPLER;
	}
}

// What the layer can't take: the constants of a hardware loop a bare uniform counts, as tools/build_shaders.py refuses,
// or too wide a resource, loop count or clip plane's register.
static bool layer_takes(const XsdkXscShader *compiled)
{
	for (size_t i = 0; i < compiled->need_count; i++) {
		const XsdkXscNeed *const need = &compiled->needs[i];
		const bool loop = need->kind == XSDK_XSC_BLOCK_LOOP && need->block != XSDK_XSC_SAMPLER &&
			need->set <= MAX_RESOURCE_INDEX && need->binding <= MAX_RESOURCE_INDEX &&
			need->offset_bytes <= MAX_RESOURCE_REGISTER && need->reg < XSDK_LOOP_CONSTANTS;
		const bool fixed =
			need->kind == XSDK_XSC_FIXED_LOOP && need->regs <= V4X_MAX_LOOP_PASSES && need->reg < XSDK_LOOP_CONSTANTS;
		const bool plane = need->kind == XSDK_XSC_CLIP_PLANE && need->reg <= MAX_RESOURCE_REGISTER;
		if (need->kind != XSDK_XSC_LITERAL && !loop && !fixed && !plane)
			return false;
	}
	for (size_t i = 0; i < compiled->resource_count; i++) {
		const XsdkXscResource *const r = &compiled->resources[i];
		if (r->set > MAX_RESOURCE_INDEX || r->binding > MAX_RESOURCE_INDEX || r->reg > MAX_RESOURCE_REGISTER ||
			r->regs > MAX_RESOURCE_REGISTER)
			return false;
	}
	return true;
}

// The bytes after a shader's V4xShader in its allocation: its container, its literals, its resources, then its clip
// planes.
static size_t tail_bytes(const CacheHeader *sizes)
{
	return (sizes->container_size_bytes + 3) / 4 * sizeof(uint32_t) + sizes->literal_count * sizeof(V4xShaderLiteral) +
		sizes->resource_count * sizeof(V4xShaderResource) + sizes->clip_plane_count * sizeof(V4xShaderClipPlane);
}

// Where a shader's parts sit in its allocation, for whoever fills them.
typedef struct {
	uint32_t *container;
	V4xShaderLiteral *literals;
	V4xShaderResource *resources;
	V4xShaderClipPlane *clip_planes;
} ShaderParts;

// A shader in one allocation, its parts pointing into the tail, which the caller fills.
static V4xShader *new_shader(const VkAllocationCallbacks *allocator, const CacheHeader *sizes, ShaderParts *parts)
{
	const size_t bytes = sizeof(V4xShader) + tail_bytes(sizes);
	V4xShader *const shader = v4x_host_alloc(allocator, bytes, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
	if (!shader)
		return NULL;
	parts->container = (uint32_t *)(shader + 1);
	parts->literals = (V4xShaderLiteral *)(parts->container + (sizes->container_size_bytes + 3) / 4);
	parts->resources = (V4xShaderResource *)(parts->literals + sizes->literal_count);
	parts->clip_planes = (V4xShaderClipPlane *)(parts->resources + sizes->resource_count);
	*shader = (V4xShader){ .spirv_hash = sizes->spirv_hash,
		.spirv_size_bytes = sizes->spirv_size_bytes,
		.container = parts->container,
		.container_size_bytes = sizes->container_size_bytes,
		.literals = parts->literals,
		.literal_count = sizes->literal_count,
		.resources = parts->resources,
		.resource_count = sizes->resource_count,
		.clip_planes = parts->clip_planes,
		.clip_plane_count = sizes->clip_plane_count };
	return shader;
}

static CacheHeader header_of(const V4xShader *shader)
{
	return (CacheHeader){ CACHE_MAGIC, CACHE_LAYOUT, shader->spirv_hash, shader->spirv_size_bytes,
		shader->container_size_bytes, shader->literal_count, shader->resource_count, shader->clip_plane_count };
}

static uint32_t needs_of(const XsdkXscShader *compiled, XsdkXscNeedKind kind)
{
	uint32_t count = 0;
	for (size_t i = 0; i < compiled->need_count; i++)
		count += compiled->needs[i].kind == kind;
	return count;
}

// The literals, then the resources, each loop's count among them after the blocks and samplers, then the clip planes,
// as the bundle has them.
static V4xShader *shader_from(
	const VkAllocationCallbacks *allocator, const XsdkXscShader *compiled, uint64_t hash, uint32_t size_bytes)
{
	const CacheHeader sizes = { CACHE_MAGIC, CACHE_LAYOUT, hash, size_bytes, (uint32_t)compiled->container_bytes,
		needs_of(compiled, XSDK_XSC_LITERAL),
		(uint32_t)compiled->resource_count + needs_of(compiled, XSDK_XSC_BLOCK_LOOP) +
			needs_of(compiled, XSDK_XSC_FIXED_LOOP),
		needs_of(compiled, XSDK_XSC_CLIP_PLANE) };
	ShaderParts parts;
	V4xShader *const shader = new_shader(allocator, &sizes, &parts);
	if (!shader)
		return NULL;
	memcpy(parts.container, compiled->container, compiled->container_bytes);
	for (size_t i = 0; i < compiled->resource_count; i++) {
		const XsdkXscResource *const r = &compiled->resources[i];
		parts.resources[i] = (V4xShaderResource){ resource_kind(r->kind), (uint8_t)r->set, (uint8_t)r->binding,
			(uint16_t)r->reg, (uint16_t)r->regs };
	}
	uint32_t literal = 0;
	uint32_t resource = (uint32_t)compiled->resource_count;
	uint32_t plane = 0;
	for (size_t i = 0; i < compiled->need_count; i++) {
		const XsdkXscNeed *const need = &compiled->needs[i];
		if (need->kind == XSDK_XSC_BLOCK_LOOP) {
			const uint8_t kind =
				need->block == XSDK_XSC_PUSH_CONSTANTS ? V4X_RESOURCE_PUSH_LOOP : V4X_RESOURCE_UNIFORM_LOOP;
			parts.resources[resource++] = (V4xShaderResource){ kind, (uint8_t)need->set, (uint8_t)need->binding,
				(uint16_t)need->offset_bytes, (uint16_t)need->reg };
			continue;
		}
		if (need->kind == XSDK_XSC_FIXED_LOOP) {
			parts.resources[resource++] =
				(V4xShaderResource){ V4X_RESOURCE_FIXED_LOOP, 0, 0, (uint16_t)need->regs, (uint16_t)need->reg };
			continue;
		}
		if (need->kind == XSDK_XSC_CLIP_PLANE) {
			parts.clip_planes[plane++] = (V4xShaderClipPlane){ (uint16_t)need->reg, (uint8_t)need->regs };
			continue;
		}
		parts.literals[literal++] = (V4xShaderLiteral){ need->reg,
			{ (float)need->value[0], (float)need->value[1], (float)need->value[2], (float)need->value[3] } };
	}
	return shader;
}

void v4x_set_shader_cache(const char *directory)
{
	cache_directory[0] = '\0';
	if (!directory)
		return;
	if (strlen(directory) >= sizeof cache_directory) {
		xsdk_log_write("vulkan4xenon: the shader cache's folder name is past %u bytes", (unsigned)CACHE_PATH_BYTES);
		return;
	}
	strcpy(cache_directory, directory);
}

V4xShaderCounts v4x_shader_counts(void)
{
	return (V4xShaderCounts){ __atomic_load_n(&compiled_count, __ATOMIC_RELAXED),
		__atomic_load_n(&read_count, __ATOMIC_RELAXED) };
}

// The folder this compiler's shaders are kept in, named after its version, so a changed one compiles afresh.
static bool cache_folder(char path[CACHE_PATH_BYTES])
{
	const int length = snprintf(path, CACHE_PATH_BYTES, "%s/xsc-%s", cache_directory, xsdk_xsc_version());
	return cache_directory[0] && length > 0 && length < (int)CACHE_PATH_BYTES;
}

static bool cache_file(uint64_t hash, uint32_t size_bytes, char path[CACHE_PATH_BYTES])
{
	char folder[CACHE_PATH_BYTES];
	if (!cache_folder(folder))
		return false;
	const int length = snprintf(path, CACHE_PATH_BYTES, "%s/%08X%08X-%u.v4xs", folder, (unsigned)(hash >> 32),
		(unsigned)hash, (unsigned)size_bytes);
	return length > 0 && length < (int)CACHE_PATH_BYTES;
}

// The shader kept for this SPIR-V, or NULL where there's none, or one that doesn't check out.
static V4xShader *read_cached(VkDevice device, uint64_t hash, uint32_t size_bytes)
{
	char path[CACHE_PATH_BYTES];
	struct stat info;
	if (!cache_file(hash, size_bytes, path) || stat(path, &info) != 0)
		return NULL;
	FILE *const file = fopen(path, "rb");
	if (!file)
		return NULL;
	CacheHeader header;
	const bool sound = fread(&header, sizeof header, 1, file) == 1 && header.magic == CACHE_MAGIC &&
		header.layout == CACHE_LAYOUT && header.spirv_hash == hash && header.spirv_size_bytes == size_bytes &&
		header.container_size_bytes < MAX_CACHED_BYTES && header.literal_count < MAX_CACHED_BYTES &&
		header.resource_count < MAX_CACHED_BYTES && header.clip_plane_count < MAX_CACHED_BYTES;
	const size_t tail = sound ? tail_bytes(&header) : 0;
	ShaderParts parts;
	V4xShader *shader = sound && (size_t)info.st_size == sizeof header + tail
		? new_shader(v4x_host_device_allocator(device, NULL), &header, &parts)
		: NULL;
	if (shader && fread(parts.container, 1, tail, file) != tail) {
		v4x_host_free(v4x_host_device_allocator(device, NULL), shader);
		shader = NULL;
	}
	(void)fclose(file);
	if (!shader)
		xsdk_log_write("vulkan4xenon: %s doesn't hold the shader it's named for", path);
	return shader;
}

static bool make_folder(const char *path)
{
	struct stat info;
	return stat(path, &info) == 0 || mkdir(path, FOLDER_MODE) == 0 || errno == EEXIST;
}

// Written beside its name and renamed into place, so a crash leaves no file half written.
static void keep_on_disk(const V4xShader *shader)
{
	char folder[CACHE_PATH_BYTES];
	char path[CACHE_PATH_BYTES];
	char written[CACHE_PATH_BYTES + sizeof ".new"];
	if (!cache_folder(folder) || !cache_file(shader->spirv_hash, shader->spirv_size_bytes, path))
		return;
	(void)snprintf(written, sizeof written, "%s.new", path);
	const CacheHeader header = header_of(shader);
	const size_t tail = tail_bytes(&header);
	FILE *const file = make_folder(cache_directory) && make_folder(folder) ? fopen(written, "wb") : NULL;
	bool kept = file && fwrite(&header, sizeof header, 1, file) == 1 && fwrite(shader + 1, 1, tail, file) == tail;
	if (file && fclose(file) != 0)
		kept = false;
	kept = kept && rename(written, path) == 0;
	if (kept)
		return;
	xsdk_log_write("vulkan4xenon: SPIR-V %08X%08X not kept in %s", (unsigned)(shader->spirv_hash >> 32),
		(unsigned)shader->spirv_hash, folder);
	if (file)
		(void)unlink(written);
}

// Through xost's compiler at xsdk's register split with the optimiser on, as Dusklight's pack is built.
static V4xShader *compile(VkDevice device, const uint32_t *code, uint32_t size_bytes, uint64_t hash)
{
	XsdkXscOptions options;
	xsdk_xsc_default_options(&options);
	options.optimize = true;
	XsdkXscShader compiled;
	const uint64_t start = now_us();
	const XsdkXscStatus status = xsdk_xsc_compile(code, size_bytes / sizeof(uint32_t), &options, &compiled);
	const uint64_t us = now_us() - start;
	const unsigned high = (unsigned)(hash >> 32);
	const unsigned low = (unsigned)hash;
	if (status != XSDK_XSC_OK) {
		xsdk_log_write("vulkan4xenon: xost refused SPIR-V %08X%08X, %u bytes: %s", high, low, (unsigned)size_bytes,
			compiled.message);
		return NULL;
	}
	V4xShader *shader = NULL;
	if (!layer_takes(&compiled))
		xsdk_log_write("vulkan4xenon: SPIR-V %08X%08X wants a bare uniform's loop constants or a resource past %u",
			high, low, (unsigned)MAX_RESOURCE_REGISTER);
	else
		shader = shader_from(v4x_host_device_allocator(device, NULL), &compiled, hash, size_bytes);
	xsdk_xsc_free(&compiled);
	if (shader)
		xsdk_log_write("vulkan4xenon: compiled SPIR-V %08X%08X, %u bytes, in %u us", high, low, (unsigned)size_bytes,
			(unsigned)us);
	return shader;
}

// Under device->shaders_lock.
static const V4xShader *kept(VkDevice device, uint64_t hash, uint32_t size_bytes)
{
	for (uint32_t i = 0; i < device->shader_count; i++) {
		const V4xShader *const shader = device->shaders[i];
		if (shader->spirv_hash == hash && shader->spirv_size_bytes == size_bytes)
			return shader;
	}
	return NULL;
}

// Under device->shaders_lock.
static bool keep(VkDevice device, V4xShader *shader)
{
	if (device->shader_count == device->shader_capacity) {
		const uint32_t capacity = device->shader_capacity ? 2 * device->shader_capacity : 16;
		V4xShader **const grown = v4x_host_realloc(v4x_host_device_allocator(device, NULL), device->shaders,
			capacity * sizeof *grown, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
		if (!grown)
			return false;
		device->shaders = grown;
		device->shader_capacity = capacity;
	}
	device->shaders[device->shader_count++] = shader;
	return true;
}

// What the cache holds for this SPIR-V, else what it compiles to, kept on the disk.
static V4xShader *cached_or_compiled(VkDevice device, const uint32_t *code, uint32_t size_bytes, uint64_t hash)
{
	V4xShader *shader = read_cached(device, hash, size_bytes);
	if (shader) {
		(void)__atomic_add_fetch(&read_count, 1, __ATOMIC_RELAXED);
		xsdk_log_write(
			"vulkan4xenon: SPIR-V %08X%08X read from the shader cache", (unsigned)(hash >> 32), (unsigned)hash);
		return shader;
	}
	shader = compile(device, code, size_bytes, hash);
	if (!shader)
		return NULL;
	(void)__atomic_add_fetch(&compiled_count, 1, __ATOMIC_RELAXED);
	keep_on_disk(shader);
	return shader;
}

// The shader this device made from this SPIR-V before, else one read or compiled now and kept.
static const V4xShader *compiled_shader(VkDevice device, const uint32_t *code, uint32_t size_bytes, uint64_t hash)
{
	(void)pthread_mutex_lock(&device->shaders_lock);
	const V4xShader *found = kept(device, hash, size_bytes);
	if (!found) {
		V4xShader *const shader = cached_or_compiled(device, code, size_bytes, hash);
		if (shader && !keep(device, shader))
			v4x_host_free(v4x_host_device_allocator(device, NULL), shader);
		else
			found = shader;
	}
	(void)pthread_mutex_unlock(&device->shaders_lock);
	return found;
}

void v4x_shader_release(VkDevice device)
{
	const VkAllocationCallbacks *const allocator = v4x_host_device_allocator(device, NULL);
	for (uint32_t i = 0; i < device->shader_count; i++)
		v4x_host_free(allocator, device->shaders[i]);
	v4x_host_free(allocator, device->shaders);
	device->shaders = NULL;
	device->shader_count = device->shader_capacity = 0;
	(void)pthread_mutex_destroy(&device->shaders_lock);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator, VkShaderModule *pShaderModule)
{
	const uint32_t size_bytes = (uint32_t)pCreateInfo->codeSize;
	const uint64_t hash = spirv_hash(pCreateInfo->pCode, size_bytes);
	const V4xShader *found = in_bundle(&v4x_shader_bundle, hash, size_bytes);
	if (!found)
		found = compiled_shader(device, pCreateInfo->pCode, size_bytes, hash);
	if (!found)
		return VK_ERROR_FEATURE_NOT_PRESENT;
	ShaderModule *const module = v4x_host_alloc(
		v4x_host_device_allocator(device, pAllocator), sizeof *module, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!module)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	module->shader = found;
	*pShaderModule = v4x_handle(module);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(
	VkDevice device, VkShaderModule shaderModule, const VkAllocationCallbacks *pAllocator)
{
	v4x_host_free(v4x_host_device_allocator(device, pAllocator), v4x_object(shaderModule));
}
