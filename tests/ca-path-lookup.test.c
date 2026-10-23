#include <assert.h>
#include "../plugin.c"
int main(int argc, char **argv)
{
    assert(argc == 2);
    assert(access(argv[1], R_OK) == 0);
    const char *selected = st_ca_bundle_path();
    assert(selected && !strcmp(selected, argv[1]));
    assert(access(selected, R_OK) == 0);
    assert(st_ca_bundle_path() == selected);
    puts("PASS actual module-relative readable CA selection and cached lookup");
}
