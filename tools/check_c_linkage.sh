#!/bin/sh
# Catch a C++ linkage mistake before it costs a CI round trip.
#
# The runtime is C, and the C++ files call into it. A block-scope declaration
# like
#     void f(void) { extern void user32_something(int); user32_something(1); }
# gets *C* language linkage -- unless it happens inside an anonymous namespace,
# where clang gives it internal linkage and emits `(anonymous namespace)::...`.
# The compile succeeds and only the NDK link fails, with
#     undefined symbol: (anonymous namespace)::user32_...
# (this really happened: touch.cpp, commit e446308).
#
# This script compiles every runtime/*.cpp the way the Android build does and
# looks for undefined references to the runtime's C API in mangled form. It
# needs no NDK: any host clang++ and the SDL2/dxvk headers in the tree.
#
# Usage: tools/check_c_linkage.sh            (from the repo root)
set -e
SDLINC=${SDLINC:--Iandroid/app/jni/SDL2/include}
[ -d android/app/jni/SDL2/include ] || { echo "run me from the repo root"; exit 2; }
mkdir -p /tmp/opencode/linkchk
ln -sfn "$PWD/android/app/jni/SDL2/include" /tmp/opencode/sdlink_inc/SDL2 2>/dev/null || {
    mkdir -p /tmp/opencode/sdlink_inc; ln -sfn "$PWD/android/app/jni/SDL2/include" /tmp/opencode/sdlink_inc/SDL2; }

bad=0
for f in runtime/*.cpp; do
    o=/tmp/opencode/linkchk/$(basename "$f" .cpp).o
    clang++ -DGAME_USM -std=c++17 -O1 -fno-strict-aliasing -Iruntime -Igen \
        -Iandroid/app/jni/dxvk/include/native/directx \
        -Iandroid/app/jni/dxvk/include/native/windows -DDXVK_NATIVE \
        -I/tmp/opencode/sdlink_inc -Wno-everything -c "$f" -o "$o"
    # undefined symbols that look like a runtime C symbol but are mangled
    hits=$(llvm-nm -u "$o" | grep -E '_Z.*(user32_|platform_|input_|usm_)' || true)
    if [ -n "$hits" ]; then
        echo "!! $f calls the C runtime through a mangled name:"
        echo "$hits"
        echo "   declare it in runtime/rt.h (included via d3d9_bridge.h's extern \"C\")"
        bad=1
    else
        echo "ok $f"
    fi
done
[ "$bad" = 0 ] || exit 1
echo "all C++ files reference the C runtime with C linkage"