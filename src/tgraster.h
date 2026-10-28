/*
 * tgraster.h — tiny software rasteriser for TrumpGoGo.
 *
 * CoreGraphics turned out to be unusable on the A4 for a real-time game: a
 * single scaled sprite blit costs ~130 ms there, a two-word HUD string costs
 * more than a second per frame, and every path fill carries ~0.3 ms of fixed
 * overhead before it touches a pixel.  Everything the game draws is flat
 * shaded (quads, triangles, ellipses, two gradients, one sprite, a dozen
 * glyphs), so this file implements exactly that and nothing else.
 *
 * Pixels are 32-bit premultiplied ARGB stored as native little-endian words,
 * so the buffer can be handed to CoreGraphics once per frame as a CGImage and
 * pushed to the screen through CALayer.contents (measured at ~0.1 ms/update).
 *
 * All coordinates are in "world units" (the 240x360 virtual canvas the scene
 * is written in); the rasteriser scales them by `scale` into buffer pixels.
 */
#ifndef TGRASTER_H
#define TGRASTER_H

#include <stdint.h>

/* A bitmap: premultiplied 0xAARRGGBB, row-major, w*h words.
 * `stride` is the distance between rows in words, which lets a sprite cell be
 * a window into the packed atlas instead of a copy. */
typedef struct {
    int w, h, stride;
    uint32_t *px;
} TGImage;

/* An 8-bit coverage font atlas laid out as one row of `count` cells. */
typedef struct {
    int cw, ch;          /* cell size in atlas pixels                      */
    int first, count;    /* first character code and how many follow       */
    int aw, ah;          /* full atlas size                                */
    unsigned char *cov;  /* aw*ah coverage bytes                           */
    unsigned char *adv;  /* count advances in font pixels                  */
} TGFont;

typedef struct TGRaster TGRaster;

/* ------------------------------------------------------------------ setup */
TGRaster *rs_new(int w, int h, float scale);
void      rs_free(TGRaster *r);
uint32_t *rs_pixels(TGRaster *r);
int       rs_width(TGRaster *r);
int       rs_height(TGRaster *r);

/* ------------------------------------------------------------- per frame */
void rs_begin(TGRaster *r);                    /* reset clip + offset     */
void rs_save(TGRaster *r);
void rs_restore(TGRaster *r);
void rs_translate(TGRaster *r, float dx, float dy);
void rs_clip(TGRaster *r, float x, float y, float w, float h);

/* ------------------------------------------------------------ fill state */
void rs_set_fill(TGRaster *r, uint32_t argb);
void rs_set_alpha(TGRaster *r, float a);
#define RS_RGB(c) ((c) & 0x00FFFFFFu)

/* --------------------------------------------------------------- shapes */
void rs_rect(TGRaster *r, float x, float y, float w, float h);
void rs_quad(TGRaster *r, float ax, float ay, float bx, float by,
                        float cx, float cy, float dx, float dy);
void rs_tri(TGRaster *r, float ax, float ay, float bx, float by,
                       float cx, float cy);
void rs_ellipse(TGRaster *r, float x, float y, float w, float h);
void rs_line(TGRaster *r, float x0, float y0, float x1, float y1, float width);
void rs_ellipse_stroke(TGRaster *r, float x, float y, float w, float h,
                       float width);
/* Arbitrary polygon (max 160 vertices), nonzero winding. */
void rs_poly(TGRaster *r, const float *xs, const float *ys, int n);

/* ------------------------------------------------------------ gradients */
/* Vertical linear gradient over the box [x, x+w] x [y0, y1]. */
void rs_gradient_v(TGRaster *r, float x, float w, float y0, float y1,
                   uint32_t top, uint32_t bottom);
/* Radial falloff: `inner` at the centre fading to `outer` at radius. */
void rs_radial(TGRaster *r, float cx, float cy, float rad,
               uint32_t inner, uint32_t outer);

/* --------------------------------------------------------------- bitmap */
void rs_blit(TGRaster *r, const TGImage *img, float x, float y,
             float w, float h, float alpha, int smooth);

/* ----------------------------------------------------------------- text */
/* `x` is the pen origin in scene units; align 0 = left, 1 = right, 2 = centre.
 * Glyph cells are drawn at their baked size (1:1 device pixels), so a font
 * baked for a 2x buffer yields 12 pt-looking text in a 240-wide scene. */
void rs_text(TGRaster *r, const TGFont *f, float x, float y,
             const char *s, uint32_t color, int align);
void rs_text_shadow(TGRaster *r, const TGFont *f, float x, float y,
                    const char *s, uint32_t color, int align);
/* Width in device pixels, i.e. what the pen actually covers. */
float rs_text_width(const TGFont *f, const char *s);

/* ------------------------------------------------------------ utilities */
/*
 * Load a ".bin" written by tools/gen_assets.py (atlas or font wrapper).
 * Returns 0 on success.  Plain fread + a tiny header, so the same file works
 * on the iPhone and on the host test harness.
 */
int  rs_load_image(const char *path, TGImage *out);
int  rs_load_font(const char *path, TGFont *out);
void rs_free_image(TGImage *img);
void rs_free_font(TGFont *f);

/* Screenshots without any image framework: 32-bit BGRA BMP (bottom-up). */
int  rs_write_bmp(const char *path, const uint32_t *px, int w, int h);

#endif /* TGRASTER_H */
