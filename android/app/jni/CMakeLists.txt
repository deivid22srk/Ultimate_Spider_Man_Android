cmake_minimum_required(VERSION 3.18)
project(usm C CXX)
set(CMAKE_CXX_STANDARD 17)
# Each recompiled file needs 1-2 GB of RAM to compile; cap parallel jobs.
set(LB_GEN_JOBS 3 CACHE STRING "parallel compiles of recompiled code")
set_property(GLOBAL APPEND PROPERTY JOB_POOLS gen=${LB_GEN_JOBS})

set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory(SDL2)
add_subdirectory(dxvk)
if(ANDROID)
    # Custom GPU drivers: opens the system loader with a user supplied
    # libvulkan.so swapped in (BSD-2-Clause, vendored under adrenotools/).
    add_subdirectory(adrenotools)
endif()
add_subdirectory(game)
