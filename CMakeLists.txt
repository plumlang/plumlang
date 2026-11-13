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

# Everything but the command-line front end is a static library, which the
# plum binary and the unit tests both link.
file(GLOB_RECURSE PLUM_LIB_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.c)
list(FILTER PLUM_LIB_SOURCES EXCLUDE REGEX "/src/cli/")
file(GLOB_RECURSE PLUM_CLI_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/cli/*.c)
file(GLOB_RECURSE PLUM_UNIT_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/unit/*.c)

add_library(plumcore STATIC ${PLUM_LIB_SOURCES})
if(NOT MSVC)
  target_link_libraries(plumcore PUBLIC m) # pow, fmod; part of the C runtime on MSVC
endif()
add_executable(plum ${PLUM_CLI_SOURCES})
add_executable(plum_unit_tests ${PLUM_UNIT_SOURCES})
target_link_libraries(plum PRIVATE plumcore)
target_link_libraries(plum_unit_tests PRIVATE plumcore)

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

foreach(target plumcore plum plum_unit_tests)
  target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    # fopen, strerror: the portable C11 functions are what we want.
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Werror -Wshadow -Wstrict-prototypes
      -Wmissing-prototypes -Wvla -Wformat=2 -Wundef)
    if(PLUM_SANITIZE)
      target_compile_options(${target} PRIVATE
        -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
endforeach()

enable_testing()
add_test(NAME unit COMMAND plum_unit_tests)
find_program(PLUM_SH sh)
if(PLUM_SH)
  add_test(NAME smoke COMMAND ${PLUM_SH} ${CMAKE_SOURCE_DIR}/tests/smoke.sh $<TARGET_FILE:plum>)
  add_test(NAME harness COMMAND ${PLUM_SH} tests/harness/self-test.sh
           WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  add_test(NAME errors-documented COMMAND ${PLUM_SH} tests/errors-documented.sh
           WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  add_test(NAME bytecode-documented COMMAND ${PLUM_SH} tests/bytecode-documented.sh
           WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  add_test(NAME golden COMMAND ${PLUM_SH} tests/golden.sh $<TARGET_FILE:plum>
           WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  add_test(NAME stdlib-documented COMMAND ${PLUM_SH} tools/stdlib-docs.sh $<TARGET_FILE:plum> --check
           WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  add_test(NAME examples COMMAND ${PLUM_SH} tests/golden.sh $<TARGET_FILE:plum> examples
           WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
