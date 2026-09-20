# dawn-nx

Dawn (Google's WebGPU implementation) ported to Nintendo Switch / Horizon, for
[wii-nx](https://github.com/nx-mod/wii-nx).

## What was ported

About 1,500 lines across 25 files, Vulkan backend only (the console's only graphics API, through
[nxvk](https://github.com/nx-mod/nxvk)):

- **Native surface**: present to the console's window via `VK_NN_vi_surface` (`dawn.json`,
  `Surface.cpp`, `SwapChainVk.cpp`)
- **Static Vulkan**: entry points resolved from NVK's archive instead of `dlopen`, since Horizon has no
  dynamic libraries (`DynamicLib`, `VulkanFunctions`)
- **Platform gaps**: no processes, no working directory, CPU count (`SystemHandle`, `SystemUtils`,
  `nvk_switch_stubs.cpp`)
- **Build**: Horizon toolchain support; RenderDoc excluded (it has no Switch support)

Tint (the shader compiler) needed no changes; it runs on-device.

## Status

Works, and proven on hardware by its own demo (`switch_smoke_test/`, built as `dawn-nx-demo.nro`):
instance, adapter and device; buffer upload, copy and readback; a compute shader; render to a texture
with vertex and uniform buffers, a sampled texture and a sampler, checked pixel by pixel; 180 frames
presented at 60 fps; then an ordered teardown. 10 of 10 on a Switch.

The shader cache persists now, through [sqlite-nx](https://github.com/nx-mod/sqlite-nx): a second
launch reloads compiled shaders instead of building them again (2859 of 2860 hits, 26 MiB).

`dawn_switch_link_nvk(<target>)` (`switch_smoke_test/NvkLink.cmake`) links any Switch executable
against NVK with the whole-archive recipe it needs.

## Known limitation

Dawn's **OpenGL and OpenGL ES backends do not come up** on Switch: both report "No supported adapters"
when handed nxvk's EGL. Vulkan is unaffected, and is the path games use. The demo runs all three, so
the failure is visible and measurable when someone wants to chase it.

## Plan

Publish as a prebuilt devkitPro portlib so games link it instead of rebuilding Dawn, which dominates
build time.

## Releases

Prebuilt packages are tagged `<upstream version>-nx-mod-v<n>`, the same convention across every nx-mod
library, so a project can pin one line per dependency.
