# dawn_switch_link_nvk(<target>): link a Switch executable against NVK, the
# Vulkan driver Dawn runs on (built and installed into portlibs by nxvk).
#
# libnvk.a must be whole-archived: Mesa's generated dispatch/trampoline tables
# are only reached through runtime string lookup, so a selective link silently
# drops them (the same recipe as nxvk's build-nro.sh and nxvk.pc). libnvk_support
# links normally. nvk_switch_stubs.cpp fills the newlib gaps NVK needs.
function(dawn_switch_link_nvk target)
    if (NOT EXISTS "${DEVKITPRO}/portlibs/switch/lib/libnvk.a")
        message(FATAL_ERROR "libnvk.a not found in ${DEVKITPRO}/portlibs/switch/lib: "
                            "build and install nxvk first (see nxvk/switch/README.md)")
    endif ()
    target_sources(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/nvk_switch_stubs.cpp")
    target_link_libraries(${target} PRIVATE
        -L${DEVKITPRO}/portlibs/switch/lib
        -Wl,-u,vk_icdGetInstanceProcAddr
        -Wl,--whole-archive -lnvk -Wl,--no-whole-archive
        -lnvk_support
        -lz)
endfunction()
