#include <stdio.h>
int main(void)
{
    /* Exit immediately after writing, to exercise final-pipe-byte ordering. */
    fputs("{\"draft\":false,\"prerelease\":false,\"tag_name\":\"v0.1.13\"}\n200", stdout);
    return 0;
}
