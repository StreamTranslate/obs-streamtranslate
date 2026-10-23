#include "../ca-path.h"
#include <assert.h>
#include <stdio.h>

static void boundary(const char *suffix)
{
    struct { unsigned char before; char path[1024]; unsigned char after; } guarded;
    char base[2048];
    memset(&guarded, 0xa5, sizeof(guarded));
    size_t n = sizeof(guarded.path) - strlen(suffix) - 1;
    memset(base, 'x', n); base[n] = 0;
    assert(st_checked_path_join(guarded.path, sizeof(guarded.path), base, suffix));
    assert(strlen(guarded.path) == sizeof(guarded.path)-1);
    assert(!strcmp(guarded.path+n, suffix));
    assert(guarded.before == 0xa5 && guarded.after == 0xa5);
    base[n] = 'x'; base[n+1] = 0;
    assert(!st_checked_path_join(guarded.path, sizeof(guarded.path), base, suffix));
    assert(!guarded.path[0] && guarded.before == 0xa5 && guarded.after == 0xa5);
    memset(base, 'y', sizeof(base)-1); base[sizeof(base)-1] = 0;
    assert(!st_checked_path_join(guarded.path, sizeof(guarded.path), base, ""));
    assert(!guarded.path[0] && guarded.before == 0xa5 && guarded.after == 0xa5);
    assert(!st_checked_path_join(guarded.path, sizeof(guarded.path), "", base));
    assert(!guarded.path[0]);
}
int main(void)
{
    boundary("/cacert.pem");
    boundary("/Contents/Resources/cacert.pem");
    boundary("\\cacert.pem");
    char out[12] = "stale";
    assert(st_checked_path_join(out, sizeof(out), "/tmp", "/ca.pem"));
    assert(!strcmp(out, "/tmp/ca.pem"));
    assert(!st_checked_path_join(out, 10, "/tmp", "/ca.pem") && !out[0]);
    assert(!st_checked_path_join(out, sizeof(out), NULL, "/ca.pem") && !out[0]);
    assert(!st_checked_path_join(out, sizeof(out), "/tmp", NULL) && !out[0]);
    assert(!st_checked_path_join(NULL, 10, "/tmp", "/ca.pem"));
    assert(!st_checked_path_join(out, 0, "/tmp", "/ca.pem"));
    puts("PASS exact fit, oversized base/suffix/combined paths, cleared rejection and boundary canaries");
}
