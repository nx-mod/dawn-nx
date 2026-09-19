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

Works: a real game renders through it. Dawn's disk shader cache does not persist yet - see sqlite-nx.

## Plan

Publish as a prebuilt devkitPro portlib so games link it instead of rebuilding Dawn, which dominates
build time.
