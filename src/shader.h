#ifndef V4X_SHADER_H
#define V4X_SHADER_H

#include <stdint.h>

#include <vulkan/vulkan.h>

#include "objects.h"
#include "vulkan4xenon.h"

// The bundle's shader compiled from this SPIR-V, or NULL.
const V4xShader *v4x_shader_find(const V4xShaderBundle *bundle, const uint32_t *code, uint32_t size_bytes);
// The module's vertex shader for an instanced pipeline: its own if it reads gl_InstanceIndex, else its instanced
// variant, from the app's bundle or compiled here and kept as the plain ones are; NULL if xost refuses it.
const V4xShader *v4x_shader_instanced(VkDevice device, const ShaderModule *module);
// Frees the shaders the device compiled, as it goes.
void v4x_shader_release(VkDevice device);

#endif
