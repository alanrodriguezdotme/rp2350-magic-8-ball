#include "sd.h"

#include "ff.h"
#include "f_util.h"

#include <stdio.h>

static FATFS s_fs;
static bool  s_mounted = false;

bool sd_card_mount(void)
{
    if (s_mounted) {
        return true;
    }

    FRESULT fr = f_mount(&s_fs, "0:", 1);  // 1 = mount immediately
    if (fr != FR_OK) {
        printf("[sd] f_mount failed: %s (%d)\r\n", FRESULT_str(fr), fr);
        return false;
    }

    printf("[sd] mounted at 0:\r\n");
    s_mounted = true;
    return true;
}

bool sd_card_list_root(void)
{
    if (!s_mounted) {
        printf("[sd] list_root: not mounted\r\n");
        return false;
    }

    DIR    dir;
    FRESULT fr = f_opendir(&dir, "0:/");
    if (fr != FR_OK) {
        printf("[sd] f_opendir failed: %s (%d)\r\n", FRESULT_str(fr), fr);
        return false;
    }

    printf("[sd] root contents:\r\n");
    int count = 0;
    for (;;) {
        FILINFO fi;
        fr = f_readdir(&dir, &fi);
        if (fr != FR_OK || fi.fname[0] == 0) {
            break;
        }
        const char *kind = (fi.fattrib & AM_DIR) ? "<DIR>" : "     ";
        printf("[sd]   %s %10llu  %s\r\n", kind, (unsigned long long)fi.fsize, fi.fname);
        count++;
    }
    f_closedir(&dir);
    printf("[sd] %d entries\r\n", count);
    return true;
}
