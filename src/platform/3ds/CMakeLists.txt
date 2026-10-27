# Boot/shutdown for the 3DS build of zplayer. libctru comes from the devkitPro toolchain.
add_library(zc_3ds STATIC platform_3ds.cpp)
target_link_libraries(zc_3ds PUBLIC base)
# See __wrap_getcwd in platform_3ds.cpp.
target_link_options(zc_3ds INTERFACE "-Wl,--wrap=getcwd")
