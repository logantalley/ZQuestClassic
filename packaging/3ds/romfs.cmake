# Stages the RomFS shared by zplayer's .3dsx and .cia into ZC_3DS_ROMFS_DIR,
# as the asset target zplayer_romfs (see stage_romfs.py for what goes in).

set(ZC_3DS_ROMFS_DIR "${CMAKE_BINARY_DIR}/romfs")
set(zc_3ds_romfs_stamp "${CMAKE_BINARY_DIR}/romfs.stamp")

file(GLOB_RECURSE zc_3ds_romfs_inputs CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/resources/*")
list(FILTER zc_3ds_romfs_inputs EXCLUDE REGEX "/resources/(docs|headers|include|quests|tilesets)/")
add_custom_command(
	OUTPUT "${zc_3ds_romfs_stamp}"
	COMMAND ${PYTHON} "${CMAKE_CURRENT_LIST_DIR}/stage_romfs.py" "${CMAKE_SOURCE_DIR}/resources" "${ZC_3DS_ROMFS_DIR}"
	COMMAND ${CMAKE_COMMAND} -E touch "${zc_3ds_romfs_stamp}"
	DEPENDS "${CMAKE_CURRENT_LIST_DIR}/stage_romfs.py" ${zc_3ds_romfs_inputs}
	COMMENT "Staging the 3DS RomFS"
	VERBATIM
)
add_custom_target(zplayer_romfs_stage DEPENDS "${zc_3ds_romfs_stamp}")

dkp_add_asset_target(zplayer_romfs "${ZC_3DS_ROMFS_DIR}")
add_dependencies(zplayer_romfs zplayer_romfs_stage)
set_property(TARGET zplayer_romfs APPEND PROPERTY DKP_ASSET_FILES "${zc_3ds_romfs_stamp}")
