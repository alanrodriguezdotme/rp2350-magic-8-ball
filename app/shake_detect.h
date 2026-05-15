#pragma once

#include <stdbool.h>

typedef enum
{
    SHAKE_EVT_NONE = 0, // no transition this tick
    SHAKE_EVT_STARTED,  // motion just rose above threshold (one-shot)
    SHAKE_EVT_ABORTED,  // motion fell back below threshold before HOLD_MS
    SHAKE_EVT_FIRED,    // motion held for HOLD_MS — answer time
} shake_event_t;

// Initializes the shake-detect state. The QMI8658 itself is initialized
// elsewhere (in app_run via QMI8658_init); this just zeroes our state.
void shake_detect_init(void);

// Call from the main loop (every 5-10 ms is plenty). Returns at most one
// event per call, edge-triggered against the prior internal state.
shake_event_t shake_detect_poll(void);

// True while the motion EMA is currently above threshold (between
// STARTED and either ABORTED or FIRED). Useful for UI that wants to
// react continuously rather than to discrete events.
bool shake_detect_is_shaking(void);

// Clear motion/cooldown state and force gravity to re-baseline on the
// next poll. Call after a long pause in polling (e.g. while audio was
// playing) so leftover motion doesn't immediately register.
void shake_detect_reset(void);
