cmake_minimum_required(VERSION 3.21)

project(AbyssusLib VERSION 0.1.0 LANGUAGES CXX)

# Shared code for SKSE plugins. Add this folder with add_subdirectory() and
# link AbyssusLib::AbyssusLib. Needs vcpkg port "imgui" with feature "dx11-binding".

find_package(imgui CONFIG REQUIRED)

add_library(AbyssusLib STATIC
    src/Notify.cpp
    include/Abyssus/Notify.h
)
add_library(AbyssusLib::AbyssusLib ALIAS AbyssusLib)

target_compile_features(AbyssusLib PUBLIC cxx_std_20)
target_include_directories(AbyssusLib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(AbyssusLib PUBLIC imgui::imgui d3d11.lib dxgi.lib)
