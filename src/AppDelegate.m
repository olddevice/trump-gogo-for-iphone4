/*
 * AppDelegate.m — window + full-screen game view. No storyboard, no nib:
 * the app is one view that draws itself every frame.
 */
#import "AppDelegate.h"
#import "GoGoView.h"
#import "tglog.h"

@interface GoGoRoot : UIViewController
@end

@implementation GoGoRoot
{
    GoGoView *game;
}

- (void)loadView
{
    game = [[GoGoView alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
    self.view = game;
}

- (void)viewDidAppear:(BOOL)animated
{
    [super viewDidAppear:animated];
    [game startEngine];
}

- (void)viewDidDisappear:(BOOL)animated
{
    [super viewDidDisappear:animated];
    [game stopEngine];
}

- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)shouldAutorotate { return NO; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations
{
    return UIInterfaceOrientationMaskPortrait;
}

- (void)dealloc
{
    // ARC is off; game is owned by self.view
    [super dealloc];
}

@end

@implementation AppDelegate
{
    UIWindow *window;
    GoGoRoot *root;
}

- (BOOL)application:(UIApplication *)app
        didFinishLaunchingWithOptions:(NSDictionary *)opts
{
    window = [[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
    root = [[GoGoRoot alloc] init];
    window.rootViewController = root;
    [window makeKeyAndVisible];

    // A runner needs the screen awake.
    [UIApplication sharedApplication].idleTimerDisabled = YES;

    tglog_write("tg: launched on %s (%.0fx%.0f @%.0fx)",
          [[[UIDevice currentDevice] model] UTF8String],
          window.bounds.size.width, window.bounds.size.height,
          [UIScreen mainScreen].scale);
    return YES;
}

- (void)applicationWillTerminate:(UIApplication *)app
{
    [UIApplication sharedApplication].idleTimerDisabled = NO;
}

- (void)dealloc
{
    [root release];
    [window release];
    [super dealloc];
}

@end
