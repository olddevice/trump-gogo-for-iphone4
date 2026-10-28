/*
 * trump_game.h — TrumpGoGo world simulation, ported from the Android
 * original (TrumpGame.java) to plain C.
 *
 * The world scrolls forward at a constant speed. Tilt (-1..1) steers
 * sideways, a tap jumps. Trees line the sides, rocks sit on the path
 * (hit one -> crash -> brief freeze, run score resets), coin lines can be
 * picked up along the way.
 *
 * Pure data + maths: no UIKit, no CoreGraphics, so it can be unit-tested
 * on the build machine (see test_game.c).
 */
#ifndef TRUMP_GAME_H
#define TRUMP_GAME_H

#include <stdint.h>

#define TG_ROAD_HALF   24.0f    /* dirt path half width */
#define TG_LINE_W       0.6f    /* white edge stripe width */
#define TG_LINE_IN     (TG_ROAD_HALF - TG_LINE_W)
#define TG_FOOT_HALF    1.0f
#define TG_X_LIMIT     (TG_LINE_IN - TG_FOOT_HALF)
#define TG_SPEED       55.0f    /* forward speed, u/s */
#define TG_MAX_VX      40.0f    /* sideways speed at full tilt */
#define TG_GRAV        26.0f
#define TG_JUMP_V      12.5f    /* apex ~3.0u, clears boulders */

#define TG_N_TREE      14
#define TG_N_OBS        8        /* rocks on the path */
#define TG_N_COIN      15        /* coins, spawned in lines */

#define TG_OBS_H        2.1f     /* boulder height (jumpable) */
#define TG_OBS_W        2.3f     /* boulder width / height ratio */
#define TG_COIN_H       2.6f     /* coin hover height (chest) */
#define TG_COIN_R       0.84f    /* coin radius */
#define TG_COIN_GAP     3.6f     /* spacing inside a coin line */
#define TG_UNITS_PER_KM 1200.0f  /* world units per kilometre on the HUD */

#define TG_NEV          4        /* collect-event ring buffer */

typedef struct {
    float px;              /* lateral world position */
    float pz;              /* distance travelled */
    float jumpY, vy;       /* jump height (world units up) */
    int   grounded;
    float vx;
    float roll;            /* -1..1 from the sensor / stick */
    int   dir;             /* -1 left profile, 0 back view, +1 right */
    float runDist;
    float animT;           /* global clock for view-side animation */

    float tx[TG_N_TREE], tz[TG_N_TREE], ts[TG_N_TREE];

    float obx[TG_N_OBS], obz[TG_N_OBS];

    float cbx[TG_N_COIN], cbz[TG_N_COIN];
    unsigned char cLive[TG_N_COIN];   /* coin in the air and collectable */
    float cPop[TG_N_COIN];            /* seconds since this slot was taken */
    int   coins;                      /* coins collected this run */
    int   pkSeq;                      /* monotonic pickup counter (SFX) */

    int   crash;
    float crashT;
    float hitX, hitZ;                 /* where the crash happened (view fx) */

    float evX[TG_NEV], evZ[TG_NEV], evT[TG_NEV];  /* -1 = free slot */
    int   evHead;

    uint64_t seed;                    /* java.util.Random compatible state */
    float groupZ, groupX;             /* coin-line spawner state */
    int   groupLeft;
} TG;

void tg_init(TG *g, uint64_t seed);
void tg_reset(TG *g);
void tg_jump(TG *g);
void tg_update(TG *g, float dt);

/* Running animation frame 0..5 from distance travelled. */
int   tg_frame(const TG *g);
/* Distance covered this run in kilometres (HUD). */
float tg_km(const TG *g);

#endif /* TRUMP_GAME_H */
