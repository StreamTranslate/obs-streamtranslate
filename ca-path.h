#ifndef ST_CA_PATH_H
#define ST_CA_PATH_H
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Inputs must not overlap output. A rejected path is always empty; never let a
 * truncated module name or CA filename select a different readable file. */
static bool st_checked_path_join(char *output, size_t capacity,
                                const char *base, const char *suffix)
{
    if (!output || !capacity) return false;
    output[0] = 0;
    if (!base || !suffix) return false;
    size_t base_length = 0, suffix_length = 0;
    while (base_length < capacity && base[base_length]) ++base_length;
    while (suffix_length < capacity && suffix[suffix_length]) ++suffix_length;
    if (base_length == capacity || suffix_length == capacity ||
        base_length > capacity - 1 - suffix_length) return false;
    memcpy(output, base, base_length);
    memcpy(output + base_length, suffix, suffix_length + 1);
    return true;
}
#endif
