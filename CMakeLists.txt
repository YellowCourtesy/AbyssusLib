cmake_minimum_required(VERSION 3.21)

project(AbyssusLib VERSION 1.0.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

# CommonLibSSE-NG (alandtse fork) is a git submodule in lib/commonlibsse-ng. Skyrim AE 1.7.x.
set(ENABLE_SKYRIM_SE OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(lib/commonlibsse-ng EXCLUDE_FROM_ALL)
include(lib/commonlibsse-ng/cmake/CommonLibSSE.cmake)

find_package(imgui CONFIG REQUIRED)

add_commonlibsse_plugin(${PROJECT_NAME}
    NAME "AbyssusLib"
    AUTHOR "AbyssusLib contributors"
    USE_ADDRESS_LIBRARY
    SOURCES src/main.cpp src/Overlay.cpp src/Overlay.h src/PCH.h include/Abyssus/API.h
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_include_directories(${PROJECT_NAME} PRIVATE src include)
target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
# Overlay.cpp uses windows.h and Direct3D directly, not CommonLibSSE.
set_source_files_properties(src/Overlay.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON COMPILE_OPTIONS "/WX-")
target_link_libraries(${PROJECT_NAME} PRIVATE imgui::imgui d3d11.lib dxgi.lib)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /WX)
endif()
