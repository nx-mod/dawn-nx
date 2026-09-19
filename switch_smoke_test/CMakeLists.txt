# Minimal smoke test that creates a WGPUInstance and requests a Vulkan
# adapter/device, to verify Dawn-for-Switch is alive end to end on NVK.
# Switch-only: not meant to build or run anywhere else.

add_executable(dawn_switch_smoke_test smoke_test.cpp nvk_switch_stubs.cpp)
target_include_directories(dawn_switch_smoke_test PRIVATE
    ${DAWN_VULKAN_HEADERS_DIR}/include
)
# Needed for VK_NN_vi_surface's types (VkViSurfaceCreateInfoNN etc.) to be
# declared at all - matches nxvk's own required Cflags (nxvk.pc).
target_compile_definitions(dawn_switch_smoke_test PRIVATE VK_USE_PLATFORM_VI_NN)
target_link_libraries(dawn_switch_smoke_test PRIVATE webgpu_dawn)
# libnvk.a must be whole-archived: without it, the linker only pulls object
# files it can see are directly needed, which silently drops Mesa's
# generated dispatch/trampoline tables (nothing calls into them by name at
# link time, only through runtime string lookup) - this is the exact fix
# nxvk's own reference linker (switch/build/build-nro.sh) and its nxvk.pc
# both use. Only -lnvk gets this treatment; -lnvk_support is a normal
# selective-link archive (matches WiiCompiled's own libtranslated.a vs.
# libswitchext.a distinction for the same underlying reason).
target_link_libraries(dawn_switch_smoke_test PRIVATE
    -L/opt/devkitpro/portlibs/switch/lib
    -Wl,-u,vk_icdGetInstanceProcAddr
    -Wl,--whole-archive -lnvk -Wl,--no-whole-archive
    -lnvk_support
    -lz
)
