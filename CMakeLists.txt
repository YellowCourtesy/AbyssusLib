cmake_minimum_required(VERSION 3.21)

project(AbyssusLib VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

# CommonLibSSE-NG (alandtse fork) is a git submodule in lib/commonlibsse-ng.
# One DLL for SE, AE and VR (tested only on AE 1.7.x). All three runtimes ON
# also lets the build use the prebuilt CommonLib bundle.
set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR ON CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(lib/commonlibsse-ng EXCLUDE_FROM_ALL)
# CommonLibSSE loads its plugin helper (add_commonlibsse_plugin) only when it
# uses a prebuilt binary. When it builds from source (for example, the build
# machine has a newer compiler than the prebuilt binary), load the helper here.
if(NOT COMMAND add_commonlibsse_plugin)
	include("${CMAKE_CURRENT_SOURCE_DIR}/lib/commonlibsse-ng/cmake/CommonLibSSE.cmake")
endif()
include(lib/commonlibsse-ng/cmake/CommonLibSSE.cmake)

find_package(imgui CONFIG REQUIRED)

# The default settings: AbyssusLib.ini at the top level. Its bytes go into
# the DLL. Edit that file to change the defaults.
set(ABYSSUS_INI "${CMAKE_CURRENT_SOURCE_DIR}/AbyssusLib.ini")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ABYSSUS_INI}")
file(READ "${ABYSSUS_INI}" ABYSSUS_INI_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," ABYSSUS_INI_BYTES "${ABYSSUS_INI_HEX}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/DefaultIni.inc"
    "// Made by CMake from AbyssusLib.ini. Do not change.\nstatic const unsigned char kDefaultIni[] = { ${ABYSSUS_INI_BYTES} 0x00 };\n")

add_commonlibsse_plugin(${PROJECT_NAME}
    NAME "AbyssusLib"
    AUTHOR "AbyssusLib contributors"
    USE_ADDRESS_LIBRARY
    SOURCES src/main.cpp src/Overlay.cpp src/Overlay.h src/Settings.cpp src/Settings.h src/IniUpdate.cpp src/IniUpdate.h src/PCH.h include/Abyssus/API.h
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_include_directories(${PROJECT_NAME} PRIVATE src include "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN ABYSSUS_VERSION="${PROJECT_VERSION}")
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
# Overlay.cpp uses windows.h and Direct3D directly, not CommonLibSSE.
set_source_files_properties(src/Overlay.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON COMPILE_OPTIONS "/WX-")
target_link_libraries(${PROJECT_NAME} PRIVATE imgui::imgui d3d11.lib dxgi.lib)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /WX)
endif()
