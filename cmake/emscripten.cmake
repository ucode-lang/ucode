# CMake toolchain file for building ucode for WebAssembly (wasm32) with
# emscripten.
#
#   cmake -B build-wasm -DCMAKE_TOOLCHAIN_FILE=cmake/emscripten.cmake
#
# Requires an activated emsdk environment (emcc on PATH). Produces
# libucode.a and ucode-modules.a for static linking into an embedder's
# executable; see the UCODE_WASM section of CMakeLists.txt.
#
# Note: the dependency setup (libmd) cannot live in this file --
# toolchain files are re-included by every try_compile() probe build --
# so CMakeLists.txt pulls in cmake/emscripten-deps.cmake when it detects
# the emscripten compiler.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR wasm32)

set(CMAKE_C_COMPILER emcc)
set(CMAKE_CXX_COMPILER em++)
set(CMAKE_ASM_COMPILER emcc)

set(CMAKE_C_COMPILER_WORKS TRUE)
set(CMAKE_CXX_COMPILER_WORKS TRUE)

# Declare the emscripten build to CMakeLists.txt. Some CMake versions
# detect emcc as plain "Clang" (no Emscripten branch in the compiler ID
# source), so do not rely on CMAKE_C_COMPILER_ID alone. An explicit
# -DUCODE_WASM=... on the command line takes precedence. (Cache writes
# in this file do not leak into try_compile() probe builds, which use
# their own cache.)
if(NOT DEFINED CACHE{UCODE_WASM})
  set(UCODE_WASM ON CACHE BOOL "Build static libraries for wasm32" FORCE)
endif()
