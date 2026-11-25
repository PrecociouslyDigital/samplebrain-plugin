cmake_minimum_required(VERSION 3.25)
project(samplebrain_plugin VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Nix's compiler wrapper finds libc++ and the SDK through environment
# variables. Spelling its include dirs out lets clangd find them too.
set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES})
set(CMAKE_OSX_ARCHITECTURES arm64)
# Nix builds its libraries for 14.0
set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0)

option(SB_BUILD_TESTS "Build the engine tests" ON)
option(SB_BUILD_BENCH "Build the engine benchmark" ON)
option(SB_BUILD_PLUGIN "Build the plugin" ON)
option(SB_RTSAN "Check the audio callback with RealtimeSanitizer" OFF)
set(SB_SANITIZE "" CACHE STRING "Sanitizer for the engine and tests: address or thread")

set(SAMPLEBRAIN_SRC "$ENV{SAMPLEBRAIN_SRC}" CACHE PATH "Patched upstream samplebrain source")
if(NOT EXISTS "${SAMPLEBRAIN_SRC}/brain/src/brain.h")
  message(FATAL_ERROR "SAMPLEBRAIN_SRC must point at patched samplebrain source; run inside `nix develop`")
endif()

if(SB_SANITIZE)
  add_compile_options(-fsanitize=${SB_SANITIZE} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${SB_SANITIZE})
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(fftw3 REQUIRED IMPORTED_TARGET fftw3)
pkg_check_modules(sndfile REQUIRED IMPORTED_TARGET sndfile)
pkg_check_modules(samplerate REQUIRED IMPORTED_TARGET samplerate)
pkg_check_modules(liblo REQUIRED IMPORTED_TARGET liblo)
pkg_check_modules(xxhash REQUIRED IMPORTED_TARGET libxxhash)

add_subdirectory(engine)

if(SB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(SB_BUILD_BENCH)
  add_subdirectory(bench)
endif()

if(SB_BUILD_PLUGIN)
  add_subdirectory(plugin)
endif()
