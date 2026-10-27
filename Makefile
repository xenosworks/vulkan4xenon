# Builds the layer and its test title against xsdk. Run inside WSL Debian, after building xsdk.

# xsdk's repository, built with its own make.
XSDK ?= ../Xbox-360-SDK
TOOLCHAIN ?= $(HOME)/xsdk-gcc/bin/powerpc-xsdk-elf-
CC := $(TOOLCHAIN)gcc
AR := $(TOOLCHAIN)ar
OBJDUMP := $(TOOLCHAIN)objdump
PYTHON ?= python3

BUILD := build

WARNINGS := -Wall -Wextra -Werror
INCLUDES := -Iinclude -Ithird_party/vulkan-headers/include -I$(XSDK)/include
CFLAGS := -mcpu=cell -fno-pic -std=gnu17 -O2 -g $(WARNINGS) $(INCLUDES) -MMD -MP
LDFLAGS := -static -nostdlib -T $(XSDK)/src/xsdk.ld
XSDK_LIBRARY := $(XSDK)/build/libxsdk.a
LIBS := -Wl,--start-group $(BUILD)/libvulkan4xenon.a -lstdc++ $(XSDK_LIBRARY) -lc -lm -lgcc -Wl,--end-group

LAYER_SOURCES := src/host.c src/proc_addr.c src/instance.c src/physical_device.c src/display.c src/device.c \
	src/memory.c src/shader.c src/pipeline.c src/render_pass.c src/command_buffer.c src/queue.c src/transfer.c src/sync.c \
	src/swapchain.c src/image.c src/sampler.c src/descriptor.c src/pipeline_state.c src/blit.c src/blit_shaders.c \
	src/query.c src/format.c
# make V4X_PROFILE=1 times the submit's stages and logs them (docs/ROADMAP.md, milestone 29); make clean first, as the
# objects don't know which build they're from.
ifeq ($(V4X_PROFILE),1)
CFLAGS += -DV4X_PROFILE
LAYER_SOURCES += src/profile.c
endif
# make V4X_FORCE_BANDS=n draws every subpass with depth in n bands (src/render_pass.c), to measure what banding costs;
# make clean first, as for V4X_PROFILE.
ifneq ($(V4X_FORCE_BANDS),)
CFLAGS += -DV4X_FORCE_BANDS=$(V4X_FORCE_BANDS)
endif
LAYER_OBJECTS := $(patsubst src/%.c,$(BUILD)/src/%.o,$(LAYER_SOURCES))

# tests/shaders.c and src/blit_shaders.c come from tools/build_shaders.py, run by hand where glslang and xost are.
TEST_SOURCES := tests/main.c tests/harness.c tests/harness_scene.c tests/test_boot_logo.c tests/test_objects.c \
	tests/test_triangle.c tests/test_packed_colour.c tests/test_texture.c tests/test_push_constants.c \
	tests/test_uniform_buffers.c tests/test_indexed.c tests/test_depth_stencil.c tests/test_raster.c \
	tests/test_render_to_texture.c tests/test_run_time_index.c tests/test_uploads.c tests/test_thin_textures.c \
	tests/test_sampled_depth.c tests/test_anisotropy.c tests/test_aurora_points.c tests/test_lod.c \
	tests/test_commands.c tests/test_copies.c tests/test_dynamic_state.c tests/test_clears.c tests/test_subpasses.c \
	tests/test_events.c tests/test_formats.c tests/test_vertex_formats.c tests/test_shader_cache.c \
	tests/test_branches.c tests/test_loops.c tests/test_shader_lod.c tests/test_facing.c tests/test_vertex_textures.c \
	tests/test_cube_3d.c tests/test_addressing.c tests/test_shadow.c tests/test_instancing.c tests/test_strips.c \
	tests/test_targets.c tests/test_two_sided_stencil.c tests/test_clip_distances.c tests/test_point_sprites.c \
	tests/test_fill_modes.c tests/test_render_areas.c tests/test_gpu_blits.c tests/test_occlusion.c tests/test_tiles.c \
	tests/test_multisample.c tests/test_small_targets.c tests/test_edram_parts.c tests/test_gpu_uploads.c \
	tests/shaders.c
TEST_OBJECTS := $(patsubst tests/%.c,$(BUILD)/tests/%.o,$(TEST_SOURCES))

# vkbench, the benchmark against xsdk's GPU layer; bench/shaders.c comes from tools/build_shaders.py too.
BENCH_SOURCES := bench/main.c bench/scenes.c bench/assets.c bench/backend_xsdk.c bench/backend_vulkan.c \
	bench/shaders.c bench/probes.c
BENCH_OBJECTS := $(patsubst bench/%.c,$(BUILD)/bench/%.o,$(BENCH_SOURCES))

.PHONY: all clean
.SECONDARY:
all: $(BUILD)/vulkan4xenon_test.xex $(BUILD)/vkbench.xex

$(BUILD)/%.xex: $(BUILD)/%.elf $(XSDK)/tools/elf2xex.py
	$(PYTHON) $(XSDK)/tools/elf2xex.py $< $@

$(BUILD)/vulkan4xenon_test.elf: $(TEST_OBJECTS) $(BUILD)/libvulkan4xenon.a $(XSDK_LIBRARY)
	$(CC) $(LDFLAGS) -Wl,-Map,$(BUILD)/vulkan4xenon_test.map -o $@ $(TEST_OBJECTS) $(LIBS)
	$(OBJDUMP) -d $@ | $(PYTHON) $(XSDK)/tools/xenia_scan.py

$(BUILD)/vkbench.elf: $(BENCH_OBJECTS) $(BUILD)/libvulkan4xenon.a $(XSDK_LIBRARY)
	$(CC) $(LDFLAGS) -Wl,-Map,$(BUILD)/vkbench.map -o $@ $(BENCH_OBJECTS) $(LIBS)
	$(OBJDUMP) -d $@ | $(PYTHON) $(XSDK)/tools/xenia_scan.py

$(BUILD)/libvulkan4xenon.a: $(LAYER_OBJECTS)
	rm -f $@
	$(AR) rcs $@ $^

$(BUILD)/src/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/tests/%.o: tests/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/bench/%.o: bench/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
