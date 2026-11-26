# External dependencies for the emscripten/wasm32 build.
#
# The emscripten sysroot does not ship libmd, so it is fetched and built
# here (included from CMakeLists.txt when UCODE_WASM is on; toolchain files
# cannot host this because they are re-included by every try_compile()
# probe build).
#
# libmd is autotools-only, so the cmake/deps/libmd wrapper CMakeLists
# compiles its sources directly.

include(ExternalProject)

set(UCODE_DEPS_PREFIX ${CMAKE_BINARY_DIR}/deps)

# The imported target below references this include directory, which only
# gets its contents when the external project builds; create it now so
# CMake's generate-time path validation passes.
file(MAKE_DIRECTORY ${UCODE_DEPS_PREFIX}/libmd/dist/include)

set(_ucode_toolchain_args "")
if(CMAKE_TOOLCHAIN_FILE)
  list(APPEND _ucode_toolchain_args -DCMAKE_TOOLCHAIN_FILE:FILEPATH=${CMAKE_TOOLCHAIN_FILE})
endif()

# ---------------------------------------------------------------------------
# libmd
# ---------------------------------------------------------------------------
ExternalProject_Add(libmd-build
  PREFIX ${UCODE_DEPS_PREFIX}/libmd
  GIT_REPOSITORY https://git.hadrons.org/git/libmd.git
  GIT_TAG 1.0.4
  GIT_SHALLOW 1
  CONFIGURE_COMMAND ${CMAKE_COMMAND}
    -S ${CMAKE_CURRENT_SOURCE_DIR}/cmake/deps/libmd
    -B ${UCODE_DEPS_PREFIX}/libmd/build
    ${_ucode_toolchain_args}
    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DLIBMD_SRC_DIR=${UCODE_DEPS_PREFIX}/libmd/src/libmd-build
    -DCMAKE_INSTALL_PREFIX=${UCODE_DEPS_PREFIX}/libmd/dist
  BUILD_COMMAND ${CMAKE_COMMAND} --build ${UCODE_DEPS_PREFIX}/libmd/build
  INSTALL_COMMAND ${CMAKE_COMMAND} --install ${UCODE_DEPS_PREFIX}/libmd/build
  UPDATE_COMMAND ""
)

add_library(libmd STATIC IMPORTED GLOBAL)
set_property(TARGET libmd PROPERTY IMPORTED_LOCATION ${UCODE_DEPS_PREFIX}/libmd/dist/lib/libmd.a)
set_property(TARGET libmd PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${UCODE_DEPS_PREFIX}/libmd/dist/include)
add_dependencies(libmd libmd-build)
