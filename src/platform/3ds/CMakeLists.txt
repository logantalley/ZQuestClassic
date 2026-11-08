# Boot/shutdown for the 3DS build of zplayer. libctru comes from the devkitPro toolchain.
add_library(zc_3ds STATIC platform_3ds.cpp jit_memory_3ds.cpp)
target_link_libraries(zc_3ds PUBLIC base)
# See __wrap_getcwd and __wrap_exit in platform_3ds.cpp.
target_link_options(zc_3ds INTERFACE "-Wl,--wrap=getcwd" "-Wl,--wrap=exit")
