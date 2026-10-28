/*
 * test_game.c — host-side unit tests for the ported world simulation.
 * Built and run by build.sh before anything is cross-compiled.
 */
#include "trump_game.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        if (!(cond)) {                                          \
            failures++;                                         \
            printf("  FAIL %s:%d  ", __FILE__, __LINE__);       \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

static void step(TG *g, int n)
{
    int i;
    for (i = 0; i < n; i++) tg_update(g, 1.0f / 60.0f);
}

static void test_spawn_layout(void)
{
    TG g;
    int i, ahead = 0;
    tg_init(&g, 42);

    printf("  spawn layout\n");
    CHECK(g.pz == 0.0f && g.px == 0.0f, "start not at origin");
    CHECK(g.coins == 0, "start coins != 0");
    for (i = 0; i < TG_N_COIN; i++) {
        CHECK(g.cLive[i] == 1, "coin %d not live at start", i);
        CHECK(g.cbz[i] > g.pz, "coin %d not ahead (%.2f)", i, g.cbz[i]);
    }
    for (i = 0; i < TG_N_OBS; i++) {
        CHECK(g.obz[i] > g.pz, "rock %d not ahead", i);
        CHECK(fabsf(g.obx[i]) <= TG_ROAD_HALF, "rock %d off the path", i);
        ahead++;
    }
    for (i = 0; i < TG_N_TREE; i++) {
        CHECK(fabsf(g.tx[i]) >= 30.0f, "tree %d on the path (%.1f)",
              i, g.tx[i]);
        CHECK(g.ts[i] >= 0.8f && g.ts[i] <= 1.5f, "tree %d size out of range", i);
    }
    CHECK(ahead == TG_N_OBS, "rock count");
}

static void test_rng_reproducible(void)
{
    TG a, b;
    printf("  deterministic course for a given seed\n");
    tg_init(&a, 1234);
    tg_init(&b, 1234);
    CHECK(memcmp(a.obz, b.obz, sizeof(a.obz)) == 0, "same seed, other rocks");
    CHECK(memcmp(a.cbx, b.cbx, sizeof(a.cbx)) == 0, "same seed, other coins");
    tg_init(&b, 4321);
    CHECK(memcmp(a.obz, b.obz, sizeof(a.obz)) != 0, "different seeds collide?!");
}

static void test_forward_motion(void)
{
    TG g;
    tg_init(&g, 7);
    printf("  forward motion\n");
    step(&g, 60);                        /* 1 s */
    CHECK(fabsf(g.pz - TG_SPEED) < 0.5f,
          "after 1 s pz=%.2f, expected ~%.1f", g.pz, TG_SPEED);
    CHECK(fabsf(g.runDist - g.pz) < 0.01f, "runDist drifted from pz");
}

static void test_steering_limits(void)
{
    TG g;
    tg_init(&g, 9);
    printf("  steering stays on the path\n");
    g.roll = 1.0f;
    step(&g, 180);
    CHECK(g.px <= TG_X_LIMIT + 0.001f, "px=%.2f past the right stripe", g.px);
    CHECK(g.px > TG_X_LIMIT - 1.0f, "px=%.2f never reached the stripe", g.px);
    CHECK(g.dir == 1, "facing should be right profile, got %d", g.dir);

    g.roll = -1.0f;
    step(&g, 180);
    CHECK(g.px >= -TG_X_LIMIT - 0.001f, "px=%.2f past the left stripe", g.px);
    CHECK(g.px < 0.0f, "px=%.2f did not cross to the left", g.px);
    CHECK(g.dir == -1, "facing should be left profile, got %d", g.dir);

    g.roll = 0.0f;
    step(&g, 60);
    CHECK(g.dir == 0, "facing should return to back view, got %d", g.dir);
}

static void test_jump_apex(void)
{
    TG g;
    float apex = 0.0f;
    int i;
    tg_init(&g, 11);
    printf("  jump arc\n");
    tg_jump(&g);
    CHECK(g.vy > 0.0f && !g.grounded, "jump did not leave the ground");
    for (i = 0; i < 120; i++) {
        tg_update(&g, 1.0f / 60.0f);
        if (g.jumpY > apex) apex = g.jumpY;
        if (g.grounded) break;
    }
    CHECK(apex > 2.85f && apex < 3.15f, "apex %.2f, expected ~3.0", apex);
    CHECK(g.grounded, "never landed");
    CHECK(g.jumpY == 0.0f, "landed below zero");

    /* a jump in mid-air must be ignored */
    tg_jump(&g);
    g.vy = 1.0f;
    tg_jump(&g);
    CHECK(g.vy == 1.0f, "double jump accepted");
}

static void test_rock_crash(void)
{
    TG g;
    int i, crashes = 0;
    tg_init(&g, 3);
    printf("  rock collision resets the run\n");
    /* park a rock right in front and walk into it */
    g.obz[0] = g.pz + 4.0f;
    g.obx[0] = 0.0f;
    for (i = 0; i < 30 && !g.crash; i++) tg_update(&g, 1.0f / 60.0f);
    CHECK(g.crash, "ran through a rock without crashing");
    if (g.crash) {
        g.coins = 5;
        for (i = 0; i < 120 && g.crash; i++) tg_update(&g, 1.0f / 60.0f);
        CHECK(!g.crash, "crash never cleared");
        CHECK(g.coins == 0, "coins survived the crash");
        CHECK(g.runDist < 1.5f, "runDist not reset after crash (%.2f)",
              g.runDist);
        crashes++;
    }
    /* jumping over the same rock must be possible: time the jump so the
     * boulder passes under the apex (jump reaches ~3.0u, rock is 2.1u) */
    tg_init(&g, 3);
    g.obz[0] = g.pz + 26.0f;             /* apex at ~26u of travel */
    g.obx[0] = 0.0f;
    tg_jump(&g);
    step(&g, 40);
    CHECK(!g.crash, "crashed while jumping over the rock");
    CHECK(crashes == 1, "crash bookkeeping");
}

static void test_coin_pickup(void)
{
    TG g;
    int i, p0;
    tg_init(&g, 21);
    printf("  coin pickup\n");
    g.cbx[0] = 0.0f;
    g.cbz[0] = g.pz + 3.0f;
    g.cLive[0] = 1;
    p0 = g.pkSeq;
    for (i = 0; i < 20 && g.coins == 0; i++) tg_update(&g, 1.0f / 60.0f);
    CHECK(g.coins == 1, "coin not collected (coins=%d)", g.coins);
    CHECK(g.pkSeq == p0 + 1, "pickup counter did not advance");
    CHECK(g.cLive[0] == 0, "collected coin still live");
    CHECK(g.evT[0] >= 0.0f, "no sparkle event queued");
}

static void test_coin_no_pickup_off_axis(void)
{
    TG g;
    int i;
    tg_init(&g, 22);
    printf("  coin needs to be within reach\n");
    g.cbx[0] = 12.0f;                 /* far to the right */
    g.cbz[0] = g.pz + 3.0f;
    g.cLive[0] = 1;
    for (i = 0; i < 20; i++) tg_update(&g, 1.0f / 60.0f);
    CHECK(g.coins == 0, "collected a coin 12 units away");
}

static void test_long_run_stability(void)
{
    TG g;
    int i, nan = 0;
    tg_init(&g, 5);
    printf("  long run stays finite\n");
    for (i = 0; i < 60 * 60; i++) {         /* 60 s at 60 fps */
        g.roll = sinf(i * 0.02f);
        if (i % 40 == 0) tg_jump(&g);
        tg_update(&g, 1.0f / 60.0f);
        if (!(g.px == g.px) || !(g.pz == g.pz) || !(g.jumpY == g.jumpY))
            nan++;
    }
    CHECK(nan == 0, "%d NaN frames", nan);
    CHECK(fabsf(g.px) <= TG_X_LIMIT + 0.001f, "left the path: px=%.2f", g.px);
    CHECK(tg_frame(&g) >= 0 && tg_frame(&g) < 6, "bad animation frame");
}

static void test_dt_clamp(void)
{
    TG g;
    tg_init(&g, 6);
    printf("  huge/zero dt is clamped\n");
    tg_update(&g, 5.0f);              /* a 5 s stall must not teleport */
    CHECK(g.pz <= TG_SPEED * 0.05f + 0.001f, "dt not clamped: pz=%.2f", g.pz);
    g.pz = 0.0f;
    tg_update(&g, 0.0f);
    CHECK(g.pz == 0.0f, "dt=0 moved the world");
    tg_update(&g, -1.0f);
    CHECK(g.pz == 0.0f, "negative dt moved the world");
}

int main(void)
{
    printf("trump_game unit tests\n");
    test_spawn_layout();
    test_rng_reproducible();
    test_forward_motion();
    test_steering_limits();
    test_jump_apex();
    test_rock_crash();
    test_coin_pickup();
    test_coin_no_pickup_off_axis();
    test_long_run_stability();
    test_dt_clamp();

    if (failures) {
        printf("\n%d CHECK(s) FAILED\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
