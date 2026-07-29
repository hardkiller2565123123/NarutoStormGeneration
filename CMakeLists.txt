# generations - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(generations LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(GENERATIONS_SOURCES
    src/main.cpp
    src/launcher.rc
    src/resource.h
    src/generations_app.cpp
    src/native_game_menu.cpp
    src/native_options_patch.cpp

    # Launcher/profile implementation used directly by generations_app.cpp.
    src/launcher.cpp
    src/profile_setup_dialog.cpp

    # Generated PPC hooks call these implementations directly.
    src/network_handshake_patch.cpp
)

if(WIN32)
    add_executable(generations WIN32 ${GENERATIONS_SOURCES})
else()
    add_executable(generations ${GENERATIONS_SOURCES})
endif()

set_target_properties(generations PROPERTIES
    OUTPUT_NAME "NarutoStormGeneration"
)

rexglue_setup_target(generations)

if(WIN32)
    # Launcher dependencies.
    target_link_libraries(generations PRIVATE
        gdiplus
        xinput9_1_0
        shell32
        ole32
    )

    # Keep release probes symbolizable without enabling the much larger debug build.
    target_link_options(generations PRIVATE
        "LINKER:/MAP:${CMAKE_CURRENT_BINARY_DIR}/NarutoStormGeneration.map"
    )
endif()
