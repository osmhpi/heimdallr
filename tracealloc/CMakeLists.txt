cmake_minimum_required(VERSION 3.0)
project(tracealloc C CXX)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/")

find_package(Memkind REQUIRED)
# find_package(Pthreads REQUIRED)
# find_package(Unwind REQUIRED)

set(target tracealloc)

set(sources
  src/interposer.cpp
  src/handler.cpp
  src/common.cpp
  src/mappings.cpp
)

add_library(${target}
  SHARED
  ${sources}
)

target_link_options(${target}
  PRIVATE
  -static
)

target_include_directories(${target}
  PRIVATE
  ${DEFAULT_INCLUDE_DIRECTORIES}
  SYSTEM
  ${MEMKIND_INCLUDE_DIRS}
#  ${UNWIND_INCLUDE_DIRS}
)

target_link_libraries(${target}
  PRIVATE
  ${DEFAULT_LIBRARIES}
  PUBLIC
  pthread
  dl
  ${MEMKIND_LIBRARIES}
#  ${UNWIND_LIBRARIES}
)

add_executable(alloctest
  test/alloctest.c
)
