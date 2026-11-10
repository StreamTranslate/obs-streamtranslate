#import <AppKit/AppKit.h>
#include <stdbool.h>

static bool st_copy_key_to_pasteboard(NSPasteboard *pasteboard, const char *key)
{
    if (!key || !*key)
        return false;
    NSString *text = [NSString stringWithUTF8String:key];
    if (!text)
        return false;

    [pasteboard clearContents];
    return [pasteboard setString:text forType:NSPasteboardTypeString];
}

bool st_copy_plugin_key_to_clipboard(const char *key)
{
    @autoreleasepool {
        return st_copy_key_to_pasteboard([NSPasteboard generalPasteboard], key);
    }
}
