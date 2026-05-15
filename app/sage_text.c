#include "sage_text.h"

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

const char *sage_text_from_path(const char *path)
{
    if (!path)
        return NULL;

    // Walk to the basename — last path separator wins.
    const char *base = path;
    for (const char *p = path; *p; p++)
    {
        if (*p == '/' || *p == '\\')
            base = p + 1;
    }

    // Trim trailing ".wav" (case-insensitive).
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

    for (size_t i = 0; i < sage_text_table_count; i++)
    {
        const char *s = sage_text_table[i].stem;
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
            return sage_text_table[i].text;
    }

    return NULL;
}
