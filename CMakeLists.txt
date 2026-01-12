cmake_minimum_required(VERSION 3.15)
project(vdlisp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_LTO "Enable LTO and agressive optimizations (disable for faster dev builds)" ON)

# Choose compilation flags based on build type and LTO option.
# - Debug: no optimizations, include debug info and frame pointers for easier debugging/profiling
# - Release: enable optimizations; if ENABLE_LTO is set, apply aggressive LTO flags
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_options(-O0 -g -fno-omit-frame-pointer -fdiagnostics-color=always)
else()
  if(ENABLE_LTO)
    add_compile_options(-O3 -march=native -flto -fdiagnostics-color=always)
  else()
    add_compile_options(-O3 -fdiagnostics-color=always)
  endif()
endif()

add_compile_definitions(_GNU_SOURCE EXPERIMENTAL_KEY_INSTRUCTIONS __STDC_CONSTANT_MACROS __STDC_FORMAT_MACROS __STDC_LIMIT_MACROS)
add_compile_options(-Wno-unused-parameter -Wno-deprecated-declarations)

include_directories(${CMAKE_SOURCE_DIR}/src)

# Prefer using llvm-config when available for portable flags
find_program(LLVM_CONFIG NAMES llvm-config llvm-config-21)
if(LLVM_CONFIG)
  message(STATUS "Found llvm-config: ${LLVM_CONFIG}")
  execute_process(COMMAND ${LLVM_CONFIG} --cxxflags OUTPUT_VARIABLE LLVM_CXXFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND ${LLVM_CONFIG} --ldflags OUTPUT_VARIABLE LLVM_LDFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND ${LLVM_CONFIG} --libs OUTPUT_VARIABLE LLVM_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND ${LLVM_CONFIG} --includedir OUTPUT_VARIABLE LLVM_INCLUDEDIR OUTPUT_STRIP_TRAILING_WHITESPACE)
  string(REPLACE " " ";" LLVM_CXXFLAGS_LIST "${LLVM_CXXFLAGS}")
  string(REPLACE " " ";" LLVM_LDFLAGS_LIST "${LLVM_LDFLAGS}")
  string(REPLACE " " ";" LLVM_LIBS_LIST "${LLVM_LIBS}")
  # apply compile flags from llvm-config
  foreach(f IN LISTS LLVM_CXXFLAGS_LIST)
    add_compile_options(${f})
  endforeach()
  foreach(lf IN LISTS LLVM_LDFLAGS_LIST)
    add_link_options(${lf})
  endforeach()
  include_directories(${LLVM_INCLUDEDIR})
  set(USE_LLVM ON)
else()
  message(WARNING "llvm-config not found; falling back to common locations")
  find_path(LLVM_INCLUDE_DIR NAMES llvm/IR/IRBuilder.h PATHS /usr/include /usr/local/include /usr/lib64/llvm21/include)
  find_library(LLVM_LIB NAMES LLVM-21 LLVM PATHS /usr/lib64 /usr/lib /usr/local/lib)
  if(LLVM_INCLUDE_DIR AND LLVM_LIB)
    message(STATUS "Found LLVM includes: ${LLVM_INCLUDE_DIR}")
    include_directories(${LLVM_INCLUDE_DIR})
    set(USE_LLVM ON)
  else()
    message(WARNING "LLVM not found in common locations; JIT will be disabled unless you set LLVM paths.")
    set(USE_LLVM OFF)
  endif()
endif()

file(GLOB VDL_SOURCES
  ${CMAKE_SOURCE_DIR}/src/*.cpp
  ${CMAKE_SOURCE_DIR}/src/jit/*.cpp
)

add_executable(vdlisp ${VDL_SOURCES})
# Force C++20 even if external flags add a different -std=...
target_compile_options(vdlisp PRIVATE -std=c++20)

if(USE_LLVM)
  if(DEFINED LLVM_LIBS_LIST)
    target_link_libraries(vdlisp PRIVATE ${LLVM_LIBS_LIST})
  else()
    target_link_libraries(vdlisp PRIVATE ${LLVM_LIB})
  endif()
  target_compile_definitions(vdlisp PRIVATE HAVE_LLVM=1)
  message(STATUS "vdlisp will link against LLVM (JIT enabled)")
else()
  message(STATUS "Building without LLVM support; JIT disabled")
endif()

find_library(READLINE_LIB NAMES readline)
if(READLINE_LIB)
  target_link_libraries(vdlisp PRIVATE ${READLINE_LIB})
else()
  message(WARNING "readline not found; build may fail if code requires it")
endif()

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

message(STATUS "Configured vdlisp (ENABLE_LTO=${ENABLE_LTO})")
