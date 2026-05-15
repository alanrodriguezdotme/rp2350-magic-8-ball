#pragma once

#include <stdbool.h>
#include <stdint.h>

// Streams a 24 kHz / mono / 16-bit signed PCM WAV file off the SD card to
// the ES8311 codec via PIO2 SM0, fed by self-chained ping-pong DMA on
// DMA_IRQ_1. (Display owns DMA_IRQ_0; we stay out of its way.)
//
// Format requirements are strict: anything that isn't PCM/1ch/24kHz/16bit
// is rejected. Pre-convert sources with tools/convert_voicelines.ps1.

bool wav_player_init(void);

// Start playback. Stops any currently-playing file first. Returns false if
// the file can't be opened or the WAV header doesn't match our format.
bool wav_player_play_file(const char *path);

bool wav_player_is_playing(void);

// Pump the decoder. Call this from the main loop; refills are done here
// (NOT in the DMA IRQ) so f_read() doesn't run in interrupt context.
// Target call interval: << 80 ms or you'll underrun.
void wav_player_poll(void);

void wav_player_stop(void);

// Lipsync stats from the most recently loaded audio chunk (~170 ms
// granularity at 24 kHz / 4096-sample buffers). Both 0..255.
//   rms : amplitude envelope, perceptually mapped (clamped at ~12000 RMS
//         before normalization so typical-loud speech reaches 255)
//   zcr : zero-crossing rate, 0 = sustained vowel, 255 = sibilant
// Returns 0 for both when no file is playing. Either output may be NULL.
void wav_player_get_lipsync(uint8_t *rms_out, uint8_t *zcr_out);
