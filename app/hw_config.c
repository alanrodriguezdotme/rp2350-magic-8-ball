// Static hardware configuration for no-OS-FatFS-SD-SDIO-SPI-RPi-Pico.
// One SD card on hardware SPI1, pins per the Waveshare schematic — see
// SD_*_PIN defines in DEV_Config.h. No card-detect line is broken out
// on this board, so use_card_detect is left false.

#include "hw_config.h"
#include "DEV_Config.h"

static spi_t spis[] = {
    {
        .hw_inst   = SD_SPI_PORT,
        .miso_gpio = SD_MISO_PIN,
        .mosi_gpio = SD_MOSI_PIN,
        .sck_gpio  = SD_SCK_PIN,
        // Conservative starting baud. The lib bumps after init handshake;
        // can tune up later once we've confirmed signal integrity.
        .baud_rate = 12 * 1000 * 1000,  // 12 MHz
    }
};

static sd_spi_if_t spi_ifs[] = {
    {
        .spi      = &spis[0],
        .ss_gpio  = SD_CS_PIN,
    }
};

static sd_card_t sd_cards[] = {
    {
        .type            = SD_IF_SPI,
        .spi_if_p        = &spi_ifs[0],
        .use_card_detect = false,
    }
};

size_t sd_get_num(void) { return count_of(sd_cards); }

sd_card_t *sd_get_by_num(size_t num)
{
    if (num >= sd_get_num()) {
        return NULL;
    }
    return &sd_cards[num];
}
