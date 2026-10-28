/*
 * GoGoView.m — TrumpGoGo for iOS.
 *
 * What the view does: frame clock, accelerometer tilt, touch pads, coin sound,
 * and pushing finished frames to the screen.
 *
 * What it does NOT do: draw.  The scene lives in gogo_scene.c and paints into
 * a TGRaster (a plain 480x720 RGBA buffer).  That is a deliberate decision —
 * CoreGraphics on an A4 turned out to cost ~130 ms for one scaled sprite blit
 * and over a second for a two-word HUD string, which no game can live with.
 * The software rasteriser does a whole frame in a few milliseconds, and the
 * result is handed to the GPU as a single CGImage via CALayer.contents, which
 * measured at ~0.1 ms per update.
 */
#import "GoGoView.h"
#import "gogo_scene.h"
#import "tgraster.h"
#import "tglog.h"

#import <QuartzCore/QuartzCore.h>
#import <CoreMotion/CoreMotion.h>
#import <AVFoundation/AVFoundation.h>
#import <math.h>
#import <sys/time.h>
#import <unistd.h>

static const float TILT_FULL_DEG = 40.0f;   // deflection that means full tilt

// on-screen controls, in scene units (only drawn when tilt is unavailable)
static const float STICK_X = 54.0f, STICK_Y = 296.0f;
static const float STICK_R = 36.0f;
static const float JUMP_X = 186.0f, JUMP_Y = 296.0f, JUMP_R = 30.0f;

@implementation GoGoView
{
    TGRaster *ras[2];          // double buffered: the layer keeps last frame
    int bufIndex;
    CGColorSpaceRef colorSpace;
    GoGoScene scene;

    CADisplayLink *link;
    CFTimeInterval lastT;
    CFTimeInterval tiltWaitFrom;
    BOOL wasLoading;

    BOOL tiltSeen;             // the accelerometer delivered a usable sample
    BOOL steerLocked;          // tests hold the steering value (see setSteer:)

    CMMotionManager *motion;
    AVAudioPlayer *coin;
    int lastPickup;

    UITouch *stickTouch;
    UITouch *jumpTouch;

    // frame timing instrumentation
    int statFrames;
    double statDt, statDraw;
    int statReports;
    BOOL measured;
}

- (id)initWithFrame:(CGRect)frame
{
    if ((self = [super initWithFrame:frame])) {
        self.backgroundColor = [UIColor blackColor];
        self.opaque = YES;
        self.multipleTouchEnabled = YES;

        colorSpace = CGColorSpaceCreateDeviceRGB();

        for (int i = 0; i < 2; i++) {
            ras[i] = rs_new((int)(GOGO_VW * GOGO_SCALE),
                            (int)(GOGO_VH * GOGO_SCALE), GOGO_SCALE);
        }
        if (!ras[0] || !ras[1]) tglog_write("tg: raster allocation failed");

        // integer-only seed: double -> 64-bit conversions need compiler-rt
        // helpers that the bare SDK does not ship
        struct timeval tv;
        gettimeofday(&tv, NULL);
        uint64_t seed = (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;

        NSString *atlas = [[NSBundle mainBundle] pathForResource:@"atlas"
                                                          ofType:@"bin"];
        NSString *font = [[NSBundle mainBundle] pathForResource:@"font"
                                                         ofType:@"bin"];
        if (!atlas || !font) {
            tglog_write("tg: assets missing (atlas=%s font=%s)",
                        atlas ? "ok" : "no", font ? "ok" : "no");
        } else if (gogo_init(&scene, ras[0], [atlas UTF8String],
                             [font UTF8String], seed) != 0) {
            tglog_write("tg: gogo_init failed");
        }
        wasLoading = YES;

        // one image per frame, stretched over the view: the GPU does the
        // 480x720 -> 640x960 upscale, which costs far less than drawing them
        self.layer.contentsGravity = kCAGravityResize;
        self.layer.magnificationFilter = kCAFilterLinear;
        self.layer.minificationFilter = kCAFilterLinear;
        self.layer.opaque = YES;

        [self setupTilt];
        [self setupSound];

        [[NSNotificationCenter defaultCenter] addObserver:self
            selector:@selector(appWillResign:)
            name:UIApplicationWillResignActiveNotification object:nil];
        [[NSNotificationCenter defaultCenter] addObserver:self
            selector:@selector(appDidBecomeActive:)
            name:UIApplicationDidBecomeActiveNotification object:nil];
    }
    return self;
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [self stopEngine];
    [motion release];
    [coin release];
    gogo_shutdown(&scene);
    for (int i = 0; i < 2; i++) rs_free(ras[i]);
    if (colorSpace) CGColorSpaceRelease(colorSpace);
    [super dealloc];
}

// ------------------------------------------------------------- start-up

- (void)setupTilt
{
    motion = [[CMMotionManager alloc] init];
    if (!motion.accelerometerAvailable) {
        tglog_write("tg: no accelerometer -> on-screen controls");
        scene.touchMode = 1;
        [motion release];
        motion = nil;
        return;
    }
    motion.accelerometerUpdateInterval = 1.0 / 60.0;
    [motion startAccelerometerUpdates];      // polled from the frame loop
    scene.touchMode = 0;
    tiltWaitFrom = CACurrentMediaTime();
    tglog_write("tg: accelerometer ready, tilt steering on");
}

- (void)setupSound
{
    if (getenv("TG_NO_SND")) return;   // headless tools
    NSString *p = [[NSBundle mainBundle] pathForResource:@"coin" ofType:@"wav"];
    if (!p) { tglog_write("tg: coin.wav missing"); return; }
    NSError *err = nil;
    coin = [[AVAudioPlayer alloc] initWithContentsOfURL:
                [NSURL fileURLWithPath:p] error:&err];
    if (!coin) {
        tglog_write("tg: coin.wav failed: %s", [[err description] UTF8String]);
        return;
    }
    coin.enableRate = YES;
    coin.volume = 0.65f;
    [coin prepareToPlay];

    // Ambient: keep whatever the user is listening to playing underneath.
    [[AVAudioSession sharedInstance]
        setCategory:AVAudioSessionCategoryAmbient error:NULL];
    [[AVAudioSession sharedInstance] setActive:YES error:NULL];
}

// ------------------------------------------------------- engine lifecycle

- (void)startEngine
{
    if (link) return;
    lastT = CACurrentMediaTime();
    link = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick:)];
    link.frameInterval = 1;
    [link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
    tglog_write("tg: engine started (%.0fx%.0f pt, buffer %dx%d)",
                self.bounds.size.width, self.bounds.size.height,
                rs_width(ras[0]), rs_height(ras[0]));
}

- (void)stopEngine
{
    if (!link) return;
    [link invalidate];
    link = nil;
}

- (void)appWillResign:(NSNotification *)n { [self stopEngine]; }

- (void)appDidBecomeActive:(NSNotification *)n
{
    if (self.window) [self startEngine];
}

// -------------------------------------------------------------- the loop

- (void)tick:(CADisplayLink *)l
{
    float dt = [self frameDelta];
    [self update:dt];
    [self drawFrame];
    [self sampleFrame:dt];
}

- (float)frameDelta
{
    CFTimeInterval now = CACurrentMediaTime();
    float dt = (float)(now - lastT);
    lastT = now;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.05f) dt = 0.05f;
    return dt;
}

- (void)update:(float)dt
{
    [self pollTilt];

    // a sensor may be advertised but stay silent — fall back to the pads
    if (!scene.touchMode && !tiltSeen &&
        CACurrentMediaTime() - tiltWaitFrom > 1.5) {
        scene.touchMode = 1;
        tglog_write("tg: tilt silent -> on-screen controls");
    }

    gogo_step(&scene, dt);

    if (wasLoading && !scene.loading) {
        wasLoading = NO;
        tglog_write("tg: run started");
    }

    if (scene.g.pkSeq != lastPickup) {     // coin picked up -> ching
        lastPickup = scene.g.pkSeq;
        [self playCoin];
    }
}

- (void)drawFrame
{
    CFTimeInterval t0 = CACurrentMediaTime();

    TGRaster *r = ras[bufIndex];
    scene.r = r;
    gogo_draw(&scene);
    [self present:r];

    bufIndex ^= 1;                          // next frame goes to the other one
    statDraw += CACurrentMediaTime() - t0;
}

/* Hand the finished buffer to the GPU.  No copy: the CGImage wraps the same
 * pixels the rasteriser just wrote, and the other buffer is used next frame so
 * the layer never reads pixels that are being overwritten. */
- (void)present:(TGRaster *)r
{
    int w = rs_width(r), h = rs_height(r);
    size_t len = (size_t)w * (size_t)h * 4;
    CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, rs_pixels(r),
                                                        len, NULL);
    if (!dp) return;
    CGImageRef img = CGImageCreate(w, h, 8, 32, (size_t)w * 4, colorSpace,
                                   kCGBitmapByteOrder32Little |
                                       kCGImageAlphaPremultipliedFirst,
                                   dp, NULL, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(dp);
    if (!img) return;
    self.layer.contents = (id)img;
    CGImageRelease(img);
}

- (void)sampleFrame:(float)dt
{
    statFrames++;
    statDt += dt;

    static CFTimeInterval lastReport = 0;
    if (statFrames < 90) return;

    double avg = statDt / statFrames;
    double draw = statDraw / statFrames;
    int cap = (link && link.frameInterval == 2) ? 30 : 60;
    statDraw = 0;
    statFrames = 0;
    statDt = 0;

    CFTimeInterval now = CACurrentMediaTime();
    if (now - lastReport > 2.0) {          // do not spam the log
        lastReport = now;
        tglog_write("tg: %.1f ms/frame (draw %.1f ms) cap=%d fps",
                    avg * 1000.0, draw * 1000.0, cap);
    }

    // One-shot downgrade: if the device cannot hold ~45 fps, drop to 30.
    if (!measured && avg > 0.0215 && link && link.frameInterval == 1) {
        link.frameInterval = 2;
        tglog_write("tg: too slow for 60 fps -> 30 fps");
    }
    measured = YES;
}

- (void)pollTilt
{
    if (steerLocked || !motion || scene.touchMode) return;
    CMAccelerometerData *d = motion.accelerometerData;
    if (!d) return;

    tiltSeen = YES;
    double x = d.acceleration.x;
    double y = d.acceleration.y;
    double z = d.acceleration.z;
    // roll about the screen's vertical axis: + = right edge down = steer right
    double ang = atan2(x, sqrt(y * y + z * z)) * (180.0 / M_PI);
    float r;
    if (fabs(ang) < 2.5) r = 0.0f;                  // dead zone, no drift
    else r = (float)(ang / TILT_FULL_DEG);
    if (r > 1.0f) r = 1.0f;
    if (r < -1.0f) r = -1.0f;
    scene.g.roll = scene.g.roll * 0.65f + r * 0.35f;    // light smoothing
}

- (void)playCoin
{
    if (!coin) return;
    coin.rate = 0.94f + (scene.g.pkSeq % 5) * 0.03f;    // pitch varies
    coin.currentTime = 0.0;
    [coin play];
}

// ------------------------------------------------------------------ input

- (void)touchesBegan:(NSSet *)touches withEvent:(UIEvent *)event
{
    if (scene.loading) return;

    if (!scene.touchMode) {              // tilt devices: tap anywhere jumps
        tg_jump(&scene.g);
        return;
    }
    for (UITouch *t in touches) {
        CGPoint p = [t locationInView:self];
        float x = p.x * GOGO_VW / self.bounds.size.width;
        float y = p.y * GOGO_VH / self.bounds.size.height;
        if (!jumpTouch && [self inJumpX:x y:y]) {
            jumpTouch = t;
            tg_jump(&scene.g);
        } else if (!stickTouch && [self inStickX:x y:y]) {
            stickTouch = t;
            [self stickToX:x y:y];
        }
    }
}

- (void)touchesMoved:(NSSet *)touches withEvent:(UIEvent *)event
{
    if (scene.loading || !scene.touchMode || !stickTouch) return;
    for (UITouch *t in touches) {
        if (t != stickTouch) continue;
        CGPoint p = [t locationInView:self];
        [self stickToX:p.x * GOGO_VW / self.bounds.size.width
                    y:p.y * GOGO_VH / self.bounds.size.height];
    }
}

- (void)touchesEnded:(NSSet *)touches withEvent:(UIEvent *)event
{
    [self releaseTouches:touches];
}

- (void)touchesCancelled:(NSSet *)touches withEvent:(UIEvent *)event
{
    [self releaseTouches:touches];
}

- (void)releaseTouches:(NSSet *)touches
{
    for (UITouch *t in touches) {
        if (t == stickTouch) {
            stickTouch = nil;
            [self releaseStick];
        }
        if (t == jumpTouch) jumpTouch = nil;
    }
}

- (BOOL)inStickX:(float)x y:(float)y
{
    if (x < 120.0f && y > 224.0f) return YES;   // generous lower-left grab
    float dx = x - STICK_X, dy = y - STICK_Y;
    return dx * dx + dy * dy < (STICK_R + 16.0f) * (STICK_R + 16.0f);
}

- (BOOL)inJumpX:(float)x y:(float)y
{
    float dx = x - JUMP_X, dy = y - JUMP_Y;
    float r = JUMP_R + 8.0f;
    return dx * dx + dy * dy < r * r;
}

- (void)stickToX:(float)x y:(float)y
{
    float dx = x - STICK_X;
    float dy = y - STICK_Y;
    if (dx > STICK_R) dx = STICK_R;
    if (dx < -STICK_R) dx = -STICK_R;
    float lim = STICK_R * 0.85f;
    if (dy > lim) dy = lim;
    if (dy < -lim) dy = -lim;
    scene.stkDX = dx;
    scene.stkDY = dy;
    scene.g.roll = dx / STICK_R;         // full deflection = full speed
}

- (void)releaseStick
{
    scene.stkDX = 0.0f;
    scene.stkDY = 0.0f;
    scene.g.roll = 0.0f;
}

// ------------------------------------------------------- test hooks

- (void)advance:(float)dt
{
    [self update:dt];
}

- (void)setSteer:(float)roll
{
    if (roll > 1.0f) roll = 1.0f;
    if (roll < -1.0f) roll = -1.0f;
    steerLocked = YES;      // stop the accelerometer from overwriting it
    scene.g.roll = roll;
}

- (void)hideForTest
{
    scene.loading = 0;
    wasLoading = NO;
    tg_reset(&scene.g);
}

- (void)tap
{
    tg_jump(&scene.g);
}

/* Write the current frame as a BMP, without needing a window server. */
- (void)renderToPath:(NSString *)path
{
    scene.r = ras[0];
    gogo_draw(&scene);
    rs_write_bmp([path UTF8String], rs_pixels(ras[0]),
                 rs_width(ras[0]), rs_height(ras[0]));
}

- (double)benchmarkRender:(int)n
{
    if (n < 1) n = 1;
    scene.r = ras[0];
    CFTimeInterval t0 = CACurrentMediaTime();
    for (int i = 0; i < n; i++) gogo_draw(&scene);
    CFTimeInterval t1 = CACurrentMediaTime();
    return (t1 - t0) * 1000.0 / n;
}

@end
