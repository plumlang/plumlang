# SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0
#
# CMake build, for MSVC and IDEs. The Makefile is the primary build on
# Linux and macOS; both build the same sources with the same warnings.
#
#   cmake -B build && cmake --build build --config Release
#   cmake -B build -DPLUM_SANITIZE=ON       (GCC/Clang: ASan + UBSan)
#   ctest --test-dir build -C Release

cmake_minimum_required(VERSION 3.16)
project(plumlang VERSION 0.0.1 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(PLUM_SANITIZE "Build with AddressSanitizer and UBSan (GCC/Clang)" OFF)

file(GLOB_RECURSE PLUM_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.c)
add_executable(plum ${PLUM_SOURCES})
target_include_directories(plum PRIVATE ${CMAKE_SOURCE_DIR}/include)

find_package(Git QUIET)
set(PLUM_COMMIT "unknown")
if(GIT_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE PLUM_COMMIT_OUT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE PLUM_GIT_RESULT
    ERROR_QUIET)
  if(PLUM_GIT_RESULT EQUAL 0)
    set(PLUM_COMMIT ${PLUM_COMMIT_OUT})
  endif()
endif()
target_compile_definitions(plum PRIVATE PLUM_COMMIT="${PLUM_COMMIT}")

if(MSVC)
  target_compile_options(plum PRIVATE /W4 /WX /utf-8)
  # fopen, strerror: the portable C11 functions are what we want.
  target_compile_definitions(plum PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(plum PRIVATE
    -Wall -Wextra -Wpedantic -Werror -Wshadow -Wstrict-prototypes
    -Wmissing-prototypes -Wvla -Wformat=2 -Wundef)
  if(PLUM_SANITIZE)
    target_compile_options(plum PRIVATE
      -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(plum PRIVATE -fsanitize=address,undefined)
  endif()
endif()

enable_testing()
find_program(PLUM_SH sh)
if(PLUM_SH)
  add_test(NAME smoke COMMAND ${PLUM_SH} ${CMAKE_SOURCE_DIR}/tests/smoke.sh $<TARGET_FILE:plum>)
endif()
