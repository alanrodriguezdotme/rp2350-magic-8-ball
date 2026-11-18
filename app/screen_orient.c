#include "screen_orient.h"

#include "QMI8658.h"
#include "lvgl.h"

#include <stdio.h>

// IMU axes, measured on hardware: +X points toward the top of the screen,
// +Z points into the screen. Upright reads X ~ +1 g; upside down ~ -1 g.
// Flat on a table puts gravity on Z, and the current rotation is kept.
#define ORIENT_POLL_MS 100
#define ORIENT_FLIP_G 0.55f   // |x| must clear this to count as a clear tilt
#define ORIENT_DWELL_MS 500   // and stay there this long before flipping
#define ORIENT_LPF_ALPHA 0.3f // smooths shakes so they don't flip the screen

static bool s_flipped = false;
static float s_x_g = 1.0f;
static uint32_t s_dwell_ms = 0;

static float read_x_g(void)
{
    float a[3];
    QMI8658_read_acc_xyz(a);
    return a[0] / ONE_G;
}

static void apply(bool flipped)
{
    s_flipped = flipped;
    lv_disp_set_rotation(NULL, flipped ? LV_DISP_ROT_180 : LV_DISP_ROT_NONE);
    printf("[orient] %s\r\n", flipped ? "flipped" : "upright");
}

static void orient_timer_cb(lv_timer_t *t)
{
    (void)t;
    s_x_g += ORIENT_LPF_ALPHA * (read_x_g() - s_x_g);

    bool want_flip = s_x_g < -ORIENT_FLIP_G;
    bool want_upright = s_x_g > ORIENT_FLIP_G;
    if ((want_flip && !s_flipped) || (want_upright && s_flipped))
    {
        s_dwell_ms += ORIENT_POLL_MS;
        if (s_dwell_ms >= ORIENT_DWELL_MS)
        {
            s_dwell_ms = 0;
            apply(!s_flipped);
        }
    }
    else
    {
        s_dwell_ms = 0;
    }
}

void screen_orient_init(void)
{
    s_x_g = read_x_g();
    if (s_x_g < -ORIENT_FLIP_G)
    {
        apply(true);
    }
    lv_timer_create(orient_timer_cb, ORIENT_POLL_MS, NULL);
}

bool screen_orient_is_flipped(void)
{
    return s_flipped;
}
