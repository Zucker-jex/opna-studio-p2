cmake_minimum_required(VERSION 3.13)
project(opna_studio_host C)

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(studio_render
  studio_render.c
  ${ROOT}/src/opna/opna.c
  ${ROOT}/src/opna/opna_fm.c
  ${ROOT}/src/opna/opna_ssg.c
  ${ROOT}/src/opna/opna_drum.c
  ${ROOT}/firmware/studio/engine.c
  ${ROOT}/firmware/studio/patches.c
  ${ROOT}/firmware/studio/fft.c
)
target_include_directories(studio_render PRIVATE
  ${ROOT}/include ${ROOT}/src/opna ${ROOT}/firmware/studio)
target_compile_options(studio_render PRIVATE -O2 -Wall -Wextra)
