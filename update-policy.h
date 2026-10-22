#ifndef ST_UPDATE_POLICY_H
#define ST_UPDATE_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Only stable numeric versions are accepted; never display untrusted markup. */
static bool st_parse_release_version(const char *text, unsigned parts[3])
{
    if (!text) return false;
    if (*text == 'v') ++text;
    for (unsigned i = 0; i < 3; ++i) {
        if (*text < '0' || *text > '9') return false;
        unsigned value = 0;
        const char *start = text;
        while (*text >= '0' && *text <= '9') {
            if (value > 9999) return false;
            value = value * 10 + (unsigned)(*text++ - '0');
        }
        if (text - start > 1 && *start == '0') return false;
        parts[i] = value;
        if (i < 2 && *text++ != '.') return false;
    }
    return *text == '\0';
}

static bool st_release_is_newer(const char *candidate, const char *installed)
{
    unsigned a[3], b[3];
    if (!st_parse_release_version(candidate, a) ||
        !st_parse_release_version(installed, b)) return false;
    for (unsigned i = 0; i < 3; ++i) {
        if (a[i] != b[i]) return a[i] > b[i];
    }
    return false;
}

#endif
