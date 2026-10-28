/*
 * ios_launch.m — ask SpringBoard to open an app by bundle id.
 *
 * A UIKit app exec'd straight from a root shell never becomes the front
 * app: UIApplicationMain blocks waiting for the activation handshake, so
 * didFinishLaunching (and therefore CADisplayLink) never runs. Going
 * through LSApplicationWorkspace performs that handshake, which is what
 * the home screen does when you tap an icon.
 *
 * Build: clang -arch armv7 -isysroot <SDK> -miphoneos-version-min=7.0 \
 *              -framework Foundation -lobjc -o ios_launch ios_launch.m
 * Usage: ios_launch com.claw.trumpgogo
 */
#import <Foundation/Foundation.h>
#import <dlfcn.h>
#include <stdio.h>

@protocol TGLSSpace
+ (id)defaultWorkspace;
- (BOOL)openApplicationWithBundleID:(NSString *)bundleID;
@end

static Class tg_workspace_class(void)
{
    Class c = NSClassFromString(@"LSApplicationWorkspace");
    if (c) return c;

    // Private framework name differs across iOS versions; try the usual spots.
    const char *paths[] = {
        "/System/Library/Frameworks/MobileCoreServices.framework/MobileCoreServices",
        "/System/Library/PrivateFrameworks/MobileCoreServices.framework/MobileCoreServices",
        "/System/Library/PrivateFrameworks/LaunchServices.framework/LaunchServices",
        "/System/Library/Frameworks/CoreServices.framework/CoreServices",
    };
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        dlopen(paths[i], RTLD_LAZY);
        c = NSClassFromString(@"LSApplicationWorkspace");
        if (c) return c;
    }
    return nil;
}

int main(int argc, char *argv[])
{
    @autoreleasepool {
        if (argc < 2) {
            fprintf(stderr, "usage: ios_launch <bundle-id>\n");
            return 2;
        }
        NSString *bid = [NSString stringWithUTF8String:argv[1]];
        Class cls = tg_workspace_class();
        if (!cls) {
            fprintf(stderr, "ios_launch: LSApplicationWorkspace not found\n");
            return 3;
        }
        id ws = [(id)cls defaultWorkspace];
        BOOL ok = [(id<TGLSSpace>)ws openApplicationWithBundleID:bid];
        printf("ios_launch: %s -> %s\n", [bid UTF8String],
               ok ? "ok" : "refused");
        return ok ? 0 : 1;
    }
}
