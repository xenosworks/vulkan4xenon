#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "harness.h"
#include "tests.h"
#include "vulkan4xenon.h"
#include "xsdk/log.h"
#include "xsdk/xsc.h"

enum {
	PATH_BYTES = 128,
	FOLDER_MODE = 0777,
	// The triangle's two shaders, test_run_time_index's two, test_branches' two and test_loops' eight, which the
	// bundle doesn't hold.
	COMPILED_SHADERS = 14,
	TRIANGLE_SHADERS = 2,
	// Stale files emptied from the folder before the run; more means something else writes there.
	MAX_STALE_FILES = 64,
	IMAGES = 2,
};

// game: is the launch folder on the console; Xenia mounts it read-only but lets titles write to cache:.
static const char *const roots[] = { "game:\\", "cache:\\" };
static const char FOLDER_NAME[] = "v4x_shader_cache";

static char folder[PATH_BYTES];
static char version_folder[PATH_BYTES];
static V4xShaderCounts at_start;

static bool usable(const char *path)
{
	struct stat info;
	return stat(path, &info) == 0 || mkdir(path, FOLDER_MODE) == 0;
}

// The first entry of a folder, else false; the folder is closed again so the entry can go.
static bool first_entry(const char *path, char name[PATH_BYTES])
{
	DIR *const dir = opendir(path);
	if (!dir)
		return false;
	const struct dirent *const entry = readdir(dir);
	const bool found = entry && strlen(entry->d_name) < PATH_BYTES;
	if (found)
		strcpy(name, entry->d_name);
	(void)closedir(dir);
	return found;
}

static bool empty_version_folder(void)
{
	char name[PATH_BYTES];
	char path[2 * PATH_BYTES];
	for (uint32_t i = 0; i < MAX_STALE_FILES; i++) {
		if (!first_entry(version_folder, name))
			return true;
		(void)snprintf(path, sizeof path, "%s\\%s", version_folder, name);
		if (unlink(path) != 0)
			return false;
	}
	return false;
}

static bool ends_with(const char *name, const char *end)
{
	const size_t name_bytes = strlen(name);
	const size_t end_bytes = strlen(end);
	return name_bytes >= end_bytes && strcmp(name + name_bytes - end_bytes, end) == 0;
}

// The layer's files in the version folder, and anything else there, such as a file left half written.
static void count_files(uint32_t *kept, uint32_t *other)
{
	*kept = *other = 0;
	DIR *const dir = opendir(version_folder);
	if (!dir)
		return;
	for (const struct dirent *entry = readdir(dir); entry; entry = readdir(dir)) {
		if (ends_with(entry->d_name, ".v4xs"))
			(*kept)++;
		else
			(*other)++;
	}
	(void)closedir(dir);
}

void test_shader_cache_start(void)
{
	folder[0] = '\0';
	for (size_t i = 0; i < sizeof roots / sizeof roots[0] && !folder[0]; i++) {
		(void)snprintf(folder, sizeof folder, "%s%s", roots[i], FOLDER_NAME);
		if (!usable(folder))
			folder[0] = '\0';
	}
	(void)snprintf(version_folder, sizeof version_folder, "%s\\xsc-%s", folder, xsdk_xsc_version());
	const bool emptied = folder[0] && empty_version_folder();
	xsdk_log_write("shader cache: %s", version_folder);
	check("shader cache: a folder for the layer's shaders, emptied of an earlier run's", emptied);
	v4x_set_shader_cache(emptied ? folder : NULL);
	at_start = v4x_shader_counts();
}

void test_shader_cache_finish(void)
{
	const V4xShaderCounts after_first = v4x_shader_counts();
	uint32_t kept = 0;
	uint32_t other = 0;
	count_files(&kept, &other);
	xsdk_log_write("shader cache: %u compiled, %u read, %u files kept, %u others",
		(unsigned)(after_first.compiled - at_start.compiled), (unsigned)(after_first.read - at_start.read),
		(unsigned)kept, (unsigned)other);
	check("shader cache: each shader compiled here is kept in a file of its own, in the compiler's version's folder",
		after_first.compiled - at_start.compiled == COMPILED_SHADERS && after_first.read == at_start.read &&
			kept == COMPILED_SHADERS && other == 0);

	// A second device keeps nothing from the first, so its shaders come from the files or the compiler.
	Harness harness = { .instance = VK_NULL_HANDLE };
	const bool made =
		harness_create_device(&harness) == VK_SUCCESS && harness_create_swapchain(&harness, IMAGES) == VK_SUCCESS;
	check("shader cache: a second device and swapchain made", made);
	if (made)
		test_triangle_from_cache(&harness);
	harness_destroy(&harness);
	const V4xShaderCounts after_second = v4x_shader_counts();
	xsdk_log_write("shader cache: the second device compiled %u and read %u",
		(unsigned)(after_second.compiled - after_first.compiled), (unsigned)(after_second.read - after_first.read));
	check("shader cache: the second device reads the triangle's shaders back and compiles none",
		after_second.compiled == after_first.compiled && after_second.read - after_first.read == TRIANGLE_SHADERS);
}
