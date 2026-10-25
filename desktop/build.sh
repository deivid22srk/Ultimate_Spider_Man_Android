#!/bin/sh
# Builds the Skyrim desktop test binary (build/lb) on Linux or WSL2.
# Needs: clang cmake ninja-build make libsdl2-dev libvulkan-dev (+ mesa-vulkan-drivers to run).
# JOBS: parallel compiles (each generated file needs ~1-2 GB of RAM).
set -e
cd "$(dirname "$0")/.."
JOBS=${JOBS:-4}
cmake -S desktop -B desktop/build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
ninja -C desktop/build -j"$JOBS"
make -j"$JOBS" build/lb DXVK="$PWD/android/app/jni/dxvk" DXVK_LIB="$PWD/desktop/build/dxvk"
