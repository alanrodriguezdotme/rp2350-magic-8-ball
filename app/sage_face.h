#ifndef SAGE_FACE_H
#define SAGE_FACE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    SAGE_POSE_IDLE = 0,
    SAGE_POSE_LISTENING,
    SAGE_POSE_SPEAKING_OMINOUS,
    SAGE_POSE_SPEAKING_SARCASTIC,
    SAGE_POSE_SPEAKING_DRY,
} sage_pose_kind_t;

// Build the parametric mystic-sage face on the active LVGL screen.
// Starts in SAGE_POSE_IDLE: half-lidded, no head sway, slow third-eye
// pulse, occasional random blink.
void sage_face_init(void);

// Cross-fade from current pose to `kind` over ~250 ms. Restarts the
// head-sway and third-eye pulse animations with the new preset's
// parameters. Calling with the same kind twice is a no-op.
void sage_face_set_pose(sage_pose_kind_t kind);

// Real-time lipsync. Both inputs are 0..255.
//   rms  -> mouth openness (added on top of the pose's resting open)
//   zcr  -> roundness vs flatness (low = vowel/round, high = sibilant/flat)
// Call every main-loop iteration while audio is playing. The face
// module low-passes internally with separate attack and release rates,
// so calling at 200 Hz with a slowly-changing source value is fine.
void sage_face_lipsync(uint8_t rms_0_255, uint8_t zcr_0_255);

// Hint that the face should perk up like it's listening (eyes wider,
// brows up, third eye pulses faster). External hook for when a mic
// pipeline detects voice activity. Internally the face also auto-perks
// on IMU-detected handling, so calling this is optional. Calling with
// `false` does NOT force-cancel the auto-perk; it just clears the
// external hold so the auto-decay timer can run normally.
void sage_face_set_listening(bool listening);

#endif // SAGE_FACE_H
