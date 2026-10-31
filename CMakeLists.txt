cmake_minimum_required(VERSION 3.13)

project(ucode C)

add_library(uc_defines INTERFACE)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CheckFunctionExists)
include(CheckSymbolExists)
include(CheckCSourceCompiles)
include(CheckIPOSupported)
include(GNUInstallDirs)

option(BUILD_OPTIMIZE_SIZE "Optimize for size" ON)
if(BUILD_OPTIMIZE_SIZE)
  add_compile_options(
    -Os
  )
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

add_compile_definitions( _GNU_SOURCE )
add_compile_options(
  -Wall
  -Werror
  -Wmissing-declarations
)

if(CMAKE_C_COMPILER_VERSION VERSION_GREATER 6)
  add_compile_options(
    -Wextra
    -Wformat
    -Werror=implicit-function-declaration
    -Werror=format-security
    -Werror=format-nonliteral
  )
endif()

add_compile_options(
  -Wno-unused-parameter
  -Wno-error=unused-variable
)

# signed overflow semantics are defined AND expected
add_compile_options(-fwrapv)

option(BUILD_FUNCTION_SECTIONS "Compile with -ffunction-sections" ON)
if(BUILD_FUNCTION_SECTIONS)
  add_compile_options(-ffunction-sections)
endif()

# Omit the async stack-unwind tables (.eh_frame) in release builds. ucode is
# pure C with no exceptions and provides its own bytecode-level stack traces,
# so these tables only serve external C-stack unwinding (backtrace(), C-level
# crash reporters, profilers) and cost roughly 23 KB of the shared library.
# Debug and RelWithDebInfo keep them to preserve native stack traces.
option(BUILD_NO_UNWIND_TABLES "Omit async stack-unwind tables in release builds" ON)
if(BUILD_NO_UNWIND_TABLES)
  # listing several configurations in a single $<CONFIG:> expression requires
  # CMake >= 3.19, so combine single-configuration expressions to stay
  # compatible with the declared minimum CMake version
  add_compile_options(
    $<$<OR:$<CONFIG:Release>,$<CONFIG:MinSizeRel>>:-fno-asynchronous-unwind-tables>
  )
endif()

# Link-time optimization in release builds. Cross translation-unit dead code
# elimination and folding of identical functions trims a further few percent
# from libucode. Off by default: it lengthens builds and assumes a matching LTO
# toolchain, so OpenWrt and default builds are unaffected unless requested.
# Only Release and MinSizeRel are enabled, Debug and RelWithDebInfo are not.
option(BUILD_LTO "Enable link-time optimization in release builds" OFF)
if(BUILD_LTO)
  check_ipo_supported(RESULT lto_supported OUTPUT lto_error)

  if(lto_supported)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON)
  else()
    message(WARNING "BUILD_LTO requested but IPO is unsupported: ${lto_error}")
  endif()
endif()

check_c_source_compiles("
int main() {
#ifndef _FORTIFY_SOURCE
#error No fortification
#endif
  return 0;
}
" DEFAULT_FORTIFY_SOURCE)

if(NOT DEFAULT_FORTIFY_SOURCE)
  add_compile_definitions( FORTIFY_SOURCE=2 )
endif()

# Emscripten / WebAssembly build:
#   cmake -B build-wasm -DCMAKE_TOOLCHAIN_FILE=cmake/emscripten.cmake
#
# Produces libucode.a and ucode-modules.a (modules with renamed
# uc_module_init symbols) for static linking into an embedder's
# executable. The CLI, udbg, dlopen plugins and examples are not built.
if(CMAKE_C_COMPILER_ID STREQUAL "Emscripten")
  set(UCODE_WASM_DEFAULT ON)
else()
  set(UCODE_WASM_DEFAULT OFF)
endif()
option(UCODE_WASM "Build static libraries for wasm32 (no CLI, no dlopen plugins)" ${UCODE_WASM_DEFAULT})

include_directories(include)

option(COMPILE_SUPPORT "Support compilation from source" ON)
if(NOT COMPILE_SUPPORT)
  target_compile_definitions(uc_defines INTERFACE NO_COMPILE)
endif()

option(LLVM_SUPPORT "Support native programs: LLVM IR output and runtime (Linux only)" OFF)
if(LLVM_SUPPORT)
  target_compile_definitions(uc_defines INTERFACE HAVE_LLVM_SUPPORT)
endif()

if(NOT UCODE_WASM)
  find_library(libuci NAMES uci)
  find_library(libubox NAMES ubox)
  find_library(libubus NAMES ubus)
  find_package(ZLIB)
  find_library(libmd NAMES libmd.a md)
  find_library(libffi NAMES ffi)

  if(LINUX)
    find_library(libnl_tiny NAMES nl-tiny)

    if(libnl_tiny AND libubox)
      set(DEFAULT_NL_SUPPORT ON)
    endif()
  endif()

  if(libuci AND libubox)
    set(DEFAULT_UCI_SUPPORT ON)
  endif()

  if(libubus)
    set(DEFAULT_UBUS_SUPPORT ON)
  endif()

  if(libubox)
    set(DEFAULT_ULOOP_SUPPORT ON)
  endif()

  if(ZLIB_FOUND)
    set(DEFAULT_ZLIB_SUPPORT ON)
  endif()

  if(libmd)
    set(DEFAULT_DIGEST_SUPPORT ON)
  endif()

  if(libffi)
    set(DEFAULT_FFI_SUPPORT ON)
  endif()
endif()

if(NOT UCODE_WASM)
  option(DEBUG_SUPPORT "Debug plugin support" ON)
  option(FS_SUPPORT "Filesystem plugin support" ON)
  option(IO_SUPPORT "IO plugin support" ON)
  option(MATH_SUPPORT "Math plugin support" ON)
  option(NETADDR_SUPPORT "Network address plugin support" ON)
  option(FFI_SUPPORT "FFI module support" ${DEFAULT_FFI_SUPPORT})
  option(UBUS_SUPPORT "Ubus plugin support" ${DEFAULT_UBUS_SUPPORT})
  option(UCI_SUPPORT "UCI plugin support" ${DEFAULT_UCI_SUPPORT})
  option(RTNL_SUPPORT "Route Netlink plugin support" ${DEFAULT_NL_SUPPORT})
  option(NL80211_SUPPORT "Wireless Netlink plugin support" ${DEFAULT_NL_SUPPORT})
  option(RESOLV_SUPPORT "NS resolve plugin support" ON)
  option(STRUCT_SUPPORT "Struct plugin support" ON)
  option(ULOOP_SUPPORT "Uloop plugin support" ${DEFAULT_ULOOP_SUPPORT})
  option(LOG_SUPPORT "Log plugin support" ON)
  option(SOCKET_SUPPORT "Socket plugin support" ON)
  option(SERIAL_SUPPORT "Serial port plugin support" ON)
  option(ZLIB_SUPPORT "Zlib plugin support" ${DEFAULT_ZLIB_SUPPORT})
  option(DIGEST_SUPPORT "Digest plugin support" ${DEFAULT_DIGEST_SUPPORT})
  option(DIGEST_SUPPORT_EXTENDED "Enable additional hash algorithms" ${DEFAULT_DIGEST_SUPPORT})
else()
  # Modules built into ucode-modules.a; each is compiled with
  # uc_module_init renamed to uc_module_init_<name> so several can be
  # linked into one executable. Server-side socket APIs (bind/listen/
  # accept) are compiled in but not supported by emscripten.
  set(UCODE_WASM_MODULES "io;math;struct;fs;zlib;resolv;socket;digest;netaddr" CACHE STRING "Modules built into ucode-modules.a")
endif()

if(UCODE_WASM)
  # No dlopen under wasm: .so entries would only cause failed lookups.
  # Embedders (e.g. the pen service) extend REQUIRE_SEARCH_PATH at
  # runtime.
  set(LIB_SEARCH_PATH "./*.uc" CACHE STRING "Default library search path")
else()
  set(LIB_SEARCH_PATH "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/ucode/*.so:${CMAKE_INSTALL_PREFIX}/share/ucode/*.uc:./*.so:./*.uc" CACHE STRING "Default library search path")
endif()
string(REPLACE ":" "\", \"" LIB_SEARCH_DEFINE "${LIB_SEARCH_PATH}")
target_compile_definitions(uc_defines INTERFACE LIB_SEARCH_PATH="${LIB_SEARCH_DEFINE}")

if(APPLE)
  set(UCODE_MODULE_LINK_OPTIONS "LINKER:-undefined,dynamic_lookup")
  # TODO: localize target(s) and switch to target_compile_definitions()
  add_compile_definitions( BIND_8_COMPAT )
endif()

if(APPLE)
  set(DEFAULT_LINK_GC_SECTIONS OFF)
else()
  set(DEFAULT_LINK_GC_SECTIONS ON)
endif()
option(LINK_GC_SECTIONS "Link with --gc-sections" ${DEFAULT_LINK_GC_SECTIONS})
if(LINK_GC_SECTIONS)
  if(UNIT_TESTING)
    message(WARNING "Unit testing **MAY** conflict with --gc-sections")
  else()
    add_link_options("LINKER:--gc-sections")
  endif()
endif()

if(DEBUG)
  add_compile_definitions( DEBUG )
  add_compile_options(
    -Og
    -g3
  )
else()
  add_compile_definitions( NDEBUG )
endif()

if(UCODE_WASM)
  include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/emscripten-deps.cmake)
else()
  include(FindPkgConfig)
endif()

set(UCODE_SOURCES lexer.c lib.c vm.c vmcore.c chunk.c vallist.c compiler.c source.c types.c json.c program.c bytecode.c platform.c cli.c)
if(LLVM_SUPPORT)
  list(APPEND UCODE_SOURCES llvm.c native_trace.c)
endif()
if(UCODE_WASM)
  add_library(libucode STATIC ${UCODE_SOURCES})
  set_target_properties(libucode PROPERTIES OUTPUT_NAME ucode)
else()
  add_library(libucode SHARED ${UCODE_SOURCES})
  set(SOVERSION 0 CACHE STRING "Override ucode library version")
  set_target_properties(libucode PROPERTIES OUTPUT_NAME ucode SOVERSION ${SOVERSION})
endif()
target_link_libraries(libucode uc_defines)

if(NOT UCODE_WASM)
  set(CLI_SOURCES main.c)
  add_executable(ucode ${CLI_SOURCES})
  target_link_libraries(ucode uc_defines libucode)

  if(LLVM_SUPPORT)
    add_executable(ucode-trace trace.c)
    target_link_libraries(ucode-trace uc_defines libucode)
  endif()
endif()

if(LLVM_SUPPORT AND NOT UCODE_WASM)
  # Bindings that are linked against libucode load this library in its place,
  # see native_abi.c. Native programs name its install directory in their
  # RUNPATH. musl reuses a loaded library only if a search by name found it,
  # so a program cannot load it by an absolute path.
  add_library(ucode-native-abi SHARED native_abi.c)
  set_target_properties(ucode-native-abi PROPERTIES
    OUTPUT_NAME ucode SOVERSION ${SOVERSION}
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/native-abi)

  # The runtime of native programs, as LLVM bitcode: everything of libucode
  # except the interpreter and the compiler. The lexer only provides utf8enc()
  # there. A native program is linked with it at the IR level, so that LLVM
  # inlines the runtime into the generated code. The bitcode must match the
  # LLVM version of opt and llc.
  find_program(NATIVE_CLANG NAMES clang clang-21 clang-20 clang-19 clang-18 clang-17 clang-16)
  find_program(NATIVE_LLVM_LINK NAMES llvm-link llvm-link-21 llvm-link-20 llvm-link-19
                                      llvm-link-18 llvm-link-17 llvm-link-16)
  find_program(NATIVE_LLVM_DIS NAMES llvm-dis llvm-dis-21 llvm-dis-20 llvm-dis-19
                                     llvm-dis-18 llvm-dis-17 llvm-dis-16)
  find_program(NATIVE_LLVM_AS NAMES llvm-as llvm-as-21 llvm-as-20 llvm-as-19
                                    llvm-as-18 llvm-as-17 llvm-as-16)
  set(NATIVE_TARGET "" CACHE STRING "Target triple of the runtime bitcode, empty for the host")
  # The triple does not select the CPU or the float ABI of a target, such as
  # the soft float of OpenWrt mips_24kc. The runtime bitcode keeps no target
  # attributes (see cmake/native-strip.cmake), so a program build passes the
  # same flags to clang when it compiles the linked program bitcode.
  set(NATIVE_TARGET_FLAGS "" CACHE STRING
    "Target CPU and ABI flags of the runtime bitcode in clang syntax, e.g. -march=mips32r2 -msoft-float")

  if(NOT NATIVE_CLANG OR NOT NATIVE_LLVM_LINK OR NOT NATIVE_LLVM_DIS OR NOT NATIVE_LLVM_AS)
    message(FATAL_ERROR "LLVM_SUPPORT requires clang, llvm-link, llvm-dis and llvm-as")
  endif()

  set(native_bc_flags -O2 -std=gnu99 -fPIE -emit-llvm -c
    -DUC_NATIVE_ABI_SONAME="libucode.so.${SOVERSION}"
    "-D$<JOIN:$<TARGET_PROPERTY:libucode,COMPILE_DEFINITIONS>,$<SEMICOLON>-D>"
    "-I$<JOIN:$<TARGET_PROPERTY:libucode,INCLUDE_DIRECTORIES>,$<SEMICOLON>-I>")

  if(NATIVE_TARGET)
    list(APPEND native_bc_flags --target=${NATIVE_TARGET})
  endif()

  if(NATIVE_TARGET_FLAGS)
    separate_arguments(native_target_flags UNIX_COMMAND "${NATIVE_TARGET_FLAGS}")
    list(APPEND native_bc_flags ${native_target_flags})
  endif()

  if(CMAKE_SYSROOT)
    list(APPEND native_bc_flags --sysroot=${CMAKE_SYSROOT})
  endif()

  set(native_bc_files)

  # vm.c and bytecode.c run the scripts that a native program does not contain,
  # which the ucode CLI compiles when the program loads them
  foreach(src lib.c vmcore.c chunk.c vallist.c source.c types.c json.c program.c platform.c
              cli.c native.c native_insn.c lexer.c vm.c bytecode.c)
    get_filename_component(name ${src} NAME_WE)
    set(bc ${CMAKE_BINARY_DIR}/native-bc/${name}.bc)

    if(src STREQUAL "lexer.c")
      set(extra -DNO_COMPILE)
    else()
      set(extra)
    endif()

    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.20)
      set(deps DEPFILE ${bc}.d)
      set(depflags -MD -MF ${bc}.d)
    else()
      set(deps IMPLICIT_DEPENDS C ${CMAKE_CURRENT_SOURCE_DIR}/${src})
      set(depflags)
    endif()

    add_custom_command(
      OUTPUT ${bc}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/native-bc
      COMMAND ${NATIVE_CLANG} ${native_bc_flags} ${extra} ${depflags}
              ${CMAKE_CURRENT_SOURCE_DIR}/${src} -o ${bc}
      DEPENDS ${src}
      ${deps}
      COMMAND_EXPAND_LISTS
      VERBATIM)

    list(APPEND native_bc_files ${bc})
  endforeach()

  add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/libucode-native.bc ${CMAKE_BINARY_DIR}/native-bc/libucode-native.ll
    COMMAND ${NATIVE_LLVM_LINK} ${native_bc_files}
            -o ${CMAKE_BINARY_DIR}/native-bc/linked.bc
    COMMAND ${NATIVE_LLVM_DIS} ${CMAKE_BINARY_DIR}/native-bc/linked.bc
            -o ${CMAKE_BINARY_DIR}/native-bc/linked.ll
    COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_BINARY_DIR}/native-bc/linked.ll
            -DOUTPUT=${CMAKE_BINARY_DIR}/native-bc/libucode-native.ll
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/native-strip.cmake
    COMMAND ${NATIVE_LLVM_AS} ${CMAKE_BINARY_DIR}/native-bc/libucode-native.ll
            -o ${CMAKE_BINARY_DIR}/libucode-native.bc
    DEPENDS ${native_bc_files} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/native-strip.cmake
    VERBATIM)

  # the symbols to keep when the program is linked with opt's internalize
  # pass: -internalize-public-api-file=libucode-native.api
  add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/libucode-native.api
    COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_BINARY_DIR}/native-bc/libucode-native.ll
            -DOUTPUT=${CMAKE_BINARY_DIR}/libucode-native.api
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/native-api.cmake
    DEPENDS ${CMAKE_BINARY_DIR}/native-bc/libucode-native.ll
            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/native-api.cmake
    VERBATIM)

  add_custom_target(ucode-native ALL
    DEPENDS ${CMAKE_BINARY_DIR}/libucode-native.bc ${CMAKE_BINARY_DIR}/libucode-native.api)
endif()

# Ensure rpath is used on macOS
set(CMAKE_MACOSX_RPATH ON)

# Automatically include linked library paths in rpath
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

if(APPLE)
  set_target_properties(libucode PROPERTIES
    INSTALL_NAME_DIR "@rpath"
  )
  set_target_properties(ucode PROPERTIES
    INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}"
  )
endif()

if(APPLE AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  execute_process(
    COMMAND brew --prefix
    OUTPUT_VARIABLE HOMEBREW_PREFIX
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )

  if(HOMEBREW_PREFIX AND EXISTS "${HOMEBREW_PREFIX}")
    message(STATUS "Using Homebrew prefix: ${HOMEBREW_PREFIX}")
    set(CMAKE_INSTALL_PREFIX "${HOMEBREW_PREFIX}" CACHE PATH "Install path" FORCE)
  endif()
endif()

if(NOT UCODE_WASM)
  check_function_exists(dlopen DLOPEN_FUNCTION_EXISTS)
  if(NOT DLOPEN_FUNCTION_EXISTS)
    target_link_libraries(libucode dl)
  endif()

  check_function_exists(fmod FMOD_FUNCTION_EXISTS)
  if(NOT FMOD_FUNCTION_EXISTS)
    target_link_libraries(libucode m)
  endif()
endif()

if(NOT UCODE_WASM)
  set(LIBRARIES "")

  if(DEBUG_SUPPORT)
    set(LIBRARIES ${LIBRARIES} debug_lib)
    add_library(debug_lib MODULE lib/debug.c lib/debug_remote.c lib/debug_proto.c)
    set_target_properties(debug_lib PROPERTIES OUTPUT_NAME debug PREFIX "")
    target_link_options(debug_lib PRIVATE ${UCODE_MODULE_LINK_OPTIONS})
    target_link_libraries(debug_lib PRIVATE libucode)
    if(libubox)
      find_path(uloop_include_dir NAMES libubox/uloop.h)
      include_directories(${uloop_include_dir})
      target_link_libraries(debug_lib PRIVATE ${libubox} ${libucode})
      target_compile_definitions(debug_lib PRIVATE HAVE_ULOOP)
    endif()
  endif()

  if(FS_SUPPORT)
    set(LIBRARIES ${LIBRARIES} fs_lib)
    add_library(fs_lib MODULE lib/fs.c)
    set_target_properties(fs_lib PROPERTIES OUTPUT_NAME fs PREFIX "")
    target_link_options(fs_lib PRIVATE ${UCODE_MODULE_LINK_OPTIONS})
  endif()

  if(IO_SUPPORT)
    set(LIBRARIES ${LIBRARIES} io_lib)
    add_library(io_lib MODULE lib/io.c)
    set_target_properties(io_lib PROPERTIES OUTPUT_NAME io PREFIX "")
    target_link_options(io_lib PRIVATE ${UCODE_MODULE_LINK_OPTIONS})
  endif()

  if(MATH_SUPPORT)
    set(LIBRARIES ${LIBRARIES} math_lib)
    add_library(math_lib MODULE lib/math.c)
    set_target_properties(math_lib PROPERTIES OUTPUT_NAME math PREFIX "")
    target_link_options(math_lib PRIVATE ${UCODE_MODULE_LINK_OPTIONS})
    check_function_exists(ceil CEIL_FUNCTION_EXISTS)
    if(NOT CEIL_FUNCTION_EXISTS)
      target_link_libraries(math_lib m)
    endif()
  endif()

  if(UBUS_SUPPORT)
    find_path(ubus_include_dir NAMES libubus.h)
    include_directories(${ubus_include_dir})
    set(LIBRARIES ${LIBRARIES} ubus_lib)
    add_library(ubus_lib MODULE lib/ubus.c)
    set_target_properties(ubus_lib PROPERTIES OUTPUT_NAME ubus PREFIX "")
    target_link_options(ubus_lib PRIVATE ${UCODE_MODULE_LINK_OPTIONS})
    target_link_libraries(ubus_lib ${libubus})
    list(APPEND CMAKE_REQUIRED_LIBRARIES ${libubox} ${libubus})
    file(WRITE "${CMAKE_BINARY_DIR}${CMAKE_FILES_DIRECTORY}/CMakeTmp/test.c" "
      #include <libubus.h>
      int main() { return UBUS_STATUS_NO_MEMORY; }
    ")
    try_compile(HAVE_NEW_UBUS_STATUS_CODES
      ${CMAKE_BINARY_DIR}
      "${CMAKE_BINARY_DIR}${CMAKE_FILES_DIRECTORY}/CMakeTmp/test.c")
    check_function_exists(ubus_flush_requests HAVE_UBUS_FLUSH_REQUESTS)
    check_function_exists(uloop_timeout_remaining64 REMAINING64_FUNCTION_EXISTS)
    check_function_exists(ubus_channel_connect HAVE_CHANNEL_SUPPORT)
    file(WRITE "${CMAKE_BINARY_DIR}${CMAKE_FILES_DIRECTORY}/CMakeTmp/test2.c" "
      #include <libubus.h>
      int main() { struct ubus_subscriber sub = { .new_obj_cb = NULL }; return 0; }
    ")
    try_compile(HAVE_UBUS_NEW_OBJ_CB
      ${CMAKE_BINARY_DIR}
      "${CMAKE_BINARY_DIR}${CMAKE_FILES_DIRECTORY}/CMakeTmp/test2.c")
    if(REMAINING64_FUNCTION_EXISTS)
      target_compile_definitions(ubus_lib PRIVATE HAVE_ULOOP_TIMEOUT_REMAINING64)
    endif()
    if(HAVE_CHANNEL_SUPPORT)
      target_compile_definitions(ubus_lib PRIVATE HAVE_UBUS_CHANNEL_SUPPORT)
    endif()
    if(HAVE_NEW_UBUS_STATUS_CODES)
      target_compile_definitions(ubus_lib PRIVATE HAVE_NEW_UBUS_STATUS_CODES)
    endif()
    if(HAVE_UBUS_FLUSH_REQUESTS)
      target_compile_definitions(ubus_lib PRIVATE HAVE_UBUS_FLUSH_REQUESTS)
    endif()
    if (HAVE_UBUS_NEW_OBJ_CB)
      target_compile_definitions(ubus_lib PRIVATE HAVE_UBUS_NEW_OBJ_CB)
    endif()
  endif()

  if(UCI_SUPPORT)
    find_path(uci_include_dir uci.h)
    include_directories(${uci_include_dir})
    set(LIBRARIES ${LIBRARIES} uci_lib)
    add_library(uci_lib MODULE lib/uci.c)
    list(APPEND CMAKE_REQUIRED_LIBRARIES ${libubox} ${libuci})
    check_function_exists(uci_set_conf2dir HAVE_UCI_CONF2DIR)
    if (HAVE_UCI_CONF2DIR)
      target_compile_definitions(uci_lib PRIVATE HAVE_UCI_CONF2DIR)
    endif()
    set_target_properties(uci_lib PROPERTIES OUTPUT_NAME uci PREFIX "")
    target_link_options(uci_lib PRIVATE ${UCODE_MODULE_LINK_OPTIONS})
    target_link_libraries(uci_lib ${libuci} ${libubox})
  endif()

  if(RTNL_SUPPORT)
    find_path(nl_include_dir NAMES netlink/msg.h PATH_SUFFIXES libnl-tiny)
    include_directories(${nl_include_dir})
    set(LIBRARIES ${LIBRARIES} rtnl_lib)
    add_library(rtnl_lib MODULE lib/rtnl.c)
    set_target_properties(rtnl_lib PROPERTIES OUTPUT_NAME rtnl PREFIX "")
    target_link_options(rtnl_lib PRIVATE ${UCODE_MODULE_LINK_OPTIONS})
    target_link_libraries(rtnl_lib ${libnl_tiny} ${libubox})
  endif()

  if(NL80211_SUPPORT)
    find_path(nl_include_dir NAMES netlink/msg.h PATH_SUFFIXES libnl-tiny)
    include_directories(${nl_include_dir})
    set(LIBRARIES ${LIBRARIES} nl80211_lib)
    add_library(nl80211_lib MODULE lib/nl80211.c)
    set_target_properties(nl80211_lib PROPERTIES OUTPUT_NAME nl80211 PREFIX "")
    target_link_options(nl80211_lib PRIVATE ${UCODE_MODULE_LINK_OPTIONS})
    target_link_libraries(nl80211_lib ${libnl_tiny} ${libubox})
  endif()

  if(RESOLV_SUPPORT)
    set(LIBRARIES ${LIBRARIES} resolv_lib)
    add_library(resolv_lib MODULE lib/resolv.c)
    set_target_properties(resolv_lib PROPERTIES OUTPUT_NAME resolv PREFIX "")
    target_link_options(resolv_lib PRIVATE ${UCODE_MODULE_LINK_OPTIONS})
    check_function_exists(res_mkquery RES_MKQUERY_FUNCTION_EXISTS)
    check_function_exists(ns_initparse NS_INITARSE_FUNCTION_EXISTS)
    check_function_exists(clock_gettime CLOCK_GETTIME_FUNCTION_EXISTS)
    if(NOT RES_MKQUERY_FUNCTION_EXISTS OR NOT NS_INITARSE_FUNCTION_EXISTS)
      target_link_libraries(resolv_lib resolv)
    endif()
    if(NOT CLOCK_GETTIME_FUNCTION_EXISTS)
      target_link_libraries(resolv_lib rt)
    endif()
  endif()

  if(STRUCT_SUPPORT)
    set(LIBRARIES ${LIBRARIES} struct_lib)
    add_library(struct_lib MODULE lib/struct.c)
    set_target_properties(struct_lib PROPERTIES OUTPUT_NAME struct PREFIX "")
    target_link_options(struct_lib PRIVATE ${UCODE_MODULE_LINK_OPTIONS})
    check_function_exists(frexp FREXP_FUNCTION_EXISTS)
    if(NOT FREXP_FUNCTION_EXISTS)
      target_link_libraries(struct_lib m)
    endif()
  endif()

  if(ULOOP_SUPPORT)
    find_path(uloop_include_dir NAMES libubox/uloop.h)
    include_directories(${uloop_include_dir})
    set(LIBRARIES ${LIBRARIES} uloop_lib)
    add_library(uloop_lib MODULE lib/uloop.c)
    set_target_properties(uloop_lib PROPERTIES OUTPUT_NAME uloop PREFIX "")
    target_link_options(uloop_lib PRIVATE ${UCODE_MODULE_LINK_OPTIONS})
    list(APPEND CMAKE_REQUIRED_LIBRARIES ${libubox})
    check_function_exists(uloop_timeout_remaining64 REMAINING64_FUNCTION_EXISTS)
    check_function_exists(uloop_interval_set INTERVAL_FUNCTION_EXISTS)
    check_function_exists(uloop_signal_add SIGNAL_FUNCTION_EXISTS)
    if(REMAINING64_FUNCTION_EXISTS)
      target_compile_definitions(uloop_lib PRIVATE HAVE_ULOOP_TIMEOUT_REMAINING64)
    endif()
    if(INTERVAL_FUNCTION_EXISTS)
      target_compile_definitions(uloop_lib PRIVATE HAVE_ULOOP_INTERVAL)
    endif()
    if(SIGNAL_FUNCTION_EXISTS)
      target_compile_definitions(uloop_lib PRIVATE HAVE_ULOOP_SIGNAL)
    endif()
    target_link_libraries(uloop_lib ${libubox})
  endif()

  if(LOG_SUPPORT)
    set(LIBRARIES ${LIBRARIES} log_lib)
    add_library(log_lib MODULE lib/log.c)
    set_target_properties(log_lib PROPERTIES OUTPUT_NAME log PREFIX "")
    target_link_options(log_lib PRIVATE ${UCODE_MODULE_LINK_OPTIONS})
    if(libubox)
      find_path(ulog_include_dir NAMES libubox/ulog.h)
      include_directories(${ulog_include_dir})
      target_link_libraries(log_lib ${libubox})
      target_compile_definitions(log_lib PRIVATE HAVE_ULOG)
    endif()
  endif()

  if(SOCKET_SUPPORT)
    set(LIBRARIES ${LIBRARIES} socket_lib)
    add_library(socket_lib MODULE lib/socket.c)
    set_target_properties(socket_lib PROPERTIES OUTPUT_NAME socket PREFIX "")
    target_link_options(socket_lib PRIVATE ${UCODE_MODULE_LINK_OPTIONS})
  endif()

  if(SERIAL_SUPPORT)
    set(LIBRARIES ${LIBRARIES} serial_lib)
    add_library(serial_lib MODULE lib/serial.c)
    set_target_properties(serial_lib PROPERTIES OUTPUT_NAME serial PREFIX "")
    target_link_options(serial_lib PRIVATE ${UCODE_MODULE_LINK_OPTIONS})
  endif()

  if(ZLIB_SUPPORT)
    set(LIBRARIES ${LIBRARIES} zlib_lib)
    add_library(zlib_lib MODULE lib/zlib.c)
    set_target_properties(zlib_lib PROPERTIES OUTPUT_NAME zlib PREFIX "")
    target_link_options(zlib_lib PRIVATE ${UCODE_MODULE_LINK_OPTIONS})
    target_link_libraries(zlib_lib ZLIB::ZLIB)
    if(ZLIB_CHUNK_SIZE)
      target_compile_definitions(zlib_lib PRIVATE UC_ZLIB_CHUNK=${ZLIB_CHUNK_SIZE})
    endif()
  endif()

  if(DIGEST_SUPPORT)
    pkg_check_modules(LIBMD REQUIRED libmd)
    include_directories(${LIBMD_INCLUDE_DIRS})
    set(LIBRARIES ${LIBRARIES} digest_lib)
    add_library(digest_lib MODULE lib/digest.c)
    set_target_properties(digest_lib PROPERTIES OUTPUT_NAME digest PREFIX "")
    if(DIGEST_SUPPORT_EXTENDED)
      target_compile_definitions(digest_lib PRIVATE HAVE_DIGEST_EXTENDED)
    endif()
    target_link_options(digest_lib PRIVATE ${UCODE_MODULE_LINK_OPTIONS})
    target_link_libraries(digest_lib ${libmd})
  endif()

  if(FFI_SUPPORT)
    pkg_check_modules(LIBFFI REQUIRED libffi)
    include_directories(${LIBFFI_INCLUDE_DIRS})
    set(LIBRARIES ${LIBRARIES} ffi_lib)
    add_library(ffi_lib MODULE lib/ffi/uc_cparse.c lib/ffi/uc_ctype.c lib/ffi/uc_cconv.c lib/ffi/uc_cdata.c lib/ffi/ffi.c)
    set_target_properties(ffi_lib PROPERTIES OUTPUT_NAME ffi PREFIX "")

    set(CMAKE_REQUIRED_INCLUDES ${LIBFFI_INCLUDE_DIRS})
    set(CMAKE_REQUIRED_LIBRARIES ${LIBFFI_LIBRARIES})

    check_c_source_compiles("
      #include <ffi.h>
      int main() { return FFI_BAD_ARGTYPE; }
    " HAVE_FFI_BAD_ARGTYPE)

    if(HAVE_FFI_BAD_ARGTYPE)
      target_compile_definitions(ffi_lib PRIVATE HAVE_FFI_BAD_ARGTYPE)
    endif()

    # A backend provides the predefined complex number types only when it
    # defines FFI_TARGET_HAS_COMPLEX_TYPE, which powerpc does not. Only declare
    # the symbols, as the check may compile to a static library when cross
    # compiling, where referencing them would not be caught by the linker.
    check_c_source_compiles("
      #include <ffi.h>
      ffi_type *types[] = { &ffi_type_complex_float, &ffi_type_complex_double,
                            &ffi_type_complex_longdouble };
      int main() { return types != 0; }
    " HAVE_FFI_COMPLEX_TYPES)

    if(HAVE_FFI_COMPLEX_TYPES)
      target_compile_definitions(ffi_lib PRIVATE HAVE_FFI_COMPLEX_TYPES)
    endif()

    target_link_options(ffi_lib PRIVATE ${UCODE_MODULE_LINK_OPTIONS})
    target_link_libraries(ffi_lib ${LIBFFI_LINK_LIBRARIES})
  endif()

  if(NETADDR_SUPPORT)
    set(LIBRARIES ${LIBRARIES} netaddr_lib)
    add_library(netaddr_lib MODULE lib/netaddr.c)
    set_target_properties(netaddr_lib PROPERTIES OUTPUT_NAME netaddr PREFIX "")
    target_link_options(netaddr_lib PRIVATE ${UCODE_MODULE_LINK_OPTIONS})
  endif()
endif() # UCODE_WASM

if(UCODE_WASM)
  # ucode-modules.a: the selected modules with renamed uc_module_init
  # symbols, for static linking into an embedder's executable together
  # with libucode.a. The embedder calls uc_module_init_<name>() itself
  # (e.g. to preload the modules into the global `modules` dictionary)
  # and lists the names in config->force_dynlink_list.
  set(UCODE_MODULE_OBJECTS "")
  foreach(mod IN LISTS UCODE_WASM_MODULES)
    add_library(ucode_mod_${mod} OBJECT lib/${mod}.c)
    target_compile_definitions(ucode_mod_${mod} PRIVATE uc_module_init=uc_module_init_${mod})
    target_include_directories(ucode_mod_${mod} PRIVATE include)
    if(mod STREQUAL "zlib")
      target_compile_options(ucode_mod_${mod} PRIVATE -sUSE_ZLIB=1)
    endif()
    if(mod STREQUAL "socket")
      # emscripten's CMSG_NXTHDR() macro expands to a size_t/ptrdiff_t
      # comparison; not fixable from the call site
      target_compile_options(ucode_mod_${mod} PRIVATE -Wno-sign-compare)
    endif()
    if(mod STREQUAL "digest")
      target_include_directories(ucode_mod_${mod} PRIVATE ${UCODE_DEPS_PREFIX}/libmd/dist/include)
      add_dependencies(ucode_mod_${mod} libmd-build)
    endif()
    list(APPEND UCODE_MODULE_OBJECTS $<TARGET_OBJECTS:ucode_mod_${mod}>)
  endforeach()

  add_library(ucode-modules STATIC ${UCODE_MODULE_OBJECTS})
  set_target_properties(ucode-modules PROPERTIES PREFIX "")
  target_link_libraries(ucode-modules PUBLIC libucode)
  if("digest" IN_LIST UCODE_WASM_MODULES)
    target_link_libraries(ucode-modules PUBLIC libmd)
  endif()
  if("zlib" IN_LIST UCODE_WASM_MODULES)
    target_link_options(ucode-modules INTERFACE -sUSE_ZLIB=1)
  endif()
endif()

if(NOT UCODE_WASM)
  if(UNIT_TESTING)
    enable_testing()
    add_compile_definitions( UNIT_TESTING )
    add_subdirectory(tests)
    list(APPEND CMAKE_CTEST_ARGUMENTS "--output-on-failure")

    if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
      add_executable(ucode-san ${CLI_SOURCES} ${UCODE_SOURCES})
      set_property(TARGET ucode-san PROPERTY ENABLE_EXPORTS 1)
      target_link_libraries(ucode-san uc_defines)
      target_compile_options(ucode-san PRIVATE -g -fno-omit-frame-pointer -fsanitize=undefined,address,leak -fno-sanitize-recover=all)
      target_link_options(ucode-san PRIVATE -fsanitize=undefined,address,leak)
    endif()
  endif()
endif() # UCODE_WASM

if(NOT UCODE_WASM)
  add_executable(udbg udbg.c debug_highlight.c debug_lineedit.c)
  target_link_libraries(udbg PRIVATE libucode)
endif()

if(UCODE_WASM)
  install(TARGETS libucode ucode-modules ARCHIVE DESTINATION lib)
else()
  install(TARGETS ucode udbg RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

  if(LLVM_SUPPORT)
    install(TARGETS ucode-trace RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()

  install(TARGETS libucode LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(TARGETS ${LIBRARIES} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/ucode)

  if(LLVM_SUPPORT)
    install(TARGETS ucode-native-abi LIBRARY
      DESTINATION ${CMAKE_INSTALL_LIBDIR}/ucode/native NAMELINK_SKIP)
  endif()
endif()

if(NOT UCODE_WASM)
  add_custom_target(utpl ALL COMMAND ${CMAKE_COMMAND} -E create_symlink ucode utpl)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/utpl DESTINATION ${CMAKE_INSTALL_BINDIR})

  if(COMPILE_SUPPORT)
    add_custom_target(ucc ALL COMMAND ${CMAKE_COMMAND} -E create_symlink ucode ucc)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ucc DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()
endif()

file(GLOB UCODE_HEADERS "include/ucode/*.h")
install(FILES ${UCODE_HEADERS} DESTINATION include/ucode)

if(NOT UCODE_WASM)
  add_subdirectory(examples)
endif()
