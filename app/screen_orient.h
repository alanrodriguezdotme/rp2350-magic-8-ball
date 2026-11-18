#ifndef SCREEN_ORIENT_H
#define SCREEN_ORIENT_H

#include <stdbool.h>

// Flips the whole UI 180 degrees when the device is held upside down.
// Polls the IMU from an LVGL timer, so it only runs while the display is
// awake. Call once after lvgl_glue_init(); the initial orientation is
// applied immediately so the first frame is already the right way up.
void screen_orient_init(void);

// True while the UI is rotated 180 degrees.
bool screen_orient_is_flipped(void);

#endif // SCREEN_ORIENT_H
