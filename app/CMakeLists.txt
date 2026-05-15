aux_source_directory(. DIR_APP_SRCS)

include_directories(../lib/Config)
include_directories(../lib/QSPI_PIO)
include_directories(../lib/AMOLED)
include_directories(../lib/Touch)
include_directories(../lib/QMI8658)
include_directories(../lib/PCF85063A)
include_directories(../lib/audio_pio)
include_directories(../lib/es8311)
include_directories(../lib/lvgl)
include_directories(.)

add_library(app ${DIR_APP_SRCS})
target_link_libraries(app PUBLIC
    Config
    AMOLED
    Touch
    PCF85063A
    QMI8658
    audio_pio
    es8311
    LVGL
    no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
    pico_stdlib
    pico_stdio_usb
    pico_rand
    hardware_clocks
    hardware_adc
    hardware_dma
    hardware_spi
    hardware_pio
    hardware_gpio
)
