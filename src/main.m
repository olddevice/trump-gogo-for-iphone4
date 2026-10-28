/*
 * main.m — entry point.
 */
#import <UIKit/UIKit.h>
#import "AppDelegate.h"
#import "tglog.h"

int main(int argc, char *argv[])
{
    tglog_write("tg: main entered");
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
    }
}
