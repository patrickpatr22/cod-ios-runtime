# Call of Duty iPad - recompiled Xbox 360 Call of Duty games (Black Ops II,
# Black Ops) on the ReXGlue runtime, with a start screen to choose the game.
#
# Inputs that are NOT in git (CI decrypts them from the game-data branch):
#   generated/default_mp/   Black Ops II default_mp.xex (Title Update 18)
#   generated/bo1_mp/       Black Ops default_mp.xex (Title Update 11)
#   generated/bo1_sp/       Black Ops default.xex (Title Update 11)
#
# iOS build (macOS host):
#   cmake -S . -B build-ios -G Ninja -DCMAKE_SYSTEM_NAME=iOS \
#         -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
#         -DCMAKE_BUILD_TYPE=Release
# Linux host build (only used to check that everything compiles):
#   cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++

cmake_minimum_required(VERSION 3.25)

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(COD_LANGUAGES C CXX OBJC OBJCXX ASM)
else()
    set(COD_LANGUAGES C CXX)
endif()

project(CallOfDutyiOS VERSION 0.1.0 LANGUAGES ${COD_LANGUAGES})

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Keep the runtime lean: no profiler, no FidelityFX.
set(REXGLUE_ENABLE_TRACY OFF CACHE BOOL "" FORCE)
set(REXGLUE_ENABLE_FIDELITYFX OFF CACHE BOOL "" FORCE)
set(REXGLUE_USE_VULKAN ON CACHE BOOL "" FORCE)
# The spatial FidelityFX output effects (FSR 1 upscaling and RCAS / CAS
# sharpening) only need the precompiled shaders that ship with the SDK, not the
# FidelityFX SDK. Defined for every target (SDK and app), as it changes the
# layout of the presenter classes.
add_compile_definitions(REX_HAS_FIDELITYFX_SDK=1)

set(BO2_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sdk" CACHE PATH
    "ReXGlue SDK source tree (with patches/rexglue-sdk-ios.patch applied)")
add_subdirectory("${BO2_SDK_DIR}" rexglue-sdk)

#==========================================================================
# Recompiled game code: one shared library per game executable ("game
# module", see src/game_module.h), loaded when its mode is started. Each is
# generated from the user's own disc (generated/<project>/, CI decrypts it
# from the game-data branch); a module whose code is missing is left out and
# the start screen shows its game as not included.
#==========================================================================
set(COD_GENERATED_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/generated")
set(COD_GAME_MODULES "")

# Hooks every module shares (Xbox Live replacements, stubs). Compiled once,
# linked into each module with hidden symbols.
add_library(cod_module_common OBJECT
    src/modules/common/xam_overrides.cpp
    src/modules/common/stubs.cpp)
target_include_directories(cod_module_common PRIVATE src)
target_link_libraries(cod_module_common PRIVATE rex::runtime)
set_target_properties(cod_module_common PROPERTIES
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON POSITION_INDEPENDENT_CODE ON)
rexglue_apply_target_settings(cod_module_common)

# cod_add_game_module(<id> <rexglue project> SOURCES <game-specific hooks>...)
function(cod_add_game_module id project)
    cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
    set(gen_dir "${COD_GENERATED_ROOT}/${project}")
    if(NOT EXISTS "${gen_dir}/${project}_init.h")
        message(STATUS "Game module ${id}: no recompiled code in ${gen_dir}, left out")
        return()
    endif()
    file(GLOB gen_sources CONFIGURE_DEPENDS "${gen_dir}/*.cpp")

    add_library(cod_${id}_code OBJECT ${gen_sources})
    target_include_directories(cod_${id}_code PRIVATE "${gen_dir}")
    target_link_libraries(cod_${id}_code PRIVATE rex::runtime)
    target_precompile_headers(cod_${id}_code PRIVATE "${gen_dir}/${project}_pch.h")
    # Line tables only: readable crash logs without the size of full debug info.
    target_compile_options(cod_${id}_code PRIVATE -gline-tables-only -Wno-unused-variable
        -Wno-unused-but-set-variable -Wno-unused-label -Wno-unused-parameter)
    set_target_properties(cod_${id}_code PROPERTIES
        CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON POSITION_INDEPENDENT_CODE ON)
    rexglue_apply_target_settings(cod_${id}_code)

    add_library(cod_${id} SHARED
        $<TARGET_OBJECTS:cod_${id}_code>
        $<TARGET_OBJECTS:cod_module_common>
        src/modules/module_entry.cpp
        ${ARG_SOURCES})
    target_compile_definitions(cod_${id} PRIVATE COD_MODULE_ID="${id}")
    target_include_directories(cod_${id} PRIVATE "${gen_dir}" src)
    target_link_libraries(cod_${id} PRIVATE rex::runtime)
    set_target_properties(cod_${id} PROPERTIES
        CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
    rexglue_configure_module_target(cod_${id} HOST callofduty)
    if(APPLE)
        # Loaded from Frameworks/ next to librexruntime.
        set_target_properties(cod_${id} PROPERTIES
            INSTALL_NAME_DIR "@rpath"
            BUILD_WITH_INSTALL_RPATH ON
            INSTALL_RPATH "@loader_path;@executable_path/Frameworks")
    endif()
    list(APPEND COD_GAME_MODULES ${id})
    set(COD_GAME_MODULES "${COD_GAME_MODULES}" PARENT_SCOPE)
endfunction()

# Black Ops II: default_mp.xex with Title Update 18 (Multiplayer + Zombies).
cod_add_game_module(bo2_mp default_mp SOURCES
    src/modules/bo2/bo2_module.cpp
    src/modules/bo2/fov.cpp
    src/modules/bo2/framerate.cpp
    src/modules/bo2/unlocks.cpp)
# Black Ops with Title Update 11: default_mp.xex (Multiplayer) and
# default.xex (Campaign + Zombies).
cod_add_game_module(bo1_mp bo1_mp SOURCES src/modules/bo1/bo1_mp_module.cpp)
cod_add_game_module(bo1_sp bo1_sp SOURCES src/modules/bo1/bo1_sp_module.cpp)
message(STATUS "Game modules: ${COD_GAME_MODULES}")

#==========================================================================
# App
#==========================================================================
# Fonts of the start screen (resources/fonts, SIL Open Font License), compiled
# into the app.
include(cmake/embed_fonts.cmake)
file(GLOB COD_FONT_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts/*.ttf")
set(COD_EMBEDDED_FONTS "${CMAKE_CURRENT_BINARY_DIR}/embedded_fonts.cpp")
cod_embed_fonts("${COD_EMBEDDED_FONTS}" ${COD_FONT_FILES})

set(COD_APP_SOURCES
    src/main.cpp
    src/cod_app.cpp
    src/games.cpp
    src/game_loader.cpp
    src/launcher.cpp
    src/ui_kit.cpp
    ${COD_EMBEDDED_FONTS}
    src/ios_boot.cpp
    src/diagnostics.cpp
    src/settings.cpp
)

if(IOS)
    add_executable(callofduty MACOSX_BUNDLE ${COD_APP_SOURCES})
    set_target_properties(callofduty PROPERTIES
        OUTPUT_NAME "CallOfDuty"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/ios/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "CallOfDuty"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.patrick.bo2recomp"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        BUILD_WITH_INSTALL_RPATH ON
    )
    target_link_libraries(callofduty PRIVATE
        "-framework UIKit" "-framework Foundation" "-framework QuartzCore"
        "-framework Metal" "-framework GameController" "-framework AVFoundation"
        "-framework AudioToolbox" "-framework CoreHaptics" "-framework CoreMotion"
        "-lobjc")
else()
    add_executable(callofduty ${COD_APP_SOURCES})
endif()

target_include_directories(callofduty PRIVATE src
    # rex_app.h pulls in ImGui headers; the symbols come from librexruntime.
    $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(callofduty PRIVATE rex::runtime ${CMAKE_DL_LIBS})
rexglue_configure_target(callofduty GPU_PLUGINS xenos)

if(IOS)
    # Embedded dylibs (librexruntime, GPU plugin) are moved into Frameworks/
    # when the IPA is packaged; MoltenVK lives there too.
    set_target_properties(callofduty PROPERTIES
        INSTALL_RPATH "@executable_path/Frameworks;@executable_path")
endif()
