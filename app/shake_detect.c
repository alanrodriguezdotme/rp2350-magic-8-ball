#include "shake_detect.h"

#include "QMI8658.h"
#include "pico/stdlib.h"

#include <math.h>
#include <stdio.h>

// Tuning knobs — adjust based on serial debug output.
#define SHAKE_THRESHOLD_G 0.80f // smoothed motion (EMA) above this counts as motion
#define SHAKE_HOLD_MS 2000      // EMA must stay above threshold this long to fire
#define SHAKE_COOLDOWN_MS 1500  // ignore further shakes for this long after firing
#define SHAKE_EMA_ALPHA 0.20f   // weight on new sample (~5-sample memory at 200Hz)
#define GRAV_EMA_ALPHA 0.02f    // very slow, tracks gravity vector but not shakes
#define DEBUG_QUIET_MS 2000     // print accel even when still, this often
#define DEBUG_MOTION_GATE 0.10f // print accel immediately if EMA exceeds this

static absolute_time_t s_motion_start;
static bool s_in_motion;
static absolute_time_t s_cooldown_until;
static absolute_time_t s_next_quiet_print;
static float s_motion_ema;
static float s_gx, s_gy, s_gz; // estimated gravity vector (g units)
static bool s_grav_primed;

void shake_detect_init(void)
{
    s_in_motion = false;
    s_motion_start = nil_time;
    s_cooldown_until = nil_time;
    s_next_quiet_print = nil_time;
    s_motion_ema = 0.0f;
    s_gx = s_gy = s_gz = 0.0f;
    s_grav_primed = false;
}

void shake_detect_reset(void)
{
    // Clear motion state but keep gravity-primed flag false so the next
    // poll re-baselines from the current attitude. Used after audio
    // playback ends so stale post-shake motion doesn't insta-fire.
    s_in_motion = false;
    s_motion_start = nil_time;
    s_cooldown_until = nil_time;
    s_motion_ema = 0.0f;
    s_grav_primed = false;
}

bool shake_detect_is_shaking(void)
{
    return s_in_motion;
}

shake_event_t shake_detect_poll(void)
{
    absolute_time_t now = get_absolute_time();

    float a[3];
    QMI8658_read_acc_xyz(a);

    // Driver returns m/s². Normalize to g so the thresholds below are
    // intuitive (1.0 = sitting still under gravity).
    float ax = a[0] / ONE_G, ay = a[1] / ONE_G, az = a[2] / ONE_G;

    // Estimate gravity with a slow EMA. Prime from the first sample so
    // we don't see a huge fake "motion" on startup.
    if (!s_grav_primed)
    {
        s_gx = ax;
        s_gy = ay;
        s_gz = az;
        s_grav_primed = true;
    }
    else
    {
        s_gx = (1.0f - GRAV_EMA_ALPHA) * s_gx + GRAV_EMA_ALPHA * ax;
        s_gy = (1.0f - GRAV_EMA_ALPHA) * s_gy + GRAV_EMA_ALPHA * ay;
        s_gz = (1.0f - GRAV_EMA_ALPHA) * s_gz + GRAV_EMA_ALPHA * az;
    }

    // Motion = residual after subtracting gravity. This is direction-
    // agnostic: shaking up/down or side/side both register equally.
    float dx = ax - s_gx, dy = ay - s_gy, dz = az - s_gz;
    float motion = sqrtf(dx * dx + dy * dy + dz * dz);

    // Low-pass the motion signal so a real shake (which oscillates and
    // crosses through zero on each direction reversal) registers as a
    // sustained elevated reading instead of a series of tiny spikes.
    s_motion_ema = (1.0f - SHAKE_EMA_ALPHA) * s_motion_ema + SHAKE_EMA_ALPHA * motion;

    // Print accel periodically (always at low cadence, immediately when
    // there's interesting motion). Comment out once thresholds are tuned.
    bool past_quiet_window = absolute_time_diff_us(now, s_next_quiet_print) <= 0;
    if (s_motion_ema >= DEBUG_MOTION_GATE || past_quiet_window)
    {
        printf("[acc] motion=%5.2f ema=%5.2f g=(%5.2f,%5.2f,%5.2f)\r\n",
               motion, s_motion_ema, s_gx, s_gy, s_gz);
        s_next_quiet_print = make_timeout_time_ms(DEBUG_QUIET_MS);
    }

    // Suppress fires during cooldown. Past-timeout diff is negative, so
    // > 0 means cooldown is still in the future.
    if (absolute_time_diff_us(now, s_cooldown_until) > 0)
    {
        return SHAKE_EVT_NONE;
    }

    if (s_motion_ema >= SHAKE_THRESHOLD_G)
    {
        if (!s_in_motion)
        {
            s_in_motion = true;
            s_motion_start = now;
            printf("[shake] STARTED ema=%.2fg\r\n", s_motion_ema);
            return SHAKE_EVT_STARTED;
        }
        int64_t held_us = absolute_time_diff_us(s_motion_start, now);
        if (held_us >= (int64_t)SHAKE_HOLD_MS * 1000LL)
        {
            printf("[shake] FIRED ema=%.2fg held=%lldms\r\n",
                   s_motion_ema, (long long)(held_us / 1000));
            s_cooldown_until = make_timeout_time_ms(SHAKE_COOLDOWN_MS);
            s_in_motion = false;
            return SHAKE_EVT_FIRED;
        }
        return SHAKE_EVT_NONE;
    }

    if (s_in_motion)
    {
        // Motion dropped below threshold before HOLD_MS — user gave up.
        int64_t held_us = absolute_time_diff_us(s_motion_start, now);
        printf("[shake] ABORTED held=%lldms\r\n", (long long)(held_us / 1000));
        s_in_motion = false;
        return SHAKE_EVT_ABORTED;
    }

    return SHAKE_EVT_NONE;
}
