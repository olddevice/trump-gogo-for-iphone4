/*
 * gogo_scene.h — the whole visible game, minus UIKit.
 *
 * Everything that used to live in GoGoView's -render: now runs against
 * TGRaster instead of CoreGraphics, so the exact same code paints the screen
 * on the phone and dumps frames in the host test harness (tools/hostshot.c).
 * The view keeps only what a view is good for: touches, the accelerometer,
 * sound and the frame clock.
 */
#ifndef GOGO_SCENE_H
#define GOGO_SCENE_H

#include "tgraster.h"
#include "trump_game.h"

#define GOGO_VW 240.0f
#define GOGO_VH 360.0f
#define GOGO_SCALE 2.0f          /* buffer pixels per world unit -> 480x720 */

typedef struct {
    TG g;
    TGRaster *r;                 /* borrowed, owned by the caller            */
    TGImage atlas;               /* packed sprite sheet, premultiplied       */
    TGFont font;
    int ready;

    float camX, camZ, pitSin, pitCos;
    float tSx, tSy, tZc;         /* projection scratch                       */

    int loading;
    float loadT;
    int touchMode;               /* on-screen stick instead of tilt          */
    float stkDX, stkDY;

    char km[24];
    char coin[24];
    int kmShown, coinShown;
    int noText;                  /* set to skip the HUD (profiling)          */
} GoGoScene;

/* Returns 0 when both asset files loaded. */
int  gogo_init(GoGoScene *s, TGRaster *r, const char *atlasPath,
               const char *fontPath, uint64_t seed);
void gogo_shutdown(GoGoScene *s);

/* Advance the world (loading splash timer included). */
void gogo_step(GoGoScene *s, float dt);

/* Paint the current state into the raster. */
void gogo_draw(GoGoScene *s);

/* Stage boundaries recorded by -DGOGO_PROFILE, in paint order.  The build has
 * no profiling (two clock reads per stage is real money on an A4), but the
 * names live here so the probes agree with the scene. */
#define GOGO_PROF_N 11
#if defined(GOGO_PROFILE)
extern double gogo_prof[GOGO_PROF_N];
#endif
#define GOGO_PROF_NAMES \
    "setup", "sky", "grass", "planks", "lines", "path", "stripes", \
    "objs", "trump", "behind", "hud"

#endif /* GOGO_SCENE_H */
