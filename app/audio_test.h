#pragma once

#include <stdbool.h>
#include <stdint.h>

// Initializes the ES8311 codec, the speaker amp, and the audio_pio
// MCLK + DOUT state machines. Returns true if the codec reports a
// plausible chip ID.
bool audio_test_init(void);

// Synthesizes a mono sine wave on the fly at the codec's configured
// sample rate (24 kHz) and streams it to the speaker. Blocking call —
// returns after duration_ms of audio has been pushed into the PIO FIFO.
void audio_test_play_tone(uint32_t freq_hz, uint32_t duration_ms);
