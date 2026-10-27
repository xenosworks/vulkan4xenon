#include "harness.h"
#include "tests.h"
#include "xsdk/display.h"
#include "xsdk/log.h"

static int passed_count;
static int failed_count;

void check(const char *name, bool passed)
{
	xsdk_log_write("%s: %s", passed ? "PASS" : "FAIL", name);
	if (passed)
		passed_count++;
	else
		failed_count++;
}

int main(void)
{
	xsdk_log_write("vulkan4xenon test title");
	Harness harness = { .instance = VK_NULL_HANDLE };
	// XAM's surface, which the boot logo should replace.
	const uint32_t shown_before_device = xsdk_display_shown_address();
	test_shader_cache_start();
	const bool made = harness_create_device(&harness) == VK_SUCCESS;
	check("setup: instance, device, command pool and fence made", made);
	if (made) {
		test_boot_logo(&harness, shown_before_device);
		test_objects(&harness);
		test_commands(&harness);
		test_copies(&harness);
		test_triangle(&harness);
		test_packed_colour(&harness);
		test_vertex_formats(&harness);
		test_texture(&harness);
		test_formats(&harness);
		test_cube_3d(&harness);
		test_addressing(&harness);
		test_shadow(&harness);
		test_push_constants(&harness);
		test_uniform_buffers(&harness);
		test_indexed(&harness);
		test_instancing(&harness);
		test_strips(&harness);
		test_depth_stencil(&harness);
		test_raster(&harness);
		test_dynamic_state(&harness);
		test_two_sided_stencil(&harness);
		test_clip_distances(&harness);
		test_point_sprites(&harness);
		test_fill_modes(&harness);
		test_clears(&harness);
		test_subpasses(&harness);
		test_targets(&harness);
		test_multisample(&harness);
		test_tiles(&harness);
		test_render_areas(&harness);
		test_small_targets(&harness);
		test_gpu_blits(&harness);
		test_render_to_texture(&harness);
		test_edram_parts(&harness);
		test_gpu_uploads(&harness);
		test_cpu_mips(&harness);
		test_run_time_index(&harness);
		test_uploads(&harness);
		test_thin_textures(&harness);
		test_sampled_depth(&harness);
		test_anisotropy(&harness);
		test_aurora_points(&harness);
		test_lod(&harness);
		test_branches(&harness);
		test_loops(&harness);
		test_shader_lod(&harness);
		test_facing(&harness);
		test_vertex_textures(&harness);
		test_occlusion(&harness);
		// Last, so the GPU's clock is past its first 2^32 ticks, 8.6 s, and a time stamp's high word counts.
		test_events(&harness);
	}
	harness_destroy(&harness);
	if (made)
		test_shader_cache_finish();
	xsdk_log_write("tests: %d passed, %d failed", passed_count, failed_count);
	return failed_count;
}
