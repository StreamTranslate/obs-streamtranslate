#import "../clipboard-macos.m"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    @autoreleasepool {
        NSPasteboard *pasteboard = [NSPasteboard pasteboardWithUniqueName];
        assert(pasteboard);
        assert(st_copy_key_to_pasteboard(pasteboard, "synthetic_key-42"));
        assert([[pasteboard stringForType:NSPasteboardTypeString] isEqualToString:@"synthetic_key-42"]);
        assert(!st_copy_key_to_pasteboard(pasteboard, ""));
        assert(!st_copy_key_to_pasteboard(pasteboard, "\xff"));
        assert([[pasteboard stringForType:NSPasteboardTypeString] isEqualToString:@"synthetic_key-42"]);
        [pasteboard releaseGlobally];
    }
    puts("Named macOS pasteboard copied the exact synthetic key and rejected empty or invalid text");
    return 0;
}
