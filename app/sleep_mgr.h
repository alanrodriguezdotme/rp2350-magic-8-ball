// Idle-sleep manager.
//
// Tracks when the user last interacted with the device. After
// SLEEP_IDLE_TIMEOUT_MS of no activity (and no audio playing), drives
// the display, codec, amp, IMU and touch into low-power states.
// The AXP2101 PWRKEY (the physical button on the side of the device)
// is the only thing that wakes it back up.
//
// While asleep, the main loop should call sleep_mgr_tick() and then
// short-circuit on sleep_mgr_is_asleep() so LVGL, wav_player and
// shake polling don't run.

#ifndef SLEEP_MGR_H
#define SLEEP_MGR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#ifndef SLEEP_IDLE_TIMEOUT_MS
#define SLEEP_IDLE_TIMEOUT_MS (30000u)
#endif

    void sleep_mgr_init(void);

    // Reset the idle deadline. Call from any user-input source (touch
    // callback, shake events, sub-threshold motion, etc.). Cheap and
    // safe to spam.
    void sleep_mgr_notify_activity(void);

    // Drive the state machine. Pass true while audio is playing so the
    // idle timeout can't fire mid-voiceline. Polls the AXP2101 PWRKEY
    // for both sleep (while awake) and wake (while asleep).
    void sleep_mgr_tick(bool audio_playing);

    bool sleep_mgr_is_asleep(void);

#ifdef __cplusplus
}
#endif

#endif // SLEEP_MGR_H
