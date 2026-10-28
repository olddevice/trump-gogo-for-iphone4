/*
 * fpprobe.c — why is one floating-point pixel loop 1000x slower than a
 * plain word fill on this device?
 *
 * Runs a handful of micro loops over the same 480x720 buffer and prints each
 * one's cost, then repeats them with flush-to-zero enabled in the FPSCR
 * (Cortex-A8 supports denormals through an exception bounce, which is the
 * classic way a per-pixel float loop falls off a cliff).
 */
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

#define W 480
#define H 720
#define N 5

static unsigned *px;
static float *cov;

static unsigned mul255(int a, int b)
{
    int t = a * b + 128;
    return (unsigned)((t + (t >> 8)) >> 8);
}

static unsigned blend_pm(unsigned d, unsigned s, int cv)
{
    if (cv <= 0) return d;
    int sa = (int)(s >> 24);
    if (cv < 255) {
        sa = (int)mul255(sa, cv);
        if (sa == 0) return d;
        s = ((unsigned)sa << 24) |
            (mul255((int)((s >> 16) & 255), cv) << 16) |
            (mul255((int)((s >> 8) & 255), cv) << 8) |
             mul255((int)(s & 255), cv);
    }
    if (sa >= 255) return s | 0xFF000000u;
    int ia = 255 - sa;
    unsigned rr = (unsigned)((s >> 16) & 255) + mul255((int)((d >> 16) & 255), ia);
    unsigned gg = (unsigned)((s >> 8) & 255) + mul255((int)((d >> 8) & 255), ia);
    unsigned bb = (unsigned)(s & 255) + mul255((int)(d & 255), ia);
    if (rr > 255) rr = 255;
    if (gg > 255) gg = 255;
    if (bb > 255) bb = 255;
    return 0xFF000000u | (rr << 16) | (gg << 8) | bb;
}

static void set_fz(int on)
{
    unsigned fpscr = 0;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    if (on) fpscr |= (1u << 24);        /* FZ  */
    else    fpscr &= ~(1u << 24);
    __asm__ volatile("vmsr fpscr, %0" :: "r"(fpscr));
}

static void report(void)
{
    unsigned fpscr = 0;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    printf("  fpscr = 0x%08x  FZ=%d DN=%d\n", fpscr,
           (fpscr >> 24) & 1, (fpscr >> 25) & 1);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    px = (unsigned *)malloc(sizeof(unsigned) * W * H);
    cov = (float *)malloc(sizeof(float) * (W + 2));
    memset(px, 0x40, sizeof(unsigned) * W * H);

    double t;
    printf("fpprobe %dx%d\n", W, H);
    report();

    /* 0: baseline — plain word stores */
    t = now_ms();
    for (int k = 0; k < N; k++)
        for (int i = 0; i < W * H; i++) px[i] = 0xFF204060u;
    printf("  word fill              %7.2f ms\n", (now_ms() - t) / N);

    /* 1: coverage row filled the way the rasteriser fills it */
    for (int i = 0; i < W + 2; i++) cov[i] = 0.0f;
    t = now_ms();
    for (int k = 0; k < N; k++)
        for (int r = 0; r < H; r++)
            for (int i = 1; i < W; i++) cov[i] += 1.0f;
    printf("  span accumulate        %7.2f ms\n", (now_ms() - t) / N);

    /* 2: the exact composite loop, coverage = 1.0 */
    for (int i = 0; i < W + 2; i++) cov[i] = 1.0f;
    t = now_ms();
    for (int k = 0; k < N; k++)
        for (int r = 0; r < H; r++) {
            unsigned *row = px + r * W;
            for (int x = 0; x < W; x++) {
                int c = (int)(cov[x] * (255.0f / 4) + 0.5f);
                if (c <= 0) continue;
                if (c > 255) c = 255;
                row[x] = blend_pm(row[x], 0xFF7EC0EEu, c);
            }
        }
    printf("  composite (opaque, c=63)%7.2f ms\n", (now_ms() - t) / N);

    /* 3: same, but the coverage multiply hoisted out */
    t = now_ms();
    for (int k = 0; k < N; k++)
        for (int r = 0; r < H; r++) {
            unsigned *row = px + r * W;
            for (int x = 0; x < W; x++)
                row[x] = blend_pm(row[x], 0xFF7EC0EEu, 255);
        }
    printf("  composite (c=255 int)  %7.2f ms\n", (now_ms() - t) / N);

    /* 4: float multiply on its own */
    t = now_ms();
    {
        float acc = 0.0f;
        for (int k = 0; k < N; k++)
            for (int i = 0; i < W * H; i++) acc += cov[i & 511] * 63.75f;
        if (acc < 0) printf("x");
    }
    printf("  float mul loop         %7.2f ms\n", (now_ms() - t) / N);

    /* 5: float -> int conversion on its own */
    t = now_ms();
    {
        int acc = 0;
        for (int k = 0; k < N; k++)
            for (int i = 0; i < W * H; i++) acc += (int)(cov[i & 511] * 63.75f + 0.5f);
        if (acc < 0) printf("x");
    }
    printf("  float->int loop        %7.2f ms\n", (now_ms() - t) / N);

    /* 6: same composite with denormals flushed */
    set_fz(1);
    printf("after enabling FZ:\n");
    report();
    t = now_ms();
    for (int k = 0; k < N; k++)
        for (int r = 0; r < H; r++) {
            unsigned *row = px + r * W;
            for (int x = 0; x < W; x++) {
                int c = (int)(cov[x] * (255.0f / 4) + 0.5f);
                if (c <= 0) continue;
                if (c > 255) c = 255;
                row[x] = blend_pm(row[x], 0xFF7EC0EEu, c);
            }
        }
    printf("  composite (opaque, c=63)%7.2f ms\n", (now_ms() - t) / N);
    set_fz(0);

    printf("fpprobe done\n");
    return 0;
}
