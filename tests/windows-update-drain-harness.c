#define ST_UPDATE_TEST_MODE
#include "../plugin.c"
#include <assert.h>
const char *st_update_test_url;
const char *st_update_test_executable;
int main(int argc, char **argv)
{
    assert(argc == 2);
    st_update_test_executable = argv[1];
    for (unsigned i = 0; i < 100; ++i) {
        char body[1024]; size_t used = 0;
        assert(st_update_capture(body, sizeof(body) - 1, &used));
        assert(used >= 4 && strcmp(body + used - 4, "\n200") == 0);
    }
    puts("PASS Windows child-exit/final-byte pipe drain (100 runs)");
}
