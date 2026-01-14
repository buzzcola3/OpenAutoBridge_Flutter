# The Flutter tooling requires that developers have CMake 3.10 or later
# installed. You should not increase this version, as doing so will cause
# the plugin to fail to compile for some customers of the plugin.
cmake_minimum_required(VERSION 3.10)

# Project-level configuration.
set(PROJECT_NAME "openautoflutter")
project(${PROJECT_NAME} LANGUAGES CXX)

# Use libc++ to satisfy OpenAutoTransport prebuilt ABI (release binaries are built with clang/libc++)
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "Prebuilt OpenAutoTransport targets require clang/libc++; rerun with CC=clang and CXX=clang++")
endif()
add_compile_options(-stdlib=libc++)
add_link_options(-stdlib=libc++)

# This value is used when generating builds using this plugin, so it must
# not be changed.
set(PLUGIN_NAME "openautoflutter_plugin")

# Any new source files that you add to the plugin should be added here.
list(APPEND PLUGIN_SOURCES
  "openautoflutter_plugin.cc"
  "oat_message_handlers.cpp"
  "av/oa_video_texture.cc"
  "common/SharedMemoryConsumer.cpp"
  "av/h264_decoder.cc"
)

# Define the plugin library target. Its name must not be changed (see comment
# on PLUGIN_NAME above).
add_library(${PLUGIN_NAME} SHARED
  ${PLUGIN_SOURCES}
)

# Apply a standard set of build settings that are configured in the
# application-level CMakeLists.txt. This can be removed for plugins that want
# full control over build settings.
apply_standard_settings(${PLUGIN_NAME})

# Symbols are hidden by default to reduce the chance of accidental conflicts
# between plugins. This should not be removed; any symbols that should be
# exported should be explicitly exported with the FLUTTER_PLUGIN_EXPORT macro.
set_target_properties(${PLUGIN_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden)
target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)

# Source include directories and library dependencies. Add any plugin-specific
# dependencies here.
target_include_directories(${PLUGIN_NAME} INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/av"
  "${CMAKE_CURRENT_SOURCE_DIR}/common"
)
target_link_libraries(${PLUGIN_NAME} PRIVATE flutter)
target_link_libraries(${PLUGIN_NAME} PRIVATE PkgConfig::GTK)
target_link_libraries(${PLUGIN_NAME} PRIVATE epoxy)

# Threads (for sem_* and timing safety on some platforms)
find_package(Threads REQUIRED)
target_link_libraries(${PLUGIN_NAME} PRIVATE Threads::Threads)

## OpenAutoTransport (prebuilt) fetch + link (v0.0.22)
set(OPENAUTO_TRANSPORT_VERSION "v0.0.22" CACHE STRING "OpenAutoTransport release tag" FORCE)
if(NOT DEFINED OPENAUTO_TRANSPORT_VARIANT)
  string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _proc)
  if(_proc MATCHES "x86_64|amd64")
    set(OPENAUTO_TRANSPORT_VARIANT "amd64_gnu")
  elseif(_proc MATCHES "aarch64|arm64")
    set(OPENAUTO_TRANSPORT_VARIANT "arm64_gnu")
  else()
    message(FATAL_ERROR "Unsupported processor ${CMAKE_SYSTEM_PROCESSOR}; set OPENAUTO_TRANSPORT_VARIANT manually (amd64_gnu|amd64_musl|arm64_gnu|arm64_musl)")
  endif()
endif()

set(_oat_base_url "https://github.com/buzzcola3/OpenAutoTransport/releases/download/${OPENAUTO_TRANSPORT_VERSION}")
set(_oat_prefix "${CMAKE_BINARY_DIR}/openautotransport/${OPENAUTO_TRANSPORT_VERSION}/prebuilt")
file(MAKE_DIRECTORY "${_oat_prefix}/include" "${_oat_prefix}/lib")

function(_oat_fetch _name _dest_dir _out_var)
  set(_url "${_oat_base_url}/${_name}")
  set(_dest "${_dest_dir}/${_name}")
  set(_needs_fetch TRUE)
  if(EXISTS "${_dest}")
    file(SIZE "${_dest}" _size)
    if(_size GREATER 0)
      set(_needs_fetch FALSE)
    else()
      file(REMOVE "${_dest}")
    endif()
  endif()
  if(_needs_fetch)
    file(DOWNLOAD "${_url}" "${_dest}" SHOW_PROGRESS STATUS _status)
    list(GET _status 0 _code)
    if(NOT _code EQUAL 0)
      message(FATAL_ERROR "Failed to download ${_url}: ${_status}")
    endif()
    file(SIZE "${_dest}" _size_after)
    if(NOT _size_after GREATER 0)
      message(FATAL_ERROR "Downloaded ${_url} but file is empty")
    endif()
  endif()
  set(${_out_var} "${_dest}" PARENT_SCOPE)
endfunction()

# Headers needed at build time
_oat_fetch("wire.hpp" "${_oat_prefix}/include" _oat_wire_hpp)
_oat_fetch("transport.hpp" "${_oat_prefix}/include" _oat_transport_hpp)
_oat_fetch("wire.capnp.h" "${_oat_prefix}/include" _oat_wire_capnp_h)
_oat_fetch("wire.capnp.c++" "${_oat_prefix}/include" _oat_wire_capnp_cpp) 

# Static libs for the selected variant
_oat_fetch("libopen_auto_transport-${OPENAUTO_TRANSPORT_VARIANT}.a" "${_oat_prefix}/lib" _oat_lib_path)
_oat_fetch("libcapnp-${OPENAUTO_TRANSPORT_VARIANT}.a" "${_oat_prefix}/lib" _oat_capnp_path)
_oat_fetch("libkj-${OPENAUTO_TRANSPORT_VARIANT}.a" "${_oat_prefix}/lib" _oat_kj_path)

add_library(open_auto_transport STATIC IMPORTED)
set_target_properties(open_auto_transport PROPERTIES IMPORTED_LOCATION "${_oat_lib_path}")
add_library(open_auto_transport_capnp STATIC IMPORTED)
set_target_properties(open_auto_transport_capnp PROPERTIES IMPORTED_LOCATION "${_oat_capnp_path}")
add_library(open_auto_transport_kj STATIC IMPORTED)
set_target_properties(open_auto_transport_kj PROPERTIES IMPORTED_LOCATION "${_oat_kj_path}")

target_include_directories(${PLUGIN_NAME} PRIVATE "${_oat_prefix}/include")
target_sources(${PLUGIN_NAME} PRIVATE "${_oat_wire_capnp_cpp}")
target_link_libraries(${PLUGIN_NAME} PRIVATE open_auto_transport open_auto_transport_capnp open_auto_transport_kj)
target_link_libraries(${PLUGIN_NAME} PRIVATE ${CMAKE_DL_LIBS})
target_link_libraries(${PLUGIN_NAME} PRIVATE c++ c++abi)

# FFmpeg (libav) for software H264 decoding
find_library(AVCODEC_LIB avcodec)
find_library(AVUTIL_LIB avutil)
find_library(SWSCALE_LIB swscale)
if (AVCODEC_LIB AND AVUTIL_LIB AND SWSCALE_LIB)
  target_link_libraries(${PLUGIN_NAME} PRIVATE ${AVCODEC_LIB} ${AVUTIL_LIB} ${SWSCALE_LIB})
endif()

# List of absolute paths to libraries that should be bundled with the plugin.
# This list could contain prebuilt libraries, or libraries created by an
# external build triggered from this build file.
set(openautoflutter_bundled_libraries
  ""
  PARENT_SCOPE
)

# === Tests ===
# These unit tests can be run from a terminal after building the example.

# Only enable test builds when building the example (which sets this variable)
# so that plugin clients aren't building the tests.
if (${include_${PROJECT_NAME}_tests})
if(${CMAKE_VERSION} VERSION_LESS "3.11.0")
message("Unit tests require CMake 3.11.0 or later")
else()
set(TEST_RUNNER "${PROJECT_NAME}_test")
enable_testing()

# Add the Google Test dependency.
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/release-1.11.0.zip
)
# Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
# Disable install commands for gtest so it doesn't end up in the bundle.
set(INSTALL_GTEST OFF CACHE BOOL "Disable installation of googletest" FORCE)

FetchContent_MakeAvailable(googletest)

# The plugin's exported API is not very useful for unit testing, so build the
# sources directly into the test binary rather than using the shared library.
add_executable(${TEST_RUNNER}
  test/openautoflutter_plugin_test.cc
  ${PLUGIN_SOURCES}
)
apply_standard_settings(${TEST_RUNNER})
target_include_directories(${TEST_RUNNER} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(${TEST_RUNNER} PRIVATE flutter)
target_link_libraries(${TEST_RUNNER} PRIVATE PkgConfig::GTK)
target_link_libraries(${TEST_RUNNER} PRIVATE epoxy)
target_link_libraries(${TEST_RUNNER} PRIVATE gtest_main gmock)
target_link_libraries(${TEST_RUNNER} PRIVATE Threads::Threads)
target_include_directories(${TEST_RUNNER} PRIVATE "${_oat_prefix}/include")
target_sources(${TEST_RUNNER} PRIVATE "${_oat_wire_capnp_cpp}")
target_link_libraries(${TEST_RUNNER} PRIVATE open_auto_transport open_auto_transport_capnp open_auto_transport_kj ${CMAKE_DL_LIBS})
target_link_libraries(${TEST_RUNNER} PRIVATE c++ c++abi)
if (AVCODEC_LIB AND AVUTIL_LIB AND SWSCALE_LIB)
  target_link_libraries(${TEST_RUNNER} PRIVATE ${AVCODEC_LIB} ${AVUTIL_LIB} ${SWSCALE_LIB})
endif()

# Enable automatic test discovery.
include(GoogleTest)
gtest_discover_tests(${TEST_RUNNER})

endif()  # CMake version check
endif()  # include_${PROJECT_NAME}_tests