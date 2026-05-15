// Parametric mystic-sage face — Phase 1.0a (idle pose only).
//
// Built from composed LVGL objects (no custom raster buffer — would blow the
// SRAM budget since the display buffer already eats ~329KB). Each face part
// is a simple primitive with styles. Animations use lv_anim.
//
// Phase 1.0a parts (female mystic / oracle):
//   - black background
//   - hood: deep-indigo filled circle (back layer)
//   - hair: two pale silver-lavender pills flanking the face, falling from
//     under the hood past the chin line
//   - face glow: warm-violet filled circle (rendered on top of hair so hair
//     only shows where it extends past the face's horizontal extent)
//   - closed eyes: two short thick rounded lines (sleeping)
//   - lips: small muted-rose pill below the eyes
//   - third eye: small cyan circle on the forehead, opacity pulses 1.5s loop
//
// Future phases will swap to a custom widget with LV_EVENT_DRAW_MAIN so we
// can do bezier hair flow, blink, mouth shapes for lip-sync, etc.

#include "sage_face.h"
#include "lvgl_glue.h"
#include "lvgl.h"

// Geometry — measured in LVGL "center alignment" offsets from screen center.
// Display is 368w x 448h.
#define HOOD_DIA           360
#define HOOD_Y_OFFSET      (-25)

#define FACE_DIA           220
#define FACE_Y_OFFSET      10

#define HAIR_W             60
#define HAIR_H             280
#define HAIR_X_OFFSET      130
#define HAIR_Y_OFFSET      70

#define LIPS_W             34
#define LIPS_H             6
#define LIPS_Y_OFFSET      80

#define EYE_W              44
#define EYE_THICKNESS      6
#define EYE_X_OFFSET       50
#define EYE_Y_OFFSET       30

#define THIRD_EYE_DIA      24
#define THIRD_EYE_Y_OFFSET (-50)

#define THIRD_EYE_PULSE_MIN 80
#define THIRD_EYE_PULSE_MAX 255
#define THIRD_EYE_PULSE_MS  1500

// Speaking-mode pulse: brighter floor, faster cycle.
#define THIRD_EYE_TALK_MIN  200
#define THIRD_EYE_TALK_MAX  255
#define THIRD_EYE_TALK_MS   350

// Lips bob during speaking. Heights interpolate; LIPS_H is the rest pose.
#define LIPS_TALK_MIN_H     4
#define LIPS_TALK_MAX_H     22
#define LIPS_TALK_MS        180

static lv_obj_t *third_eye = NULL;
static lv_obj_t *lips      = NULL;
static bool      s_speaking = false;

static lv_obj_t *make_filled_circle(lv_obj_t *parent, int dia, int xoff, int yoff, lv_color_t color);
static lv_obj_t *make_pill(lv_obj_t *parent, int w, int h, int xoff, int yoff, lv_color_t color);
static lv_obj_t *make_eye_line(lv_obj_t *parent, int xoff, int yoff, lv_color_t color);
static void anim_third_eye_opa(void *obj, int32_t v);
static void anim_lips_h(void *obj, int32_t v);
static void start_third_eye_idle_pulse(void);
static void start_third_eye_talk_pulse(void);
static void start_lips_talk_bob(void);
static void stop_lips_talk_bob(void);

void sage_face_init(void)
{
    lv_obj_t *scr = lv_scr_act();

    // Black background, no scrolling, no padding (so LV_ALIGN_CENTER
    // offsets are measured from the actual screen center).
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Color palette (RGB888 → LVGL converts to RGB565 per lv_conf.h)
    lv_color_t color_hood      = lv_color_make(26, 18, 64);    // deep indigo
    lv_color_t color_face_glow = lv_color_make(58, 40, 96);    // warm violet
    lv_color_t color_hair      = lv_color_make(200, 195, 225); // pale silver-lavender
    lv_color_t color_eye_line  = lv_color_make(40, 30, 80);    // dark indigo
    lv_color_t color_lips      = lv_color_make(150, 75, 110);  // muted rose
    lv_color_t color_third_eye = lv_color_make(0, 216, 255);   // cyan

    // Back-to-front layering: LVGL renders children in creation order.
    // hood → hair (flanks the face, falling from under the hood) → face
    // glow (drawn on top of hair so hair only shows where it extends past
    // the face) → eyes/lips/third-eye on top.
    make_filled_circle(scr, HOOD_DIA, 0, HOOD_Y_OFFSET, color_hood);

    make_pill(scr, HAIR_W, HAIR_H, -HAIR_X_OFFSET, HAIR_Y_OFFSET, color_hair);
    make_pill(scr, HAIR_W, HAIR_H,  HAIR_X_OFFSET, HAIR_Y_OFFSET, color_hair);

    make_filled_circle(scr, FACE_DIA, 0, FACE_Y_OFFSET, color_face_glow);

    // Closed eyes — sleeping, just rounded thick lines
    make_eye_line(scr, -EYE_X_OFFSET, EYE_Y_OFFSET, color_eye_line);
    make_eye_line(scr,  EYE_X_OFFSET, EYE_Y_OFFSET, color_eye_line);

    // Lips — small rose mark below the eyes
    lips = make_pill(scr, LIPS_W, LIPS_H, 0, LIPS_Y_OFFSET, color_lips);

    // Third eye — pulsing cyan dot
    third_eye = make_filled_circle(scr, THIRD_EYE_DIA, 0, THIRD_EYE_Y_OFFSET, color_third_eye);

    start_third_eye_idle_pulse();
}

void sage_face_set_speaking(bool speaking)
{
    if (s_speaking == speaking) {
        return;
    }
    s_speaking = speaking;

    if (speaking) {
        start_third_eye_talk_pulse();
        start_lips_talk_bob();
    } else {
        start_third_eye_idle_pulse();
        stop_lips_talk_bob();
    }
}

static void start_third_eye_idle_pulse(void)
{
    if (!third_eye) return;
    lv_anim_del(third_eye, anim_third_eye_opa);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, third_eye);
    lv_anim_set_values(&a, THIRD_EYE_PULSE_MIN, THIRD_EYE_PULSE_MAX);
    lv_anim_set_time(&a, THIRD_EYE_PULSE_MS);
    lv_anim_set_playback_time(&a, THIRD_EYE_PULSE_MS);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, anim_third_eye_opa);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void start_third_eye_talk_pulse(void)
{
    if (!third_eye) return;
    lv_anim_del(third_eye, anim_third_eye_opa);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, third_eye);
    lv_anim_set_values(&a, THIRD_EYE_TALK_MIN, THIRD_EYE_TALK_MAX);
    lv_anim_set_time(&a, THIRD_EYE_TALK_MS);
    lv_anim_set_playback_time(&a, THIRD_EYE_TALK_MS);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, anim_third_eye_opa);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void start_lips_talk_bob(void)
{
    if (!lips) return;
    lv_anim_del(lips, anim_lips_h);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, lips);
    lv_anim_set_values(&a, LIPS_TALK_MIN_H, LIPS_TALK_MAX_H);
    lv_anim_set_time(&a, LIPS_TALK_MS);
    lv_anim_set_playback_time(&a, LIPS_TALK_MS);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, anim_lips_h);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void stop_lips_talk_bob(void)
{
    if (!lips) return;
    lv_anim_del(lips, anim_lips_h);
    // Animate back to rest pose so it doesn't snap.
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, lips);
    lv_anim_set_values(&a, lv_obj_get_height(lips), LIPS_H);
    lv_anim_set_time(&a, 200);
    lv_anim_set_exec_cb(&a, anim_lips_h);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void anim_lips_h(void *obj, int32_t v)
{
    lv_obj_set_height((lv_obj_t *)obj, v);
    // Re-center vertically so the bob grows symmetrically around
    // LIPS_Y_OFFSET instead of pinning the top edge.
    lv_obj_align((lv_obj_t *)obj, LV_ALIGN_CENTER, 0, LIPS_Y_OFFSET);
}

static lv_obj_t *make_filled_circle(lv_obj_t *parent, int dia, int xoff, int yoff, lv_color_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, dia, dia);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_align(o, LV_ALIGN_CENTER, xoff, yoff);
    return o;
}

// Rounded rectangle with full-radius ends (pill shape). Radius is set to
// half the smaller dimension so short ends are fully rounded.
static lv_obj_t *make_pill(lv_obj_t *parent, int w, int h, int xoff, int yoff, lv_color_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    int r = (w < h ? w : h) / 2;
    lv_obj_set_style_radius(o, r, 0);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_align(o, LV_ALIGN_CENTER, xoff, yoff);
    return o;
}

static lv_obj_t *make_eye_line(lv_obj_t *parent, int xoff, int yoff, lv_color_t color)
{
    // Static points — LVGL stores a pointer; safe because lv_line keeps the
    // ref alive for the object's lifetime. One pair per eye so they don't
    // share state.
    static lv_point_t left_pts[2]  = { {0, 0}, {EYE_W, 0} };
    static lv_point_t right_pts[2] = { {0, 0}, {EYE_W, 0} };

    lv_obj_t *line = lv_line_create(parent);
    lv_line_set_points(line, (xoff < 0) ? left_pts : right_pts, 2);
    lv_obj_set_style_line_color(line, color, 0);
    lv_obj_set_style_line_width(line, EYE_THICKNESS, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_align(line, LV_ALIGN_CENTER, xoff, yoff);
    return line;
}

static void anim_third_eye_opa(void *obj, int32_t v)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}
