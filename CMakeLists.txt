cmake_minimum_required(VERSION 3.15)
project(vdlisp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_LTO "Enable LTO and agressive optimizations" ON)

# ---- 编译模式 ----
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---- 全局编译选项 ----
add_compile_options(-fdiagnostics-color=always)
add_compile_options(-Wno-unused-parameter -Wno-deprecated-declarations)

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_options(-O0 -g -fno-omit-frame-pointer)
else()
  add_compile_options(-O3)
  if(ENABLE_LTO)
    add_compile_options(-flto -march=native)
  endif()
endif()

# ---- 库源文件（显式列出，避免 file(GLOB) 的增量构建陷阱） ----
set(VDLISP_LIB_SRC
    src/helpers.cpp
    src/nanbox.cpp
    src/state.cpp
)

# ---- 公共编译参数 ----
set(VDLISP_DEFINES
    _GNU_SOURCE EXPERIMENTAL_KEY_INSTRUCTIONS
    __STDC_CONSTANT_MACROS __STDC_FORMAT_MACROS __STDC_LIMIT_MACROS
)
set(VDLISP_INCLUDES
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

find_package(Boost 1.80 REQUIRED)
list(APPEND VDLISP_INCLUDES ${Boost_INCLUDE_DIRS})
message(STATUS "Boost ${Boost_VERSION} found")

# ---- 静态库 ----
add_library(vdlisp STATIC ${VDLISP_LIB_SRC})
target_compile_definitions(vdlisp PRIVATE ${VDLISP_DEFINES})
target_include_directories(vdlisp PUBLIC ${VDLISP_INCLUDES})

# ---- 动态库（需独立编译，因 VDLISP_BUILD_SHARED 改变符号可见性） ----
add_library(vdlisp_shared SHARED ${VDLISP_LIB_SRC})
target_compile_definitions(vdlisp_shared PRIVATE
    ${VDLISP_DEFINES} VDLISP_BUILD_SHARED)
target_include_directories(vdlisp_shared PUBLIC ${VDLISP_INCLUDES})
target_link_options(vdlisp_shared PRIVATE -Wl,--exclude-all-symbols)
set_target_properties(vdlisp_shared PROPERTIES
    BUILD_RPATH "${CMAKE_CURRENT_BINARY_DIR}")

# ---- 可执行文件 ----
add_executable(vdlisp_exe src/main.cpp)
target_link_libraries(vdlisp_exe PRIVATE vdlisp)
target_compile_definitions(vdlisp_exe PRIVATE ${VDLISP_DEFINES})
target_include_directories(vdlisp_exe PRIVATE ${VDLISP_INCLUDES})
set_target_properties(vdlisp_exe PROPERTIES OUTPUT_NAME vdlisp)

# 启动脚本部署
add_custom_command(TARGET vdlisp_exe POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:vdlisp_exe>/scripts"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/scripts/lang_basics.lisp"
          "$<TARGET_FILE_DIR:vdlisp_exe>/scripts/lang_basics.lisp"
)

# ---- 可选的 readline 支持 ----
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(READLINE IMPORTED_TARGET readline)
  if(READLINE_FOUND)
    target_link_libraries(vdlisp_exe PRIVATE PkgConfig::READLINE)
  else()
    message(WARNING "readline not found (pkg-config); REPL history disabled")
  endif()
else()
  find_library(READLINE_LIB NAMES readline)
  if(READLINE_LIB)
    target_link_libraries(vdlisp_exe PRIVATE ${READLINE_LIB})
  else()
    message(WARNING "readline not found; REPL history disabled")
  endif()
endif()

message(STATUS "Configured vdlisp (ENABLE_LTO=${ENABLE_LTO})")
