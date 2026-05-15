// Parametric mystic-sage face — expressive build.
//
// All face parts are pure LVGL primitives composed on the screen (no canvas
// or sprite buffer — keeps SRAM cost ~0 since the framebuffer already eats
// ~329 KB). Animations are driven by the LVGL anim engine plus a couple of
// lv_timers for blink scheduling and pose cross-fade.
//
// Layout:
//   screen
//   └── head_container    (lv_obj at screen center, animated for sway)
//       ├── third_eye
//       ├── brow_l / brow_r            (lv_line with 3 control points)
//       ├── eye_l / eye_r              (sclera; height shrinks for blink)
//       ├── pupil_l / pupil_r          (sibling of eye, repositioned by gaze)
//       ├── mouth_top / mouth_bot      (lv_line with 3 control points)
//
// Pose presets store target values for every animatable property; switching
// pose runs a 250 ms cross-fade timer that lerps every value and calls
// apply_pose_visuals() each tick.

#include "sage_face.h"
#include "lvgl_glue.h"
#include "lvgl.h"

#include "QMI8658.h"
#include "pico/rand.h"

#include <stdint.h>

// ------------------------------------------------------------------ geometry

#define EYE_W 44 // sclera width (also fully-open height)
#define EYE_X_OFFSET 50
#define EYE_Y_OFFSET (-20) // negative = above face center
#define EYE_MIN_H 2

#define PUPIL_DIA 18

#define BROW_W 56
#define BROW_THICK 5
#define BROW_X_OFFSET 52
#define BROW_Y_OFFSET (-2)

#define MOUTH_W 64
#define MOUTH_THICK 5
#define MOUTH_Y_OFFSET 70 // distance below face center; with eyes
                          // at -20 (eye bottom ~y=2) this leaves
                          // ~70 px of breathing room.
#define MOUTH_MAX_OPEN 60 // peak vertical gap at full lipsync drive
#define MOUTH_CORNER_RANGE 10

#define THIRD_EYE_DIA 24
#define THIRD_EYE_Y_OFFSET (-90)

#define POSE_XFADE_MS 250

// --------------------------------------------------------------- pose preset

typedef struct
{
    // Eyebrows. inner = end nearest the nose, outer = end nearest the
    // temple. Negative Y = up. Per-side values let us do asymmetric brows
    // (one raised brow for sarcasm).
    int8_t brow_inner_l_dy, brow_outer_l_dy;
    int8_t brow_inner_r_dy, brow_outer_r_dy;

    uint8_t eye_open_pct;      // 0 closed, 100 full circle
    int8_t pupil_dx, pupil_dy; // gaze nudge

    int8_t mouth_curve;     // -ve frown, +ve smile
    int8_t mouth_rest_open; // baseline gap between top and bottom lip

    uint8_t third_eye_min_opa;
    uint8_t third_eye_max_opa;
    uint16_t third_eye_period_ms;
} sage_pose_t;

static const sage_pose_t s_poses[5] = {
    [SAGE_POSE_IDLE] = {
        .brow_inner_l_dy = 0,
        .brow_outer_l_dy = 0,
        .brow_inner_r_dy = 0,
        .brow_outer_r_dy = 0,
        .eye_open_pct = 30,
        .pupil_dx = 0,
        .pupil_dy = 0,
        .mouth_curve = 0,
        .mouth_rest_open = 0,
        .third_eye_min_opa = 80,
        .third_eye_max_opa = 220,
        .third_eye_period_ms = 1500,
    },
    [SAGE_POSE_LISTENING] = {
        // Perked-up. Eyes wide, brows raised, pupils up a touch (looking
        // at the speaker), third eye pulses brighter and faster.
        .brow_inner_l_dy = -4,
        .brow_outer_l_dy = -3,
        .brow_inner_r_dy = -4,
        .brow_outer_r_dy = -3,
        .eye_open_pct = 90,
        .pupil_dx = 0,
        .pupil_dy = -2,
        .mouth_curve = 1,
        .mouth_rest_open = 0,
        .third_eye_min_opa = 160,
        .third_eye_max_opa = 255,
        .third_eye_period_ms = 600,
    },
    [SAGE_POSE_SPEAKING_OMINOUS] = {
        .brow_inner_l_dy = 6,
        .brow_outer_l_dy = -2,
        .brow_inner_r_dy = 6,
        .brow_outer_r_dy = -2,
        .eye_open_pct = 50,
        .pupil_dx = 0,
        .pupil_dy = 2,
        .mouth_curve = -6,
        .mouth_rest_open = 2,
        .third_eye_min_opa = 180,
        .third_eye_max_opa = 255,
        .third_eye_period_ms = 700,
    },
    [SAGE_POSE_SPEAKING_SARCASTIC] = {
        .brow_inner_l_dy = -8,
        .brow_outer_l_dy = -10, // left brow up
        .brow_inner_r_dy = 0,
        .brow_outer_r_dy = 0,
        .eye_open_pct = 85,
        .pupil_dx = 4,
        .pupil_dy = -2,
        .mouth_curve = 0,
        .mouth_rest_open = 1,
        .third_eye_min_opa = 200,
        .third_eye_max_opa = 255,
        .third_eye_period_ms = 350,
    },
    [SAGE_POSE_SPEAKING_DRY] = {
        .brow_inner_l_dy = -2,
        .brow_outer_l_dy = 0,
        .brow_inner_r_dy = -2,
        .brow_outer_r_dy = 0,
        .eye_open_pct = 70,
        .pupil_dx = 0,
        .pupil_dy = 0,
        .mouth_curve = 8,
        .mouth_rest_open = 1,
        .third_eye_min_opa = 160,
        .third_eye_max_opa = 240,
        .third_eye_period_ms = 900,
    },
};

// ------------------------------------------------------------------- runtime

typedef struct
{
    lv_obj_t *head;
    lv_obj_t *brow_l, *brow_r;
    lv_obj_t *eye_l, *eye_r;
    lv_obj_t *pupil_l, *pupil_r;
    lv_obj_t *mouth_top, *mouth_bot;
    lv_obj_t *third_eye;
} sage_parts_t;

static sage_parts_t s_p;

// lv_line stores the pointer we hand it (it does not copy). Keep the
// backing arrays as static globals and call lv_line_set_points() after
// mutating them.
static lv_point_t s_brow_l_pts[3];
static lv_point_t s_brow_r_pts[3];
static lv_point_t s_mouth_top_pts[3];
static lv_point_t s_mouth_bot_pts[3];

static sage_pose_kind_t s_pose_kind = SAGE_POSE_IDLE;
static sage_pose_t s_pose_now;
static sage_pose_t s_pose_from;
static sage_pose_t s_pose_to;

static uint8_t s_lipsync_open_smoothed = 0;
static int8_t s_lipsync_corner_smoothed = 0;

// Idle gaze drift: ever-present small pupil offset, lerped by lv_anim to
// a fresh random target every few seconds. Adds liveliness; works in any
// pose, since speaking poses' pupil_dx/dy treat this as a small jitter on
// top of the intentional gaze direction.
static int8_t s_gaze_dx = 0;
static int8_t s_gaze_dy = 0;
static lv_timer_t *s_gaze_timer = NULL;

static lv_timer_t *s_blink_timer = NULL;
static lv_timer_t *s_xfade_timer = NULL;
static int32_t s_xfade_t = 0;
static const int32_t XFADE_STEP_MS = 16;
static const int32_t XFADE_TICKS = POSE_XFADE_MS / 16;

// Physics-driven head motion. Tunables and integrator live further down
// near physics_timer_cb(); these are the bits sage_face_init() needs to
// see at top.
#define PHYS_TICK_MS 20
static lv_timer_t *s_phys_timer = NULL;

// Listening / perked state. Auto-decay timestamp is set both by external
// callers (sage_face_set_listening) and by IMU motion detection inside
// the physics tick. Defined up here so sage_face_set_listening() (which
// appears earlier in the file) can see them.
#define LISTEN_HOLD_MS 1500
static uint32_t s_listen_until_ms = 0;
static bool s_listen_external = false;

// True while the blink anim owns the eye height. apply_eye_open() must
// not fight it during this window.
static bool s_blinking = false;

// ----------------------------------------------------------------- prototypes

static void apply_pose_visuals(const sage_pose_t *p);
static void apply_eye_open(uint8_t pct);
static void apply_pupils(int8_t dx, int8_t dy);
static void refresh_pupils(void);
static void apply_brows(const sage_pose_t *p);
static void apply_mouth(int8_t curve, int8_t rest_open, uint8_t lipsync_open, int8_t lipsync_corner);
static void apply_third_eye_pulse(const sage_pose_t *p);

static void blink_timer_cb(lv_timer_t *t);
static void blink_anim_exec(void *unused, int32_t v);
static void blink_anim_done(lv_anim_t *a);
static void xfade_timer_cb(lv_timer_t *t);
static void anim_third_eye_opa(void *obj, int32_t v);
static void physics_timer_cb(lv_timer_t *t);
static void gaze_timer_cb(lv_timer_t *t);
static void anim_gaze_x(void *unused, int32_t v);
static void anim_gaze_y(void *unused, int32_t v);

static lv_obj_t *make_filled_circle(lv_obj_t *parent, int dia, int xoff, int yoff, lv_color_t color);
static lv_obj_t *make_line(lv_obj_t *parent, lv_point_t *pts, int xoff, int yoff, lv_color_t color, int thick);

static int32_t lerp_i32(int32_t a, int32_t b, int32_t t1000);
static int clampi(int v, int lo, int hi);
static uint32_t blink_jitter_ms(void);

// ============================================================================
// Init
// ============================================================================

void sage_face_init(void)
{
    lv_color_t color_brow = lv_color_make(40, 30, 80);
    lv_color_t color_sclera = lv_color_make(220, 215, 235);
    lv_color_t color_pupil = lv_color_make(20, 14, 50);
    lv_color_t color_lips = lv_color_make(150, 75, 110);
    lv_color_t color_third_eye = lv_color_make(0, 216, 255);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Head container — invisible, full-size, centered. Children align
    // relative to it; animating its x offset moves the whole head.
    s_p.head = lv_obj_create(scr);
    lv_obj_remove_style_all(s_p.head);
    lv_obj_set_size(s_p.head, DISP_HOR_RES, DISP_VER_RES);
    lv_obj_align(s_p.head, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(s_p.head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_p.head, LV_OPA_TRANSP, 0);

    s_p.third_eye = make_filled_circle(s_p.head, THIRD_EYE_DIA, 0, THIRD_EYE_Y_OFFSET, color_third_eye);

    // Brow line containers sit just above each eye. make_line() uses
    // LV_ALIGN_CENTER, so xoff is the line's CENTER (not its left edge);
    // we want each brow centered over its eye.
    s_p.brow_l = make_line(s_p.head, s_brow_l_pts, -BROW_X_OFFSET, EYE_Y_OFFSET + BROW_Y_OFFSET - EYE_W / 2 - BROW_W / 2, color_brow, BROW_THICK);
    s_p.brow_r = make_line(s_p.head, s_brow_r_pts, BROW_X_OFFSET, EYE_Y_OFFSET + BROW_Y_OFFSET - EYE_W / 2 - BROW_W / 2, color_brow, BROW_THICK);

    s_p.eye_l = make_filled_circle(s_p.head, EYE_W, -EYE_X_OFFSET, EYE_Y_OFFSET, color_sclera);
    s_p.eye_r = make_filled_circle(s_p.head, EYE_W, EYE_X_OFFSET, EYE_Y_OFFSET, color_sclera);

    s_p.pupil_l = make_filled_circle(s_p.head, PUPIL_DIA, -EYE_X_OFFSET, EYE_Y_OFFSET, color_pupil);
    s_p.pupil_r = make_filled_circle(s_p.head, PUPIL_DIA, EYE_X_OFFSET, EYE_Y_OFFSET, color_pupil);

    // Mouth lines are centered horizontally on the face midline.
    s_p.mouth_top = make_line(s_p.head, s_mouth_top_pts, 0, MOUTH_Y_OFFSET - MOUTH_W / 2, color_lips, MOUTH_THICK);
    s_p.mouth_bot = make_line(s_p.head, s_mouth_bot_pts, 0, MOUTH_Y_OFFSET - MOUTH_W / 2, color_lips, MOUTH_THICK);

    // Snap to idle pose, no cross-fade.
    s_pose_kind = SAGE_POSE_IDLE;
    s_pose_now = s_poses[SAGE_POSE_IDLE];
    apply_pose_visuals(&s_pose_now);
    apply_third_eye_pulse(&s_pose_now);

    s_blink_timer = lv_timer_create(blink_timer_cb, blink_jitter_ms(), NULL);
    // Kick off the gaze drift right away — first shift in ~2 s, then re-
    // jitter inside the callback.
    s_gaze_timer = lv_timer_create(gaze_timer_cb, 2000, NULL);
    // Physics tick at 50 Hz reads the IMU and integrates head position.
    // Independent of pose state — it just keeps running.
    s_phys_timer = lv_timer_create(physics_timer_cb, PHYS_TICK_MS, NULL);
}

// ============================================================================
// Public API
// ============================================================================

void sage_face_set_pose(sage_pose_kind_t kind)
{
    if (kind == s_pose_kind)
        return;
    s_pose_kind = kind;

    s_pose_from = s_pose_now;
    s_pose_to = s_poses[kind];
    s_xfade_t = 0;

    if (s_xfade_timer)
    {
        lv_timer_del(s_xfade_timer);
    }
    s_xfade_timer = lv_timer_create(xfade_timer_cb, XFADE_STEP_MS, NULL);

    // Restart sway and pulse with new pose params immediately — they
    // aren't lerped; they just take new params and run.
    apply_third_eye_pulse(&s_pose_to);
}

void sage_face_lipsync(uint8_t rms_0_255, uint8_t zcr_0_255)
{
    // Mouth open: fast attack, slower release for natural-looking speech.
    const uint8_t ATK = 90;
    const uint8_t REL = 22;
    if (rms_0_255 > s_lipsync_open_smoothed)
    {
        uint8_t up = rms_0_255 - s_lipsync_open_smoothed;
        s_lipsync_open_smoothed += (up < ATK) ? up : ATK;
    }
    else
    {
        uint8_t down = s_lipsync_open_smoothed - rms_0_255;
        s_lipsync_open_smoothed -= (down < REL) ? down : REL;
    }

    // ZCR maps to mouth-corner pull. High ZCR (sibilants, unvoiced) leaves
    // corners outward; low ZCR (rounded vowels) pulls them inward. Smooth
    // by 1 step per call so it visibly lags the open dimension.
    int target_corner = ((int)zcr_0_255 - 128) * MOUTH_CORNER_RANGE / 128;
    if (target_corner > s_lipsync_corner_smoothed)
    {
        s_lipsync_corner_smoothed += 1;
    }
    else if (target_corner < s_lipsync_corner_smoothed)
    {
        s_lipsync_corner_smoothed -= 1;
    }

    apply_mouth(s_pose_now.mouth_curve, s_pose_now.mouth_rest_open,
                s_lipsync_open_smoothed, s_lipsync_corner_smoothed);
}

void sage_face_set_listening(bool listening)
{
    s_listen_external = listening;
    if (listening)
    {
        // Extend the auto-decay window too, so a brief mic event keeps
        // the face perked for at least HOLD_MS even after the caller
        // sets `false`.
        s_listen_until_ms = lv_tick_get() + LISTEN_HOLD_MS;
    }
}

// ============================================================================
// Apply pose -> visuals
// ============================================================================

static void apply_pose_visuals(const sage_pose_t *p)
{
    s_pose_now = *p;
    if (!s_blinking)
    {
        apply_eye_open(p->eye_open_pct);
    }
    refresh_pupils();
    apply_brows(p);
    apply_mouth(p->mouth_curve, p->mouth_rest_open,
                s_lipsync_open_smoothed, s_lipsync_corner_smoothed);
}

static void apply_eye_open(uint8_t pct)
{
    int h = clampi((EYE_W * pct) / 100, EYE_MIN_H, EYE_W);
    lv_obj_set_height(s_p.eye_l, h);
    lv_obj_set_height(s_p.eye_r, h);
    lv_obj_align(s_p.eye_l, LV_ALIGN_CENTER, -EYE_X_OFFSET, EYE_Y_OFFSET);
    lv_obj_align(s_p.eye_r, LV_ALIGN_CENTER, EYE_X_OFFSET, EYE_Y_OFFSET);
    bool pupils_visible = pct > 25;
    if (pupils_visible)
    {
        lv_obj_clear_flag(s_p.pupil_l, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_p.pupil_r, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(s_p.pupil_l, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_p.pupil_r, LV_OBJ_FLAG_HIDDEN);
    }
}

static void apply_pupils(int8_t dx, int8_t dy)
{
    lv_obj_align(s_p.pupil_l, LV_ALIGN_CENTER, -EYE_X_OFFSET + dx, EYE_Y_OFFSET + dy);
    lv_obj_align(s_p.pupil_r, LV_ALIGN_CENTER, EYE_X_OFFSET + dx, EYE_Y_OFFSET + dy);
}

// Combine the current pose's intentional gaze with the always-on idle
// gaze drift, then push to the pupils. Callers go through this whenever
// either input changes.
static void refresh_pupils(void)
{
    int dx = (int)s_pose_now.pupil_dx + (int)s_gaze_dx;
    int dy = (int)s_pose_now.pupil_dy + (int)s_gaze_dy;
    if (dx > 31)
        dx = 31;
    else if (dx < -31)
        dx = -31;
    if (dy > 31)
        dy = 31;
    else if (dy < -31)
        dy = -31;
    apply_pupils((int8_t)dx, (int8_t)dy);
}

static void apply_brows(const sage_pose_t *p)
{
    // Each brow line lives in its own coordinate space (BROW_W wide).
    // base = vertical center of the line container so dy can swing both ways.
    // For the LEFT brow (on the user's left, screen left), "outer" is the
    // end nearest the temple = X=0; "inner" (toward nose) is X=BROW_W.
    // Right brow mirrors that.
    const int base = BROW_W / 2;

    s_brow_l_pts[0].x = 0;
    s_brow_l_pts[0].y = base + p->brow_outer_l_dy;
    s_brow_l_pts[1].x = BROW_W / 2;
    s_brow_l_pts[1].y = base + (p->brow_inner_l_dy + p->brow_outer_l_dy) / 2;
    s_brow_l_pts[2].x = BROW_W;
    s_brow_l_pts[2].y = base + p->brow_inner_l_dy;
    lv_line_set_points(s_p.brow_l, s_brow_l_pts, 3);

    s_brow_r_pts[0].x = 0;
    s_brow_r_pts[0].y = base + p->brow_inner_r_dy;
    s_brow_r_pts[1].x = BROW_W / 2;
    s_brow_r_pts[1].y = base + (p->brow_inner_r_dy + p->brow_outer_r_dy) / 2;
    s_brow_r_pts[2].x = BROW_W;
    s_brow_r_pts[2].y = base + p->brow_outer_r_dy;
    lv_line_set_points(s_p.brow_r, s_brow_r_pts, 3);
}

static void apply_mouth(int8_t curve, int8_t rest_open, uint8_t lipsync_open, int8_t lipsync_corner)
{
    const int center_y = MOUTH_W / 2;

    int open = rest_open + ((int)lipsync_open * MOUTH_MAX_OPEN) / 255;
    if (open > MOUTH_MAX_OPEN)
        open = MOUTH_MAX_OPEN;

    int corner_x_inset = -lipsync_corner;
    if (corner_x_inset < 0)
        corner_x_inset = 0;
    if (corner_x_inset > MOUTH_W / 4)
        corner_x_inset = MOUTH_W / 4;

    int corner_y = center_y - curve;
    int top_mid_y = center_y - open / 2;
    int bot_mid_y = center_y + open / 2;

    s_mouth_top_pts[0].x = corner_x_inset;
    s_mouth_top_pts[0].y = corner_y;
    s_mouth_top_pts[1].x = MOUTH_W / 2;
    s_mouth_top_pts[1].y = top_mid_y;
    s_mouth_top_pts[2].x = MOUTH_W - corner_x_inset;
    s_mouth_top_pts[2].y = corner_y;
    lv_line_set_points(s_p.mouth_top, s_mouth_top_pts, 3);

    s_mouth_bot_pts[0].x = corner_x_inset;
    s_mouth_bot_pts[0].y = corner_y;
    s_mouth_bot_pts[1].x = MOUTH_W / 2;
    s_mouth_bot_pts[1].y = bot_mid_y;
    s_mouth_bot_pts[2].x = MOUTH_W - corner_x_inset;
    s_mouth_bot_pts[2].y = corner_y;
    lv_line_set_points(s_p.mouth_bot, s_mouth_bot_pts, 3);
}

static void anim_third_eye_opa(void *obj, int32_t v)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void apply_third_eye_pulse(const sage_pose_t *p)
{
    if (!s_p.third_eye)
        return;
    lv_anim_del(s_p.third_eye, anim_third_eye_opa);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_p.third_eye);
    lv_anim_set_values(&a, p->third_eye_min_opa, p->third_eye_max_opa);
    lv_anim_set_time(&a, p->third_eye_period_ms);
    lv_anim_set_playback_time(&a, p->third_eye_period_ms);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, anim_third_eye_opa);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

// ============================================================================
// Physics-driven head motion
// ============================================================================
//
// The head container's translate_x/y is driven by a free 2-D particle in
// a box. There is intentionally NO central spring — once the head moves,
// it stays where it stops. Behaviors:
//   1. Persistent drift     — a tiny per-tick random nudge ("wander") on
//      velocity keeps the head gently floating around even at rest.
//   2. Tilt response        — raw accel minus gravity-EMA is fed as force,
//      so tilting transiently nudges the head while the EMA re-baselines.
//   3. Shake = rattle       — sharp shakes spike velocity hard; the head
//      rattles between the screen-edge walls (high restitution).
//   4. Listening auto-perk  — sub-shake handling motion (above wander
//      noise floor) bumps the face into the LISTENING pose for ~1.5 s.

#define PHYS_FORCE_PER_G 3000.0f    // px/s² of force per g of LINEAR accel
#define PHYS_DAMPING 0.5f           // 1/s; light enough that motion persists
#define PHYS_GRAV_TAU_S 0.6f        // gravity-EMA time constant
#define PHYS_WANDER_FORCE 6.0f      // px/s² random noise per tick
#define PHYS_MAX_VEL 450.0f         // px/s clamp (so violent shakes don't teleport)
#define PHYS_MAX_OFFSET_X 100.0f    // X wall in px from center
#define PHYS_MAX_OFFSET_Y_UP 130.0f // upward wall (negative Y; brows clip first)
#define PHYS_MAX_OFFSET_Y_DN 80.0f  // downward wall (positive Y; mouth clips first)
#define PHYS_BOUNCE 0.78f           // restitution at the wall (rattle-y)
// Sign overrides — flip these if the face slides opposite to the device
// motion. Tune empirically on hardware.
#define PHYS_AX_SIGN (1.0f)
#define PHYS_AY_SIGN (1.0f)

// Listening auto-perk: sub-shake motion above this magnitude (in g, on
// the linear-accel residual) extends the listening hold by HOLD_MS.
// LISTEN_HOLD_MS and the listening state vars are declared up near the
// top of the file so sage_face_set_listening() can see them.
#define LISTEN_TRIGGER_G 0.06f

static float s_phys_pos_x = 0.0f, s_phys_pos_y = 0.0f;
static float s_phys_vel_x = 0.0f, s_phys_vel_y = 0.0f;
static float s_phys_grav_x = 0.0f, s_phys_grav_y = 0.0f;
static bool s_phys_grav_primed = false;

static void physics_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_p.head)
        return;

    const float dt = PHYS_TICK_MS / 1000.0f;

    float a[3];
    QMI8658_read_acc_xyz(a); // m/s²
    float ax_g = (a[0] / ONE_G) * PHYS_AX_SIGN;
    float ay_g = (a[1] / ONE_G) * PHYS_AY_SIGN;

    // Gravity vector tracking (slow EMA). Prime on first sample so we
    // don't see a fake "shake" at boot.
    if (!s_phys_grav_primed)
    {
        s_phys_grav_x = ax_g;
        s_phys_grav_y = ay_g;
        s_phys_grav_primed = true;
    }
    const float galpha = dt / (PHYS_GRAV_TAU_S + dt);
    s_phys_grav_x += galpha * (ax_g - s_phys_grav_x);
    s_phys_grav_y += galpha * (ay_g - s_phys_grav_y);

    // Linear accel = total - gravity. This is what drives motion.
    const float lin_x = ax_g - s_phys_grav_x;
    const float lin_y = ay_g - s_phys_grav_y;

    // Random wander nudge so the head never fully stops.
    uint32_t r = get_rand_32();
    const float wx = (((int)(r & 0xFF)) - 127.5f) * (PHYS_WANDER_FORCE / 127.5f);
    const float wy = (((int)((r >> 8) & 0xFF)) - 127.5f) * (PHYS_WANDER_FORCE / 127.5f);

    // Sum of forces (per unit mass), all in px/s². No spring — head's
    // location persists wherever it ends up.
    float fx = lin_x * PHYS_FORCE_PER_G - PHYS_DAMPING * s_phys_vel_x + wx;
    float fy = lin_y * PHYS_FORCE_PER_G - PHYS_DAMPING * s_phys_vel_y + wy;

    // Semi-implicit Euler.
    s_phys_vel_x += fx * dt;
    s_phys_vel_y += fy * dt;

    // Velocity cap.
    if (s_phys_vel_x > PHYS_MAX_VEL)
        s_phys_vel_x = PHYS_MAX_VEL;
    if (s_phys_vel_x < -PHYS_MAX_VEL)
        s_phys_vel_x = -PHYS_MAX_VEL;
    if (s_phys_vel_y > PHYS_MAX_VEL)
        s_phys_vel_y = PHYS_MAX_VEL;
    if (s_phys_vel_y < -PHYS_MAX_VEL)
        s_phys_vel_y = -PHYS_MAX_VEL;

    s_phys_pos_x += s_phys_vel_x * dt;
    s_phys_pos_y += s_phys_vel_y * dt;

    // Bounce off the screen-edge clamps. Kill the velocity component
    // that's pushing INTO the wall.
    if (s_phys_pos_x > PHYS_MAX_OFFSET_X)
    {
        s_phys_pos_x = PHYS_MAX_OFFSET_X;
        if (s_phys_vel_x > 0)
            s_phys_vel_x = -s_phys_vel_x * PHYS_BOUNCE;
    }
    else if (s_phys_pos_x < -PHYS_MAX_OFFSET_X)
    {
        s_phys_pos_x = -PHYS_MAX_OFFSET_X;
        if (s_phys_vel_x < 0)
            s_phys_vel_x = -s_phys_vel_x * PHYS_BOUNCE;
    }
    if (s_phys_pos_y > PHYS_MAX_OFFSET_Y_DN)
    {
        s_phys_pos_y = PHYS_MAX_OFFSET_Y_DN;
        if (s_phys_vel_y > 0)
            s_phys_vel_y = -s_phys_vel_y * PHYS_BOUNCE;
    }
    else if (s_phys_pos_y < -PHYS_MAX_OFFSET_Y_UP)
    {
        s_phys_pos_y = -PHYS_MAX_OFFSET_Y_UP;
        if (s_phys_vel_y < 0)
            s_phys_vel_y = -s_phys_vel_y * PHYS_BOUNCE;
    }

    lv_obj_set_style_translate_x(s_p.head, (int32_t)s_phys_pos_x, 0);
    lv_obj_set_style_translate_y(s_p.head, (int32_t)s_phys_pos_y, 0);

    // ---- Listening auto-perk ----
    // Only manage pose when in IDLE / LISTENING; speaking poses own the
    // face during playback.
    if (s_pose_kind == SAGE_POSE_IDLE || s_pose_kind == SAGE_POSE_LISTENING)
    {
        const float lx = lin_x < 0 ? -lin_x : lin_x;
        const float ly = lin_y < 0 ? -lin_y : lin_y;
        if (lx + ly > LISTEN_TRIGGER_G)
        {
            s_listen_until_ms = lv_tick_get() + LISTEN_HOLD_MS;
        }
        bool listening = s_listen_external || lv_tick_get() < s_listen_until_ms;
        sage_pose_kind_t want = listening ? SAGE_POSE_LISTENING : SAGE_POSE_IDLE;
        if (want != s_pose_kind)
        {
            sage_face_set_pose(want);
        }
    }
}

// ============================================================================
// Idle gaze drift
// ============================================================================

static void anim_gaze_x(void *unused, int32_t v)
{
    (void)unused;
    s_gaze_dx = (int8_t)v;
    refresh_pupils();
}

static void anim_gaze_y(void *unused, int32_t v)
{
    (void)unused;
    s_gaze_dy = (int8_t)v;
    refresh_pupils();
}

static void gaze_timer_cb(lv_timer_t *t)
{
    // Re-jitter for the next shift: 1.2–3.2 s gives quick darting eye
    // movement that reads as actively looking around.
    uint32_t r = get_rand_32();
    lv_timer_set_period(t, 1200 + (r % 2000));

    int8_t tx = (int8_t)((int)((r >> 16) % 13) - 6); // -6..+6 px
    int8_t ty = (int8_t)((int)((r >> 24) % 9) - 4);  // -4..+4 px

    lv_anim_t ax;
    lv_anim_init(&ax);
    lv_anim_set_var(&ax, &s_gaze_dx);
    lv_anim_set_values(&ax, s_gaze_dx, tx);
    lv_anim_set_time(&ax, 350);
    lv_anim_set_exec_cb(&ax, anim_gaze_x);
    lv_anim_set_path_cb(&ax, lv_anim_path_ease_in_out);
    lv_anim_start(&ax);

    lv_anim_t ay;
    lv_anim_init(&ay);
    lv_anim_set_var(&ay, &s_gaze_dy);
    lv_anim_set_values(&ay, s_gaze_dy, ty);
    lv_anim_set_time(&ay, 350);
    lv_anim_set_exec_cb(&ay, anim_gaze_y);
    lv_anim_set_path_cb(&ay, lv_anim_path_ease_in_out);
    lv_anim_start(&ay);
}

// ============================================================================
// Cross-fade timer
// ============================================================================

static void xfade_timer_cb(lv_timer_t *t)
{
    s_xfade_t += (1000 / XFADE_TICKS);
    if (s_xfade_t > 1000)
        s_xfade_t = 1000;
    int32_t k = s_xfade_t;

    sage_pose_t lerped;
    lerped.brow_inner_l_dy = (int8_t)lerp_i32(s_pose_from.brow_inner_l_dy, s_pose_to.brow_inner_l_dy, k);
    lerped.brow_outer_l_dy = (int8_t)lerp_i32(s_pose_from.brow_outer_l_dy, s_pose_to.brow_outer_l_dy, k);
    lerped.brow_inner_r_dy = (int8_t)lerp_i32(s_pose_from.brow_inner_r_dy, s_pose_to.brow_inner_r_dy, k);
    lerped.brow_outer_r_dy = (int8_t)lerp_i32(s_pose_from.brow_outer_r_dy, s_pose_to.brow_outer_r_dy, k);
    lerped.eye_open_pct = (uint8_t)lerp_i32(s_pose_from.eye_open_pct, s_pose_to.eye_open_pct, k);
    lerped.pupil_dx = (int8_t)lerp_i32(s_pose_from.pupil_dx, s_pose_to.pupil_dx, k);
    lerped.pupil_dy = (int8_t)lerp_i32(s_pose_from.pupil_dy, s_pose_to.pupil_dy, k);
    lerped.mouth_curve = (int8_t)lerp_i32(s_pose_from.mouth_curve, s_pose_to.mouth_curve, k);
    lerped.mouth_rest_open = (int8_t)lerp_i32(s_pose_from.mouth_rest_open, s_pose_to.mouth_rest_open, k);
    // Third-eye anim isn't lerped — already restarted in set_pose. Head
    // motion is handled by physics_timer_cb and is independent of pose.
    lerped.third_eye_min_opa = s_pose_to.third_eye_min_opa;
    lerped.third_eye_max_opa = s_pose_to.third_eye_max_opa;
    lerped.third_eye_period_ms = s_pose_to.third_eye_period_ms;

    apply_pose_visuals(&lerped);

    if (s_xfade_t >= 1000)
    {
        lv_timer_del(t);
        s_xfade_timer = NULL;
    }
}

// ============================================================================
// Blink scheduler
// ============================================================================

static void blink_anim_exec(void *unused, int32_t v)
{
    (void)unused;
    int h = clampi((EYE_W * v) / 100, EYE_MIN_H, EYE_W);
    lv_obj_set_height(s_p.eye_l, h);
    lv_obj_set_height(s_p.eye_r, h);
    lv_obj_align(s_p.eye_l, LV_ALIGN_CENTER, -EYE_X_OFFSET, EYE_Y_OFFSET);
    lv_obj_align(s_p.eye_r, LV_ALIGN_CENTER, EYE_X_OFFSET, EYE_Y_OFFSET);
}

static void blink_anim_done(lv_anim_t *a)
{
    (void)a;
    s_blinking = false;
    apply_eye_open(s_pose_now.eye_open_pct);
}

static void blink_timer_cb(lv_timer_t *t)
{
    lv_timer_set_period(t, blink_jitter_ms());

    if (s_blinking)
        return;
    s_blinking = true;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_p.eye_l); // var unused by exec_cb, lv_anim wants something
    lv_anim_set_values(&a, s_pose_now.eye_open_pct, 5);
    lv_anim_set_time(&a, 60);
    lv_anim_set_playback_time(&a, 60);
    lv_anim_set_exec_cb(&a, blink_anim_exec);
    lv_anim_set_ready_cb(&a, blink_anim_done);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static uint32_t blink_jitter_ms(void)
{
    return 3000 + (get_rand_32() % 4000); // 3..7 s
}

// ============================================================================
// Helpers
// ============================================================================

static lv_obj_t *make_filled_circle(lv_obj_t *parent, int dia, int xoff, int yoff, lv_color_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, dia, dia);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(o, LV_ALIGN_CENTER, xoff, yoff);
    return o;
}

static lv_obj_t *make_line(lv_obj_t *parent, lv_point_t *pts, int xoff, int yoff, lv_color_t color, int thick)
{
    pts[0].x = 0;
    pts[0].y = 0;
    pts[1].x = 1;
    pts[1].y = 0;
    pts[2].x = 2;
    pts[2].y = 0;

    lv_obj_t *line = lv_line_create(parent);
    lv_line_set_points(line, pts, 3);
    lv_obj_set_style_line_color(line, color, 0);
    lv_obj_set_style_line_width(line, thick, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_align(line, LV_ALIGN_CENTER, xoff, yoff);
    return line;
}

static int32_t lerp_i32(int32_t a, int32_t b, int32_t t1000)
{
    return a + ((b - a) * t1000) / 1000;
}

static int clampi(int v, int lo, int hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}
