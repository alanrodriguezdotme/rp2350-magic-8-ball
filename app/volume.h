#pragma once

#include <stdint.h>

// Three discrete codec output levels, cycled by the BOOT button.
// Codec values stay below 73 (the previous default) which clips on
// peak-y voicelines.
typedef enum
{
    VOLUME_LOW = 0,
    VOLUME_MED = 1,
    VOLUME_HIGH = 2,
    VOLUME_COUNT = 3,
} volume_level_t;

// Set the current level to MEDIUM and push it to the codec. Call after
// es8311_init / audio_test_init so the codec is ready.
void volume_init(void);

// Advance to the next level (wraps), apply to the codec, and play a
// short blocking test beep so the user can hear the new level.
void volume_cycle(void);

// Codec value (0..100) for the currently selected level. wav_player and
// audio_test re-assert this on every play.
uint8_t volume_get(void);

volume_level_t volume_get_level(void);
