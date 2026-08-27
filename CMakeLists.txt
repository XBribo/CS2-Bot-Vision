cmake_minimum_required(VERSION 3.20)
project(BotVision CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "BotVision requires x64 build (use -A x64).")
endif()

set(HL2SDK $ENV{HL2SDKCS2})
set(MMS $ENV{MMSOURCE_DEV})
set(PROTO_DIR $ENV{CSGO_PROTO})

if(HL2SDK AND NOT PROTO_DIR)
    set(PROTO_DIR "${HL2SDK}/common")
endif()

foreach(v HL2SDK MMS)
    if(NOT ${v} OR NOT EXISTS "${${v}}")
        message(FATAL_ERROR "${v} env var not set or path missing: '${${v}}'")
    endif()
endforeach()
if(NOT PROTO_DIR OR NOT EXISTS "${PROTO_DIR}/network_connection.proto")
    message(FATAL_ERROR "PROTO_DIR/CSGO_PROTO missing network_connection.proto: '${PROTO_DIR}'")
endif()

if(DEFINED ENV{PROTOC} AND EXISTS "$ENV{PROTOC}")
    set(PROTOC_EXE "$ENV{PROTOC}")
else()
    find_program(PROTOC_EXE protoc REQUIRED)
endif()
execute_process(
    COMMAND "${PROTOC_EXE}" --version
    OUTPUT_VARIABLE PROTOC_VER
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(NOT PROTOC_VER MATCHES "libprotoc 3\\.21\\.")
    message(FATAL_ERROR "protoc must be 3.21.x (got '${PROTOC_VER}')")
endif()

message(STATUS "protoc: ${PROTOC_EXE} (${PROTOC_VER})")

set(PROTO_FILES network_connection.proto)
set(GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GENERATED_DIR}")

set(PROTO_SRCS "")
set(PROTO_INPUTS "")
foreach(p ${PROTO_FILES})
    string(REGEX REPLACE "\\.proto$" ".pb.cc" cc ${p})
    string(REGEX REPLACE "\\.proto$" ".pb.h" hh ${p})
    list(APPEND PROTO_SRCS "${GENERATED_DIR}/${cc}" "${GENERATED_DIR}/${hh}")
    list(APPEND PROTO_INPUTS "${PROTO_DIR}/${p}")
endforeach()

add_custom_command(
    OUTPUT ${PROTO_SRCS}
    COMMAND "${PROTOC_EXE}" --cpp_out=${GENERATED_DIR} --proto_path=${PROTO_DIR} ${PROTO_FILES}
    DEPENDS ${PROTO_INPUTS}
    WORKING_DIRECTORY "${PROTO_DIR}"
    COMMENT "protoc ${PROTO_FILES}"
    VERBATIM
)

include(FetchContent)

if(DEFINED ENV{FUNCHOOK_SOURCE} AND EXISTS "$ENV{FUNCHOOK_SOURCE}/CMakeLists.txt")
    set(FETCHCONTENT_SOURCE_DIR_FUNCHOOK "$ENV{FUNCHOOK_SOURCE}" CACHE PATH "")
endif()

set(FUNCHOOK_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(FUNCHOOK_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(FUNCHOOK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FUNCHOOK_INSTALL OFF CACHE BOOL "" FORCE)
set(FUNCHOOK_DISASM distorm CACHE STRING "" FORCE)

FetchContent_Declare(
    funchook
    GIT_REPOSITORY https://github.com/kubo/funchook.git
    GIT_TAG v1.1.3
    GIT_SHALLOW TRUE
    GIT_SUBMODULES_RECURSE TRUE
)

FetchContent_MakeAvailable(funchook)
set_target_properties(funchook-static PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(SDK_SOURCES
    ${HL2SDK}/public/tier0/memoverride.cpp
    ${HL2SDK}/tier1/keyvalues3.cpp
    ${HL2SDK}/tier1/convar.cpp
    ${MMS}/core/sourcehook/sourcehook.cpp
    ${MMS}/core/sourcehook/sourcehook_impl_chookidman.cpp
    ${MMS}/core/sourcehook/sourcehook_impl_chookmaninfo.cpp
    ${MMS}/core/sourcehook/sourcehook_impl_cvfnptr.cpp
    ${MMS}/core/sourcehook/sourcehook_impl_cproto.cpp
)

add_library(
    BotVision SHARED
    src/plugin.cpp
    src/BotVision/BotVision.cpp
    src/SmokeVision/SmokeVision.cpp
    src/HeVision/HeVision.cpp
    src/BulletVision/BulletVision.cpp
    src/common/commands.cpp
    src/common/game_time.cpp
    src/common/memory.cpp
    src/common/platform.cpp
    src/common/schema_resolver.cpp
    src/common/sig_scan.cpp
    ${PROTO_SRCS}
    ${SDK_SOURCES}
)

target_include_directories(
    BotVision
    PRIVATE
    src
    src/BotVision
    src/SmokeVision
    src/HeVision
    src/BulletVision
    src/common
    ${CMAKE_SOURCE_DIR}/third_party
    ${GENERATED_DIR}
    ${HL2SDK}/thirdparty/protobuf-3.21.8/src
    ${MMS}/core ${MMS}/core/sourcehook ${MMS}/core/sourcemm
    ${HL2SDK}/public
    ${HL2SDK}/public/engine
    ${HL2SDK}/public/game/server
    ${HL2SDK}/public/game/shared
    ${HL2SDK}/public/mathlib
    ${HL2SDK}/public/tier0
    ${HL2SDK}/public/entity2
    ${HL2SDK}/game/shared
    ${HL2SDK}/common
)

target_compile_definitions(
    BotVision
    PRIVATE
    META_IS_SOURCE2
    PLATFORM_64BITS
    X64BITS
    GOOGLE_PROTOBUF_NO_RTTI
)

if(WIN32)
    set(BOT_VISION_PLATFORM_DIR win64)
    set(BOT_VISION_SUFFIX ".dll")
    set(HL2SDK_LIBDIR ${HL2SDK}/lib/public/win64)

    # MSVC has no <byteswap.h> clash, so tier1 goes on the normal include path
    target_include_directories(BotVision PRIVATE ${HL2SDK}/public/tier1)
    target_link_libraries(
        BotVision
        PRIVATE
        funchook-static
        psapi
        ${HL2SDK_LIBDIR}/tier0.lib
        ${HL2SDK_LIBDIR}/mathlib.lib
        ${HL2SDK_LIBDIR}/interfaces.lib
        ${HL2SDK_LIBDIR}/2015/libprotobuf.lib
    )
    if(MSVC)
        target_link_options(BotVision PRIVATE /NODEFAULTLIB:LIBCMTD)
    endif()
    target_compile_definitions(
        BotVision
        PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32
        _WIN32
        _WINDOWS
        COMPILER_MSVC
        COMPILER_MSVC64
        NOMINMAX
        _ALLOW_KEYWORD_MACROS
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(BOT_VISION_PLATFORM_DIR linuxsteamrt64)
    set(BOT_VISION_SUFFIX ".so")
    set(HL2SDK_LIBDIR ${HL2SDK}/lib/linux64)

    target_link_libraries(
        BotVision
        PRIVATE
        funchook-static
        ${HL2SDK_LIBDIR}/libtier0.so
        ${HL2SDK_LIBDIR}/mathlib.a
        ${HL2SDK_LIBDIR}/interfaces.a
        ${HL2SDK_LIBDIR}/release/libprotobuf.a
        pthread
        dl
    )
    target_compile_definitions(
        BotVision
        PRIVATE
        _GNU_SOURCE
        POSIX
        _LINUX
        LINUX
        COMPILER_GCC
        stricmp=strcasecmp
        _stricmp=strcasecmp
        _strnicmp=strncasecmp
        strnicmp=strncasecmp
        _snprintf=snprintf
        _vsnprintf=vsnprintf
        _alloca=alloca
        strcmpi=strcasecmp
    )
    target_compile_options(
        BotVision
        PRIVATE
        -fPIC
        -fno-rtti
        -fvisibility=hidden
        # Keep the SDK tier1 headers after the system headers on Linux
        -idirafter ${HL2SDK}/public/tier1
        -Wno-non-virtual-dtor
        -Wno-overloaded-virtual
        -Wno-register
        -Wno-narrowing
        -Wno-deprecated-copy
    )
    set_target_properties(
        BotVision
        PROPERTIES
        SKIP_BUILD_RPATH TRUE
        BUILD_RPATH ""
        INSTALL_RPATH ""
    )
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
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

add_custom_command(
    TARGET BotVision
    POST_BUILD
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
)

if(MSVC)
    target_compile_options(
        BotVision
        PRIVATE
        /W3
        /permissive-
        /utf-8 # UTF-8
        /wd4267 # size_t -> int conversion
        /wd4244 # narrowing conversion
        /wd4146 # unary minus on unsigned
        /wd4996 # deprecated CRT
        /wd4661 # template instantiation needed
    )
endif()
