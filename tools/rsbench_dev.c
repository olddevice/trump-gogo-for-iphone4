/*
 * rsbench_dev.c — primitive-level cost probe for TGRaster on the device.
 *
 * A whole frame being slow never says which primitive is to blame, and this
 * CPU has a couple of traps (out-of-line pixel code, VFP<->ARM register
 * transfers in inner loops).  Each case is timed separately, unbuffered, so a
 * pathological primitive shows up as a number instead of a mystery.
 *
 *   ./rsbench_dev [atlas.bin]
 */
#include "tgraster.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static double now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

static TGRaster *r;

/* The phone is a phone: SpringBoard, the SSH session and whatever the user is
 * doing all steal cycles, so a mean is meaningless.  Report the minimum of N
 * runs, which is the closest thing to a clean measurement. */
static double best_ms(double *samples, int n)
{
    double m = 1e9;
    for (int i = 0; i < n; i++) if (samples[i] < m) m = samples[i];
    return m;
}

static void rect_case(const char *name, float w, float h, int n)
{
    double s[64];
    for (int k = 0; k < n && k < 64; k++) {
        double t = now_ms();
        rs_begin(r);
        rs_set_fill(r, 0xFF63A34Fu);
        rs_rect(r, 0, 0, w, h);
        s[k] = now_ms() - t;
    }
    printf("  %-24s %8.3f ms (min of %d, max %.1f)\n",
           name, best_ms(s, n), n, s[0] > 0 ? s[n - 1] : 0);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    r = rs_new(480, 720, 2.0f);
    if (!r) { printf("no raster\n"); return 1; }
    printf("probe start %dx%d\n", rs_width(r), rs_height(r));

    double t = now_ms();
    for (int k = 0; k < 5; k++) {
        uint32_t *px = rs_pixels(r);
        for (int i = 0; i < 480 * 720; i++) px[i] = 0xFF204060u;
    }
    printf("  %-24s %8.3f ms\n", "plain word fill", (now_ms() - t) / 5);

    rect_case("rect 480x720 (full)", 240, 360, 3);
    rect_case("rect 240x360", 120, 180, 5);
    rect_case("rect 480x40 (wide/short)", 240, 20, 5);
    rect_case("rect 40x720 (tall/thin)", 20, 360, 5);
    rect_case("rect 120x120", 60, 60, 5);

    t = now_ms();
    for (int k = 0; k < 5; k++) {
        rs_begin(r);
        rs_set_fill(r, 0xFF63A34Fu);
        for (int i = 0; i < 40; i++)
            rs_quad(r, 20.0f + (i % 8) * 28.0f, 40.0f + (i / 8) * 40.0f,
                    44.0f + (i % 8) * 28.0f, 40.0f + (i / 8) * 40.0f,
                    44.0f + (i % 8) * 28.0f, 70.0f + (i / 8) * 40.0f,
                    20.0f + (i % 8) * 28.0f, 70.0f + (i / 8) * 40.0f);
    }
    printf("  %-24s %8.3f ms\n", "40 quads (48x60 px)", (now_ms() - t) / 5);

    t = now_ms();
    for (int k = 0; k < 5; k++) {
        rs_begin(r);
        rs_set_fill(r, 0xFF63A34Fu);
        for (int i = 0; i < 40; i++)
            rs_ellipse(r, 20.0f + (i % 8) * 28.0f, 40.0f + (i / 8) * 40.0f,
                       24.0f, 24.0f);
    }
    printf("  %-24s %8.3f ms\n", "40 ellipses (24 dia)", (now_ms() - t) / 5);

    {
        double s2[8];
        for (int k = 0; k < 8; k++) {
            double t2 = now_ms();
            rs_begin(r);
            rs_set_fill(r, 0xFF63A34Fu);
            rs_ellipse(r, 40.0f, 40.0f, 160.0f, 160.0f);
            s2[k] = now_ms() - t2;
        }
        printf("  %-24s %8.3f ms (min of 8)\n", "1 big ellipse (160 dia)",
               best_ms(s2, 8));
    }

    t = now_ms();
    for (int k = 0; k < 5; k++) {
        rs_begin(r);
        rs_gradient_v(r, -8, 256, -2, 170, 0xFF7EC0EEu, 0xFFCFEAFBu);
    }
    printf("  %-24s %8.3f ms\n", "gradient (upper third)", (now_ms() - t) / 5);

    if (argc > 1) {
        TGImage im;
        if (rs_load_image(argv[1], &im) == 0) {
            TGImage c;
            c.w = 106; c.h = 74; c.stride = im.stride;
            c.px = im.px + (size_t)74 * 3 * im.stride + 106 * 2;
            t = now_ms();
            for (int k = 0; k < 5; k++) {
                rs_begin(r);
                rs_blit(r, &c, 80, 300, 98, 68, 1.0f, 1);
            }
            printf("  %-24s %8.3f ms\n", "sprite bilinear", (now_ms() - t) / 5);
            t = now_ms();
            for (int k = 0; k < 5; k++) {
                rs_begin(r);
                rs_blit(r, &c, 80, 300, 98, 68, 1.0f, 0);
            }
            printf("  %-24s %8.3f ms\n", "sprite nearest", (now_ms() - t) / 5);
            rs_free_image(&im);
        }
    }

    printf("probe done\n");
    return 0;
}
