#include "sage_tone.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

sage_tone_t sage_tone_from_path(const char *path)
{
    if (!path)
        return SAGE_TONE_OMINOUS;

    // Walk to the basename — last path separator wins. Handles both
    // FAT-style "0:/voicelines/foo.wav" and any backslash variants.
    const char *base = path;
    for (const char *p = path; *p; p++)
    {
        if (*p == '/' || *p == '\\')
            base = p + 1;
    }

    // Trim trailing ".wav" (case-insensitive). If it isn't there we just
    // compare the whole basename — the lookup will miss and we'll log it.
    size_t n = strlen(base);
    if (n >= 4)
    {
        const char *ext = base + n - 4;
        if (ext[0] == '.' &&
            tolower((unsigned char)ext[1]) == 'w' &&
            tolower((unsigned char)ext[2]) == 'a' &&
            tolower((unsigned char)ext[3]) == 'v')
        {
            n -= 4;
        }
    }

    for (size_t i = 0; i < sage_tone_table_count; i++)
    {
        const char *s = sage_tone_table[i].stem;
        size_t sn = strlen(s);
        if (sn != n)
            continue;

        bool match = true;
        for (size_t j = 0; j < n; j++)
        {
            if (tolower((unsigned char)base[j]) != s[j])
            {
                match = false;
                break;
            }
        }
        if (match)
            return sage_tone_table[i].tone;
    }

    printf("[tone] unknown voiceline '%.*s', defaulting to ominous\r\n",
           (int)n, base);
    return SAGE_TONE_OMINOUS;
}
