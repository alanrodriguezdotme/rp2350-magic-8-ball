#include "voicelines.h"

#include "ff.h"
#include "pico/rand.h"

#include <stdio.h>
#include <string.h>

#define VL_MAX_FILES     64
#define VL_MAX_FILENAME  48
#define VL_MAX_PATH      80

static char   s_dir[VL_MAX_PATH];
static char   s_files[VL_MAX_FILES][VL_MAX_FILENAME];
static size_t s_count     = 0;
static int    s_last_pick = -1;
static char   s_path_buf[VL_MAX_PATH];

static bool ends_with_wav(const char *name)
{
    size_t n = strlen(name);
    if (n < 5) return false;
    const char *e = name + n - 4;
    return e[0] == '.' &&
           (e[1] == 'w' || e[1] == 'W') &&
           (e[2] == 'a' || e[2] == 'A') &&
           (e[3] == 'v' || e[3] == 'V');
}

bool voicelines_scan(const char *dir)
{
    s_count     = 0;
    s_last_pick = -1;

    if (strlen(dir) >= sizeof(s_dir)) {
        printf("[vl] dir path too long: %s\r\n", dir);
        return false;
    }
    strcpy(s_dir, dir);

    DIR     d;
    FRESULT fr = f_opendir(&d, dir);
    if (fr != FR_OK) {
        printf("[vl] f_opendir(%s) failed: %d\r\n", dir, fr);
        return false;
    }

    for (;;) {
        FILINFO fi;
        fr = f_readdir(&d, &fi);
        if (fr != FR_OK || fi.fname[0] == 0) break;
        if (fi.fattrib & AM_DIR) continue;
        if (!ends_with_wav(fi.fname)) continue;

        if (strlen(fi.fname) >= VL_MAX_FILENAME) {
            printf("[vl] skip (name too long): %s\r\n", fi.fname);
            continue;
        }
        if (s_count >= VL_MAX_FILES) {
            printf("[vl] catalog full at %d, skipping rest\r\n", VL_MAX_FILES);
            break;
        }
        strcpy(s_files[s_count++], fi.fname);
    }
    f_closedir(&d);

    printf("[vl] indexed %u .wav file(s) in %s\r\n", (unsigned)s_count, dir);
    return s_count > 0;
}

size_t voicelines_count(void)
{
    return s_count;
}

const char *voicelines_pick_random(void)
{
    if (s_count == 0) return NULL;

    int pick;
    if (s_count == 1) {
        pick = 0;
    } else {
        pick = (int)(get_rand_32() % s_count);
        // Avoid immediate repeat: bump by 1 if we'd play the last one
        // again. Slight bias for the next-up entry, but with 30+ lines
        // it's invisible and far less annoying than hearing the same
        // line twice in a row.
        if (pick == s_last_pick) {
            pick = (pick + 1) % (int)s_count;
        }
    }
    s_last_pick = pick;

    snprintf(s_path_buf, sizeof(s_path_buf), "%s/%s", s_dir, s_files[pick]);
    return s_path_buf;
}
