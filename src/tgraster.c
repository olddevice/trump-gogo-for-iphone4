/*
 * tgraster.c — see tgraster.h for why this exists.
 *
 * Layout of the rasteriser:
 *   - every shape is turned into a list of edges and scan-converted with
 *     four sub-scanlines per pixel row and fractional span ends, which gives
 *     clean anti-aliasing at a cost the A4 can afford;
 *   - coverage lands in a one-row float buffer that is blended in a single
 *     pass, so a shape spanning 40 rows touches each pixel exactly once;
 *   - the destination is written straight into the frame buffer handed to
 *     CALayer, so nothing is copied between the rasteriser and the screen.
 */
#include "tgraster.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#define MAXP 160          /* most vertices in one path                     */
#define MAXE 160          /* most edges after clipping                     */
#define MAXSPAN 16        /* most spans per sub-scanline (an annulus is 2) */
#define SUBSCAN 2         /* sub-scanlines per pixel row                   */
#define COV_SHIFT 1       /* log2(SUBSCAN): coverage 8.8 -> 0..255         */

struct TGRaster {
    int w, h;
    uint32_t *px;
    float s;                     /* world units -> buffer pixels          */
    float ox, oy;                /* translation, in world units           */

    int cx0, cy0, cx1, cy1;      /* clip rectangle, buffer space          */

    uint32_t fill;               /* premultiplied current fill            */

    /* saved states: ox, oy, clip                                     */
    float st[8][6];
    int nst;

    /* scratch, allocated once */
    int *cov;                    /* one row of coverage, w+1 entries      */
    float bx[MAXP], by[MAXP];    /* transformed vertices                  */
    float ex0[MAXE], ey0[MAXE], ey1[MAXE], edx[MAXE];
    signed char ew[MAXE];
    float xs[MAXE];              /* crossings on one sub-scanline         */
    signed char ws[MAXE];
};

/* ------------------------------------------------------------- helpers */

static inline uint32_t mul255(int a, int b)
{
    int t = a * b + 128;
    return (uint32_t)((t + (t >> 8)) >> 8);
}

/* floorf/ceilf are only VFP instructions on ARMv8; on this CPU each call
 * leaves the unit and lands in libm, and geometry code calls them several
 * times per shape.  For anything inside the int range this is exact and
 * compiles to a conversion plus a conditional. */
static inline int ifloor(float v)
{
    int i = (int)v;
    return (v < (float)i) ? i - 1 : i;
}

static inline int iceil(float v)
{
    int i = (int)v;
    return (v > (float)i) ? i + 1 : i;
}

/* Source-over of a premultiplied pixel `s` at `cov` (0..255) onto an opaque
 * destination.  Inlined everywhere: an out-of-line call per pixel costs more
 * than the blend itself on this class of hardware. */
static inline __attribute__((always_inline))
uint32_t blend_pm(uint32_t d, uint32_t s, int cov)
{
    if (cov <= 0) return d;
    int sa = (int)(s >> 24);
    if (cov < 255) {
        sa = (int)mul255(sa, cov);
        if (sa == 0) return d;
        s = ((uint32_t)sa << 24) |
            (mul255((int)((s >> 16) & 255), cov) << 16) |
            (mul255((int)((s >>  8) & 255), cov) <<  8) |
             mul255((int)( s        & 255), cov);
    }
    if (sa >= 255) return s | 0xFF000000u;
    int ia = 255 - sa;
    unsigned rr = (unsigned)((s >> 16) & 255) + mul255((int)((d >> 16) & 255), ia);
    unsigned gg = (unsigned)((s >>  8) & 255) + mul255((int)((d >>  8) & 255), ia);
    unsigned bb = (unsigned)( s        & 255) + mul255((int)( d        & 255), ia);
    if (rr > 255) rr = 255;
    if (gg > 255) gg = 255;
    if (bb > 255) bb = 255;
    return 0xFF000000u | (rr << 16) | (gg << 8) | bb;
}

static inline uint32_t premul(uint32_t argb)
{
    unsigned a = argb >> 24;
    if (a == 255) return argb;
    if (a == 0) return 0;
    unsigned r = mul255((int)((argb >> 16) & 255), (int)a);
    unsigned g = mul255((int)((argb >>  8) & 255), (int)a);
    unsigned b = mul255((int)( argb        & 255), (int)a);
    return (a << 24) | (r << 16) | (g << 8) | b;
}

/* ---------------------------------------------------------- life cycle */

TGRaster *rs_new(int w, int h, float scale)
{
    TGRaster *r = (TGRaster *)calloc(1, sizeof(TGRaster));
    if (!r) return NULL;
    r->w = w;
    r->h = h;
    r->px = (uint32_t *)calloc((size_t)w * h, 4);
    r->cov = (int *)calloc((size_t)w + 2, sizeof(int));
    if (!r->px || !r->cov) { rs_free(r); return NULL; }
    r->s = scale;
    r->cx0 = 0; r->cy0 = 0; r->cx1 = w; r->cy1 = h;
    r->fill = 0xFF000000u;
    return r;
}

void rs_free(TGRaster *r)
{
    if (!r) return;
    free(r->px);
    free(r->cov);
    free(r);
}

uint32_t *rs_pixels(TGRaster *r) { return r->px; }
int rs_width(TGRaster *r)  { return r->w; }
int rs_height(TGRaster *r) { return r->h; }

void rs_begin(TGRaster *r)
{
    r->ox = r->oy = 0.0f;
    r->cx0 = 0; r->cy0 = 0; r->cx1 = r->w; r->cy1 = r->h;
    r->nst = 0;
    r->fill = 0xFF000000u;
}

void rs_save(TGRaster *r)
{
    if (r->nst >= 8) return;
    float *s = r->st[r->nst++];
    s[0] = r->ox; s[1] = r->oy;
    s[2] = (float)r->cx0; s[3] = (float)r->cy0;
    s[4] = (float)r->cx1; s[5] = (float)r->cy1;
}

void rs_restore(TGRaster *r)
{
    if (r->nst <= 0) return;
    float *s = r->st[--r->nst];
    r->ox = s[0]; r->oy = s[1];
    r->cx0 = (int)s[2]; r->cy0 = (int)s[3];
    r->cx1 = (int)s[4]; r->cy1 = (int)s[5];
}

void rs_translate(TGRaster *r, float dx, float dy)
{
    r->ox += dx;
    r->oy += dy;
}

void rs_clip(TGRaster *r, float x, float y, float w, float h)
{
    int x0 = ifloor((x + r->ox) * r->s);
    int y0 = ifloor((y + r->oy) * r->s);
    int x1 = iceil((x + w + r->ox) * r->s);
    int y1 = iceil((y + h + r->oy) * r->s);
    if (x0 > r->cx0) r->cx0 = x0;
    if (y0 > r->cy0) r->cy0 = y0;
    if (x1 < r->cx1) r->cx1 = x1;
    if (y1 < r->cy1) r->cy1 = y1;
    if (r->cx1 < r->cx0) r->cx1 = r->cx0;
    if (r->cy1 < r->cy0) r->cy1 = r->cy0;
}

void rs_set_fill(TGRaster *r, uint32_t argb) { r->fill = premul(argb); }

void rs_set_alpha(TGRaster *r, float a)
{
    if (a >= 1.0f) return;
    if (a < 0.0f) a = 0.0f;
    unsigned al = (unsigned)((r->fill >> 24) * a);
    uint32_t c = (r->fill & 0x00FFFFFFu) | (al << 24);
    r->fill = c;
}

/* ------------------------------------------------------------- filling */

/*
 * Blend one row of coverage (8.8 fixed point, SUBSCAN * 256 at full cover)
 * into the frame buffer.  Two things make this fast enough for an A4: the
 * loop is integer-only, and interior pixels -- which is nearly all of a big
 * shape like the path -- are detected as runs and stored straight into the
 * row instead of going through the blend.  Sampling the blend on every
 * covered pixel would cost roughly eight multiply-accumulates per pixel over
 * the whole shape, and the path alone covers half the screen.
 */
static void composite_row(TGRaster *r, int y, int x0, int x1)
{
    if (y < r->cy0 || y >= r->cy1) return;
    if (x0 < r->cx0) x0 = r->cx0;
    if (x1 >= r->cx1) x1 = r->cx1 - 1;
    if (x1 < x0) return;

    uint32_t *row = r->px + (size_t)y * r->w;
    const uint32_t s = r->fill;
    const int opaque = ((s >> 24) == 255);
    const int *cv = r->cov;
    const int full = SUBSCAN * 256;      /* coverage of a fully covered pixel */

    int x = x0;
    while (x <= x1) {
        int c = cv[x];
        if (c >= full) {                 /* run of solid pixels */
            int e = x + 1;
            while (e <= x1 && cv[e] >= full) e++;
            if (opaque) {
                uint32_t *p = row + x, *q = row + e;
                while (p < q) *p++ = s;
            } else {
                for (int i = x; i < e; i++) row[i] = blend_pm(row[i], s, 255);
            }
            x = e;
            continue;
        }
        if (c > 0) {
            int cc = c >> COV_SHIFT;
            row[x] = blend_pm(row[x], s, cc > 255 ? 255 : cc);
        }
        x++;
    }
}

/* Coverage (8.8 fixed point) that the span [a,b) gives pixel column x. */
static inline int span_cov(float a, float b, int x)
{
    float l = (a > (float)x) ? a : (float)x;
    float h = (b < (float)(x + 1)) ? b : (float)(x + 1);
    if (h <= l) return 0;
    int c = (int)((h - l) * 256.0f);
    return c > 256 ? 256 : c;
}

static void fill_poly(TGRaster *r, const float *wx, const float *wy, int n)
{
    if (n < 3 || n > MAXP) return;

    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    for (int i = 0; i < n; i++) {
        r->bx[i] = (wx[i] + r->ox) * r->s;
        r->by[i] = (wy[i] + r->oy) * r->s;
        if (r->bx[i] < minx) minx = r->bx[i];
        if (r->bx[i] > maxx) maxx = r->bx[i];
        if (r->by[i] < miny) miny = r->by[i];
        if (r->by[i] > maxy) maxy = r->by[i];
    }

    int y0 = ifloor(miny);
    int y1 = iceil(maxy);
    int x0 = ifloor(minx);
    int x1 = iceil(maxx);
    if (y0 < r->cy0) y0 = r->cy0;
    if (y1 > r->cy1) y1 = r->cy1;
    if (x0 < r->cx0) x0 = r->cx0;
    if (x1 > r->cx1) x1 = r->cx1;
    if (y1 <= y0 || x1 <= x0) return;

    /* edges */
    int ne = 0;
    for (int i = 0; i < n && ne < MAXE; i++) {
        int j = (i + 1 == n) ? 0 : i + 1;
        float ax = r->bx[i], ay = r->by[i];
        float bx2 = r->bx[j], by2 = r->by[j];
        if (ay == by2) continue;
        if (ay < by2) {
            r->ex0[ne] = ax; r->ey0[ne] = ay; r->ey1[ne] = by2;
            r->ew[ne] = 1;
        } else {
            r->ex0[ne] = bx2; r->ey0[ne] = by2; r->ey1[ne] = ay;
            r->ew[ne] = -1;
        }
        r->edx[ne] = (bx2 - ax) / (by2 - ay);
        ne++;
    }
    if (ne < 2) return;

    /*
     * Per row the sub-scanlines give (almost always) the same spans, shifted
     * by a fraction of a pixel.  The pixels those spans agree on are solid,
     * so they can be written straight into the row; only the two ends need
     * coverage blending.  That is one pass over the row instead of five
     * (clear, accumulate twice, scan, fill) -- and passes are what this costs,
     * because the A4 has bandwidth for roughly one pass and nothing more.
     *
     * When the sub-scanlines disagree about how many spans there are -- the
     * vertex rows of a shape, or a row crossing the hole of a stroked ellipse
     * -- the row falls back to the coverage buffer below, which handles any
     * topology.
     */
    const float step = 1.0f / SUBSCAN;
    const uint32_t solid = r->fill;
    const int opaque = ((solid >> 24) == 255);
    int ns[SUBSCAN];
    float sa[SUBSCAN][MAXSPAN], sb[SUBSCAN][MAXSPAN];

    for (int y = y0; y < y1; y++) {
        if (y < r->cy0 || y >= r->cy1) continue;

        int tooMany = 0;
        for (int s2 = 0; s2 < SUBSCAN; s2++) {
            float sy = (float)y + ((float)s2 + 0.5f) * step;
            int nc = 0;
            for (int e = 0; e < ne; e++) {
                if (sy < r->ey0[e] || sy >= r->ey1[e]) continue;
                float x = r->ex0[e] + (sy - r->ey0[e]) * r->edx[e];
                /* insertion sort keeps the crossing list ordered */
                int k = nc++;
                while (k > 0 && r->xs[k - 1] > x) {
                    r->xs[k] = r->xs[k - 1];
                    r->ws[k] = r->ws[k - 1];
                    k--;
                }
                r->xs[k] = x;
                r->ws[k] = r->ew[e];
            }
            ns[s2] = 0;
            if (nc < 2) continue;

            int wind = 0;
            float start = 0.0f;
            for (int k = 0; k < nc; k++) {
                if (wind == 0) start = r->xs[k];
                wind += r->ws[k];
                if (wind != 0) continue;
                float a = start, b = r->xs[k];
                if (b <= a) continue;
                if (a < (float)x0) a = (float)x0;
                if (b > (float)x1) b = (float)x1;
                if (b <= a) continue;
                if (ns[s2] == MAXSPAN) { tooMany = 1; break; }
                sa[s2][ns[s2]] = a;
                sb[s2][ns[s2]] = b;
                ns[s2]++;
            }
        }

        int same = !tooMany;
        for (int s2 = 1; s2 < SUBSCAN && same; s2++)
            if (ns[s2] != ns[0]) same = 0;

        if (same) {
            if (ns[0] == 0) continue;
            uint32_t *row = r->px + (size_t)y * r->w;
            for (int k = 0; k < ns[0]; k++) {
                float amin = sa[0][k], amax = amin;
                float bmin = sb[0][k], bmax = bmin;
                for (int s2 = 1; s2 < SUBSCAN; s2++) {
                    if (sa[s2][k] < amin) amin = sa[s2][k];
                    if (sa[s2][k] > amax) amax = sa[s2][k];
                    if (sb[s2][k] < bmin) bmin = sb[s2][k];
                    if (sb[s2][k] > bmax) bmax = sb[s2][k];
                }
                int ix0 = iceil(amax), ix1 = ifloor(bmin);  /* solid */
                int jx0 = ifloor(amin), jx1 = iceil(bmax);  /* touched */
                if (ix0 < x0) ix0 = x0;
                if (ix1 > x1) ix1 = x1;
                if (jx0 < x0) jx0 = x0;
                if (jx1 > x1) jx1 = x1;
                if (ix1 > ix0) {
                    if (opaque) {
                        uint32_t *p = row + ix0, *q = row + ix1;
                        while (p < q) *p++ = solid;
                    } else {
                        for (int x = ix0; x < ix1; x++)
                            row[x] = blend_pm(row[x], solid, 255);
                    }
                }
                for (int x = jx0; x < jx1; x++) {
                    if (x >= ix0 && x < ix1) continue;
                    int c8 = 0;
                    for (int s2 = 0; s2 < SUBSCAN; s2++)
                        c8 += span_cov(sa[s2][k], sb[s2][k], x);
                    if (c8 <= 0) continue;
                    int c = c8 >> COV_SHIFT;
                    row[x] = blend_pm(row[x], solid, c > 255 ? 255 : c);
                }
            }
            continue;
        }

        /* general case: accumulate coverage, then blend the row */
        int *cv = r->cov;
        memset(cv + x0, 0, (size_t)(x1 - x0) * sizeof(int));
        int lo = x1, hi = x0 - 1;
        for (int s2 = 0; s2 < SUBSCAN; s2++) {
            for (int k = 0; k < ns[s2]; k++) {
                float a = sa[s2][k], b = sb[s2][k];
                int ia = (int)a, ib = (int)b;
                if (ia < x0) ia = x0;
                if (ib >= x1) ib = x1 - 1;
                if (ib < ia) continue;
                if (ia == ib) {
                    cv[ia] += (int)((b - a) * 256.0f);
                } else {
                    cv[ia] += (int)(((float)(ia + 1) - a) * 256.0f);
                    for (int m = ia + 1; m < ib; m++) cv[m] += 256;
                    cv[ib] += (int)((b - (float)ib) * 256.0f);
                }
                if (ia < lo) lo = ia;
                if (ib > hi) hi = ib;
            }
        }
        if (hi >= lo) composite_row(r, y, lo, hi);
    }
}

/* -------------------------------------------------------------- shapes */

void rs_rect(TGRaster *r, float x, float y, float w, float h)
{
    if (w < 0) { x += w; w = -w; }
    if (h < 0) { y += h; h = -h; }
    if (w <= 0.0f || h <= 0.0f) return;

    /* Backdrops (sky, grass, the launcher plate) are opaque axis-aligned
     * rectangles covering a large part of the screen.  Sending those through
     * the coverage machinery costs several times more than the fill itself, so
     * snap to whole pixels and store the word directly.  Overscanning by less
     * than a pixel keeps neighbouring shapes seam-free. */
    if ((r->fill >> 24) == 255) {
        int x0 = ifloor((x + r->ox) * r->s);
        int x1 = iceil((x + w + r->ox) * r->s);
        int y0 = ifloor((y + r->oy) * r->s);
        int y1 = iceil((y + h + r->oy) * r->s);
        if (x0 < r->cx0) x0 = r->cx0;
        if (x1 > r->cx1) x1 = r->cx1;
        if (y0 < r->cy0) y0 = r->cy0;
        if (y1 > r->cy1) y1 = r->cy1;
        if (x1 <= x0 || y1 <= y0) return;
        uint32_t c = r->fill;
        for (int yy = y0; yy < y1; yy++) {
            uint32_t *row = r->px + (size_t)yy * r->w;
            for (int xx = x0; xx < x1; xx++) row[xx] = c;
        }
        return;
    }

    float px[4] = { x, x + w, x + w, x };
    float py[4] = { y, y, y + h, y + h };
    fill_poly(r, px, py, 4);
}

void rs_quad(TGRaster *r, float ax, float ay, float bx, float by,
                        float cx, float cy, float dx, float dy)
{
    float px[4] = { ax, bx, cx, dx };
    float py[4] = { ay, by, cy, dy };
    fill_poly(r, px, py, 4);
}

void rs_tri(TGRaster *r, float ax, float ay, float bx, float by,
                       float cx, float cy)
{
    float px[3] = { ax, bx, cx };
    float py[3] = { ay, by, cy };
    fill_poly(r, px, py, 3);
}

/* Number of segments for an ellipse of the given radius: small ellipses are
 * a handful of pixels wide and would only pay for the extra vertices. */
static int ellipse_steps(float rx, float ry)
{
    float rad = (rx > ry ? rx : ry) * 0.5f;
    int n = (int)(rad * 1.6f) + 10;
    if (n < 10) n = 10;
    if (n > 44) n = 44;
    return n;
}

static void ellipse_pts(float cx, float cy, float rx, float ry,
                        int n, float *px, float *py)
{
    float step = 6.28318531f / (float)n;
    for (int i = 0; i < n; i++) {
        float a = step * (float)i;
        px[i] = cx + cosf(a) * rx;
        py[i] = cy + sinf(a) * ry;
    }
}

void rs_ellipse(TGRaster *r, float x, float y, float w, float h)
{
    if (w <= 0.0f || h <= 0.0f) return;
    float rx = w * 0.5f, ry = h * 0.5f;
    int n = ellipse_steps(w, h);
    ellipse_pts(x + rx, y + ry, rx, ry, n, r->bx, r->by);
    fill_poly(r, r->bx, r->by, n);
}

void rs_poly(TGRaster *r, const float *xs, const float *ys, int n)
{
    fill_poly(r, xs, ys, n);
}

void rs_line(TGRaster *r, float x0, float y0, float x1, float y1, float width){
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.0001f) return;
    float hw = width * 0.5f;
    float nx = -dy / len * hw, ny = dx / len * hw;
    float px[4] = { x0 + nx, x1 + nx, x1 - nx, x0 - nx };
    float py[4] = { y0 + ny, y1 + ny, y1 - ny, y0 - ny };
    fill_poly(r, px, py, 4);
}

void rs_ellipse_stroke(TGRaster *r, float x, float y, float w, float h,
                       float width)
{
    if (w <= 0.0f || h <= 0.0f) return;
    float rx = w * 0.5f, ry = h * 0.5f;
    float cx = x + rx, cy = y + ry;
    int n = ellipse_steps(w, h);
    if (n > 60) n = 60;
    float px[120], py[120];
    /* outer contour clockwise, inner contour counter-clockwise: the nonzero
     * rule then leaves the middle empty, exactly like a stroked path. */
    ellipse_pts(cx, cy, rx, ry, n, px, py);
    float hw = width * 0.5f;
    float ix = rx - hw, iy = ry - hw;
    if (ix < 0.1f) ix = 0.1f;
    if (iy < 0.1f) iy = 0.1f;
    float qx[60], qy[60];
    ellipse_pts(cx, cy, ix, iy, n, qx, qy);
    float ax[120], ay[120];
    for (int i = 0; i < n; i++) { ax[i] = px[i]; ay[i] = py[i]; }
    for (int i = 0; i < n; i++) {
        ax[n + i] = qx[n - 1 - i];
        ay[n + i] = qy[n - 1 - i];
    }
    fill_poly(r, ax, ay, n * 2);
}

/* ----------------------------------------------------------- gradients */

void rs_gradient_v(TGRaster *r, float x, float w, float y0, float y1,
                   uint32_t top, uint32_t bottom)
{
    int bx0 = ifloor((x + r->ox) * r->s);
    int bx1 = iceil((x + w + r->ox) * r->s);
    int by0 = ifloor((y0 + r->oy) * r->s);
    int by1 = iceil((y1 + r->oy) * r->s);
    if (bx0 < r->cx0) bx0 = r->cx0;
    if (bx1 > r->cx1) bx1 = r->cx1;
    if (by0 < r->cy0) by0 = r->cy0;
    if (by1 > r->cy1) by1 = r->cy1;
    if (bx1 <= bx0 || by1 <= by0) return;

    float span = (float)(by1 - by0);
    unsigned r0 = (top >> 16) & 255, g0 = (top >> 8) & 255, b0 = top & 255;
    unsigned r1 = (bottom >> 16) & 255, g1 = (bottom >> 8) & 255,
             b1 = bottom & 255;
    for (int y = by0; y < by1; y++) {
        float t = ((float)y - (float)by0 + 0.5f) / span;
        unsigned rr = (unsigned)(r0 + (r1 - r0) * t);
        unsigned gg = (unsigned)(g0 + (g1 - g0) * t);
        unsigned bb = (unsigned)(b0 + (b1 - b0) * t);
        uint32_t c = 0xFF000000u | (rr << 16) | (gg << 8) | bb;
        uint32_t *row = r->px + (size_t)y * r->w;
        for (int x2 = bx0; x2 < bx1; x2++) row[x2] = c;
    }
}

void rs_radial(TGRaster *r, float cx, float cy, float rad,
               uint32_t inner, uint32_t outer)
{
    float sx = (cx + r->ox) * r->s, sy = (cy + r->oy) * r->s;
    float sr = rad * r->s;
    if (sr < 0.5f) return;
    int x0 = (int)(sx - sr), x1 = (int)(sx + sr) + 1;
    int y0 = (int)(sy - sr), y1 = (int)(sy + sr) + 1;
    if (x0 < r->cx0) x0 = r->cx0;
    if (x1 > r->cx1) x1 = r->cx1;
    if (y0 < r->cy0) y0 = r->cy0;
    if (y1 > r->cy1) y1 = r->cy1;
    if (x1 <= x0 || y1 <= y0) return;

    uint32_t pi = premul(inner), po = premul(outer);
    float inv = 1.0f / sr;
    for (int y = y0; y < y1; y++) {
        float dy = (float)y + 0.5f - sy;
        uint32_t *row = r->px + (size_t)y * r->w;
        for (int x = x0; x < x1; x++) {
            float dx = (float)x + 0.5f - sx;
            float d = sqrtf(dx * dx + dy * dy) * inv;
            if (d >= 1.0f) continue;
            /* lerp premultiplied colours */
            unsigned ia = (unsigned)((pi >> 24) + ((int)(po >> 24) - (int)(pi >> 24)) * d);
            unsigned ir = (unsigned)(((pi >> 16) & 255) + ((int)((po >> 16) & 255) - (int)((pi >> 16) & 255)) * d);
            unsigned ig = (unsigned)(((pi >>  8) & 255) + ((int)((po >>  8) & 255) - (int)((pi >>  8) & 255)) * d);
            unsigned ib = (unsigned)(( pi        & 255) + ((int)( po        & 255) - (int)( pi        & 255)) * d);
            uint32_t s = (ia << 24) | (ir << 16) | (ig << 8) | ib;
            row[x] = blend_pm(row[x], s, 255);
        }
    }
}

/* -------------------------------------------------------------- bitmap */

void rs_blit(TGRaster *r, const TGImage *img, float x, float y,
             float w, float h, float alpha, int smooth)
{
    if (!img || !img->px || img->w <= 0 || img->h <= 0) return;
    if (w <= 0.0f || h <= 0.0f) return;

    float fx0 = (x + r->ox) * r->s;
    float fy0 = (y + r->oy) * r->s;
    float fx1 = (x + w + r->ox) * r->s;
    float fy1 = (y + h + r->oy) * r->s;

    int x0 = ifloor(fx0), x1 = iceil(fx1);
    int y0 = ifloor(fy0), y1 = iceil(fy1);
    if (x0 < r->cx0) x0 = r->cx0;
    if (x1 > r->cx1) x1 = r->cx1;
    if (y0 < r->cy0) y0 = r->cy0;
    if (y1 > r->cy1) y1 = r->cy1;
    if (x1 <= x0 || y1 <= y0) return;

    int ga = (int)(alpha * 255.0f + 0.5f);
    if (ga <= 0) return;
    if (ga > 255) ga = 255;

    const int sw = img->w, sh = img->h, stride = img->stride;
    const uint32_t *src = img->px;

    /* Source coordinates are stepped in 16.16 fixed point.  The float version
     * of this loop called floorf once per pixel and measured three seconds a
     * frame on the device; integer stepping is what the sprite budget can
     * actually afford, and 1/65536 texel of drift over a sprite is nothing. */
    float sxf = (float)sw * 65536.0f / (fx1 - fx0);
    float syf = (float)sh * 65536.0f / (fy1 - fy0);
    const int du = (int)(sxf + 0.5f);
    const int ustart = (int)(((float)x0 + 0.5f - fx0) * sxf);

    for (int py = y0; py < y1; py++) {
        uint32_t *row = r->px + (size_t)py * r->w;
        int vfix = (int)(((float)py + 0.5f - fy0) * syf);

        if (!smooth) {                        /* nearest neighbour */
            int sy = vfix >> 16;
            if (sy < 0) sy = 0;
            if (sy >= sh) sy = sh - 1;
            const uint32_t *srow = src + (size_t)sy * stride;
            int u = ustart;
            for (int px = x0; px < x1; px++, u += du) {
                int sx = u >> 16;
                if (sx < 0) sx = 0; else if (sx >= sw) sx = sw - 1;
                uint32_t s = srow[sx];
                if ((s >> 24) == 0) continue;
                if (ga >= 255 && (s >> 24) == 255) { row[px] = s | 0xFF000000u; continue; }
                row[px] = blend_pm(row[px], s, ga);
            }
            continue;
        }

        /* bilinear: sample at texel centres, clamp at the edges.  The vertical
         * weights are constant for the whole row, so they are folded in first
         * and the horizontal pass only needs two multiplies per channel. */
        int vy = vfix - 32768;
        int wy1 = (vy >> 8) & 255,  wy0 = 256 - wy1;
        int vy0 = vy >> 16, vy1 = vy0 + 1;
        if (vy0 < 0) vy0 = 0; else if (vy0 >= sh) vy0 = sh - 1;
        if (vy1 < 0) vy1 = 0; else if (vy1 >= sh) vy1 = sh - 1;
        const uint32_t *r0 = src + (size_t)vy0 * stride;
        const uint32_t *r1 = src + (size_t)vy1 * stride;

        int u = ustart;
        for (int px = x0; px < x1; px++, u += du) {
            int ux0 = u >> 16;
            int wx1 = (u >> 8) & 255, wx0 = 256 - wx1;
            int ux1 = ux0 + 1;
            if (ux0 < 0) ux0 = 0; else if (ux0 >= sw) ux0 = sw - 1;
            if (ux1 < 0) ux1 = 0; else if (ux1 >= sw) ux1 = sw - 1;

            uint32_t a00 = r0[ux0], a10 = r0[ux1];
            uint32_t a01 = r1[ux0], a11 = r1[ux1];

            unsigned av0 = ((a00 >> 24) & 255) * wy0 + ((a01 >> 24) & 255) * wy1;
            unsigned rv0 = ((a00 >> 16) & 255) * wy0 + ((a01 >> 16) & 255) * wy1;
            unsigned gv0 = ((a00 >>  8) & 255) * wy0 + ((a01 >>  8) & 255) * wy1;
            unsigned bv0 = ( a00        & 255) * wy0 + ( a01        & 255) * wy1;
            unsigned av1 = ((a10 >> 24) & 255) * wy0 + ((a11 >> 24) & 255) * wy1;
            unsigned rv1 = ((a10 >> 16) & 255) * wy0 + ((a11 >> 16) & 255) * wy1;
            unsigned gv1 = ((a10 >>  8) & 255) * wy0 + ((a11 >>  8) & 255) * wy1;
            unsigned bv1 = ( a10        & 255) * wy0 + ( a11        & 255) * wy1;

            unsigned sa = (av0 * wx0 + av1 * wx1) >> 16;
            if (sa == 0) continue;
            unsigned sr = (rv0 * wx0 + rv1 * wx1) >> 16;
            unsigned sg = (gv0 * wx0 + gv1 * wx1) >> 16;
            unsigned sb = (bv0 * wx0 + bv1 * wx1) >> 16;

            if (sa >= 255 && ga >= 255) {     /* solid interior of the sprite */
                row[px] = 0xFF000000u | (sr << 16) | (sg << 8) | sb;
                continue;
            }
            uint32_t s = (sa << 24) | (sr << 16) | (sg << 8) | sb;
            row[px] = blend_pm(row[px], s, ga);
        }
    }
}

/* ---------------------------------------------------------------- text */

static int glyph_index(const TGFont *f, int ch)
{
    int i = ch - f->first;
    if (i < 0 || i >= f->count) i = 0;
    return i;
}

/* Width of a string in *device* pixels.  The glyph cells are baked at their
 * final on-screen size and blitted 1:1, so their advances are device units
 * too -- only the pen origin goes through the raster scale. */
float rs_text_width(const TGFont *f, const char *s)
{
    if (!f || !f->cov) return 0.0f;
    float w = 0.0f;
    for (; *s; s++) w += (float)f->adv[glyph_index(f, (unsigned char)*s)];
    return w;
}

void rs_text(TGRaster *r, const TGFont *f, float x, float y,
             const char *s, uint32_t color, int align)
{
    if (!f || !f->cov || !s || !*s) return;

    /* integer placement keeps the glyphs crisp */
    int px = ifloor((x + r->ox) * r->s + 0.5f);
    int py = ifloor((y + r->oy) * r->s + 0.5f);

    /* alignment is resolved in device pixels, since that is where the pen
     * walks; doing it in scene units leaves the text short of the margin. */
    float total = rs_text_width(f, s);
    if (align == 1) px -= (int)(total + 0.5f);      /* right aligned */
    else if (align == 2) px -= (int)(total * 0.5f + 0.5f);  /* centred */

    uint32_t sp = premul(color);
    int sa = (int)(sp >> 24);
    if (sa == 0) return;
    unsigned cr = (sp >> 16) & 255, cg = (sp >> 8) & 255, cb = sp & 255;

    for (; *s; s++) {
        int gi = glyph_index(f, (unsigned char)*s);
        int gx = px;
        int gy = py;
        const unsigned char *cov = f->cov + (size_t)gi * f->cw;

        for (int cy = 0; cy < f->ch; cy++) {
            int dy = gy + cy;
            if (dy < r->cy0 || dy >= r->cy1) continue;
            uint32_t *row = r->px + (size_t)dy * r->w;
            const unsigned char *src = cov + (size_t)cy * f->aw;
            for (int cx = 0; cx < f->cw; cx++) {
                int c = src[cx];
                if (!c) continue;
                int dx = gx + cx;
                if (dx < r->cx0 || dx >= r->cx1) continue;
                int a = (int)mul255(c, sa);
                if (a == 0) continue;
                uint32_t s2 = ((uint32_t)a << 24) |
                              (mul255((int)cr, a) << 16) |
                              (mul255((int)cg, a) << 8) |
                               mul255((int)cb, a);
                row[dx] = blend_pm(row[dx], s2, 255);
            }
        }
        /* the pen walks in *device* pixels, so the advance is not scaled */
        px += f->adv[gi];
    }
}

void rs_text_shadow(TGRaster *r, const TGFont *f, float x, float y,
                    const char *s, uint32_t color, int align)
{
    rs_text(r, f, x + 1.0f, y + 1.0f, s, 0x66000000u, align);
    rs_text(r, f, x, y, s, color, align);
}

/* -------------------------------------------------------------- loading */

static int read_u16(FILE *fp, int *out)
{
    unsigned char b[2];
    if (fread(b, 1, 2, fp) != 2) return -1;
    *out = b[0] | (b[1] << 8);
    return 0;
}

int rs_load_image(const char *path, TGImage *out)
{
    memset(out, 0, sizeof(*out));
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    char magic[4];
    int w = 0, h = 0;
    if (fread(magic, 1, 4, fp) != 4 || memcmp(magic, "TGAT", 4) ||
        read_u16(fp, &w) || read_u16(fp, &h) || w <= 0 || h <= 0) {
        fclose(fp);
        return -2;
    }
    size_t n = (size_t)w * (size_t)h;
    out->px = (uint32_t *)malloc(n * 4);
    out->w = w;
    out->h = h;
    out->stride = w;
    if (!out->px || fread(out->px, 4, n, fp) != n) {
        rs_free_image(out);
        fclose(fp);
        return -3;
    }
    fclose(fp);
    return 0;
}

void rs_free_image(TGImage *img)
{
    if (!img) return;
    free(img->px);
    img->px = NULL;
    img->w = img->h = 0;
}

int rs_load_font(const char *path, TGFont *out)
{
    memset(out, 0, sizeof(*out));
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    char magic[4];
    int cw = 0, ch = 0, first = 0, count = 0;
    if (fread(magic, 1, 4, fp) != 4 || memcmp(magic, "TGFT", 4) ||
        read_u16(fp, &cw) || read_u16(fp, &ch) ||
        read_u16(fp, &first) || read_u16(fp, &count) ||
        cw <= 0 || ch <= 0 || count <= 0) {
        fclose(fp);
        return -2;
    }
    out->cw = cw;
    out->ch = ch;
    out->first = first;
    out->count = count;
    out->aw = cw * count;
    out->ah = ch;
    size_t n = (size_t)out->aw * ch;
    out->cov = (unsigned char *)malloc(n);
    out->adv = (unsigned char *)malloc((size_t)count);
    if (!out->cov || !out->adv) {
        rs_free_font(out);
        fclose(fp);
        return -3;
    }
    if (fread(out->cov, 1, n, fp) != n ||
        fread(out->adv, 1, (size_t)count, fp) != (size_t)count) {
        rs_free_font(out);
        fclose(fp);
        return -4;
    }
    fclose(fp);
    return 0;
}

void rs_free_font(TGFont *f)
{
    if (!f) return;
    free(f->cov);
    free(f->adv);
    f->cov = NULL;
    f->adv = NULL;
}

/* ------------------------------------------------------------ BMP dump */

static void put32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v & 255);
    p[1] = (unsigned char)((v >> 8) & 255);
    p[2] = (unsigned char)((v >> 16) & 255);
    p[3] = (unsigned char)((v >> 24) & 255);
}

int rs_write_bmp(const char *path, const uint32_t *px, int w, int h)
{
    int stride = (w * 3 + 3) & ~3;
    unsigned datasz = (unsigned)stride * (unsigned)h;
    unsigned char head[54];
    memset(head, 0, sizeof(head));
    head[0] = 'B'; head[1] = 'M';
    put32(head + 2, 54u + datasz);
    put32(head + 10, 54u);
    put32(head + 14, 40u);
    put32(head + 18, (unsigned)w);
    put32(head + 22, (unsigned)h);
    head[26] = 1;
    head[28] = 24;
    put32(head + 34, datasz);

    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    if (fwrite(head, 1, 54, fp) != 54) { fclose(fp); return -2; }
    unsigned char *line = (unsigned char *)calloc(1, (size_t)stride);
    if (!line) { fclose(fp); return -3; }
    for (int y = h - 1; y >= 0; y--) {
        const uint32_t *row = px + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            uint32_t c = row[x];
            line[x * 3 + 0] = (unsigned char)(c & 255);          /* B */
            line[x * 3 + 1] = (unsigned char)((c >> 8) & 255);   /* G */
            line[x * 3 + 2] = (unsigned char)((c >> 16) & 255);  /* R */
        }
        if (fwrite(line, 1, (size_t)stride, fp) != (size_t)stride) {
            free(line);
            fclose(fp);
            return -4;
        }
    }
    free(line);
    fclose(fp);
    return 0;
}
