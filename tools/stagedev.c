/*
 * stagedev.c — one frame at a time, with a per-stage breakdown.
 *
 * bench_dev measures a steady state; when a single frame takes longer than the
 * SSH session lives, that is useless.  This paints a couple of frames and
 * prints where the time went straight away, unbuffered.
 *
 *   ./stagedev atlas.bin font.bin [frames] [w] [h]
 */
#include "gogo_scene.h"
#include "trump_game.h"

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

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) {
        printf("usage: stagedev atlas.bin font.bin [frames] [w] [h]\n");
        return 2;
    }
    int frames = (argc > 3) ? atoi(argv[3]) : 3;
    int w = (argc > 4) ? atoi(argv[4]) : 480;
    int h = (argc > 5) ? atoi(argv[5]) : 720;

    TGRaster *r = rs_new(w, h, (float)w / GOGO_VW);
    if (!r) { printf("no raster\n"); return 1; }
    static GoGoScene s;
    if (gogo_init(&s, r, argv[1], argv[2], 4242ULL) != 0) {
        printf("assets missing\n");
        return 1;
    }
    printf("buffer %dx%d\n", w, h);

    s.loading = 0;
    tg_reset(&s.g);
    for (int i = 0; i < 60; i++) gogo_step(&s, 1.0f / 30.0f);
    printf("world stepped\n");

    /* The phone is shared with SpringBoard and whatever else is installed, so
     * a single frame can be starved for seconds.  The minimum of N is the only
     * number that says what the renderer itself costs. */
    double bestTotal = 1e9;
#ifdef GOGO_PROFILE
    double bestStage[GOGO_PROF_N];
    for (int i = 0; i < GOGO_PROF_N; i++) bestStage[i] = 1e9;
#endif

    for (int k = 0; k < frames; k++) {
#ifdef GOGO_PROFILE
        memset(gogo_prof, 0, sizeof(gogo_prof));
#endif
        double t0 = now_ms();
        gogo_draw(&s);
        double t1 = now_ms();
        if (t1 - t0 < bestTotal) bestTotal = t1 - t0;
        printf("frame %d: %.2f ms", k, t1 - t0);
#ifdef GOGO_PROFILE
        static const char *nm[GOGO_PROF_N] = { GOGO_PROF_NAMES };
        for (int i = 0; i < GOGO_PROF_N; i++) {
            printf(" | %s %.2f", nm[i], gogo_prof[i]);
            if (gogo_prof[i] < bestStage[i]) bestStage[i] = gogo_prof[i];
        }
#endif
        printf("\n");
        gogo_step(&s, 1.0f / 30.0f);
    }

    printf("best of %d: %.2f ms", frames, bestTotal);
#ifdef GOGO_PROFILE
    {
        static const char *nm[GOGO_PROF_N] = { GOGO_PROF_NAMES };
        for (int i = 0; i < GOGO_PROF_N; i++) printf(" | %s %.2f", nm[i], bestStage[i]);
    }
#endif
    printf("\n");
    printf("done\n");
    return 0;
}
