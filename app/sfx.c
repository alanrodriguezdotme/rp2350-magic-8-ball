#include "sfx.h"
#include "wav_player.h"

#include <stdio.h>

bool sfx_play(const char *path)
{
    if (!path)
    {
        return false;
    }
    if (!wav_player_play_file(path))
    {
        printf("[sfx] play failed for %s\r\n", path);
        return false;
    }
    return true;
}
