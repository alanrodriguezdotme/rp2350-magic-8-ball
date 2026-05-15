aux_source_directory(. DIR_APP_SRCS)

# Generate sage_tone_table.gen.c from voicelines/SCRIPT.md. Lives in the
# build dir so it doesn't pollute the source tree (and isn't picked up by
# aux_source_directory above — we add it explicitly to the library below).
find_program(POWERSHELL_EXE NAMES pwsh powershell REQUIRED)
set(SAGE_TONE_GEN ${CMAKE_CURRENT_BINARY_DIR}/sage_tone_table.gen.c)
add_custom_command(
    OUTPUT ${SAGE_TONE_GEN}
    COMMAND ${POWERSHELL_EXE} -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_SOURCE_DIR}/tools/gen_voicelines_tones.ps1
            -ScriptMd ${CMAKE_SOURCE_DIR}/voicelines/SCRIPT.md
            -Output ${SAGE_TONE_GEN}
    DEPENDS ${CMAKE_SOURCE_DIR}/voicelines/SCRIPT.md
            ${CMAKE_SOURCE_DIR}/tools/gen_voicelines_tones.ps1
    COMMENT "Generating sage_tone_table.gen.c from voicelines/SCRIPT.md"
    VERBATIM
)

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

add_library(app ${DIR_APP_SRCS} ${SAGE_TONE_GEN})
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
