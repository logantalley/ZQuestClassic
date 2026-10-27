# Allegro on the 3DS: SDL backend, no OpenGL.
#
# Included by the root CMakeLists after FetchContent_MakeAvailable(allegro5)
# when ZC_3DS is on. The allegro5 FetchContent_Declare must apply the patch:
#
#   PATCH_COMMAND git restore . && git apply ${CMAKE_SOURCE_DIR}/third_party/allegro5_3ds.patch
#
# and, before FetchContent_MakeAvailable(allegro5), set ALLEGRO_SDL=ON and
# SHARED=OFF, and fetch SDL2 first so the SDL2-static target exists (the patch
# makes Allegro link that target instead of searching for a system SDL2).
#
# The patch drops src/sdl/sdl_display.c (OpenGL-only); this adds the software
# display that replaces it.

get_target_property(_zc_allegro_sources allegro SOURCES)
if("src/sdl/sdl_display.c" IN_LIST _zc_allegro_sources)
	message(FATAL_ERROR "allegro_3ds.cmake: third_party/allegro5_3ds.patch was not applied to allegro5")
endif()
unset(_zc_allegro_sources)

target_sources(allegro PRIVATE ${CMAKE_CURRENT_LIST_DIR}/sdl_soft_display.c)
