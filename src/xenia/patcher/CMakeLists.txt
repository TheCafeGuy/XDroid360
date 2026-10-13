add_library(xenia-patcher STATIC)
xe_platform_sources(xenia-patcher ${CMAKE_CURRENT_SOURCE_DIR} RECURSIVE)
target_link_libraries(xenia-patcher PUBLIC xenia-base)
xe_target_defaults(xenia-patcher)

# xenia-canary/game-patches carries no license file, so the Android app does
# not embed it by default; users can still put patch files in <storage>/patches.
if(ANDROID)
  set(_xe_embed_patches_default OFF)
else()
  set(_xe_embed_patches_default ON)
endif()
option(XENIA_EMBED_GAME_PATCHES "Embed build/data_repos/game-patches"
       ${_xe_embed_patches_default})
if(XENIA_EMBED_GAME_PATCHES)
  set(_xe_patches_dir "${PROJECT_SOURCE_DIR}/build/data_repos/game-patches/patches")
else()
  # A missing directory makes embed_bundle.py emit an empty bundle.
  set(_xe_patches_dir "${CMAKE_CURRENT_BINARY_DIR}/no-embedded-patches")
endif()
xe_embed_compressed_bundle(xenia-patcher "${_xe_patches_dir}" patches)
