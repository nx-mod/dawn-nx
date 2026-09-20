# dawn_switch_link_nvk(<target> [GL]): link a Switch executable against NVK, the
# Vulkan driver Dawn runs on (built and installed into portlibs by nxvk). With
# GL, also link nxvk's OpenGL package (EGL + Zink over NVK), which Dawn's
# OpenGL ES and OpenGL backends use; install it with nxvk's `make install-gl`.
#
# libnvk.a (and libnvk_gl.a) must be whole-archived: Mesa's generated dispatch
# and trampoline tables are only reached through runtime string lookup, so a
# selective link silently drops them (the same recipe as nxvk.pc / nxvk-gl.pc).
# nvk_switch_stubs.cpp fills the newlib gaps NVK needs.
function(dawn_switch_link_nvk target)
    cmake_parse_arguments(PARSE_ARGV 1 NVK "GL" "" "")
    set(_lib "${DEVKITPRO}/portlibs/switch/lib")
    if (NOT EXISTS "${_lib}/libnvk.a")
        message(FATAL_ERROR "libnvk.a not found in ${_lib}: build and install nxvk first "
                            "(see nxvk/switch/README.md)")
    endif ()
    set(_whole -lnvk)
    if (NVK_GL)
        if (NOT EXISTS "${_lib}/libnvk_gl.a")
            message(FATAL_ERROR "libnvk_gl.a not found in ${_lib}: build and install nxvk's "
                                "OpenGL package first (make gl && make install-gl)")
        endif ()
        set(_whole -lnvk_gl -lnvk)
    endif ()
    target_sources(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/nvk_switch_stubs.cpp")
    if (NVK_GL)
        # EGL/GLES headers live in portlibs, which is on the link path but not
        # the include path.
        target_include_directories(${target} PRIVATE "${DEVKITPRO}/portlibs/switch/include")
    endif ()
    target_link_libraries(${target} PRIVATE
        -L${_lib}
        -Wl,-u,vk_icdGetInstanceProcAddr
        -Wl,--whole-archive ${_whole} -Wl,--no-whole-archive
        -Wl,--start-group -lnvk_support -lz -Wl,--end-group)
endfunction()
