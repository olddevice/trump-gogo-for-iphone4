/*
 * GoGoView.h — TrumpGoGo view controller layer for iOS.
 *
 * The view owns the frame clock, the accelerometer, the touch pads, the sound
 * and the presentation; every pixel comes from gogo_scene.c drawing into a
 * TGRaster (see tgraster.h for why CoreGraphics is not used for the scene).
 */
#import <UIKit/UIKit.h>

@interface GoGoView : UIView

- (void)startEngine;
- (void)stopEngine;

// ---- test hooks ---------------------------------------------------------
// They drive the view without a window-server connection, so behaviour can be
// checked on the device from a plain command-line binary.
- (void)advance:(float)dt;              // step the simulation
- (void)setSteer:(float)roll;           // force the steering value (-1..1)
- (void)tap;                            // same as tapping the screen
- (void)hideForTest;                    // skip the loading splash
- (void)renderToPath:(NSString *)path;  // write the current frame as a BMP
- (double)benchmarkRender:(int)n;       // ms per drawn frame

@end
