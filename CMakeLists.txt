cmake_minimum_required(VERSION 3.13)

project(ucode C)

add_library(uc_defines INTERFACE)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CheckFunctionExists)
include(CheckSymbolExists)
include(CheckCSourceCompiles)
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

include_directories(include)

option(COMPILE_SUPPORT "Support compilation from source" ON)
if(NOT COMPILE_SUPPORT)
  target_compile_definitions(uc_defines INTERFACE NO_COMPILE)
endif()

find_library(libuci NAMES uci)
find_library(libubox NAMES ubox)
find_library(libubus NAMES ubus)
find_library(libblobmsg_json NAMES blobmsg_json)
find_library(wslay NAMES wslay)
find_path(wslay_include NAMES wslay/wslay.h)
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

if(libubus AND libblobmsg_json)
  set(DEFAULT_UBUS_SUPPORT ON)
endif()

if(libubox)
  set(DEFAULT_ULOOP_SUPPORT ON)
endif()

if(wslay AND wslay_include AND libubox)
  set(DEFAULT_WEBSOCKET_SUPPORT ON)
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

option(DEBUG_SUPPORT "Debug plugin support" ON)
option(FS_SUPPORT "Filesystem plugin support" ON)
option(IO_SUPPORT "IO plugin support" ON)
option(MATH_SUPPORT "Math plugin support" ON)
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
option(WEBSOCKET_SUPPORT "WebSocket plugin support" ${DEFAULT_WEBSOCKET_SUPPORT})
option(SERIAL_SUPPORT "Serial port plugin support" ON)
option(ZLIB_SUPPORT "Zlib plugin support" ${DEFAULT_ZLIB_SUPPORT})
option(DIGEST_SUPPORT "Digest plugin support" ${DEFAULT_DIGEST_SUPPORT})
option(DIGEST_SUPPORT_EXTENDED "Enable additional hash algorithms" ${DEFAULT_DIGEST_SUPPORT})

set(LIB_SEARCH_PATH "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/ucode/*.so:${CMAKE_INSTALL_PREFIX}/share/ucode/*.uc:./*.so:./*.uc" CACHE STRING "Default library search path")
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

include(FindPkgConfig)
pkg_check_modules(JSONC REQUIRED json-c)
include_directories(${JSONC_INCLUDE_DIRS})

set(UCODE_SOURCES lexer.c lib.c vm.c chunk.c vallist.c compiler.c source.c types.c program.c platform.c)
add_library(libucode SHARED ${UCODE_SOURCES})
set(SOVERSION 0 CACHE STRING "Override ucode library version")
set_target_properties(libucode PROPERTIES OUTPUT_NAME ucode SOVERSION ${SOVERSION})
target_link_libraries(libucode uc_defines ${JSONC_LINK_LIBRARIES})

set(CLI_SOURCES main.c)
add_executable(ucode ${CLI_SOURCES})
target_link_libraries(ucode uc_defines libucode)

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

check_function_exists(dlopen DLOPEN_FUNCTION_EXISTS)
if(NOT DLOPEN_FUNCTION_EXISTS)
  target_link_libraries(libucode dl)
endif()

check_function_exists(fmod FMOD_FUNCTION_EXISTS)
if(NOT FMOD_FUNCTION_EXISTS)
  target_link_libraries(libucode m)
endif()

list(APPEND CMAKE_REQUIRED_INCLUDES ${JSONC_INCLUDE_DIRS})
list(APPEND CMAKE_REQUIRED_LIBRARIES ${JSONC_LINK_LIBRARIES})
check_symbol_exists(json_tokener_get_parse_end "json-c/json.h" HAVE_PARSE_END)
if(HAVE_PARSE_END)
  target_compile_definitions(uc_defines INTERFACE HAVE_PARSE_END)
endif()
check_symbol_exists(json_object_new_array_ext "json-c/json.h" HAVE_ARRAY_EXT)
if(HAVE_ARRAY_EXT)
  target_compile_definitions(uc_defines INTERFACE HAVE_ARRAY_EXT)
endif()
check_symbol_exists(json_object_new_uint64 "json-c/json.h" HAVE_JSON_UINT64)
if(HAVE_JSON_UINT64)
  target_compile_definitions(uc_defines INTERFACE HAVE_JSON_UINT64)
endif()

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
  target_link_libraries(ubus_lib ${libubus} ${libblobmsg_json})
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

if(WEBSOCKET_SUPPORT)
  set(LIBRARIES ${LIBRARIES} websocket_lib)
  add_library(websocket_lib MODULE lib/websocket.c)
  set_target_properties(websocket_lib PROPERTIES OUTPUT_NAME websocket PREFIX "")
  target_include_directories(websocket_lib PRIVATE ${wslay_include})
  target_link_options(websocket_lib PRIVATE ${UCODE_MODULE_LINK_OPTIONS})
  target_link_libraries(websocket_lib ${wslay} ${libubox})
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

  target_link_options(ffi_lib PRIVATE ${UCODE_MODULE_LINK_OPTIONS})
  target_link_libraries(ffi_lib ${LIBFFI_LINK_LIBRARIES})
endif()

if(UNIT_TESTING)
  enable_testing()
  add_compile_definitions( UNIT_TESTING )
  add_subdirectory(tests)
  list(APPEND CMAKE_CTEST_ARGUMENTS "--output-on-failure")

  if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    add_executable(ucode-san ${CLI_SOURCES} ${UCODE_SOURCES})
    set_property(TARGET ucode-san PROPERTY ENABLE_EXPORTS 1)
    target_link_libraries(ucode-san uc_defines ${JSONC_LINK_LIBRARIES})
    target_compile_options(ucode-san PRIVATE -g -fno-omit-frame-pointer -fsanitize=undefined,address,leak -fno-sanitize-recover=all)
    target_link_options(ucode-san PRIVATE -fsanitize=undefined,address,leak)
  endif()
endif()

add_executable(udbg udbg.c debug_highlight.c debug_lineedit.c)
target_link_libraries(udbg PRIVATE libucode ${JSONC_LINK_LIBRARIES})
install(TARGETS ucode udbg RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS libucode LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS ${LIBRARIES} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/ucode)

add_custom_target(utpl ALL COMMAND ${CMAKE_COMMAND} -E create_symlink ucode utpl)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/utpl DESTINATION ${CMAKE_INSTALL_BINDIR})

if(COMPILE_SUPPORT)
  add_custom_target(ucc ALL COMMAND ${CMAKE_COMMAND} -E create_symlink ucode ucc)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ucc DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

file(GLOB UCODE_HEADERS "include/ucode/*.h")
install(FILES ${UCODE_HEADERS} DESTINATION include/ucode)

add_subdirectory(examples)
