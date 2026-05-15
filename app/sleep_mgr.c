#include "sleep_mgr.h"

#include "AXP2101.h"
#include "AMOLED_1in8.h"
#include "DEV_Config.h"
#include "FT3168.h"
#include "QMI8658.h"
#include "es8311.h"
#include "qspi_pio.h"
#include "shake_detect.h"
#include "wav_player.h"
#include "lvgl.h"

#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/i2c.h"

#include <stdio.h>

// Display brightness used after wake. The boot sequence sets 60; if
// that ever becomes user-tweakable we should snapshot it on entry
// instead.
#define SLEEP_AWAKE_BRIGHTNESS (60)

// SH8601 command bytes. Same opcodes the panel uses elsewhere; we
// just don't have helpers for these specific commands so we send
// them directly via QSPI_REGISTER_Write.
#define SH8601_CMD_SLEEP_IN (0x10)
#define SH8601_CMD_SLEEP_OUT (0x11)
#define SH8601_CMD_DISPLAY_OFF (0x28)
#define SH8601_CMD_DISPLAY_ON (0x29)

static bool s_asleep = false;
static uint32_t s_idle_deadline = 0;

static uint32_t now_ms(void)
{
    return to_ms_since_boot(get_absolute_time());
}

static void reset_deadline(void)
{
    s_idle_deadline = now_ms() + SLEEP_IDLE_TIMEOUT_MS;
}

static void send_panel_cmd(uint8_t cmd)
{
    QSPI_Select(qspi);
    QSPI_REGISTER_Write(qspi, cmd);
    QSPI_Deselect(qspi);
}

static void sleep_enter(void)
{
    if (s_asleep)
        return;

    printf("[sleep] entering\r\n");

    // Stop any in-flight playback. Defensive — sleep_mgr_tick already
    // gates the timeout path on !audio_playing, but an explicit
    // power-button press while awake bypasses that gate.
    wav_player_stop();

    // Amp first, codec second. Order matters: dropping the amp before
    // the DAC bias goes away avoids the classic power-off pop.
    gpio_put(PA_CTRL, 0);
    es8311_voice_mute(true);

    // LVGL's flush_cb is fire-and-forget DMA. If we send a panel
    // command while a previous flush is still draining over the
    // shared QSPI bus the bytes interleave and the panel never sees
    // a clean DISPLAY_OFF / SLEEP_IN — it just stays on showing the
    // last frame. Wait for the bus to go idle before touching it.
    while (dma_channel_is_busy(dma_tx))
    {
        tight_loop_contents();
    }

    // Display: dim to zero before issuing the sleep-in so the panel
    // doesn't briefly flash full-brightness on the way out.
    AMOLED_1IN8_SetBrightness(0);
    send_panel_cmd(SH8601_CMD_DISPLAY_OFF);
    send_panel_cmd(SH8601_CMD_SLEEP_IN);

    // Touch controller into hibernate. REG_POWER_MODE = 0x03 per the
    // FT-series convention (matches the FT3168_POWER_HIBERNATE enum
    // ordinal in FT3168.h).
    DEV_I2C_Write_Byte(FT3168_I2C_ADDR, REG_POWER_MODE, 0x03);

    // IMU completely off — wake source is the power button only, so
    // we don't need wake-on-motion.
    QMI8658_enableSensors(QMI8658_CTRL7_DISABLE_ALL);

    s_asleep = true;
}

static void sleep_exit(void)
{
    if (!s_asleep)
        return;

    printf("[sleep] exiting\r\n");

    // Sleep-out has up to a 120 ms internal wakeup delay per the
    // SH8601 init sequence. Fire it first and overlap the rest of
    // the wake work during that wait.
    send_panel_cmd(SH8601_CMD_SLEEP_OUT);

    // Codec back up. We never touched the analog/DAC powerdown bits
    // on sleep_enter, just the mute, so unmute is sufficient.
    es8311_voice_mute(false);

    // IMU back to the same enable mask QMI8658_init() uses
    // (accel + gyro). Reset shake-detect state so the gravity vector
    // re-baselines and a hand-pickup doesn't immediately fire.
    QMI8658_enableSensors(QMI8658_CTRL7_ACC_ENABLE | QMI8658_CTRL7_GYR_ENABLE);
    shake_detect_reset();

    // Touch back to active mode.
    DEV_I2C_Write_Byte(FT3168_I2C_ADDR, REG_POWER_MODE, 0x00);

    // Amp last (anti-pop, mirror of the enter ordering).
    gpio_put(PA_CTRL, 1);

    // Pad out the remainder of the SH8601 wake delay before we start
    // pushing pixels.
    sleep_ms(120);

    // SH8601 RAM is not retained across SLEEP_IN, so without a full
    // repaint we'd flash garbage when DISPLAY_ON fires. Mark every
    // LVGL object dirty and force a synchronous flush so the panel
    // RAM holds a real frame before pixels are revealed.
    s_asleep = false; // let lv_refr_now do its thing
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);

    // lv_refr_now only kicks off the DMA flush — it does NOT wait for
    // it to finish. We share the QSPI bus between pixel DMA and the
    // brightness / DISPLAY_ON commands below, so any command issued
    // while the DMA is still running gets interleaved with pixel
    // bytes and the panel sees garbage. Spin until the bus is idle.
    while (dma_channel_is_busy(dma_tx))
    {
        tight_loop_contents();
    }

    // Now safe to reveal pixels and ramp brightness from black.
    AMOLED_1IN8_SetBrightness(0);
    send_panel_cmd(SH8601_CMD_DISPLAY_ON);
    for (int i = 1; i <= 8; ++i)
    {
        AMOLED_1IN8_SetBrightness((SLEEP_AWAKE_BRIGHTNESS * i) / 8);
        sleep_ms(10);
    }

    reset_deadline();
}

void sleep_mgr_init(void)
{
    s_asleep = false;
    reset_deadline();
}

void sleep_mgr_notify_activity(void)
{
    // No-op while asleep — wake transitions are explicit, not
    // activity-driven. (And we don't want stray IMU/touch noise
    // bumping the deadline before the user actually wakes the
    // device.)
    if (s_asleep)
        return;
    reset_deadline();
}

bool sleep_mgr_is_asleep(void)
{
    return s_asleep;
}

void sleep_mgr_tick(bool audio_playing)
{
    bool pressed = AXP2101_PwrKey_Short_Pressed();

    if (s_asleep)
    {
        if (pressed)
            sleep_exit();
        return;
    }

    // Awake.
    if (pressed)
    {
        // Explicit user action — sleep immediately, audio be damned.
        sleep_enter();
        return;
    }

    if (audio_playing)
    {
        // Don't let the timeout fire mid-voiceline. Push the deadline
        // out so we get a fresh 30 s window once playback ends.
        reset_deadline();
        return;
    }

    if ((int32_t)(now_ms() - s_idle_deadline) >= 0)
    {
        sleep_enter();
    }
}
