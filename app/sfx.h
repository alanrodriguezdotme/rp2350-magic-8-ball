#pragma once

#include <stdbool.h>

// Canonical SD-card paths for the system sound effects (boot intro and
// shake hold). These live in their own directory so voicelines_scan
// doesn't accidentally pick them as random answers.
#define SFX_ANSWER_PATH "0:/sfx/answer.wav"
#define SFX_SHAKE_PATH "0:/sfx/shake.wav"

// Try to play `path` via wav_player_play_file. Returns true if playback
// started; false if the file couldn't be opened (e.g. SD missing). On
// failure, the caller is expected to fall back to a tone — sfx_play()
// stays silent rather than imposing a tone choice.
bool sfx_play(const char *path);
