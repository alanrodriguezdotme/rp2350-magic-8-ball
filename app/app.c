#include "app.h"
#include "lvgl_glue.h"
#include "sage_face.h"
#include "sage_tone.h"
#include "sage_text.h"
#include "audio_test.h"
#include "boot_button.h"
#include "shake_detect.h"
#include "sleep_mgr.h"
#include "sfx.h"
#include "sd.h"
#include "volume.h"
#include "wav_player.h"
#include "voicelines.h"

#include "DEV_Config.h"
#include "AMOLED_1in8.h"
#include "qspi_pio.h"
#include "FT3168.h"
#include "QMI8658.h"
#include "PCF85063A.h"
#include "AXP2101.h"
#include "lvgl.h"

#include "pico/stdlib.h"
#include "pico/stdio_usb.h"

#include <stdio.h>

int app_run(void)
{
    if (DEV_Module_Init() != 0)
    {
        return -1;
    }

    // Give the host up to 3s to enumerate the USB CDC device. Without
    // this our boot prints fire into the void and the user sees nothing.
    // If no host shows up, fall through and keep going — board still works
    // headless.
    stdio_init_all();
    for (int i = 0; i < 30 && !stdio_usb_connected(); i++)
    {
        sleep_ms(100);
    }

    printf("Magic 8-Ball: booting sage...\r\n");

    // QSPI PIO bring-up for the SH8601 AMOLED
    QSPI_GPIO_Init(qspi);
    QSPI_PIO_Init(qspi);
    QSPI_4Wrie_Mode(&qspi);

    AMOLED_1IN8_Init();
    AMOLED_1IN8_SetBrightness(60);
    AMOLED_1IN8_Clear(BLACK);

    FT3168_Init(FT3168_Point_Mode);
    PCF85063A_Init();
    if (!AXP2101_Init())
    {
        printf("Magic 8-Ball: AXP2101 not detected; sleep/wake button will not work.\r\n");
    }
    QMI8658_init();

    lvgl_glue_init();
    sage_face_init();

    printf("Magic 8-Ball: sage is awake.\r\n");

    // Audio bring-up: codec + amp. Volume tier is initialized first so
    // the codec's volume register is set to a known value (mid by
    // default) before any playback. The historical 440 Hz boot beep is
    // gone \u2014 we play voicelines/sfx/answer.wav with a face fade-in
    // instead, once the SD card and wav_player are up.
    if (audio_test_init())
    {
        printf("Magic 8-Ball: codec OK.\r\n");
    }
    else
    {
        printf("Magic 8-Ball: codec FAILED to init.\r\n");
    }
    volume_init();

    // SD card bring-up. Failure is non-fatal so we can still boot the
    // sage face and serial debug; WAV playback will just no-op once it
    // exists.
    bool sd_ok = sd_card_mount();
    if (sd_ok)
    {
        sd_card_list_root();
    }

    // Wire up streaming WAV playback (chained DMA on DMA_IRQ_1; the
    // display owns DMA_IRQ_0). Safe to init even if SD failed; play
    // calls will just fail to open the file.
    wav_player_init();

    bool vl_ok = false;
    if (sd_ok)
    {
        vl_ok = voicelines_scan("0:/voicelines");
    }

    shake_detect_init();
    sleep_mgr_init();

    // -------------------------------------------------------------------
    // Boot intro: play answer.wav while the face fades in. We pump the
    // LVGL + wav_player loop synchronously here until playback ends so
    // the user always sees the full intro before the main loop takes
    // over. If answer.wav can't play (no SD, missing file), we still do
    // the visual fade so the device doesn't sit blank.
    // -------------------------------------------------------------------
    bool intro_playing = sd_ok && sfx_play(SFX_ANSWER_PATH);
    sage_face_fade_in(2500);
    if (intro_playing)
    {
        // Don't drive lipsync here — answer.wav is an SFX, not the
        // sage's voice, so the mouth should stay at its idle resting
        // position while it plays.
        while (wav_player_is_playing())
        {
            lv_task_handler();
            wav_player_poll();
            DEV_Delay_ms(5);
        }
        sage_face_set_pose(SAGE_POSE_IDLE);
    }
    shake_detect_reset();

    // -------------------------------------------------------------------
    // Main loop state machine.
    //
    //   IDLE     -> waiting for user motion. Polls shake_detect.
    //   SHAKING  -> shake.wav playing while user holds the shake; we
    //               poll for ABORTED (stop sfx, back to IDLE) or
    //               FIRED (transition to INTRO).
    //   INTRO    -> answer.wav playing as the response intro.
    //               When it ends, pick a voiceline and go to SPEAKING.
    //   SPEAKING -> voiceline playing with caption + speaking pose.
    //               When it ends, back to IDLE.
    //
    // Lipsync feeding and the BOOT-button volume cycle run in every
    // state. The volume cycle is gated on \"no audio currently playing\"
    // so the preview tone doesn't collide with a voiceline.
    // -------------------------------------------------------------------
    typedef enum
    {
        ST_IDLE = 0,
        ST_SHAKING,
        ST_INTRO,
        ST_SPEAKING,
    } app_state_t;
    app_state_t state = ST_IDLE;
    const char *pending_voiceline = NULL;

    uint32_t tick = 0;
    while (1)
    {
        bool playing = wav_player_is_playing();

        // Drive the sleep state machine first. While asleep we skip
        // LVGL, wav_player, shake polling, and BOOT button polling
        // entirely — that's where the bulk of the awake-vs-asleep
        // CPU + I2C savings come from.
        sleep_mgr_tick(playing);
        if (sleep_mgr_is_asleep())
        {
            DEV_Delay_ms(20);
            if (++tick % 100 == 0)
            {
                printf("[hb] tick=%lu state=ASLEEP\r\n", (unsigned long)tick);
            }
            continue;
        }

        lv_task_handler();
        wav_player_poll();

        // Re-check after wav_player_poll in case a file just ended.
        playing = wav_player_is_playing();

        // Lipsync feed only runs while the *voiceline* is playing.
        // shake.wav and answer.wav are SFX — the sage isn't speaking
        // yet — so leave the mouth at its idle resting position. We
        // still call lipsync(0,...) in non-speaking states so the
        // smoothed mouth-open value bleeds back to zero between turns.
        if (playing && state == ST_SPEAKING)
        {
            uint8_t rms = 0, zcr = 0;
            wav_player_get_lipsync(&rms, &zcr);
            sage_face_lipsync(rms, zcr);
        }
        else
        {
            sage_face_lipsync(0, 128);
        }

        // BOOT button cycles volume. Only honor it when no audio is
        // playing so the preview beep doesn't trample a voiceline.
        if (!playing && boot_button_just_pressed())
        {
            volume_cycle();

            // Flash battery % (top-left) and the new volume tier
            // (top-right) for a beat so the user has visual confirmation
            // and a way to glance at battery without cluttering the main
            // screen. AXP2101 returns 0xFF when no battery is connected
            // (USB-only on the bench); fall back to "?" for that case.
            char left[16];
            uint8_t pct = AXP2101_GetBatteryPercent();
            const char *bat_icon;
            if (pct == 0xFF)
            {
                bat_icon = LV_SYMBOL_BATTERY_EMPTY;
                snprintf(left, sizeof(left), "%s ?", bat_icon);
            }
            else
            {
                if (pct > 87)
                    bat_icon = LV_SYMBOL_BATTERY_FULL;
                else if (pct > 62)
                    bat_icon = LV_SYMBOL_BATTERY_3;
                else if (pct > 37)
                    bat_icon = LV_SYMBOL_BATTERY_2;
                else if (pct > 12)
                    bat_icon = LV_SYMBOL_BATTERY_1;
                else
                    bat_icon = LV_SYMBOL_BATTERY_EMPTY;
                snprintf(left, sizeof(left), "%s %u%%", bat_icon, pct);
            }

            const char *right;
            switch (volume_get_level())
            {
            case VOLUME_LOW:
                right = "Quiet";
                break;
            case VOLUME_MED:
                right = "Medium";
                break;
            case VOLUME_HIGH:
            default:
                right = "Loud";
                break;
            }
            sage_face_show_status_bar(left, right);
        }

        shake_event_t evt = shake_detect_poll();

        // Any motion (event OR sub-threshold hold) counts as activity
        // so the user fiddling with the device pushes the sleep
        // deadline out.
        if (evt != SHAKE_EVT_NONE || shake_detect_is_shaking())
        {
            sleep_mgr_notify_activity();
        }

        switch (state)
        {
        case ST_IDLE:
            if (evt == SHAKE_EVT_STARTED)
            {
                printf("[state] IDLE -> SHAKING\r\n");
                sage_face_set_pose(SAGE_POSE_LISTENING);
                if (!sfx_play(SFX_SHAKE_PATH))
                {
                    // Fallback so the user gets *some* feedback.
                    audio_test_play_tone(660, 200);
                }
                state = ST_SHAKING;
            }
            break;

        case ST_SHAKING:
            if (evt == SHAKE_EVT_ABORTED)
            {
                printf("[state] SHAKING -> IDLE (aborted)\r\n");
                wav_player_stop();
                sage_face_lipsync(0, 128);
                sage_face_set_pose(SAGE_POSE_IDLE);
                state = ST_IDLE;
            }
            else if (evt == SHAKE_EVT_FIRED)
            {
                printf("[state] SHAKING -> INTRO (fired)\r\n");
                // shake.wav is ~2 s long and the hold is 2 s, so it
                // typically ended on its own; stop just in case.
                wav_player_stop();
                pending_voiceline = vl_ok ? voicelines_pick_random() : NULL;
                if (sfx_play(SFX_ANSWER_PATH))
                {
                    state = ST_INTRO;
                }
                else
                {
                    // No SFX file \u2014 skip the intro and jump straight
                    // to the voiceline (or fallback beep) so the user
                    // still gets an answer.
                    audio_test_play_tone(660, 200);
                    goto start_voiceline;
                }
            }
            else if (!playing)
            {
                // shake.wav ended but the user is still shaking and the
                // 2 s window hasn't elapsed yet. Leave the state alone;
                // we'll either FIRE or ABORT shortly.
            }
            break;

        case ST_INTRO:
            if (!playing)
            {
                printf("[state] INTRO -> SPEAKING\r\n");
            start_voiceline:
                if (pending_voiceline && wav_player_play_file(pending_voiceline))
                {
                    sage_tone_t tone = sage_tone_from_path(pending_voiceline);
                    sage_pose_kind_t pose;
                    switch (tone)
                    {
                    case SAGE_TONE_OMINOUS:
                        pose = SAGE_POSE_SPEAKING_OMINOUS;
                        break;
                    case SAGE_TONE_SARCASTIC:
                        pose = SAGE_POSE_SPEAKING_SARCASTIC;
                        break;
                    case SAGE_TONE_DRY:
                        pose = SAGE_POSE_SPEAKING_DRY;
                        break;
                    default:
                        pose = SAGE_POSE_SPEAKING_OMINOUS;
                        break;
                    }
                    sage_face_set_pose(pose);
                    const char *caption = sage_text_from_path(pending_voiceline);
                    if (caption)
                    {
                        sage_face_show_response(caption);
                    }
                    printf("[face] pose=%d for %s\r\n", (int)pose, pending_voiceline);
                    state = ST_SPEAKING;
                }
                else
                {
                    // No voiceline catalog or open failed: stand-in
                    // beep so something audible happens, then back to idle.
                    audio_test_play_tone(660, 400);
                    sage_face_set_pose(SAGE_POSE_IDLE);
                    state = ST_IDLE;
                    shake_detect_reset();
                }
                pending_voiceline = NULL;
            }
            break;

        case ST_SPEAKING:
            if (!playing)
            {
                printf("[state] SPEAKING -> IDLE\r\n");
                sage_face_set_pose(SAGE_POSE_IDLE);
                sage_face_lipsync(0, 128);
                sage_face_hide_response();
                shake_detect_reset();
                state = ST_IDLE;
            }
            break;
        }

        DEV_Delay_ms(5);
        // Heartbeat every ~2s so we know main loop is alive.
        if (++tick % 400 == 0)
        {
            printf("[hb] tick=%lu state=%d\r\n", (unsigned long)tick, (int)state);
        }
    }

    return 0;
}
