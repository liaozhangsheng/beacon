#import <AppKit/AppKit.h>

namespace beacon {
int foreground_process_id() {
    @autoreleasepool {
        return NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
    }
}
}  // namespace beacon
