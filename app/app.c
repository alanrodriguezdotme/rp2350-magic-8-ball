#include "app.h"
#include "lvgl_glue.h"
#include "sage_face.h"
#include "sage_tone.h"
#include "sage_text.h"
#include "audio_test.h"
#include "shake_detect.h"
#include "sd.h"
#include "wav_player.h"
#include "voicelines.h"

#include "DEV_Config.h"
#include "AMOLED_1in8.h"
#include "qspi_pio.h"
#include "FT3168.h"
#include "QMI8658.h"
#include "PCF85063A.h"
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
    QMI8658_init();

    lvgl_glue_init();
    sage_face_init();

    printf("Magic 8-Ball: sage is awake.\r\n");

    // Audio bring-up: codec + amp + a brief 440Hz beep so we know the
    // speaker is alive. Replace with real WAV playback later.
    if (audio_test_init())
    {
        printf("Magic 8-Ball: codec OK, beeping...\r\n");
        audio_test_play_tone(440, 800);
    }
    else
    {
        printf("Magic 8-Ball: codec FAILED to init.\r\n");
    }

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

    uint32_t tick = 0;
    bool was_playing = false;
    while (1)
    {
        lv_task_handler();
        wav_player_poll();

        bool playing = wav_player_is_playing();

        if (playing)
        {
            // Pull the latest amplitude / sibilance snapshot from the
            // wav_player and feed it to the face. The face module low-
            // passes internally with attack > release, so calling at
            // ~200 Hz with a value that only updates per WAV chunk
            // (~170 ms) still produces smooth mouth motion.
            uint8_t rms = 0, zcr = 0;
            wav_player_get_lipsync(&rms, &zcr);
            sage_face_lipsync(rms, zcr);
        }

        if (playing != was_playing)
        {
            if (playing)
            {
                // Pose was already set when we started playback below.
                // Nothing to do on the rising edge here.
            }
            else
            {
                // Playback just ended: drop back to idle pose and reset
                // the shake detector so residual motion from the user
                // shaking *during* playback doesn't fire instantly.
                sage_face_set_pose(SAGE_POSE_IDLE);
                // Force the mouth shut quickly by injecting one zero
                // sample; the lipsync low-pass would otherwise hold
                // the last RMS value visibly.
                sage_face_lipsync(0, 128);
                sage_face_hide_response();
                shake_detect_reset();
            }
        }
        was_playing = playing;

        if (!playing && shake_detect_poll())
        {
            const char *path = vl_ok ? voicelines_pick_random() : NULL;
            if (path && wav_player_play_file(path))
            {
                sage_tone_t tone = sage_tone_from_path(path);
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
                // Caption: show the spoken line on screen (white fill,
                // black outline) for the duration of playback. Text
                // pointer comes from the static sage_text_table so the
                // label can keep using it without copying.
                const char *caption = sage_text_from_path(path);
                if (caption)
                {
                    sage_face_show_response(caption);
                }
                printf("[face] pose=%d for %s\r\n", (int)pose, path);
            }
            else
            {
                // No catalog or file open failed: stand-in beep so the
                // shake at least produces audible feedback.
                audio_test_play_tone(660, 400);
            }
        }

        DEV_Delay_ms(5);
        // Heartbeat every ~2s so we know main loop is alive.
        if (++tick % 400 == 0)
        {
            printf("[hb] tick=%lu\r\n", (unsigned long)tick);
        }
    }

    return 0;
}
