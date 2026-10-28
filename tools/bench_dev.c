/*
 * bench_dev.c — the on-device renderer probe.
 *
 * Pure C, no UIKit and no window server, so it runs over SSH on a phone where
 * a shell-launched UIKit app would never get activated.  Two jobs:
 *
 *   ./bench_dev atlas.bin font.bin [frames] [w] [h] [shotdir]
 *
 *     frames   how many frames to time (default 120)
 *     w h      buffer size to use (default 480x720)
 *     shotdir  write a set of BMPs there: splash, running, steer right,
 *              mid-jump, steer left, touch pads, crash flash
 *
 * It paints with exactly the code the app paints with, so a frame it writes
 * is a frame the phone would show.
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

static void dump(TGRaster *r, const char *dir, int n)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/f%d.bmp", dir, n);
    if (rs_write_bmp(path, rs_pixels(r), rs_width(r), rs_height(r)) == 0)
        printf("  frame %d -> %s\n", n, path);
    else
        printf("  frame %d -> could not write %s\n", n, path);
}

int main(int argc, char **argv)
{
    /* unbuffered: if the box is slow enough that this takes minutes, the
     * log still shows how far it got */
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 3) {
        printf("usage: bench_dev atlas.bin font.bin [frames] [w] [h] [shotdir]\n");
        return 2;
    }
    int frames = (argc > 3) ? atoi(argv[3]) : 120;
    int w = (argc > 4) ? atoi(argv[4]) : 480;
    int h = (argc > 5) ? atoi(argv[5]) : 720;
    const char *shots = (argc > 6) ? argv[6] : NULL;
    float scale = (float)w / GOGO_VW;

    TGRaster *r = rs_new(w, h, scale);
    if (!r) { printf("no raster\n"); return 1; }

    static GoGoScene s;
    if (gogo_init(&s, r, argv[1], argv[2], 4242ULL) != 0) {
        printf("assets missing or unreadable\n");
        return 1;
    }

    /* ---- visual check ------------------------------------------------- */
    if (shots) {
        for (int i = 0; i < 36; i++) { gogo_step(&s, 1.0f/30.0f); gogo_draw(&s); }
        dump(r, shots, 0);                       /* loading splash */

        s.loading = 0;
        tg_reset(&s.g);
        for (int i = 0; i < 90; i++) { gogo_step(&s, 1.0f/30.0f); gogo_draw(&s); }
        dump(r, shots, 1);                       /* running */

        s.g.roll = 0.85f;
        for (int i = 0; i < 45; i++) { gogo_step(&s, 1.0f/30.0f); gogo_draw(&s); }
        dump(r, shots, 2);                       /* steering right */

        tg_jump(&s.g);
        for (int i = 0; i < 14; i++) { gogo_step(&s, 1.0f/30.0f); gogo_draw(&s); }
        dump(r, shots, 3);                       /* mid-jump */

        s.g.roll = -0.85f;
        for (int i = 0; i < 20; i++) { gogo_step(&s, 1.0f/30.0f); gogo_draw(&s); }
        dump(r, shots, 4);                       /* steering left */

        s.touchMode = 1;
        s.stkDX = 11.0f;
        s.stkDY = -7.0f;
        gogo_draw(&s);
        dump(r, shots, 5);                       /* on-screen controls */
        s.touchMode = 0;

        s.g.crash = 1;                           /* red flash + shake */
        s.g.crashT = 0.30f;
        gogo_draw(&s);
        dump(r, shots, 6);
        s.g.crash = 0;
        s.g.crashT = 0.0f;
    }

    /* ---- timing ------------------------------------------------------- */
    printf("warm-up...\n");
    for (int i = 0; i < 40; i++) gogo_step(&s, 1.0f / 30.0f);
    s.loading = 0;
    tg_reset(&s.g);
    for (int i = 0; i < 60; i++) { gogo_step(&s, 1.0f / 30.0f); gogo_draw(&s); }
    s.g.roll = 0.85f;
    for (int i = 0; i < 30; i++) { gogo_step(&s, 1.0f / 30.0f); gogo_draw(&s); }

    printf("timing %d frames...\n", frames);
#ifdef GOGO_PROFILE
    memset(gogo_prof, 0, sizeof(gogo_prof));
#endif
    double t0 = now_ms();
    for (int i = 0; i < frames; i++) {
        gogo_step(&s, 1.0f / 30.0f);
        gogo_draw(&s);
    }
    double t1 = now_ms();

    printf("buffer %dx%d  %d frames  %.2f ms/frame  (%.0f fps ceiling)\n",
           w, h, frames, (t1 - t0) / frames, 1000.0 * frames / (t1 - t0));

#ifdef GOGO_PROFILE
    {
        static const char *nm[GOGO_PROF_N] = { GOGO_PROF_NAMES };
        printf("  stage breakdown:");
        for (int i = 0; i < GOGO_PROF_N; i++)
            printf(" %s %.2f", nm[i], gogo_prof[i] / frames);
        printf(" ms\n");
    }
#endif

    s.noText = 1;
    t0 = now_ms();
    for (int i = 0; i < frames; i++) {
        gogo_step(&s, 1.0f / 30.0f);
        gogo_draw(&s);
    }
    t1 = now_ms();
    printf("  without HUD text: %.2f ms/frame\n", (t1 - t0) / frames);
    return 0;
}
