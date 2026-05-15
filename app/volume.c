#include "volume.h"
#include "audio_test.h"

#include "es8311.h"

#include <stdio.h>

// Codec values were picked empirically: 73 (the old hardcoded default)
// clipped on loud voicelines, so HIGH lands at 70 to keep some analog
// headroom; LOW/MED step down in roughly equal perceptual increments.
static const uint8_t s_codec_value[VOLUME_COUNT] = {
    [VOLUME_LOW] = 40,
    [VOLUME_MED] = 55,
    [VOLUME_HIGH] = 70,
};

static const char *const s_label[VOLUME_COUNT] = {
    [VOLUME_LOW] = "LOW",
    [VOLUME_MED] = "MED",
    [VOLUME_HIGH] = "HIGH",
};

static volume_level_t s_level = VOLUME_MED;

void volume_init(void)
{
    s_level = VOLUME_MED;
    es8311_voice_volume_set(s_codec_value[s_level]);
    printf("[vol] init -> %s (codec=%u)\r\n",
           s_label[s_level], s_codec_value[s_level]);
}

void volume_cycle(void)
{
    s_level = (volume_level_t)((s_level + 1) % VOLUME_COUNT);
    es8311_voice_volume_set(s_codec_value[s_level]);
    printf("[vol] cycle -> %s (codec=%u)\r\n",
           s_label[s_level], s_codec_value[s_level]);
    // Blocking 880 Hz preview, ~150 ms, at the freshly applied level.
    audio_test_play_tone(880, 150);
}

uint8_t volume_get(void)
{
    return s_codec_value[s_level];
}

volume_level_t volume_get_level(void)
{
    return s_level;
}
