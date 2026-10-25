cmake_minimum_required(VERSION 3.10)

project(spz
  DESCRIPTION "A 3D Gaussians format"
  LANGUAGES C CXX
  VERSION 1.1.0)

# zlib is required to build the project
#find_package(ZLIB REQUIRED)

set(spz_sources
  "${CMAKE_CURRENT_SOURCE_DIR}/src/cc/load-spz.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/cc/splat-c-types.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/cc/splat-types.cc"
)

set(spz_headers
  "${CMAKE_CURRENT_SOURCE_DIR}/src/cc/load-spz.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/cc/splat-c-types.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/cc/splat-types.h"
)

# create the library and configure it
add_library(spz STATIC ${spz_sources} ${spz_headers} )
add_library(spz::spz ALIAS spz)

# The upgraded spz (SPZ header version 4+) also needs zstd for compression.
# nvpro_core2's vendored zstd was removed in its latest slim-down, so fetch our
# own copy when no libzstd_static target exists (the collision the old comment
# warned about is gone with the vendored copy).
if(NOT TARGET libzstd_static)
  include(FetchContent)
  set(ZSTD_BUILD_SHARED OFF)
  set(ZSTD_BUILD_PROGRAMS OFF)
  set(ZSTD_BUILD_TESTS OFF)
  set(ZSTD_LEGACY_SUPPORT OFF)
  FetchContent_Declare(zstd
    URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
    SOURCE_SUBDIR build/cmake)  # zstd's CMake entry lives here, not at the archive root
  FetchContent_MakeAvailable(zstd)
endif()
target_link_libraries(spz PRIVATE zlib libzstd_static)

target_include_directories(spz
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/cc>
  INTERFACE $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

set_target_properties(spz PROPERTIES
  PUBLIC_HEADER "${spz_headers}"
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
)
