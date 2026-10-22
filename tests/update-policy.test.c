#include "../update-policy.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    assert(st_release_is_newer("v0.1.12", "0.1.11"));
    assert(st_release_is_newer("0.1.100", "0.1.99"));
    assert(st_release_is_newer("0.2.0", "0.1.999"));
    assert(st_release_is_newer("1.0.0", "0.999.999"));
    assert(!st_release_is_newer("v0.1.11", "0.1.11"));
    assert(!st_release_is_newer("0.1.9", "0.1.11"));
    const char *bad[] = {"", "v", "1", "1.2", "1.2.", "1..2", "1.2.3.4", "-1.2.3",
        "1.2.3-beta", "1.2.3+build", "<b>1.2.3</b>", "01.2.3", " 1.2.3", "1.2.3\n",
        "999999999999999999999999.0.0", "1.2.3<script>", NULL};
    unsigned parts[3];
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        assert(!st_parse_release_version(bad[i], parts));
    assert(!st_release_is_newer(NULL, "0.1.11"));
    assert(!st_release_is_newer("0.1.12", NULL));
    puts("PASS stable numeric versions, ordering, equality, invalid/prerelease/markup/overflow rejection");
}
