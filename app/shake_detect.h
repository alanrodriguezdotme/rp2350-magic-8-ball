#pragma once

#include <stdbool.h>

// Initializes the shake-detect state. The QMI8658 itself is initialized
// elsewhere (in app_run via QMI8658_init); this just zeroes our state.
void shake_detect_init(void);

// Call from the main loop (every 5-10ms is plenty). Returns true once
// when a shake gesture is detected. Subsequent calls return false until
// the cooldown expires.
bool shake_detect_poll(void);

// Clear motion/cooldown state and force gravity to re-baseline on the
// next poll. Call after a long pause in polling (e.g. while audio was
// playing) so leftover motion doesn't immediately register.
void shake_detect_reset(void);
