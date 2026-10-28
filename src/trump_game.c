/*
 * trump_game.c — see trump_game.h. Direct port of TrumpGame.java.
 *
 * The random generator reproduces java.util.Random bit for bit (48-bit LCG),
 * so a given seed lays out exactly the same course the Android build would.
 */
#include "trump_game.h"
#include <math.h>
#include <string.h>

#define JMASK ((1ULL << 48) - 1)

static uint32_t jnext(TG *g, int bits)
{
    g->seed = (g->seed * 0x5DEECE66DULL + 0xBULL) & JMASK;
    return (uint32_t)(g->seed >> (48 - bits));
}

static float jfloat(TG *g)
{
    return (float)(jnext(g, 24) / 16777216.0);   /* / 2^24 */
}

static int jint(TG *g, int n)
{
    return (int)(jnext(g, 31) % (uint32_t)n);
}

static int jbool(TG *g)
{
    return (int)jnext(g, 1);
}

/* ------------------------------------------------------------------ spawn */

static float side_x(TG *g)
{
    float x = 30.0f + jfloat(g) * 34.0f;         /* 30..64, clear of the path */
    return jbool(g) ? x : -x;
}

static float road_x(TG *g)
{
    return (jfloat(g) * 2.0f - 1.0f) * (TG_ROAD_HALF - 4.0f);
}

static void spawn_coin(TG *g, int i, float zBase)
{
    if (g->groupLeft <= 0 || g->groupZ < g->pz + 10.0f) {   /* new line */
        g->groupX = road_x(g);
        g->groupZ = zBase + jfloat(g) * 40.0f;
        g->groupLeft = 3 + jint(g, 3);                      /* 3..5 coins */
    }
    g->cbx[i] = g->groupX;
    g->cbz[i] = g->groupZ;
    g->groupZ += TG_COIN_GAP;
    g->groupLeft--;
    g->cLive[i] = 1;
    g->cPop[i] = 0.0f;
}

/* ------------------------------------------------------------------ setup */

void tg_init(TG *g, uint64_t seed)
{
    memset(g, 0, sizeof(*g));
    g->seed = (seed ^ 0x5DEECE66DULL) & JMASK;   /* java.util.Random ctor */
    tg_reset(g);
}

void tg_reset(TG *g)
{
    int i;

    g->px = 0.0f; g->pz = 0.0f; g->jumpY = 0.0f; g->vy = 0.0f;
    g->grounded = 1; g->vx = 0.0f; g->roll = 0.0f; g->runDist = 0.0f;
    g->animT = 0.0f; g->coins = 0; g->dir = 0;
    g->crash = 0; g->crashT = 0.0f;
    for (i = 0; i < TG_NEV; i++) g->evT[i] = -1.0f;

    for (i = 0; i < TG_N_TREE; i++) {
        g->tx[i] = side_x(g);
        g->tz[i] = 40.0f + i * (150.0f / TG_N_TREE) + jfloat(g) * 18.0f;
        g->ts[i] = 0.8f + jfloat(g) * 0.7f;
    }
    for (i = 0; i < TG_N_OBS; i++) {
        g->obz[i] = 60.0f + i * (140.0f / TG_N_OBS) + jfloat(g) * 14.0f;
        g->obx[i] = road_x(g);
    }
    g->groupLeft = 0;
    for (i = 0; i < TG_N_COIN; i++) spawn_coin(g, i, 50.0f + i * 8.0f);
}

void tg_jump(TG *g)
{
    if (g->grounded && !g->crash) {
        g->vy = TG_JUMP_V;
        g->grounded = 0;
    }
}

/* ------------------------------------------------------------------ update */

void tg_update(TG *g, float dt)
{
    int i;

    if (dt <= 0.0f) return;
    if (dt > 0.05f) dt = 0.05f;
    g->animT += dt;

    /* age sparkle events */
    for (i = 0; i < TG_NEV; i++) {
        if (g->evT[i] >= 0.0f) {
            g->evT[i] += dt;
            if (g->evT[i] > 0.45f) g->evT[i] = -1.0f;
        }
    }

    /* crash freeze: world holds still, then the run restarts fresh */
    if (g->crash) {
        g->crashT += dt;
        if (g->crashT > 0.9f) {
            g->crash = 0; g->crashT = 0.0f;
            g->coins = 0;
            g->runDist = 0.0f;               /* distance counter starts over */
            for (i = 0; i < TG_N_OBS; i++) { /* push rocks safely ahead */
                g->obz[i] = g->pz + 60.0f + i * (120.0f / TG_N_OBS)
                          + jfloat(g) * 12.0f;
                g->obx[i] = road_x(g);
            }
            g->groupLeft = 0;
            for (i = 0; i < TG_N_COIN; i++) spawn_coin(g, i, g->pz + 50.0f);
        }
        return;
    }

    {
        float tvx = g->roll * TG_MAX_VX;
        float k = 8.0f * dt;
        if (k > 1.0f) k = 1.0f;
        g->vx += (tvx - g->vx) * k;
    }
    g->px += g->vx * dt;
    if (g->px < -TG_X_LIMIT) { g->px = -TG_X_LIMIT; g->vx = 0.0f; }
    if (g->px >  TG_X_LIMIT) { g->px =  TG_X_LIMIT; g->vx = 0.0f; }

    /* facing: profile while steering, back to the camera when running
     * straight. Dead-zone + hysteresis stops flicker. */
    if (g->dir == 0) {
        if (g->roll > 0.15f) g->dir = 1;
        else if (g->roll < -0.15f) g->dir = -1;
    } else if (fabsf(g->roll) < 0.06f) {
        g->dir = 0;
    } else {
        g->dir = (g->roll > 0.0f) ? 1 : -1;
    }

    {
        float dz = TG_SPEED * dt;
        g->pz += dz;
        g->runDist += dz;
    }

    if (!g->grounded) {
        g->vy -= TG_GRAV * dt;
        g->jumpY += g->vy * dt;
        if (g->jumpY <= 0.0f) {
            g->jumpY = 0.0f;
            g->vy = 0.0f;
            g->grounded = 1;
        }
    }

    /* recycle scenery */
    for (i = 0; i < TG_N_TREE; i++) {
        if (g->tz[i] < g->pz - 14.0f) {
            g->tz[i] = g->pz + 150.0f + jfloat(g) * 30.0f;
            g->tx[i] = side_x(g);
            g->ts[i] = 0.8f + jfloat(g) * 0.7f;
        }
    }

    /* rocks: recycle ahead, keep clear of coin lines */
    for (i = 0; i < TG_N_OBS; i++) {
        if (g->obz[i] < g->pz - 14.0f) {
            float nz = g->pz + 150.0f + jfloat(g) * 30.0f;
            float nx = road_x(g);
            int k;
            for (k = 0; k < TG_N_COIN; k++) {
                if (fabsf(g->cbz[k] - nz) < 6.0f
                        && fabsf(g->cbx[k] - nx) < 4.5f) {
                    nz += 10.0f;
                    k = -1;                     /* re-check all coins */
                    if (nz > g->pz + 185.0f) { nz = g->pz + 185.0f; break; }
                }
            }
            g->obz[i] = nz;
            g->obx[i] = nx;
        }
    }

    /* coins: taken slots come back in a fresh line far ahead; untouched
     * coins recycle once they fall behind */
    for (i = 0; i < TG_N_COIN; i++) {
        if (g->cLive[i]) {
            if (g->cbz[i] < g->pz - 10.0f) spawn_coin(g, i, g->pz + 140.0f);
        } else {
            g->cPop[i] += dt;
            if (g->cPop[i] >= 0.25f) spawn_coin(g, i, g->pz + 140.0f);
        }
    }

    /* ---- collision & pickup ---- */
    for (i = 0; i < TG_N_OBS; i++) {
        if (fabsf(g->pz - g->obz[i]) < 2.0f
                && fabsf(g->px - g->obx[i]) < 2.6f
                && g->jumpY < TG_OBS_H) {
            g->crash = 1;
            g->crashT = 0.0f;
            g->hitX = g->px; g->hitZ = g->pz;
            g->obz[i] = g->pz - 20.0f;          /* kick the rock behind us */
            return;
        }
    }
    for (i = 0; i < TG_N_COIN; i++) {
        if (!g->cLive[i]) continue;
        if (fabsf(g->pz - g->cbz[i]) < 2.2f
                && fabsf(g->px - g->cbx[i]) < 2.2f) {
            g->coins++;
            g->pkSeq++;
            g->cLive[i] = 0;                    /* gone from the world now */
            g->cPop[i] = 0.0f;
            g->evX[g->evHead] = g->cbx[i];
            g->evZ[g->evHead] = g->cbz[i];
            g->evT[g->evHead] = 0.0f;
            g->evHead = (g->evHead + 1) % TG_NEV;
        }
    }
}

int tg_frame(const TG *g)
{
    return ((int)(g->runDist / 6.0f)) % 6;
}

float tg_km(const TG *g)
{
    return g->runDist / TG_UNITS_PER_KM;
}
