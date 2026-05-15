#include "audio_test.h"

#include "audio_pio.h"
#include "es8311.h"
#include "DEV_Config.h"
#include "volume.h"

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"

#include <math.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

bool audio_test_init(void)
{
    // Power up the NS4150B speaker amp. Without this you can talk to
    // the codec all day and hear nothing.
    gpio_init(PA_CTRL);
    gpio_set_dir(PA_CTRL, GPIO_OUT);
    gpio_put(PA_CTRL, 1);

    es8311_init(pico_audio);
    es8311_sample_frequency_config(pico_audio.mclk_freq, pico_audio.sample_freq);
    es8311_voice_volume_set(volume_get());

    uint16_t chip_id = es8311_read_id();
    printf("ES8311 chip ID: 0x%04x\r\n", chip_id);

    mclk_pio_init();
    dout_pio_init();

    // Datasheet ID bytes are 0x83, 0x11. The vendor driver returns them
    // little-endian (0x1183); some forks return big-endian (0x8311).
    // Accept either so we don't bail out on a perfectly happy codec.
    return chip_id == 0x8311 || chip_id == 0x1183;
}

void audio_test_play_tone(uint32_t freq_hz, uint32_t duration_ms)
{
    const uint32_t fs = pico_audio.sample_freq; // configured to 24000
    const float w = 2.0f * M_PI * (float)freq_hz / (float)fs;
    const int16_t amplitude = 12000; // ~37% of int16 max — easy on the ears

    const uint64_t total_samples = (uint64_t)fs * (uint64_t)duration_ms / 1000ULL;

    for (uint64_t n = 0; n < total_samples; n++)
    {
        int16_t s = (int16_t)(amplitude * sinf((float)n * w));
        // audio_pio expects 32-bit words with the 16-bit sample in the
        // upper half; see data_treating() in lib/audio_pio/audio_pio.c.
        uint32_t word = ((uint32_t)(uint16_t)s) << 16;
        pio_sm_put_blocking(pico_audio.pio_2, pico_audio.sm_dout, word);
    }
}
