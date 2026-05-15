#pragma once

#include <stdbool.h>
#include <stddef.h>

// Scan `dir` for .wav files and store their names. Subsequent
// pick_random calls return paths under this directory. Returns true
// if at least one file was indexed.
bool voicelines_scan(const char *dir);

size_t voicelines_count(void);

// Returns a path like "0:/voicelines/foo.wav" suitable for
// wav_player_play_file. The buffer is reused; copy if you need to
// hold onto it past the next call. Returns NULL if the catalog is
// empty.
const char *voicelines_pick_random(void);
