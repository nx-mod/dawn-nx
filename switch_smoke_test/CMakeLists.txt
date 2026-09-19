# dawn-nx demo: WebGPU on Nintendo Switch through Dawn's C++ API - device,
# buffers, compute, render-to-texture with readback checks, and presenting to
# the screen. Builds dawn-nx-demo.nro; the report goes to
# sdmc:/switch/dawn-nx-demo.log.

add_executable(dawn_nx_demo demo.cpp)
target_compile_features(dawn_nx_demo PRIVATE cxx_std_20)
target_link_libraries(dawn_nx_demo PRIVATE webgpu_dawn)
dawn_switch_link_nvk(dawn_nx_demo)

nx_generate_nacp(dawn-nx-demo.nacp NAME "dawn-nx demo" AUTHOR "nx-mod" VERSION "1.0.0")
nx_create_nro(dawn_nx_demo OUTPUT dawn-nx-demo.nro NACP dawn-nx-demo.nacp)
