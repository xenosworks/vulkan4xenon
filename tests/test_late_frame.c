#include <stdbool.h>
#include <stdint.h>

#include "harness.h"
#include "tests.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/thread.h"

enum {
	IMAGES = 4,
	FRAMES = 30,
	LATE_FRAME = FRAMES / 2,
	// Over two refreshes of 16.7 ms and under three: four images absorb it, three miss a vblank.
	LATE_FRAME_MS = 40,
	LAST_FLIP_TIMEOUT_MS = 1000,
};

// Acquires as RetroArch does, with no timeout, and presents at once; one frame comes late.
static bool run_frames(const Harness *harness)
{
	for (uint32_t frame = 0; frame < FRAMES; frame++) {
		uint32_t index = 0;
		if (!harness_ok("vkAcquireNextImageKHR",
				vkAcquireNextImageKHR(
					harness->device, harness->swapchain, UINT64_MAX, VK_NULL_HANDLE, VK_NULL_HANDLE, &index)))
			return false;
		if (frame == LATE_FRAME)
			(void)xsdk_thread_sleep(LATE_FRAME_MS);
		const VkPresentInfoKHR present = {
			.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
			.swapchainCount = 1,
			.pSwapchains = &harness->swapchain,
			.pImageIndices = &index,
		};
		if (!harness_ok("vkQueuePresentKHR", vkQueuePresentKHR(harness->queue, &present)))
			return false;
	}
	return true;
}

void test_late_frame(void)
{
	Harness harness = { .instance = VK_NULL_HANDLE };
	const bool made =
		harness_create_device(&harness) == VK_SUCCESS && harness_create_swapchain(&harness, IMAGES) == VK_SUCCESS;
	check("late frame: a swapchain of four images", made && harness.image_count == IMAGES);
	const bool ran = made && run_frames(&harness);
	XsdkPresentStats stats = { .flips = 0 };
	for (uint32_t waited_ms = 0; ran && stats.flips < FRAMES && waited_ms < LAST_FLIP_TIMEOUT_MS; waited_ms++) {
		(void)xsdk_thread_sleep(1);
		xsdk_present_stats(&stats);
	}
	xsdk_log_write("late frame: %u flips over vblanks %u to %u, %u missed", (unsigned)stats.flips,
		(unsigned)stats.first_flip_vblank, (unsigned)stats.last_flip_vblank, (unsigned)stats.missed);
	check("late frame: a frame 40 ms late shows with every frame on its own vblank, none missed",
		ran && stats.flips == FRAMES && stats.missed == 0 &&
			stats.last_flip_vblank - stats.first_flip_vblank == FRAMES - 1);
	harness_destroy(&harness);
}
