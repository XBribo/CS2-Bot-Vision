cmake_minimum_required(VERSION 3.20)
project(BotVision VERSION 0.4.0 LANGUAGES CXX)

# -DPLUGIN_VERSION overrides the project version.
if(NOT DEFINED PLUGIN_VERSION)
    set(PLUGIN_VERSION "${PROJECT_VERSION}")
endif()
find_package(Git QUIET)
set(PLUGIN_BUILD_INFO "${CMAKE_BINARY_DIR}/generated/plugin_build_info.h")
add_custom_target(BotVision_build_info
    COMMAND "${CMAKE_COMMAND}"
        "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
        "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
        "-DOUTPUT_FILE=${PLUGIN_BUILD_INFO}"
        -P "${CMAKE_SOURCE_DIR}/cmake/UpdateBuildInfo.cmake"
    BYPRODUCTS "${PLUGIN_BUILD_INFO}"
    VERBATIM
)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format clang-format.exe)
set(BOT_VISION_FORMAT_FILES "")
# Check formatting in plugin sources only.
foreach(extension c cc cpp cxx h hh hpp hxx inl)
    file(GLOB plugin_format_top CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/*.${extension}" "${CMAKE_SOURCE_DIR}/src/core/*.${extension}")
    file(GLOB_RECURSE plugin_format_nested CONFIGURE_DEPENDS LIST_DIRECTORIES false
        "${CMAKE_SOURCE_DIR}/src/features/*.${extension}" "${CMAKE_SOURCE_DIR}/src/bridge/*.${extension}"
        "${CMAKE_SOURCE_DIR}/src/utils/*.${extension}" "${CMAKE_SOURCE_DIR}/src/core/entity/*.${extension}")
    list(APPEND BOT_VISION_FORMAT_FILES ${plugin_format_top} ${plugin_format_nested})
endforeach()

option(ENABLE_FORMAT_CHECK "Run clang-format check before building BotVision" OFF)

if(CLANG_FORMAT_EXECUTABLE)
    add_custom_target(format
        COMMAND "${CLANG_FORMAT_EXECUTABLE}" -i --style=file ${BOT_VISION_FORMAT_FILES}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Formatting BotVision sources"
        VERBATIM
    )
    add_custom_target(format-check
        COMMAND "${CLANG_FORMAT_EXECUTABLE}" --dry-run --Werror --style=file ${BOT_VISION_FORMAT_FILES}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Checking BotVision source formatting"
        VERBATIM
    )
else()
    add_custom_target(format
        COMMAND ${CMAKE_COMMAND} -E echo "clang-format was not found. Set CLANG_FORMAT_EXECUTABLE."
        COMMAND ${CMAKE_COMMAND} -E false
    )
    add_custom_target(format-check
        COMMAND ${CMAKE_COMMAND} -E echo "clang-format was not found. Set CLANG_FORMAT_EXECUTABLE."
        COMMAND ${CMAKE_COMMAND} -E false
    )
endif()

add_subdirectory(src/core/cs2_sdk)

add_library(
    BotVision SHARED
    src/plugin.cpp
    src/features/vision/BotVision.cpp
    src/features/smoke/SmokeVision.cpp
    src/features/he/HeVision.cpp
    src/features/bullet/BulletVision.cpp
    src/core/commands.cpp
    src/core/config.cpp
    src/core/game_time.cpp
    src/utils/memory.cpp
)

target_link_libraries(BotVision PRIVATE cs2_sdk::sdk)
cs2_sdk_configure_plugin(BotVision)
add_dependencies(BotVision BotVision_build_info)

if(ENABLE_FORMAT_CHECK)
    add_dependencies(BotVision format-check)
endif()

target_include_directories(BotVision PRIVATE
    "${CMAKE_BINARY_DIR}/generated"
    src
    src/core
    src/utils
)

target_compile_definitions(
    BotVision
    PRIVATE
    SEMVER="${PLUGIN_VERSION}"
)

if(WIN32)
    set(BOT_VISION_PLATFORM_DIR win64)
    set(BOT_VISION_SUFFIX ".dll")
else()
    set(BOT_VISION_PLATFORM_DIR linuxsteamrt64)
    set(BOT_VISION_SUFFIX ".so")
endif()

set(BOT_VISION_PACKAGE_BIN_DIR "${CMAKE_BINARY_DIR}/package/addons/BotVision/bin/${BOT_VISION_PLATFORM_DIR}")
set(BOT_VISION_VDF_FILE "addons/BotVision/bin/${BOT_VISION_PLATFORM_DIR}/BotVision")

configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/BotVision.vdf.in"
    "${CMAKE_BINARY_DIR}/BotVision.vdf"
    @ONLY
)

set_target_properties(
    BotVision
    PROPERTIES
    OUTPUT_NAME "BotVision"
    PREFIX ""
    SUFFIX "${BOT_VISION_SUFFIX}"
    RUNTIME_OUTPUT_DIRECTORY         "${BOT_VISION_PACKAGE_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${BOT_VISION_PACKAGE_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${BOT_VISION_PACKAGE_BIN_DIR}"
    LIBRARY_OUTPUT_DIRECTORY         "${BOT_VISION_PACKAGE_BIN_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${BOT_VISION_PACKAGE_BIN_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG   "${BOT_VISION_PACKAGE_BIN_DIR}"
)

add_custom_target(BotVision_resources
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${CMAKE_BINARY_DIR}/package/addons/metamod"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${CMAKE_BINARY_DIR}/package/addons/BotVision"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_BINARY_DIR}/BotVision.vdf"
        "${CMAKE_BINARY_DIR}/package/addons/metamod/BotVision.vdf"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/configs/addons/BotVision/gamedata.json"
        "${CMAKE_BINARY_DIR}/package/addons/BotVision/gamedata.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/configs/addons/BotVision/config.json"
        "${CMAKE_BINARY_DIR}/package/addons/BotVision/config.json"
    VERBATIM
)
add_dependencies(BotVision BotVision_resources)
