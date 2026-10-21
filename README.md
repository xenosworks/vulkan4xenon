<div align="center">
  <img src="assets/vulkan4xenon.png" alt="Vulkan 4 xenon" width="640">
</div>

# vulkan4xenon

A Vulkan 1.0 subset for the Xbox 360, built as a static library on the xsdk homebrew SDK's GPU layer, which drives
Xenos directly under the retail kernel.

It's measured against two things. Dusklight, a reimplementation of Twilight Princess built on encounter's Aurora, runs
on an RGH console through it, at 1280x720 with 4 samples of multisampling
([dusklight-xenon](https://github.com/xenosworks/dusklight-xenon)). And as ports are expected from any system up to
the GameCube and Wii, and from PC games of the same years, Direct3D 9 is the yardstick: of docs/PARITY.md's 47 rows,
the layer has 40 and part of 7.

**Status:** milestones 1 to 7 and 17 to 31 are done on the console, the last on 2026-10-21, when vulkan4xenon_test
passed 523 of 523 checks and vkbench 17 of 17. 8 was tried and reverted; 18's last step was put off and 29 closed
short of its targets, both by the user's choice. 9 to 15 brought Dusklight up on the console, from its boot screens to
play in Ordon, and 16, Dusklight's menus, comes next. docs/ROADMAP.md has each milestone and how it ran, and
docs/SUPPORT.md lists what exists.

## Shaders

Apps pass standard SPIR-V to `vkCreateShaderModule`. Shaders are compiled offline into Xenos containers, bundled with
the app and looked up by their SPIR-V. `tools/build_shaders.py` does it: GLSL → `glslang -V` → xost → a C source with
each shader's SPIR-V and the bundle. SPIR-V the bundle doesn't hold is compiled on the console, through xost's
compiler in xsdk, the first time a module is made from it, and kept in a folder the app names with
`v4x_set_shader_cache` (docs/SUPPORT.md, Shaders).

```
python3 tools/build_shaders.py --xost ../Xbox-360-SDK/xenos-toolchain --output tests/shaders tests/shaders/*.vert tests/shaders/*.frag
```

## Building

The layer builds with xsdk's toolchain in WSL Debian, against an xsdk checkout next to this one (`XSDK=` points
elsewhere). Build xsdk first, then:

```
make
```

That makes `build/libvulkan4xenon.a`, the test title, `build/vulkan4xenon_test.xex`, and the benchmark,
`build/vkbench.xex`.

## Benchmark

vkbench runs the same scenes through xsdk's GPU layer and then through vulkan4xenon, with
the same shaders, and logs each frame's draw + submit and GPU time for both, what xsdk's ring did, and whether they
drew the same picture.
Every frame goes in `game:\vkbench_frames.csv` and the summary in `game:\vkbench_summary.csv`. docs/ROADMAP.md,
milestone 5, has the scenes and how they're timed.

## Licence

vulkan4xenon is under the MIT licence (LICENSE), copyright Mena. THIRD_PARTY_NOTICES.md lists the code from elsewhere
in the repository and points to xsdk's notices, which every title built on it ships.

## Trademarks

The Vulkan logo is a registered trademark of the Khronos Group, used here at the project owner's choice. vulkan4xenon
doesn't claim conformance. vulkan4xenon isn't affiliated with or endorsed by Microsoft; Xbox and Xbox 360 are
trademarks of Microsoft, and the repository holds no Microsoft code.
