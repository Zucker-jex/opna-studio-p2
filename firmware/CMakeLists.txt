cmake_minimum_required(VERSION 3.13)
project(opna_studio_fw C ASM)

set(CMAKE_C_STANDARD 11)
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

# ---------------------------------------------------------------------------
# Shared sources
# ---------------------------------------------------------------------------
set(CORE_SRC
  ${ROOT}/src/opna/opna.c
  ${ROOT}/src/opna/opna_fm.c
  ${ROOT}/src/opna/opna_ssg.c
  ${ROOT}/src/opna/opna_drum.c
)
set(STUDIO_SRC
  studio/app.c
  studio/ui.c
  studio/engine.c
  studio/fft.c
  studio/patches.c
  studio/gfx_ext.c
)
set_source_files_properties(${CORE_SRC} PROPERTIES COMPILE_OPTIONS "-O3")
# The engine holds the per-sample gain/capture and event loops: build it -O3.
# UI/app are size-sensitive and stay -Os.
set_source_files_properties(studio/engine.c studio/fft.c
  PROPERTIES COMPILE_OPTIONS "-O3;-Wall;-Wextra")
set_source_files_properties(studio/ui.c studio/app.c studio/gfx_ext.c studio/patches.c
  PROPERTIES COMPILE_OPTIONS "-Os;-Wall;-Wextra")

# ---------------------------------------------------------------------------
# QEMU / semihosting showcase build
# ---------------------------------------------------------------------------
add_executable(opna_studio_sim
  startup/startup.c
  bsp/sim/semihosting.c
  bsp/sim/board_sim.c
  gui/gfx.c
  ${STUDIO_SRC}
  app/main_sim.c
  ${CORE_SRC}
)
target_include_directories(opna_studio_sim PRIVATE
  bsp bsp/sim gui studio ${ROOT}/include ${ROOT}/src/opna)
target_compile_definitions(opna_studio_sim PRIVATE PFM_SIM=1)
target_compile_options(opna_studio_sim PRIVATE -Os)
target_link_options(opna_studio_sim PRIVATE
  -T${CMAKE_CURRENT_SOURCE_DIR}/ld/qemu_netduino2.ld
  -Wl,-Map=${CMAKE_BINARY_DIR}/opna_studio_sim.map)
set_target_properties(opna_studio_sim PROPERTIES
  LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ld/qemu_netduino2.ld)
add_custom_command(TARGET opna_studio_sim POST_BUILD
  COMMAND ${CMAKE_SIZE} $<TARGET_FILE:opna_studio_sim>)

# ---------------------------------------------------------------------------
# Real STM32 Primer2 hardware
# ---------------------------------------------------------------------------
set(FREERTOS_SRC
  freertos/tasks.c freertos/list.c freertos/queue.c freertos/portable/port.c)
set_source_files_properties(${FREERTOS_SRC} PROPERTIES COMPILE_OPTIONS "-w;-O2")

set(FATFS_SRC fatfs/ff.c fatfs/diskio.c)
set_source_files_properties(${FATFS_SRC} PROPERTIES COMPILE_OPTIONS "-w;-Os")

add_executable(opna_studio_hw
  startup/startup.c
  bsp/primer2/board_primer2.c
  bsp/primer2/sd_spi.c
  bsp/primer2/pwr_support.c
  gui/gfx.c
  ${STUDIO_SRC}
  app/main_hw.c
  ${CORE_SRC}
  ${FREERTOS_SRC}
  ${FATFS_SRC}
)
target_include_directories(opna_studio_hw PRIVATE
  bsp bsp/primer2 gui studio
  freertos freertos/include freertos/portable fatfs
  ${ROOT}/include ${ROOT}/src/opna)
target_compile_definitions(opna_studio_hw PRIVATE
  PFM_RTOS=1 PFM_RAMFUNC=1 PFM_FM_CH=6)
target_compile_options(opna_studio_hw PRIVATE -Os -Wall -Wextra)
target_link_options(opna_studio_hw PRIVATE
  -T${CMAKE_CURRENT_SOURCE_DIR}/ld/stm32f103ve.ld
  -Wl,-Map=${CMAKE_BINARY_DIR}/opna_studio_hw.map)
set_target_properties(opna_studio_hw PROPERTIES
  LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ld/stm32f103ve.ld)
add_custom_command(TARGET opna_studio_hw POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:opna_studio_hw> ${CMAKE_BINARY_DIR}/opna_studio_hw.bin
  COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:opna_studio_hw> ${CMAKE_BINARY_DIR}/opna_studio_hw.hex
  COMMAND ${CMAKE_SIZE} $<TARGET_FILE:opna_studio_hw>)
