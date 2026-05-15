#include "app.h"
#include "lvgl_glue.h"
#include "sage_face.h"
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
    if (DEV_Module_Init() != 0) {
        return -1;
    }

    // Give the host up to 3s to enumerate the USB CDC device. Without
    // this our boot prints fire into the void and the user sees nothing.
    // If no host shows up, fall through and keep going — board still works
    // headless.
    stdio_init_all();
    for (int i = 0; i < 30 && !stdio_usb_connected(); i++) {
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
    if (audio_test_init()) {
        printf("Magic 8-Ball: codec OK, beeping...\r\n");
        audio_test_play_tone(440, 800);
    } else {
        printf("Magic 8-Ball: codec FAILED to init.\r\n");
    }

    // SD card bring-up. Failure is non-fatal so we can still boot the
    // sage face and serial debug; WAV playback will just no-op once it
    // exists.
    bool sd_ok = sd_card_mount();
    if (sd_ok) {
        sd_card_list_root();
    }

    // Wire up streaming WAV playback (chained DMA on DMA_IRQ_1; the
    // display owns DMA_IRQ_0). Safe to init even if SD failed; play
    // calls will just fail to open the file.
    wav_player_init();

    bool vl_ok = false;
    if (sd_ok) {
        vl_ok = voicelines_scan("0:/voicelines");
    }

    shake_detect_init();

    uint32_t tick = 0;
    bool was_playing = false;
    while (1) {
        lv_task_handler();
        wav_player_poll();

        bool playing = wav_player_is_playing();
        if (playing != was_playing) {
            // Drive the face animation off the same edge that gates
            // shake detection. Speaking pose comes up the instant a WAV
            // starts and drops back to idle when it finishes.
            sage_face_set_speaking(playing);
            if (was_playing && !playing) {
                // Playback just ended: reset the shake detector so any
                // residual motion from the user shaking *during* playback
                // doesn't fire instantly.
                shake_detect_reset();
            }
        }
        was_playing = playing;

        if (!playing && shake_detect_poll()) {
            const char *path = vl_ok ? voicelines_pick_random() : NULL;
            if (!path || !wav_player_play_file(path)) {
                // No catalog or file open failed: stand-in beep so the
                // shake at least produces audible feedback.
                audio_test_play_tone(660, 400);
            }
        }

        DEV_Delay_ms(5);
        // Heartbeat every ~2s so we know main loop is alive.
        if (++tick % 400 == 0) {
            printf("[hb] tick=%lu\r\n", (unsigned long)tick);
        }
    }

    return 0;
}
