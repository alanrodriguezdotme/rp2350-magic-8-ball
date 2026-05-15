// LVGL display + touch wiring for the Waveshare RP2350-Touch-AMOLED-1.8.
// Lifted (with cleanup) from Waveshare's LVGL example. Don't touch unless
// the display or touch behavior changes — this is the bridge between LVGL's
// rendering pipeline and the SH8601 / FT3168 drivers.

#include "lvgl_glue.h"

#include "DEV_Config.h"
#include "AMOLED_1in8.h"
#include "qspi_pio.h"
#include "FT3168.h"
#include "sleep_mgr.h"
#include "lvgl.h"

#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/irq.h"

static lv_disp_drv_t disp_drv;
static lv_disp_draw_buf_t disp_buf;
static lv_color_t buf0[DISP_HOR_RES * DISP_VER_RES];

static lv_indev_drv_t indev_ts;
static uint16_t ts_x;
static uint16_t ts_y;
static lv_indev_state_t ts_act = LV_INDEV_STATE_RELEASED;

static struct repeating_timer lvgl_timer;

static void disp_flush_cb(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p);
static void touch_callback(uint gpio, uint32_t events);
static void ts_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data);
static void dma_handler(void);
static bool lvgl_tick_cb(struct repeating_timer *t);

void lvgl_glue_init(void)
{
    add_repeating_timer_ms(5, lvgl_tick_cb, NULL, &lvgl_timer);

    lv_init();

    lv_disp_draw_buf_init(&disp_buf, buf0, NULL, DISP_HOR_RES * DISP_VER_RES);
    lv_disp_drv_init(&disp_drv);
    disp_drv.flush_cb = disp_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.hor_res = DISP_HOR_RES;
    disp_drv.ver_res = DISP_VER_RES;
    lv_disp_drv_register(&disp_drv);

    lv_indev_drv_init(&indev_ts);
    indev_ts.type = LV_INDEV_TYPE_POINTER;
    indev_ts.read_cb = ts_read_cb;
    lv_indev_drv_register(&indev_ts);

    DEV_KEY_Config(Touch_INT_PIN);
    DEV_IRQ_SET(Touch_INT_PIN, GPIO_IRQ_EDGE_RISE, &touch_callback);

    dma_channel_set_irq0_enabled(dma_tx, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_handler);
    irq_set_enabled(DMA_IRQ_0, true);
}

static void disp_flush_cb(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
    AMOLED_1IN8_SetWindows(area->x1, area->y1, area->x2 + 1, area->y2 + 1);
    QSPI_Select(qspi);
    QSPI_Pixel_Write(qspi, 0x2c);

    channel_config_set_dreq(&c, pio_get_dreq(qspi.pio, qspi.sm, true));
    dma_channel_configure(dma_tx,
                          &c,
                          &qspi.pio->txf[qspi.sm],
                          color_p,
                          ((area->x2 + 1 - area->x1) * (area->y2 + 1 - area->y1)) * 2,
                          true);
}

static void touch_callback(uint gpio, uint32_t events)
{
    if (gpio == Touch_INT_PIN)
    {
        FT3168_Get_Point();
        ts_x = FT3168.x_point;
        ts_y = FT3168.y_point;
        ts_act = LV_INDEV_STATE_PRESSED;
        // Any touch counts as activity — even one that doesn't hit
        // a widget keeps the sleep timer at bay.
        sleep_mgr_notify_activity();
    }
}

static void ts_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    data->point.x = ts_x;
    data->point.y = ts_y;
    data->state = ts_act;
    ts_act = LV_INDEV_STATE_RELEASED;
}

static void dma_handler(void)
{
    if (dma_channel_get_irq0_status(dma_tx))
    {
        dma_channel_acknowledge_irq0(dma_tx);
        QSPI_Deselect(qspi);
        lv_disp_flush_ready(&disp_drv);
    }
}

static bool lvgl_tick_cb(struct repeating_timer *t)
{
    (void)t;
    lv_tick_inc(5);
    return true;
}
