// Tarot-card sage face.
//
// The screen is a tarot card: a static gold frame (borders, corner stars,
// sun rays, halo, caption plate) with an engraved line-art face floating in
// the middle. Everything is pure LVGL — no image assets — and the static
// frame is a single custom-drawn object so it costs no per-element memory.
//
// Layout:
//   screen
//   ├── root               full-screen; opacity drives the boot fade-in
//   │   ├── card           static frame, drawn by card_draw_cb()
//   │   ├── sparkles       burst pool, radiates from the third eye
//   │   ├── head           face-sized box; physics nudges it a few px
//   │   │   ├── third_eye  triangle + inner eye, pulses via opacity
//   │   │   ├── brow_l / brow_r
//   │   │   ├── eye_win_l / eye_win_r   clip iris + pupil to the lid gap
//   │   │   ├── lid_*      top/bottom lid curves, drawn over the windows
//   │   │   ├── nose
//   │   │   └── mouth_top / mouth_bot
//   │   ├── title          "THE SAGE" on the plate at boot, then fades out
//   │   └── caption        response text on the plate while speaking
//   └── status pills       battery / volume, above everything
//
// Redraw budget: the panel is a single full-frame buffer pushed over QSPI,
// so every invalidated pixel costs real time. The head box is kept tight and
// every animated setter skips work when its output hasn't changed.

#include "sage_face.h"
#include "lvgl_glue.h"
#include "screen_orient.h"
#include "lvgl.h"

#include "QMI8658.h"
#include "pico/rand.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(font_cinzel_17);
LV_FONT_DECLARE(font_cinzel_22);

// ------------------------------------------------------------------- palette

#define COL_GOLD lv_color_hex(0xE0B85E)
#define COL_GOLD_LIGHT lv_color_hex(0xF3D58A)
#define COL_GOLD_DARK lv_color_hex(0x8A6A2E)
#define COL_BRONZE lv_color_hex(0x6B5426)

// ------------------------------------------------------------- card geometry
// Screen coordinates (368 x 448).

#define SCR_CX (DISP_HOR_RES / 2)
#define SCR_CY (DISP_VER_RES / 2)

#define CARD_OUTER_INSET 14
#define CARD_OUTER_RADIUS 30
#define CARD_OUTER_W 2
#define CARD_INNER_INSET 22
#define CARD_INNER_RADIUS 24

#define STAR_INSET_X 38
#define STAR_INSET_Y 52
#define STAR_ARM 12  // center to tip
#define STAR_WAIST 3 // half-width of each diamond

#define SUN_CY 196 // shared center of the rays and the halo
#define HALO_R 100
#define RAY_COUNT 36
#define RAY_R_IN 116
#define RAY_R_OUT_LONG 154
#define RAY_R_OUT_SHORT 136
#define RAY_MAX_Y 326   // rays tuck behind the plate
#define RAY_SKIP_Y 300  // drop rays that would start under the mouth
#define RAY_MIN_LEN 12

#define PLATE_TOP_Y 338
#define PLATE_BOT_Y 418
#define PLATE_CY ((PLATE_TOP_Y + PLATE_BOT_Y) / 2)
#define PLATE_RULE_X0 52
#define PLATE_RULE_X1 (DISP_HOR_RES - PLATE_RULE_X0)
#define PLATE_TEXT_W 264 // widest column that still fits every caption in 3 lines
#define TITLE_LETTER_SPACE 6

// ------------------------------------------------------------- face geometry
// Offsets are from the head box center unless noted.

#define HEAD_CY 206 // screen y of the head box center
#define HEAD_W 200
#define HEAD_H 250

#define LINE_W 3 // stroke for brows, lids, mouth

#define THIRD_EYE_DY (-87)
#define THIRD_EYE_BOX_W 60
#define THIRD_EYE_BOX_H 52

#define BROW_W 56
#define BROW_DX 46
#define BROW_DY (-28)
#define BROW_ARCH 5
#define BROW_PT_COUNT 5
#define BROW_BOX_W (BROW_W + 2 * LINE_W)
#define BROW_BOX_H 40

#define EYE_W 60
#define EYE_DX 46
#define EYE_DY 6
#define LID_TOP_MAX 13 // top lid bulge at 100% open
#define LID_BOT_MAX 11
#define LID_PT_COUNT 7
#define EYE_BOX_W (EYE_W + 2 * LINE_W)
#define EYE_BOX_H (2 * LID_TOP_MAX + 2 * LINE_W)
#define EYE_MIN_GAP 3 // below this the iris window is hidden

#define IRIS_DIA 24
#define IRIS_RING_W 2
#define PUPIL_DIA 9
#define GAZE_MAX_X ((EYE_W - IRIS_DIA) / 2)
#define GAZE_MAX_Y 8

#define NOSE_DY 39
#define NOSE_BOX_W 16
#define NOSE_BOX_H 40

#define MOUTH_W 56
#define MOUTH_DY 86
#define MOUTH_MAX_OPEN 28 // peak lip gap at full lipsync drive
#define MOUTH_LENS 3      // resting droop of the lower lip (closed lens shape)
#define MOUTH_CURVE_MAX 8
#define MOUTH_CORNER_RANGE 10
#define MOUTH_PT_COUNT 7
#define MOUTH_BOX_W (MOUTH_W + 2 * LINE_W)
#define MOUTH_BOX_H 70

// Sparkle burst (sage_face_burst_sparkles). Pool of small gold dots behind
// the face; each burst gives every dot a fresh angle / distance / delay /
// size and animates it outward from the third eye.
#define SPARKLE_COUNT 24
#define SPARKLE_ORIGIN_Y 124 // screen y of the third eye's pupil
#define SPARKLE_DIA_MIN 4
#define SPARKLE_DIA_MAX 10
#define SPARKLE_TRAVEL_MIN 110
#define SPARKLE_TRAVEL_MAX 230
#define SPARKLE_DURATION_MS 1500
#define SPARKLE_DELAY_MAX_MS 220

#define POSE_XFADE_MS 250
#define STAR_TWINKLE_MS 50

// --------------------------------------------------------------- pose preset

typedef struct
{
    // Eyebrows. inner = end nearest the nose, outer = end nearest the
    // temple. Negative Y = up. Per-side values allow one raised brow.
    int8_t brow_inner_l_dy, brow_outer_l_dy;
    int8_t brow_inner_r_dy, brow_outer_r_dy;

    uint8_t eye_open_pct;      // 0 closed, 100 fully open
    int8_t pupil_dx, pupil_dy; // gaze nudge

    int8_t mouth_curve;     // -ve frown, +ve smile (px of sag at the middle)
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
        .eye_open_pct = 45,
        .pupil_dx = 0,
        .pupil_dy = 0,
        .mouth_curve = 2,
        .mouth_rest_open = 0,
        .third_eye_min_opa = 110,
        .third_eye_max_opa = 255,
        .third_eye_period_ms = 1500,
    },
    [SAGE_POSE_LISTENING] = {
        // Perked up: eyes wide, brows raised, third eye quick and bright.
        .brow_inner_l_dy = -4,
        .brow_outer_l_dy = -3,
        .brow_inner_r_dy = -4,
        .brow_outer_r_dy = -3,
        .eye_open_pct = 100,
        .pupil_dx = 0,
        .pupil_dy = -2,
        .mouth_curve = 1,
        .mouth_rest_open = 0,
        .third_eye_min_opa = 170,
        .third_eye_max_opa = 255,
        .third_eye_period_ms = 600,
    },
    [SAGE_POSE_SPEAKING_OMINOUS] = {
        .brow_inner_l_dy = 6,
        .brow_outer_l_dy = -2,
        .brow_inner_r_dy = 6,
        .brow_outer_r_dy = -2,
        .eye_open_pct = 55,
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
        .mouth_curve = 6,
        .mouth_rest_open = 1,
        .third_eye_min_opa = 160,
        .third_eye_max_opa = 240,
        .third_eye_period_ms = 900,
    },
};

// ------------------------------------------------------------------- runtime

typedef struct
{
    lv_obj_t *root;
    lv_obj_t *card;
    lv_obj_t *head;
    lv_obj_t *third_eye;
    lv_obj_t *brow_l, *brow_r;
    lv_obj_t *eye_win_l, *eye_win_r;
    lv_obj_t *iris_l, *iris_r;
    lv_obj_t *pupil_l, *pupil_r;
    lv_obj_t *lid_top_l, *lid_bot_l, *lid_top_r, *lid_bot_r;
    lv_obj_t *nose;
    lv_obj_t *mouth_top, *mouth_bot;
    lv_obj_t *title;
    lv_obj_t *caption;
} sage_parts_t;

static sage_parts_t s_p;

// lv_line keeps the pointer we hand it (no copy), so every point array is
// static and lv_line_set_points() is called after mutating it.
static lv_point_t s_brow_l_pts[BROW_PT_COUNT];
static lv_point_t s_brow_r_pts[BROW_PT_COUNT];
static lv_point_t s_lid_top_pts[LID_PT_COUNT]; // shared by both eyes
static lv_point_t s_lid_bot_pts[LID_PT_COUNT];
static lv_point_t s_mouth_top_pts[MOUTH_PT_COUNT];
static lv_point_t s_mouth_bot_pts[MOUTH_PT_COUNT];

static lv_point_t s_third_eye_tri[4] = {{30, 3}, {56, 47}, {4, 47}, {30, 3}};
static lv_point_t s_third_eye_lid_top[5] = {{18, 35}, {24, 31}, {30, 30}, {36, 31}, {42, 35}};
static lv_point_t s_third_eye_lid_bot[5] = {{18, 35}, {24, 39}, {30, 40}, {36, 39}, {42, 35}};
static lv_point_t s_nose_pts[3] = {{8, 2}, {2, 37}, {14, 37}};

// Card frame, precomputed once in init.
static lv_point_t s_rays[RAY_COUNT][2];
static int s_ray_count = 0;
static uint8_t s_star_opa[4] = {255, 255, 255, 255};
static uint16_t s_star_phase = 0;
static lv_timer_t *s_star_timer = NULL;

static sage_pose_kind_t s_pose_kind = SAGE_POSE_IDLE;
static sage_pose_t s_pose_now;
static sage_pose_t s_pose_from;
static sage_pose_t s_pose_to;

static uint8_t s_lipsync_open_smoothed = 0;
static int8_t s_lipsync_corner_smoothed = 0;

// Idle gaze drift: small always-on pupil offset, lerped to a fresh random
// target every few seconds, layered on top of the pose's gaze.
static int8_t s_gaze_dx = 0;
static int8_t s_gaze_dy = 0;
static lv_timer_t *s_gaze_timer = NULL;

static lv_timer_t *s_blink_timer = NULL;
static lv_timer_t *s_xfade_timer = NULL;
static int32_t s_xfade_t = 0;
static const int32_t XFADE_STEP_MS = 16;
static const int32_t XFADE_TICKS = POSE_XFADE_MS / 16;

// Last values pushed to LVGL, so unchanged updates don't trigger redraws.
#define UNSET (-32768)
static int16_t s_lid_top_now = UNSET, s_lid_bot_now = UNSET;
static int16_t s_eye_win_dy = 0;
static int16_t s_pupil_dx_now = UNSET, s_pupil_dy_now = UNSET, s_pupil_win_dy_now = UNSET;
static int32_t s_head_tx = UNSET, s_head_ty = UNSET;

#define PHYS_TICK_MS 20
static lv_timer_t *s_phys_timer = NULL;

// Listening / perked state. Auto-decay timestamp is set both by external
// callers (sage_face_set_listening) and by IMU motion in the physics tick.
#define LISTEN_HOLD_MS 1500
static uint32_t s_listen_until_ms = 0;
static bool s_listen_external = false;

#define TEXT_FADE_MS 300

// "THE SAGE" is a boot-only title: it fades in with the face, holds, then
// fades out for good. Shaking before then just fades it out early.
#define TITLE_HOLD_MS 4000
#define TITLE_FADE_OUT_MS 1200
static lv_timer_t *s_title_timer = NULL;

// Top status bar: battery on the left, volume tier on the right. Hidden by
// default; the volume button pops it up briefly, then it auto-fades.
#define STATUS_BAR_FADE_MS 200
#define STATUS_BAR_HOLD_MS 1000
#define STATUS_BAR_TOP_PAD 12
#define STATUS_BAR_SIDE_PAD 12
#define STATUS_BAR_PILL_PAD_X 10
#define STATUS_BAR_PILL_PAD_Y 4
static lv_obj_t *s_status_left_pill = NULL;
static lv_obj_t *s_status_left_label = NULL;
static lv_obj_t *s_status_right_pill = NULL;
static lv_obj_t *s_status_right_label = NULL;
static lv_timer_t *s_status_hide_timer = NULL;

// True while the blink anim owns the lids; pose updates must not fight it.
static bool s_blinking = false;
static int32_t s_blink_anim_var; // lv_anim needs a unique var pointer

typedef struct
{
    int16_t end_dx;
    int16_t end_dy;
} sparkle_target_t;

static lv_obj_t *s_sparkle_container = NULL;
static lv_obj_t *s_sparkles[SPARKLE_COUNT];
static sparkle_target_t s_sparkle_targets[SPARKLE_COUNT];

// ----------------------------------------------------------------- prototypes

static void card_build_geometry(void);
static void card_draw_cb(lv_event_t *e);
static void star_twinkle_cb(lv_timer_t *t);
static void star_area(int k, lv_area_t *a);

static void apply_pose_visuals(const sage_pose_t *p);
static void apply_eye_open(uint8_t pct);
static void refresh_pupils(void);
static void apply_brows(const sage_pose_t *p);
static void apply_mouth(int8_t curve, int8_t rest_open, uint8_t lipsync_open, int8_t lipsync_corner);
static void apply_third_eye_pulse(const sage_pose_t *p);

static void blink_timer_cb(lv_timer_t *t);
static void blink_anim_exec(void *unused, int32_t v);
static void blink_anim_done(lv_anim_t *a);
static void xfade_timer_cb(lv_timer_t *t);
static void physics_timer_cb(lv_timer_t *t);
static void gaze_timer_cb(lv_timer_t *t);
static void anim_gaze_x(void *unused, int32_t v);
static void anim_gaze_y(void *unused, int32_t v);

static void anim_obj_opa(void *obj, int32_t v);
static void fade_obj(lv_obj_t *obj, lv_opa_t target, uint32_t ms);

static lv_obj_t *make_box(lv_obj_t *parent, int w, int h);
static lv_obj_t *make_circle(lv_obj_t *parent, int dia, lv_color_t color);
static lv_obj_t *make_line_box(lv_obj_t *parent, lv_point_t *pts, uint16_t cnt,
                               int w, int h, int dx, int dy, lv_color_t color, int width);

static int32_t lerp_i32(int32_t a, int32_t b, int32_t t1000);
static int clampi(int v, int lo, int hi);
static int32_t sin_scale(int16_t deg, int32_t amp);
static uint32_t blink_jitter_ms(void);

// ============================================================================
// Init
// ============================================================================

void sage_face_init(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Root starts invisible; the boot sequence calls sage_face_fade_in().
    s_p.root = make_box(scr, DISP_HOR_RES, DISP_VER_RES);
    lv_obj_align(s_p.root, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_opa(s_p.root, LV_OPA_TRANSP, 0);

    // ---- Card frame (static) ----
    card_build_geometry();
    s_p.card = make_box(s_p.root, DISP_HOR_RES, DISP_VER_RES);
    lv_obj_align(s_p.card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(s_p.card, card_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    s_star_timer = lv_timer_create(star_twinkle_cb, STAR_TWINKLE_MS, NULL);

    // ---- Sparkle pool (behind the face) ----
    s_sparkle_container = make_box(s_p.root, DISP_HOR_RES, DISP_VER_RES);
    lv_obj_align(s_sparkle_container, LV_ALIGN_CENTER, 0, 0);
    for (int i = 0; i < SPARKLE_COUNT; i++)
    {
        s_sparkles[i] = make_circle(s_sparkle_container, SPARKLE_DIA_MAX, COL_GOLD_LIGHT);
        lv_obj_align(s_sparkles[i], LV_ALIGN_CENTER, 0, SPARKLE_ORIGIN_Y - SCR_CY);
        lv_obj_set_user_data(s_sparkles[i], (void *)(intptr_t)i);
        lv_obj_set_style_bg_opa(s_sparkles[i], LV_OPA_TRANSP, 0);
    }

    // ---- Head (moves) ----
    s_p.head = make_box(s_p.root, HEAD_W, HEAD_H);
    lv_obj_align(s_p.head, LV_ALIGN_CENTER, 0, HEAD_CY - SCR_CY);

    // Third eye: a soft wide stroke under the triangle fakes a glow.
    s_p.third_eye = make_box(s_p.head, THIRD_EYE_BOX_W, THIRD_EYE_BOX_H);
    lv_obj_align(s_p.third_eye, LV_ALIGN_CENTER, 0, THIRD_EYE_DY);
    lv_obj_t *tri_glow = make_line_box(s_p.third_eye, s_third_eye_tri, 4,
                                       THIRD_EYE_BOX_W, THIRD_EYE_BOX_H, 0, 0, COL_GOLD_DARK, 6);
    lv_obj_set_style_line_opa(tri_glow, LV_OPA_40, 0);
    make_line_box(s_p.third_eye, s_third_eye_tri, 4,
                  THIRD_EYE_BOX_W, THIRD_EYE_BOX_H, 0, 0, COL_GOLD, 2);
    make_line_box(s_p.third_eye, s_third_eye_lid_top, 5,
                  THIRD_EYE_BOX_W, THIRD_EYE_BOX_H, 0, 0, COL_GOLD_LIGHT, 2);
    make_line_box(s_p.third_eye, s_third_eye_lid_bot, 5,
                  THIRD_EYE_BOX_W, THIRD_EYE_BOX_H, 0, 0, COL_GOLD_LIGHT, 2);
    lv_obj_t *third_pupil = make_circle(s_p.third_eye, 6, COL_GOLD_LIGHT);
    lv_obj_set_pos(third_pupil, 27, 32);

    s_p.brow_l = make_line_box(s_p.head, s_brow_l_pts, BROW_PT_COUNT,
                               BROW_BOX_W, BROW_BOX_H, -BROW_DX, BROW_DY, COL_GOLD, LINE_W);
    s_p.brow_r = make_line_box(s_p.head, s_brow_r_pts, BROW_PT_COUNT,
                               BROW_BOX_W, BROW_BOX_H, BROW_DX, BROW_DY, COL_GOLD, LINE_W);

    // Eye windows clip the iris + pupil to the current lid gap. They are
    // rectangles, but the iris sits where the lid curve is nearly flat, so
    // the overshoot is under a pixel and hidden by the lid stroke.
    s_p.eye_win_l = make_box(s_p.head, EYE_W, LID_TOP_MAX + LID_BOT_MAX);
    s_p.eye_win_r = make_box(s_p.head, EYE_W, LID_TOP_MAX + LID_BOT_MAX);
    lv_obj_t *wins[2] = {s_p.eye_win_l, s_p.eye_win_r};
    lv_obj_t **irises[2] = {&s_p.iris_l, &s_p.iris_r};
    lv_obj_t **pupils[2] = {&s_p.pupil_l, &s_p.pupil_r};
    for (int i = 0; i < 2; i++)
    {
        lv_obj_t *iris = make_box(wins[i], IRIS_DIA, IRIS_DIA);
        lv_obj_set_style_radius(iris, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(iris, COL_GOLD_LIGHT, 0);
        lv_obj_set_style_border_width(iris, IRIS_RING_W, 0);
        lv_obj_set_style_border_opa(iris, LV_OPA_COVER, 0);
        *irises[i] = iris;
        *pupils[i] = make_circle(wins[i], PUPIL_DIA, COL_GOLD_LIGHT);
    }

    s_p.lid_top_l = make_line_box(s_p.head, s_lid_top_pts, LID_PT_COUNT,
                                  EYE_BOX_W, EYE_BOX_H, -EYE_DX, EYE_DY, COL_GOLD, LINE_W);
    s_p.lid_bot_l = make_line_box(s_p.head, s_lid_bot_pts, LID_PT_COUNT,
                                  EYE_BOX_W, EYE_BOX_H, -EYE_DX, EYE_DY, COL_GOLD, LINE_W);
    s_p.lid_top_r = make_line_box(s_p.head, s_lid_top_pts, LID_PT_COUNT,
                                  EYE_BOX_W, EYE_BOX_H, EYE_DX, EYE_DY, COL_GOLD, LINE_W);
    s_p.lid_bot_r = make_line_box(s_p.head, s_lid_bot_pts, LID_PT_COUNT,
                                  EYE_BOX_W, EYE_BOX_H, EYE_DX, EYE_DY, COL_GOLD, LINE_W);

    s_p.nose = make_line_box(s_p.head, s_nose_pts, 3,
                             NOSE_BOX_W, NOSE_BOX_H, 0, NOSE_DY, COL_GOLD_DARK, 2);

    s_p.mouth_top = make_line_box(s_p.head, s_mouth_top_pts, MOUTH_PT_COUNT,
                                  MOUTH_BOX_W, MOUTH_BOX_H, 0, MOUTH_DY, COL_GOLD, LINE_W);
    s_p.mouth_bot = make_line_box(s_p.head, s_mouth_bot_pts, MOUTH_PT_COUNT,
                                  MOUTH_BOX_W, MOUTH_BOX_H, 0, MOUTH_DY, COL_GOLD, LINE_W);

    // ---- Plate text ----
    s_p.title = lv_label_create(s_p.root);
    lv_obj_set_style_text_font(s_p.title, &font_cinzel_22, 0);
    lv_obj_set_style_text_color(s_p.title, COL_GOLD, 0);
    lv_obj_set_style_text_letter_space(s_p.title, TITLE_LETTER_SPACE, 0);
    lv_label_set_text_static(s_p.title, "THE SAGE");
    lv_obj_align(s_p.title, LV_ALIGN_CENTER, 0, PLATE_CY - SCR_CY);

    s_p.caption = lv_label_create(s_p.root);
    lv_obj_set_width(s_p.caption, PLATE_TEXT_W);
    lv_label_set_long_mode(s_p.caption, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_p.caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_p.caption, &font_cinzel_17, 0);
    lv_obj_set_style_text_color(s_p.caption, COL_GOLD_LIGHT, 0);
    lv_obj_set_style_text_line_space(s_p.caption, 2, 0);
    lv_label_set_text_static(s_p.caption, "");
    lv_obj_align(s_p.caption, LV_ALIGN_CENTER, 0, PLATE_CY - SCR_CY);
    lv_obj_set_style_opa(s_p.caption, LV_OPA_TRANSP, 0);

    // ---- Status pills (on the screen, above the card) ----
    for (int side = 0; side < 2; side++)
    {
        lv_obj_t *pill = make_box(scr, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(pill, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(pill, LV_OPA_80, 0);
        lv_obj_set_style_border_color(pill, COL_BRONZE, 0);
        lv_obj_set_style_border_width(pill, 1, 0);
        lv_obj_set_style_border_opa(pill, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_hor(pill, STATUS_BAR_PILL_PAD_X, 0);
        lv_obj_set_style_pad_ver(pill, STATUS_BAR_PILL_PAD_Y, 0);
        lv_obj_set_style_opa(pill, LV_OPA_TRANSP, 0);

        lv_obj_t *label = lv_label_create(pill);
        lv_obj_set_style_text_color(label, COL_GOLD_LIGHT, 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
        lv_label_set_text(label, "");

        if (side == 0)
        {
            lv_obj_align(pill, LV_ALIGN_TOP_LEFT, STATUS_BAR_SIDE_PAD, STATUS_BAR_TOP_PAD);
            s_status_left_pill = pill;
            s_status_left_label = label;
        }
        else
        {
            lv_obj_align(pill, LV_ALIGN_TOP_RIGHT, -STATUS_BAR_SIDE_PAD, STATUS_BAR_TOP_PAD);
            s_status_right_pill = pill;
            s_status_right_label = label;
        }
    }

    // Snap to idle pose, no cross-fade.
    s_pose_kind = SAGE_POSE_IDLE;
    s_pose_now = s_poses[SAGE_POSE_IDLE];
    apply_pose_visuals(&s_pose_now);
    apply_third_eye_pulse(&s_pose_now);

    s_blink_timer = lv_timer_create(blink_timer_cb, blink_jitter_ms(), NULL);
    s_gaze_timer = lv_timer_create(gaze_timer_cb, 2000, NULL);
    s_phys_timer = lv_timer_create(physics_timer_cb, PHYS_TICK_MS, NULL);

    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    printf("[face] tarot face up; lv_mem used=%lu free=%lu frag=%u%%\r\n",
           (unsigned long)(mon.total_size - mon.free_size),
           (unsigned long)mon.free_size, (unsigned)mon.frag_pct);
}

// ============================================================================
// Card frame
// ============================================================================

static void card_build_geometry(void)
{
    s_ray_count = 0;
    for (int i = 0; i < RAY_COUNT; i++)
    {
        int16_t ang = (int16_t)((i * 360) / RAY_COUNT);
        int32_t c = lv_trigo_sin((int16_t)(ang + 90));
        int32_t s = lv_trigo_sin(ang);

        int r_out = (i & 1) ? RAY_R_OUT_SHORT : RAY_R_OUT_LONG;
        int y_in = SUN_CY + (int)((s * RAY_R_IN) >> LV_TRIGO_SHIFT);
        if (y_in > RAY_SKIP_Y)
            continue;
        if (s > 0)
        {
            int lim = (int)(((int32_t)(RAY_MAX_Y - SUN_CY) << LV_TRIGO_SHIFT) / s);
            if (r_out > lim)
                r_out = lim;
        }
        if (r_out - RAY_R_IN < RAY_MIN_LEN)
            continue;

        s_rays[s_ray_count][0].x = (lv_coord_t)(SCR_CX + ((c * RAY_R_IN) >> LV_TRIGO_SHIFT));
        s_rays[s_ray_count][0].y = (lv_coord_t)y_in;
        s_rays[s_ray_count][1].x = (lv_coord_t)(SCR_CX + ((c * r_out) >> LV_TRIGO_SHIFT));
        s_rays[s_ray_count][1].y = (lv_coord_t)(SUN_CY + ((s * r_out) >> LV_TRIGO_SHIFT));
        s_ray_count++;
    }
}

static void star_center(int k, lv_coord_t *x, lv_coord_t *y)
{
    *x = (k & 1) ? (DISP_HOR_RES - STAR_INSET_X) : STAR_INSET_X;
    *y = (k & 2) ? (DISP_VER_RES - STAR_INSET_Y) : STAR_INSET_Y;
}

static void star_area(int k, lv_area_t *a)
{
    lv_coord_t x, y;
    star_center(k, &x, &y);
    a->x1 = x - STAR_ARM - 2;
    a->y1 = y - STAR_ARM - 2;
    a->x2 = x + STAR_ARM + 2;
    a->y2 = y + STAR_ARM + 2;
}

static void card_draw_cb(lv_event_t *e)
{
    const lv_area_t *clip = lv_event_get_param(e);

    // Double border.
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_opa = LV_OPA_TRANSP;
    rd.border_color = COL_GOLD;
    rd.border_width = CARD_OUTER_W;
    rd.radius = CARD_OUTER_RADIUS;
    lv_area_t a = {CARD_OUTER_INSET, CARD_OUTER_INSET,
                   DISP_HOR_RES - 1 - CARD_OUTER_INSET, DISP_VER_RES - 1 - CARD_OUTER_INSET};
    lv_draw_rect(&a, clip, &rd);

    rd.border_color = COL_BRONZE;
    rd.border_width = 1;
    rd.radius = CARD_INNER_RADIUS;
    a.x1 = CARD_INNER_INSET;
    a.y1 = CARD_INNER_INSET;
    a.x2 = DISP_HOR_RES - 1 - CARD_INNER_INSET;
    a.y2 = DISP_VER_RES - 1 - CARD_INNER_INSET;
    lv_draw_rect(&a, clip, &rd);

    // Sun rays and the halo behind the head.
    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = COL_BRONZE;
    ld.width = 2;
    for (int i = 0; i < s_ray_count; i++)
    {
        lv_draw_line(&s_rays[i][0], &s_rays[i][1], clip, &ld);
    }

    lv_draw_arc_dsc_t ad;
    lv_draw_arc_dsc_init(&ad);
    ad.color = COL_GOLD_DARK;
    ad.width = 2;
    lv_draw_arc(SCR_CX, SUN_CY, HALO_R, 180, 360, clip, &ad);

    // Plate rules.
    ld.width = 1;
    lv_point_t p1 = {PLATE_RULE_X0, PLATE_TOP_Y}, p2 = {PLATE_RULE_X1, PLATE_TOP_Y};
    lv_draw_line(&p1, &p2, clip, &ld);
    p1.y = p2.y = PLATE_BOT_Y;
    lv_draw_line(&p1, &p2, clip, &ld);

    // Corner stars: two crossed convex diamonds each (lv_draw_polygon only
    // handles convex shapes).
    lv_draw_rect_dsc_t sd;
    lv_draw_rect_dsc_init(&sd);
    sd.bg_color = COL_GOLD;
    for (int k = 0; k < 4; k++)
    {
        lv_coord_t x, y;
        star_center(k, &x, &y);
        sd.bg_opa = s_star_opa[k];
        lv_point_t v[4] = {{x, y - STAR_ARM}, {x + STAR_WAIST, y}, {x, y + STAR_ARM}, {x - STAR_WAIST, y}};
        lv_point_t h[4] = {{x - STAR_ARM, y}, {x, y - STAR_WAIST}, {x + STAR_ARM, y}, {x, y + STAR_WAIST}};
        lv_draw_polygon(v, 4, clip, &sd);
        lv_draw_polygon(h, 4, clip, &sd);
    }
}

static void star_twinkle_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_p.card)
        return;
    s_star_phase = (uint16_t)((s_star_phase + 6) % 360); // ~3 s cycle
    for (int k = 0; k < 4; k++)
    {
        int16_t ang = (int16_t)((s_star_phase + k * 97) % 360);
        int32_t s = lv_trigo_sin(ang); // -32767..32767
        uint8_t opa = (uint8_t)(140 + (((s + 32767) * 115) >> 16));
        if (opa != s_star_opa[k])
        {
            s_star_opa[k] = opa;
            lv_area_t a;
            star_area(k, &a);
            lv_obj_invalidate_area(s_p.card, &a);
        }
    }
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

    // The pulse isn't lerped; it just restarts with the new pose's params.
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

    // ZCR maps to mouth-corner pull. High ZCR (sibilants) leaves corners
    // outward; low ZCR (rounded vowels) pulls them inward. One step per
    // call so it visibly lags the open dimension.
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
        // Extend the auto-decay window so a brief mic event keeps the face
        // perked for at least HOLD_MS even after the caller sets `false`.
        s_listen_until_ms = lv_tick_get() + LISTEN_HOLD_MS;
    }
}

static void title_dismiss(uint32_t fade_ms)
{
    if (s_title_timer)
    {
        lv_timer_del(s_title_timer);
        s_title_timer = NULL;
    }
    fade_obj(s_p.title, LV_OPA_TRANSP, fade_ms);
}

static void title_timer_cb(lv_timer_t *t)
{
    (void)t;
    s_title_timer = NULL; // one-shot; LVGL deletes it after this returns
    fade_obj(s_p.title, LV_OPA_TRANSP, TITLE_FADE_OUT_MS);
}

void sage_face_show_response(const char *text)
{
    if (!s_p.caption)
        return;
    if (!text)
    {
        sage_face_hide_response();
        return;
    }
    // text pointer must outlive the caption — callers pass string literals
    // or pointers into the static sage_text_table.
    lv_label_set_text_static(s_p.caption, text);
    title_dismiss(TEXT_FADE_MS);
    fade_obj(s_p.caption, LV_OPA_COVER, TEXT_FADE_MS);
}

void sage_face_hide_response(void)
{
    if (!s_p.caption)
        return;
    fade_obj(s_p.caption, LV_OPA_TRANSP, TEXT_FADE_MS);
}

// ----- top status bar (auto-hiding) ---------------------------------------

static void status_bar_hide_cb(lv_timer_t *t)
{
    (void)t;
    fade_obj(s_status_left_pill, LV_OPA_TRANSP, STATUS_BAR_FADE_MS);
    fade_obj(s_status_right_pill, LV_OPA_TRANSP, STATUS_BAR_FADE_MS);
    s_status_hide_timer = NULL;
}

void sage_face_show_status_bar(const char *left_text, const char *right_text)
{
    if (!s_status_left_label || !s_status_right_label)
        return;

    lv_label_set_text(s_status_left_label, left_text ? left_text : "");
    lv_label_set_text(s_status_right_label, right_text ? right_text : "");

    // Re-align after the text change so the right pill stays anchored to
    // the right edge as its width changes.
    lv_obj_align(s_status_left_pill, LV_ALIGN_TOP_LEFT,
                 STATUS_BAR_SIDE_PAD, STATUS_BAR_TOP_PAD);
    lv_obj_align(s_status_right_pill, LV_ALIGN_TOP_RIGHT,
                 -STATUS_BAR_SIDE_PAD, STATUS_BAR_TOP_PAD);

    fade_obj(s_status_left_pill, LV_OPA_COVER, STATUS_BAR_FADE_MS);
    fade_obj(s_status_right_pill, LV_OPA_COVER, STATUS_BAR_FADE_MS);

    if (s_status_hide_timer)
    {
        lv_timer_reset(s_status_hide_timer);
    }
    else
    {
        s_status_hide_timer = lv_timer_create(status_bar_hide_cb,
                                              STATUS_BAR_HOLD_MS, NULL);
        lv_timer_set_repeat_count(s_status_hide_timer, 1);
    }
}

void sage_face_fade_in(uint32_t ms)
{
    fade_obj(s_p.root, LV_OPA_COVER, ms);

    if (s_title_timer)
        lv_timer_del(s_title_timer);
    s_title_timer = lv_timer_create(title_timer_cb, ms + TITLE_HOLD_MS, NULL);
    lv_timer_set_repeat_count(s_title_timer, 1);
}

// ----- sparkle burst ------------------------------------------------------

static void anim_sparkle_progress(void *obj, int32_t v)
{
    // v is 0..1000. One anim drives translate + opacity per sparkle.
    lv_obj_t *o = (lv_obj_t *)obj;
    int idx = (int)(intptr_t)lv_obj_get_user_data(o);
    int32_t dx = ((int32_t)s_sparkle_targets[idx].end_dx * v) / 1000;
    int32_t dy = ((int32_t)s_sparkle_targets[idx].end_dy * v) / 1000;
    lv_obj_set_style_translate_x(o, dx, 0);
    lv_obj_set_style_translate_y(o, dy, 0);

    lv_opa_t opa;
    if (v < 80)
        opa = (lv_opa_t)((v * 255) / 80);
    else if (v < 600)
        opa = LV_OPA_COVER;
    else
        opa = (lv_opa_t)(255 - ((v - 600) * 255) / 400);
    lv_obj_set_style_bg_opa(o, opa, 0);
}

void sage_face_burst_sparkles(void)
{
    if (!s_sparkle_container)
        return;
    for (int i = 0; i < SPARKLE_COUNT; i++)
    {
        lv_obj_t *s = s_sparkles[i];
        lv_anim_del(s, anim_sparkle_progress);

        uint32_t r = get_rand_32();
        // Even angular slices + jitter so RNG clumping can't make a
        // one-sided burst.
        int slice = 360 / SPARKLE_COUNT;
        int angle = (slice * i) + (int)(r % slice);
        int dist = SPARKLE_TRAVEL_MIN +
                   (int)((r >> 16) % (SPARKLE_TRAVEL_MAX - SPARKLE_TRAVEL_MIN));
        uint32_t delay = (r >> 8) % SPARKLE_DELAY_MAX_MS;
        int dia = SPARKLE_DIA_MIN +
                  (int)((r >> 24) % (SPARKLE_DIA_MAX - SPARKLE_DIA_MIN + 1));

        s_sparkle_targets[i].end_dx = (int16_t)sin_scale((int16_t)(angle + 90), dist);
        s_sparkle_targets[i].end_dy = (int16_t)sin_scale((int16_t)angle, dist);

        lv_obj_set_size(s, dia, dia);
        lv_obj_align(s, LV_ALIGN_CENTER, 0, SPARKLE_ORIGIN_Y - SCR_CY);
        lv_obj_set_style_translate_x(s, 0, 0);
        lv_obj_set_style_translate_y(s, 0, 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_TRANSP, 0);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s);
        lv_anim_set_values(&a, 0, 1000);
        lv_anim_set_time(&a, SPARKLE_DURATION_MS);
        lv_anim_set_delay(&a, delay);
        lv_anim_set_exec_cb(&a, anim_sparkle_progress);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
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
    int top = (LID_TOP_MAX * (int)pct) / 100;
    int bot = (LID_BOT_MAX * (int)pct) / 100;
    if (top == s_lid_top_now && bot == s_lid_bot_now)
        return;
    s_lid_top_now = (int16_t)top;
    s_lid_bot_now = (int16_t)bot;

    // Both eyes share one pair of point arrays (lv_line doesn't copy them).
    const int cx = EYE_BOX_W / 2;
    const int cy = EYE_BOX_H / 2;
    for (int i = 0; i < LID_PT_COUNT; i++)
    {
        int16_t ang = (int16_t)((180 * i) / (LID_PT_COUNT - 1));
        lv_coord_t x = (lv_coord_t)(cx - EYE_W / 2 + (EYE_W * i) / (LID_PT_COUNT - 1));
        s_lid_top_pts[i].x = x;
        s_lid_top_pts[i].y = (lv_coord_t)(cy - sin_scale(ang, top));
        s_lid_bot_pts[i].x = x;
        s_lid_bot_pts[i].y = (lv_coord_t)(cy + sin_scale(ang, bot));
    }
    lv_line_set_points(s_p.lid_top_l, s_lid_top_pts, LID_PT_COUNT);
    lv_line_set_points(s_p.lid_bot_l, s_lid_bot_pts, LID_PT_COUNT);
    lv_line_set_points(s_p.lid_top_r, s_lid_top_pts, LID_PT_COUNT);
    lv_line_set_points(s_p.lid_bot_r, s_lid_bot_pts, LID_PT_COUNT);

    // Size each iris window to the lid gap. Its center sits (bot-top)/2
    // below the eye line, which refresh_pupils() compensates for.
    int gap = top + bot;
    s_eye_win_dy = (int16_t)((bot - top) / 2);
    lv_obj_t *wins[2] = {s_p.eye_win_l, s_p.eye_win_r};
    for (int i = 0; i < 2; i++)
    {
        if (gap < EYE_MIN_GAP)
        {
            lv_obj_add_flag(wins[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(wins[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(wins[i], gap);
        lv_obj_align(wins[i], LV_ALIGN_CENTER, i ? EYE_DX : -EYE_DX, EYE_DY + s_eye_win_dy);
    }
    refresh_pupils();
}

// Combine the pose's intentional gaze with the idle drift and position the
// iris + pupil inside each eye window.
static void refresh_pupils(void)
{
    int dx = clampi((int)s_pose_now.pupil_dx + (int)s_gaze_dx, -GAZE_MAX_X, GAZE_MAX_X);
    int dy = clampi((int)s_pose_now.pupil_dy + (int)s_gaze_dy, -GAZE_MAX_Y, GAZE_MAX_Y);
    if (dx == s_pupil_dx_now && dy == s_pupil_dy_now && s_eye_win_dy == s_pupil_win_dy_now)
        return;
    s_pupil_dx_now = (int16_t)dx;
    s_pupil_dy_now = (int16_t)dy;
    s_pupil_win_dy_now = s_eye_win_dy;

    int y = dy - s_eye_win_dy;
    lv_obj_align(s_p.iris_l, LV_ALIGN_CENTER, dx, y);
    lv_obj_align(s_p.iris_r, LV_ALIGN_CENTER, dx, y);
    lv_obj_align(s_p.pupil_l, LV_ALIGN_CENTER, dx, y);
    lv_obj_align(s_p.pupil_r, LV_ALIGN_CENTER, dx, y);
}

static void set_brow(lv_obj_t *line, lv_point_t *pts, int left_dy, int right_dy)
{
    const int base = BROW_BOX_H / 2;
    for (int i = 0; i < BROW_PT_COUNT; i++)
    {
        int16_t ang = (int16_t)((180 * i) / (BROW_PT_COUNT - 1));
        int t1000 = (1000 * i) / (BROW_PT_COUNT - 1);
        pts[i].x = (lv_coord_t)(LINE_W + (BROW_W * i) / (BROW_PT_COUNT - 1));
        pts[i].y = (lv_coord_t)(base + lerp_i32(left_dy, right_dy, t1000) - sin_scale(ang, BROW_ARCH));
    }
    lv_line_set_points(line, pts, BROW_PT_COUNT);
}

static void apply_brows(const sage_pose_t *p)
{
    // Points run left to right on screen. Left brow: outer end first.
    // Right brow: inner end first.
    set_brow(s_p.brow_l, s_brow_l_pts, p->brow_outer_l_dy, p->brow_inner_l_dy);
    set_brow(s_p.brow_r, s_brow_r_pts, p->brow_inner_r_dy, p->brow_outer_r_dy);
}

static void apply_mouth(int8_t curve, int8_t rest_open, uint8_t lipsync_open, int8_t lipsync_corner)
{
    const int cx = MOUTH_BOX_W / 2;
    const int cy = MOUTH_BOX_H / 2;
    const int half_w = MOUTH_W / 2;

    // 2x input gain so quieter voicelines still use the full open range.
    int gained_open = (int)lipsync_open * 2;
    if (gained_open > 255)
        gained_open = 255;
    int open = clampi(rest_open + (gained_open * MOUTH_MAX_OPEN) / 255, 0, MOUTH_MAX_OPEN);
    int half_open = open / 2;

    int inset = clampi(-lipsync_corner, 0, MOUTH_W / 4);
    int sag = clampi(curve, -MOUTH_CURVE_MAX, MOUTH_CURVE_MAX);

    // Both lips follow a center line that sags by `sag` in the middle
    // (smile) or bows up (frown); the lips then part around it. The lower
    // lip always droops a little extra so a closed mouth reads as a lens.
    lv_point_t top[MOUTH_PT_COUNT], bot[MOUTH_PT_COUNT];
    const int x_left = cx - half_w + inset;
    const int x_span = (cx + half_w - inset) - x_left;
    for (int i = 0; i < MOUTH_PT_COUNT; i++)
    {
        int16_t ang = (int16_t)((180 * i) / (MOUTH_PT_COUNT - 1));
        int mid = cy - sag / 2 + sin_scale(ang, sag);
        top[i].x = bot[i].x = (lv_coord_t)(x_left + (x_span * i) / (MOUTH_PT_COUNT - 1));
        top[i].y = (lv_coord_t)(mid - sin_scale(ang, half_open));
        bot[i].y = (lv_coord_t)(mid + sin_scale(ang, half_open + MOUTH_LENS));
    }

    // Main loop calls this every pass; skip the redraw when nothing moved.
    if (memcmp(top, s_mouth_top_pts, sizeof(top)) == 0 &&
        memcmp(bot, s_mouth_bot_pts, sizeof(bot)) == 0)
        return;
    memcpy(s_mouth_top_pts, top, sizeof(top));
    memcpy(s_mouth_bot_pts, bot, sizeof(bot));
    lv_line_set_points(s_p.mouth_top, s_mouth_top_pts, MOUTH_PT_COUNT);
    lv_line_set_points(s_p.mouth_bot, s_mouth_bot_pts, MOUTH_PT_COUNT);
}

static void apply_third_eye_pulse(const sage_pose_t *p)
{
    if (!s_p.third_eye)
        return;
    lv_anim_del(s_p.third_eye, anim_obj_opa);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_p.third_eye);
    lv_anim_set_values(&a, p->third_eye_min_opa, p->third_eye_max_opa);
    lv_anim_set_time(&a, p->third_eye_period_ms);
    lv_anim_set_playback_time(&a, p->third_eye_period_ms);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, anim_obj_opa);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

// ============================================================================
// Physics-driven head motion
// ============================================================================
//
// The head floats inside the card. A weak spring pulls it toward a slow
// figure-eight bob around center; IMU linear acceleration kicks it, and
// hard shakes rattle it against small walls so it never leaves the halo.
// Listening auto-perk: sub-shake handling motion bumps the face into the
// LISTENING pose for ~1.5 s.

#define PHYS_FORCE_PER_G 900.0f // px/s² per g of linear accel
#define PHYS_SPRING 9.0f        // 1/s², pull toward the bob target
#define PHYS_DAMPING 2.5f       // 1/s
#define PHYS_GRAV_TAU_S 0.6f    // gravity-EMA time constant
#define PHYS_MAX_VEL 160.0f     // px/s
#define PHYS_MAX_OFFSET_X 14.0f
#define PHYS_MAX_OFFSET_Y 10.0f
#define PHYS_BOUNCE 0.6f
#define BOB_AMP_X 5
#define BOB_AMP_Y 3
#define BOB_MS_PER_DEG_X 28 // ~10 s period
#define BOB_MS_PER_DEG_Y 19 // ~7 s period
// IMU -> screen mapping, measured on hardware with the UI upright: +X
// points toward the top of the screen and +Y toward the right. The head
// lags behind the device like a weight in a box, so accelerating toward
// screen-top pushes it down (+screen y) and toward screen-right pushes it
// left (-screen x). screen_orient's 180-degree flip negates both.
#define PHYS_SCREEN_X_FROM_IMU_Y (-1.0f)
#define PHYS_SCREEN_Y_FROM_IMU_X (1.0f)

#define LISTEN_TRIGGER_G 0.06f

static float s_phys_pos_x = 0.0f, s_phys_pos_y = 0.0f;
static float s_phys_vel_x = 0.0f, s_phys_vel_y = 0.0f;
static float s_phys_grav_x = 0.0f, s_phys_grav_y = 0.0f;
static bool s_phys_grav_primed = false;

static void phys_wall(float *pos, float *vel, float lim)
{
    if (*pos > lim)
    {
        *pos = lim;
        if (*vel > 0)
            *vel = -*vel * PHYS_BOUNCE;
    }
    else if (*pos < -lim)
    {
        *pos = -lim;
        if (*vel < 0)
            *vel = -*vel * PHYS_BOUNCE;
    }
}

static int32_t round_px(float v)
{
    return (int32_t)(v + (v >= 0 ? 0.5f : -0.5f));
}

static void physics_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_p.head)
        return;

    const float dt = PHYS_TICK_MS / 1000.0f;

    float a[3];
    QMI8658_read_acc_xyz(a); // m/s²
    const bool flipped = screen_orient_is_flipped();
    static bool s_last_flipped = false;
    if (flipped != s_last_flipped)
    {
        // The axis signs just inverted; re-baseline gravity instead of
        // treating the sign change as a 2 g jolt.
        s_last_flipped = flipped;
        s_phys_grav_primed = false;
    }
    const float flip = flipped ? -1.0f : 1.0f;
    float ax_g = (a[1] / ONE_G) * PHYS_SCREEN_X_FROM_IMU_Y * flip;
    float ay_g = (a[0] / ONE_G) * PHYS_SCREEN_Y_FROM_IMU_X * flip;

    // Prime the gravity estimate on the first sample so boot isn't a "shake".
    if (!s_phys_grav_primed)
    {
        s_phys_grav_x = ax_g;
        s_phys_grav_y = ay_g;
        s_phys_grav_primed = true;
    }
    const float galpha = dt / (PHYS_GRAV_TAU_S + dt);
    s_phys_grav_x += galpha * (ax_g - s_phys_grav_x);
    s_phys_grav_y += galpha * (ay_g - s_phys_grav_y);

    const float lin_x = ax_g - s_phys_grav_x;
    const float lin_y = ay_g - s_phys_grav_y;

    uint32_t now = lv_tick_get();
    const float bob_x = (float)sin_scale((int16_t)((now / BOB_MS_PER_DEG_X) % 360), BOB_AMP_X * 64) / 64.0f;
    const float bob_y = (float)sin_scale((int16_t)((now / BOB_MS_PER_DEG_Y) % 360), BOB_AMP_Y * 64) / 64.0f;

    float fx = lin_x * PHYS_FORCE_PER_G + PHYS_SPRING * (bob_x - s_phys_pos_x) - PHYS_DAMPING * s_phys_vel_x;
    float fy = lin_y * PHYS_FORCE_PER_G + PHYS_SPRING * (bob_y - s_phys_pos_y) - PHYS_DAMPING * s_phys_vel_y;

    // Semi-implicit Euler.
    s_phys_vel_x += fx * dt;
    s_phys_vel_y += fy * dt;
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
    phys_wall(&s_phys_pos_x, &s_phys_vel_x, PHYS_MAX_OFFSET_X);
    phys_wall(&s_phys_pos_y, &s_phys_vel_y, PHYS_MAX_OFFSET_Y);

    // Setting a style always invalidates, so only touch it on a real move.
    int32_t tx = round_px(s_phys_pos_x);
    int32_t ty = round_px(s_phys_pos_y);
    if (tx != s_head_tx)
    {
        s_head_tx = tx;
        lv_obj_set_style_translate_x(s_p.head, tx, 0);
    }
    if (ty != s_head_ty)
    {
        s_head_ty = ty;
        lv_obj_set_style_translate_y(s_p.head, ty, 0);
    }

    // ---- Listening auto-perk ----
    // Only manage pose in IDLE / LISTENING; speaking poses own the face.
    if (s_pose_kind == SAGE_POSE_IDLE || s_pose_kind == SAGE_POSE_LISTENING)
    {
        const float lx = lin_x < 0 ? -lin_x : lin_x;
        const float ly = lin_y < 0 ? -lin_y : lin_y;
        if (lx + ly > LISTEN_TRIGGER_G)
        {
            s_listen_until_ms = now + LISTEN_HOLD_MS;
        }
        bool listening = s_listen_external || now < s_listen_until_ms;
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
    // 1.2–3.2 s between shifts reads as actively looking around.
    uint32_t r = get_rand_32();
    lv_timer_set_period(t, 1200 + (r % 2000));

    int8_t tx = (int8_t)((int)((r >> 16) % 13) - 6); // -6..+6 px
    int8_t ty = (int8_t)((int)((r >> 24) % 7) - 3);  // -3..+3 px

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
    // The third-eye pulse was already restarted in set_pose.
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
    apply_eye_open((uint8_t)v);
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
    lv_anim_set_var(&a, &s_blink_anim_var);
    lv_anim_set_values(&a, s_pose_now.eye_open_pct, 0);
    lv_anim_set_time(&a, 70);
    lv_anim_set_playback_time(&a, 90);
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

static void anim_obj_opa(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void fade_obj(lv_obj_t *obj, lv_opa_t target, uint32_t ms)
{
    if (!obj)
        return;
    lv_anim_del(obj, anim_obj_opa);
    lv_opa_t cur = lv_obj_get_style_opa(obj, 0);
    if (cur == target)
        return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, cur, target);
    lv_anim_set_time(&a, ms);
    lv_anim_set_exec_cb(&a, anim_obj_opa);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

// Unstyled, non-scrollable, non-clickable container.
static lv_obj_t *make_box(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *make_circle(lv_obj_t *parent, int dia, lv_color_t color)
{
    lv_obj_t *o = make_box(parent, dia, dia);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

// Line whose points live in a fixed w x h box centered (dx, dy) from the
// parent's center. The fixed size matters: by default lv_line sizes itself
// to its points, so a centered line would shift whenever its points change.
static lv_obj_t *make_line_box(lv_obj_t *parent, lv_point_t *pts, uint16_t cnt,
                               int w, int h, int dx, int dy, lv_color_t color, int width)
{
    lv_obj_t *line = lv_line_create(parent);
    lv_line_set_points(line, pts, cnt);
    lv_obj_set_size(line, w, h);
    lv_obj_align(line, LV_ALIGN_CENTER, dx, dy);
    lv_obj_set_style_line_color(line, color, 0);
    lv_obj_set_style_line_width(line, width, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
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

// amp * sin(deg), rounded toward zero.
static int32_t sin_scale(int16_t deg, int32_t amp)
{
    return ((int32_t)lv_trigo_sin(deg) * amp) / LV_TRIGO_SIN_MAX;
}
