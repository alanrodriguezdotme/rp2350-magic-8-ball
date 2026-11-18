# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)
set(PICO_BOARD pico2 CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

if (PICO_SDK_VERSION_STRING VERSION_LESS "1.4.0")
  message(FATAL_ERROR "Raspberry Pi Pico SDK version 1.4.0 (or later) required. Your version is ${PICO_SDK_VERSION_STRING}")
endif()

project(magic_8_ball C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

# Pull in carlk3's no-OS-FatFS-SD-SDIO-SPI-RPi-Pico via FetchContent.
# Pinned to v3.6.2 (latest tag at time of writing). Project isn't a git
# repo yet, so submodule isn't an option; FetchContent downloads at
# configure time and caches under build/_deps/.
include(FetchContent)
FetchContent_Declare(
    no_os_fatfs
    GIT_REPOSITORY https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico.git
    GIT_TAG v3.6.2
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(no_os_fatfs)
if(NOT no_os_fatfs_POPULATED)
    FetchContent_Populate(no_os_fatfs)
    add_subdirectory(${no_os_fatfs_SOURCE_DIR}/src ${no_os_fatfs_BINARY_DIR})
endif()

# Add compilation subdirectory
add_subdirectory(./app)
add_subdirectory(./lib/Config)
add_subdirectory(./lib/QSPI_PIO)
add_subdirectory(./lib/AMOLED)
add_subdirectory(./lib/Touch)
add_subdirectory(./lib/QMI8658)
add_subdirectory(./lib/PCF85063A)
add_subdirectory(./lib/AXP2101)
add_subdirectory(./lib/audio_pio)
add_subdirectory(./lib/es8311)
add_subdirectory(./lib/lvgl)

# Add header file directory
include_directories(./app)
include_directories(./lib/Config)
include_directories(./lib/QSPI_PIO)
include_directories(./lib/AMOLED)
include_directories(./lib/Touch)
include_directories(./lib/QMI8658)
include_directories(./lib/PCF85063A)
include_directories(./lib/AXP2101)
include_directories(./lib/audio_pio)
include_directories(./lib/es8311)
include_directories(./lib/lvgl)

# PICO_CONFIG: PICO_XOSC_STARTUP_DELAY_MULTIPLIER, Multiplier to lengthen xosc startup delay to accommodate slow-starting oscillators, type=int, min=1, default=1, group=hardware_xosc

add_executable(magic_8_ball main.c)

pico_generate_pio_header(magic_8_ball ${CMAKE_CURRENT_LIST_DIR}/lib/QSPI_PIO/qspi.pio
                        OUTPUT_DIR ${CMAKE_CURRENT_LIST_DIR}/lib/QSPI_PIO)

pico_generate_pio_header(magic_8_ball ${CMAKE_CURRENT_LIST_DIR}/lib/audio_pio/audio_pio.pio
                        OUTPUT_DIR ${CMAKE_CURRENT_LIST_DIR}/lib/audio_pio)

pico_set_program_name(magic_8_ball "magic_8_ball")
pico_set_program_version(magic_8_ball "1.0.0")

pico_enable_stdio_uart(magic_8_ball 0)
pico_enable_stdio_usb(magic_8_ball 1)

# Add the standard library to the build

# Tell LVGL to use a plain `#include "lv_conf.h"` and find it via the
# include path (which we set to ./app above). This is more robust than
# LVGL's default `../../lv_conf.h` source-relative search.
add_compile_definitions(LV_CONF_INCLUDE_SIMPLE)
add_compile_options(-O2)


  target_link_libraries(magic_8_ball
                        Config
                        QSPI_PIO
                        AMOLED
                        Touch
                        QMI8658
                        PCF85063A
                        AXP2101
                        audio_pio
                        es8311
                        LVGL
                        app
                        no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
                        pico_stdlib
                        hardware_pio 
                        hardware_pwm 
                        hardware_dma
                        hardware_spi
                        hardware_i2c
                        hardware_gpio
                        hardware_adc
)

# Add the standard include files to the build
target_include_directories(magic_8_ball PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}
)

pico_add_extra_outputs(magic_8_ball)

