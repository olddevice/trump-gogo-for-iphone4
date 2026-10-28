/*
 * hostshot.c — render the game on the build machine.
 *
 * The scene is pure C over TGRaster, so the frames it produces here are the
 * frames the phone draws (same code, same assets, same pixel format).  That
 * turns the visual side of the port into a local iteration instead of a
 * round-trip over SSH: `make shots` writes build/host/N.bmp.
 *
 *   ./hostshot <assets dir> <out dir> [frames]
 */
#include "../src/gogo_scene.h"
#include "../src/trump_game.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static const int BW = (int)(GOGO_VW * GOGO_SCALE);
static const int BH = (int)(GOGO_VH * GOGO_SCALE);

static GoGoScene scene;
static TGRaster *ras;

static void dump(const char *dir, int n)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%d.bmp", dir, n);
    if (rs_write_bmp(path, rs_pixels(ras), BW, BH) != 0) {
        printf("  !! could not write %s\n", path);
        return;
    }
    printf("  wrote %s\n", path);
}

/* Step and paint in lockstep, exactly like the app's display link: the camera
 * glide is per drawn frame, so a harness that only paints at the end would
 * photograph a camera that never caught up. */
static void run(float dt, int frames)
{
    for (int i = 0; i < frames; i++) {
        gogo_step(&scene, dt);
        gogo_draw(&scene);
    }
}

int main(int argc, char **argv)
{
    const char *assets = (argc > 1) ? argv[1] : "assets";
    const char *out = (argc > 2) ? argv[2] : "build/host";
    int bench = (argc > 3) ? atoi(argv[3]) : 60;

    char atlas[512], font[512];
    snprintf(atlas, sizeof(atlas), "%s/atlas.bin", assets);
    snprintf(font, sizeof(font), "%s/font.bin", assets);

    ras = rs_new(BW, BH, GOGO_SCALE);
    if (!ras) { printf("out of memory\n"); return 1; }
    if (gogo_init(&scene, ras, atlas, font, 1234ULL) != 0) {
        printf("asset load failed (%s / %s)\n", atlas, font);
        return 1;
    }
    printf("atlas %dx%d, font cell %dx%d\n", scene.atlas.w, scene.atlas.h,
           scene.font.cw, scene.font.ch);

    /* 1: the loading splash */
    run(1.0f / 30.0f, 36);
    dump(out, 0);

    /* 2-5: gameplay, steering both ways and one jump */
    scene.loading = 0;
    tg_reset(&scene.g);

    run(1.0f / 30.0f, 90);
    dump(out, 1);

    scene.g.roll = 0.85f;
    run(1.0f / 30.0f, 45);
    dump(out, 2);

    tg_jump(&scene.g);
    run(1.0f / 30.0f, 14);
    dump(out, 3);

    scene.g.roll = -0.85f;
    run(1.0f / 30.0f, 20);
    dump(out, 4);

    /* 6: on-screen controls, as a device without tilt would show them */
    scene.touchMode = 1;
    scene.stkDX = 11.0f;
    scene.stkDY = -7.0f;
    gogo_draw(&scene);
    dump(out, 5);
    scene.touchMode = 0;

    /* 7: a crash, so the red flash and the shake get looked at too */
    scene.g.crash = 1;
    scene.g.crashT = 0.30f;
    gogo_draw(&scene);
    dump(out, 6);

    /* cost of a frame on this machine, for reference only */
    if (bench > 0) {
        struct timeval tv0, tv1;
        gettimeofday(&tv0, NULL);
        for (int i = 0; i < bench; i++) {
            scene.g.animT += 1.0f / 30.0f;
            gogo_draw(&scene);
        }
        gettimeofday(&tv1, NULL);
        double ms = ((double)(tv1.tv_sec - tv0.tv_sec) * 1e3 +
                     (double)(tv1.tv_usec - tv0.tv_usec) / 1e3) / bench;
        printf("home machine: %.3f ms/frame over %d frames (%dx%d)\n",
               ms, bench, BW, BH);
    }

    gogo_shutdown(&scene);
    rs_free(ras);
    return 0;
}
