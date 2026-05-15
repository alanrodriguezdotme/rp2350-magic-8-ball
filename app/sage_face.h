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

// Show a caption-style response at the bottom of the screen. White text
// with a black outline; fades in over ~300 ms. The text pointer must
// remain valid for the lifetime of the displayed caption (the label
// uses lv_label_set_text_static and does not copy). Passing NULL is
// equivalent to sage_face_hide_response().
void sage_face_show_response(const char *text);

// Fade the response text out over ~300 ms.
void sage_face_hide_response(void);

// Briefly show a translucent top status bar with `left_text` in the
// top-left corner and `right_text` in the top-right corner, then auto-
// fade it out after ~1 s. Used by the volume button to flash battery %
// and the new volume tier without cluttering the main screen. Either
// pointer may be NULL to leave that side blank. Strings are copied
// internally so callers can pass a stack buffer.
void sage_face_show_status_bar(const char *left_text, const char *right_text);

// Fade the entire face in from fully transparent to fully opaque over
// `ms` milliseconds. Used for the boot intro so the sage materializes
// instead of popping in. Calling more than once just restarts from the
// current opacity.
void sage_face_fade_in(uint32_t ms);

// Fire a one-shot sparkle burst radiating outward from the third eye,
// rendered behind the face features so the sage's silhouette occludes
// the inner sparkles. Each sparkle is given a fresh random angle,
// distance, and start delay so the burst looks organic. Calling again
// while a previous burst is still in flight retargets the same pool.
void sage_face_burst_sparkles(void);

#endif // SAGE_FACE_H
