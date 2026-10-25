# Desktop (Linux/WSL) build of dxvk-native d3d9 from the Android tree's sources.
cmake_minimum_required(VERSION 3.18)
project(dxvk_desktop C CXX)
set(CMAKE_CXX_STANDARD 17)
find_path(SDL2_INC SDL.h PATH_SUFFIXES SDL2)
include_directories(${SDL2_INC})
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../android/app/jni/dxvk dxvk)
