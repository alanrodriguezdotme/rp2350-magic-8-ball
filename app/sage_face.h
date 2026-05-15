#ifndef SAGE_FACE_H
#define SAGE_FACE_H

#include <stdbool.h>

// Builds the parametric mystic-sage face on the active LVGL screen.
// Idle pose: closed eyes, slow third-eye pulse.
void sage_face_init(void);

// Switch between idle and speaking poses. Speaking pose brightens and
// speeds up the third-eye pulse and bobs the lips open/close so the
// face looks like it's talking. Safe to call repeatedly with the same
// value (no-op).
void sage_face_set_speaking(bool speaking);

#endif // SAGE_FACE_H
