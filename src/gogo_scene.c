/*
 * gogo_scene.c — the visible game: a pseudo-3D dirt path running away from a
 * camera that sits high behind the runner, trees along the shoulders, boulders
 * to jump over, coin lines to collect, and a tilt/tap HUD.
 *
 * This is a line-by-line port of the original Android GogoView (SurfaceView +
 * android.graphics.Canvas) into plain C over TGRaster.  Geometry, colours and
 * draw order are unchanged from that build; only the API underneath moved.
 */
#include "gogo_scene.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef GOGO_PROFILE
/* Optional per-stage timing, used by tools/bench_dev.c with -DGOGO_PROFILE.
 * Off in the app: it costs two clock reads per stage. */
#include <sys/time.h>
double gogo_prof[GOGO_PROF_N];
static double prof_now(void)      /* clock_gettime is iOS 10+, this is not */
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}
#define PROF_START() double _pl = prof_now()
#define PROF_MARK(i) do { double _n = prof_now(); gogo_prof[i] += _n - _pl; _pl = _n; } while (0)
#else
#define PROF_START() do { } while (0)
#define PROF_MARK(i) do { } while (0)
#endif

/* --------------------------------------------------------------- camera */
static const float FOCAL  = 170.0f;
static const float CCX    = 120.0f;
static const float CCY    = 175.0f;   /* canvas centre, nudged down a little */
static const float CAM_H  = 11.4f;    /* eye height                          */
static const float CAM_D  = 11.0f;    /* behind the runner                   */
static const float PITCH  = 0.231f;   /* rad, looking down                   */
static const float SPR_H  = 5.1f;     /* sprite height in world units        */

/* 6x4 sheet, 106x74 cells: rows are left profile / back / right profile /
 * front (the splash pose). */
static const int FW = 106;
static const int FH = 74;
static const int ROW_FRONT = 3;

static const float LOAD_TIME = 3.0f;

/* on-screen controls, used when the accelerometer never delivers a sample */
static const float STICK_X = 54.0f, STICK_Y = 296.0f;
static const float STICK_R = 36.0f, KNOB_R = 15.0f;
static const float JUMP_X = 186.0f, JUMP_Y = 296.0f, JUMP_R = 30.0f;

/* -------------------------------------------------------------- colours */
#define C_SKY_TOP   0xFF7EC0EEu
#define C_SKY_BOT   0xFFCFEAFBu
#define C_GRASS     0xFF63A34Fu
#define C_GRASS_D   0xFF578F45u
#define C_PATH      0xFFC9A36Bu
#define C_PATH_D    0xFFB18A55u
#define C_TRUNK     0xFF7A5230u
#define C_LEAF      0xFF3E7C3Au
#define C_LEAF_D    0xFF336630u
#define C_LOAD_BG   0xFF101014u   /* splash backdrop: black lifted a notch */
#define C_ROCK      0xFF8D939Eu
#define C_ROCK_D    0xFF6C7079u
#define C_ROCK_L    0xFFAEB4BFu
#define C_GOLD      0xFFFFC93Cu
#define C_GOLD_D    0xFFC8951Bu
#define C_GOLD_L    0xFFFFEFAFu

/* ------------------------------------------------------------ lifecycle */

int gogo_init(GoGoScene *s, TGRaster *r, const char *atlasPath,
              const char *fontPath, uint64_t seed)
{
    memset(s, 0, sizeof(*s));
    s->r = r;
    tg_init(&s->g, seed);
    s->loading = 1;
    s->loadT = 0.0f;
    s->kmShown = -1;
    s->coinShown = -1;
    s->ready = (rs_load_image(atlasPath, &s->atlas) == 0) &&
               (rs_load_font(fontPath, &s->font) == 0);
    return s->ready ? 0 : -1;
}

void gogo_shutdown(GoGoScene *s)
{
    rs_free_image(&s->atlas);
    rs_free_font(&s->font);
    s->ready = 0;
}

void gogo_step(GoGoScene *s, float dt)
{
    if (s->loading) {
        s->loadT += dt;
        if (s->loadT >= LOAD_TIME) {
            s->loading = 0;
            tg_reset(&s->g);        /* fresh run when the path appears */
        }
        return;
    }
    tg_update(&s->g, dt);
}

/* ------------------------------------------------------------- 3D maths */

static void proj(GoGoScene *s, float x, float y, float z)
{
    float dx = x - s->camX;
    float dz = z - s->camZ;
    float hr = CAM_H + y;                       /* camera height above it */
    float zc = hr * s->pitSin + dz * s->pitCos;
    if (zc < 1.0f) zc = 1.0f;
    float yc = -hr * s->pitCos + dz * s->pitSin;
    s->tSx = CCX + FOCAL * dx / zc;
    s->tSy = CCY - FOCAL * yc / zc;
    s->tZc = zc;
}

/* A window into the packed atlas. */
static TGImage cell(const GoGoScene *s, int row, int frame)
{
    TGImage c;
    c.w = FW;
    c.h = FH;
    c.stride = s->atlas.stride;
    c.px = s->atlas.px + (size_t)(row * FH) * (size_t)s->atlas.stride
                        + (size_t)(frame * FW);
    return c;
}

/* ---------------------------------------------------------------- pieces */

static void path_quad(GoGoScene *s, float x0, float x1, float zn, float zf)
{
    float a, b, c, d, ay, by, cy, dy;
    proj(s, x0, 0, zn);  a = s->tSx; ay = s->tSy;
    proj(s, x1, 0, zn);  b = s->tSx; by = s->tSy;
    proj(s, x1, 0, zf);  c = s->tSx; cy = s->tSy;
    proj(s, x0, 0, zf);  d = s->tSx; dy = s->tSy;
    rs_quad(s->r, a, ay, b, by, c, cy, d, dy);
}

/* Isosceles triangle sitting on (cx, baseY), symmetric about cx -- the
 * Java original's drawTri().  Both shoulders have to be drawn or the tree
 * comes out as a right triangle with its whole left side missing. */
static void draw_tri(GoGoScene *s, float cx, float baseY, float w, float h,
                     uint32_t col)
{
    rs_set_fill(s->r, col);
    rs_tri(s->r, cx - w * 0.5f, baseY, cx + w * 0.5f, baseY, cx, baseY - h);
}

static void draw_tree(GoGoScene *s, int i)
{
    float z = s->g.tz[i];
    if (z - s->camZ < 8.5f || z - s->camZ > 150.0f) return;
    proj(s, s->g.tx[i], 0, z);
    float sx = s->tSx, sy = s->tSy, zc = s->tZc;
    if (sx < -40.0f || sx > GOGO_VW + 40.0f) return;

    float h = FOCAL * 12.0f * s->g.ts[i] / zc;
    float w = h * 0.42f;
    float cw = w * 2.0f;                 /* crown twice as wide as the trunk */

    /* trunk, buried a little in the crown base so the two never part */
    rs_set_fill(s->r, C_TRUNK);
    rs_rect(s->r, sx - w * 0.09f, sy - h * 0.34f, w * 0.18f, h * 0.34f);

    /* two stacked foliage triangles */
    draw_tri(s, sx, sy - h * 0.28f, cw * 0.66f, h * 0.62f, C_LEAF_D);
    draw_tri(s, sx, sy - h * 0.50f, cw * 0.50f, h * 0.50f, C_LEAF);
}

/* One shading facet of a boulder: a quad bulging towards the top. */
static void rock_facet(GoGoScene *s, float sx, float sy, float w, float ty,
                       float x0, float x1, float xb)
{
    float ax = sx + w * x0,  ay = sy;
    float bx = sx + w * ((x0 + x1) * 0.5f - 0.08f), by = sy + ty * 0.55f;
    float cx = sx + w * x1,  cy = sy + ty;
    float dx = sx + w * xb,  dy = sy;
    rs_quad(s->r, ax, ay, bx, by, cx, cy, dx, dy);
}

static void draw_rock(GoGoScene *s, int i)
{
    float z = s->g.obz[i];
    if (z - s->camZ < 8.5f || z - s->camZ > 150.0f) return;
    proj(s, s->g.obx[i], 0, z);
    float sx = s->tSx, sy = s->tSy, zc = s->tZc;
    if (sx < -30.0f || sx > GOGO_VW + 30.0f) return;

    float h = FOCAL * TG_OBS_H / zc;
    float w = h * TG_OBS_W;

    rs_set_fill(s->r, 0x44000000u);
    rs_ellipse(s->r, sx - w * 0.55f, sy - h * 0.14f, w * 1.10f, h * 0.28f);

    float px[6], py[6];
    px[0] = sx - w * 0.50f; py[0] = sy;
    px[1] = sx - w * 0.42f; py[1] = sy - h * 0.72f;
    px[2] = sx - w * 0.16f; py[2] = sy - h;
    px[3] = sx + w * 0.20f; py[3] = sy - h * 0.92f;
    px[4] = sx + w * 0.50f; py[4] = sy - h * 0.40f;
    px[5] = sx + w * 0.44f; py[5] = sy;
    rs_set_fill(s->r, C_ROCK);
    rs_poly(s->r, px, py, 6);

    rs_set_fill(s->r, C_ROCK_D);
    rock_facet(s, sx, sy, w, -h * 0.45f, 0.05f, 0.50f, 0.44f);
    rs_set_fill(s->r, C_ROCK_L);
    rock_facet(s, sx, sy, w, -h * 0.72f, -0.40f, 0.28f, 0.30f);
}

static void draw_coin(GoGoScene *s, int i)
{
    if (!s->g.cLive[i]) return;
    float z = s->g.cbz[i];
    if (z - s->camZ < 8.5f || z - s->camZ > 150.0f) return;

    float bob = sinf(s->g.animT * 3.2f + z * 0.7f) * 0.22f;
    proj(s, s->g.cbx[i], -(TG_COIN_H + bob), z);
    float sx = s->tSx, sy = s->tSy, zc = s->tZc;
    if (sx < -30.0f || sx > GOGO_VW + 30.0f) return;
    float r = FOCAL * TG_COIN_R / zc;

    /* drop shadow on the dirt sells the "floating" look */
    proj(s, s->g.cbx[i], 0, z);
    float gx = s->tSx, gy = s->tSy;
    float sr = r * (1.05f - bob * 0.6f);
    rs_set_fill(s->r, 0x3A000000u);
    rs_ellipse(s->r, gx - sr, gy - sr * 0.3f, sr * 2.0f, sr * 0.6f);

    float sq = fabsf(cosf(s->g.animT * 4.0f + z * 0.5f));   /* spin squash */
    if (sq < 0.22f) sq = 0.22f;

    rs_set_fill(s->r, C_GOLD_D);
    rs_ellipse(s->r, sx - r * 1.1f * sq, sy - r * 1.1f, r * 2.2f * sq, r * 2.2f);
    rs_set_fill(s->r, C_GOLD);
    rs_ellipse(s->r, sx - r * sq * 0.82f, sy - r * 0.82f,
               r * 1.64f * sq, r * 1.64f);
    rs_set_fill(s->r, C_GOLD_L);
    rs_ellipse(s->r, sx - r * 0.5f * sq, sy - r * 0.55f,
               r * 0.45f * sq + 0.6f, r * 0.45f);
}

static void draw_collect_fx(GoGoScene *s)
{
    for (int k = 0; k < TG_NEV; k++) {
        float t = s->g.evT[k];
        if (t < 0.0f) continue;
        float f = t / 0.45f;
        if (f > 1.0f) f = 1.0f;
        proj(s, s->g.evX[k], -(TG_COIN_H + f * 1.2f), s->g.evZ[k]);
        float sx = s->tSx, sy = s->tSy, zc = s->tZc;
        float r = FOCAL * 0.24f / zc * (1.0f + f * 2.2f);
        int a = (int)(200.0f * (1.0f - f));
        rs_set_fill(s->r, ((unsigned)a << 24) | 0xF8D48Au);
        for (int d = 0; d < 6; d++) {
            float ang = (float)(M_PI * 2.0 * d / 6.0) + f * 2.2f;
            float px = sx + cosf(ang) * r;
            float py = sy + sinf(ang) * r * 0.7f;
            float rad = 2.8f * (1.0f - f);
            if (rad < 0.8f) rad = 0.8f;
            rs_ellipse(s->r, px - rad, py - rad, rad * 2.0f, rad * 2.0f);
        }
    }
}

static void draw_trump(GoGoScene *s)
{
    proj(s, s->g.px, 0, s->g.pz);
    float shX = s->tSx, shY = s->tSy;
    float shrink = 1.0f - fminf(1.0f, s->g.jumpY * 0.55f);
    rs_set_fill(s->r, 0x55000000u);
    rs_ellipse(s->r, shX - 19.0f * shrink, shY - 4.8f * shrink,
               38.0f * shrink, 9.6f * shrink);

    proj(s, s->g.px, -s->g.jumpY, s->g.pz);
    float h = FOCAL * SPR_H / s->tZc;
    float w = h * (float)FW / (float)FH;

    int fr = tg_frame(&s->g);
    int row = s->g.dir + 1;
    if (row < 0) row = 0;
    if (row > 2) row = 2;

    float alpha = 1.0f;
    if (s->g.crash) {                     /* blink before recovery */
        float t = s->g.crashT;
        if (t < 0.66f && fmodf(t, 0.33f) < 0.165f) alpha = 0.35f;
    }
    TGImage im = cell(s, row, fr);
    rs_blit(s->r, &im, s->tSx - w * 0.5f, s->tSy - h, w, h, alpha, 1);
}

static void draw_touch_controls(GoGoScene *s)
{
    if (!s->touchMode) return;
    TGRaster *r = s->r;

    rs_set_fill(r, 0x2EFFFFFFu);
    rs_ellipse(r, STICK_X - STICK_R, STICK_Y - STICK_R,
               STICK_R * 2, STICK_R * 2);
    rs_set_fill(r, 0x66FFFFFFu);
    rs_ellipse_stroke(r, STICK_X - STICK_R, STICK_Y - STICK_R,
                      STICK_R * 2, STICK_R * 2, 1.6f);

    rs_set_fill(r, 0x2EFFFFFFu);
    rs_ellipse(r, JUMP_X - JUMP_R, JUMP_Y - JUMP_R, JUMP_R * 2, JUMP_R * 2);
    rs_set_fill(r, 0x66FFFFFFu);
    rs_ellipse_stroke(r, JUMP_X - JUMP_R, JUMP_Y - JUMP_R,
                      JUMP_R * 2, JUMP_R * 2, 1.6f);

    /* up arrow inside the jump button */
    {
        float t = 11.0f, cx = JUMP_X, cy = JUMP_Y;
        float px[6], py[6];
        px[0] = cx;            py[0] = cy - t;
        px[1] = cx + t * 0.82f; py[1] = cy + t * 0.42f;
        px[2] = cx + t * 0.34f; py[2] = cy + t * 0.42f;
        px[3] = cx;            py[3] = cy - t * 0.22f;
        px[4] = cx - t * 0.34f; py[4] = cy + t * 0.42f;
        px[5] = cx - t * 0.82f; py[5] = cy + t * 0.42f;
        rs_set_fill(r, 0x80FFFFFFu);
        rs_quad(r, px[0], py[0], px[1], py[1], px[2], py[2], px[3], py[3]);
        rs_quad(r, px[0], py[0], px[3], py[3], px[4], py[4], px[5], py[5]);
    }

    /* the knob the player drags */
    rs_set_fill(r, 0x7AFFFFFFu);
    rs_ellipse(r, STICK_X + s->stkDX - KNOB_R, STICK_Y + s->stkDY - KNOB_R,
               KNOB_R * 2, KNOB_R * 2);
    rs_set_fill(r, 0x9AFFFFFFu);
    rs_ellipse_stroke(r, STICK_X + s->stkDX - KNOB_R,
                      STICK_Y + s->stkDY - KNOB_R,
                      KNOB_R * 2, KNOB_R * 2, 1.4f);
}

/* ------------------------------------------------------------------ HUD */

static void draw_dist(GoGoScene *s)
{
    if (s->noText) return;
    int h = (int)(tg_km(&s->g) * 100.0f);
    if (h != s->kmShown) {
        s->kmShown = h;
        snprintf(s->km, sizeof(s->km), "%d.%02d km", h / 100, h % 100);
    }
    rs_text_shadow(s->r, &s->font, 232.0f, 9.0f, s->km, 0xFFFFFFFFu, 1);
}

static void draw_coin_count(GoGoScene *s)
{
    if (s->noText) return;
    int n = s->g.coins;
    if (n != s->coinShown) {
        s->coinShown = n;
        snprintf(s->coin, sizeof(s->coin), "COINS %d", n);
    }
    rs_text_shadow(s->r, &s->font, 8.0f, 9.0f, s->coin, 0xFFFFFFFFu, 0);
}

static void draw_loading(GoGoScene *s)
{
    TGRaster *r = s->r;
    /* near-black, not pure black: the panel looks like a dead screen at 0 and
     * the sprite's drop shadow vanishes into it */
    rs_set_fill(r, C_LOAD_BG);
    rs_rect(r, -8, -8, GOGO_VW + 16, GOGO_VH + 16);

    int fr = ((int)(s->loadT / 0.1f)) % 6;
    float h = 96.0f;
    float w = h * (float)FW / (float)FH;
    float cx = GOGO_VW * 0.5f, feetY = 218.0f;

    /* faint spotlight so he is not floating in the void */
    rs_radial(r, cx, feetY - 14.0f, 84.0f, 0x1AFFFFFFu, 0x00FFFFFFu);

    TGImage im = cell(s, ROW_FRONT, fr);
    rs_blit(r, &im, cx - w * 0.5f, feetY - h, w, h, 1.0f, 1);

    if (s->noText) return;
    rs_text(r, &s->font, cx, feetY + 42.0f, "loading...",
            0xFFFFFFFFu, 2);          /* centred on the screen axis */
}

/* ----------------------------------------------------------------- paint */

void gogo_draw(GoGoScene *s)
{
    TGRaster *r = s->r;
    PROF_START();
    rs_begin(r);

    if (!s->ready) {                      /* nothing loaded: say so loudly */
        rs_set_fill(r, 0xFF802020u);
        rs_rect(r, -8, -8, GOGO_VW + 16, GOGO_VH + 16);
        return;
    }
    if (s->loading) {
        draw_loading(s);
        return;
    }

    s->pitSin = sinf(PITCH);
    s->pitCos = cosf(PITCH);
    s->camX += (s->g.px * 0.7f - s->camX) * 0.12f;  /* camera glides after him */
    s->camZ = s->g.pz - CAM_D;

    rs_save(r);
    if (s->g.crash) {                                /* crash shake */
        float k = 1.0f - s->g.crashT / 0.9f;
        rs_translate(r, sinf(s->g.animT * 71.0f) * 4.0f * k,
                        cosf(s->g.animT * 83.0f) * 3.0f * k);
    }

    PROF_MARK(0);
    float horizon = CCY - FOCAL * s->pitSin / s->pitCos;
    if (horizon < 1.0f) horizon = 1.0f;

    /* sky */
    rs_gradient_v(r, -8.0f, GOGO_VW + 16.0f, -2.0f, horizon,
                  C_SKY_TOP, C_SKY_BOT);
    PROF_MARK(1);

    /* grass */
    rs_set_fill(r, C_GRASS);
    rs_rect(r, -8, horizon, GOGO_VW + 16, GOGO_VH - horizon + 8);
    PROF_MARK(2);

    /* moving cross planks on the path: the speed cue */
    rs_set_fill(r, C_PATH_D);
    {
        float z0 = floorf((s->camZ + 8.0f) / 12.0f) * 12.0f;
        for (float z = z0; z < s->camZ + 150.0f; z += 12.0f) {
            if (((int)(z / 12.0f) & 1) != 0) continue;
            if (z - s->camZ < 8.4f) continue;
            float gn = 2.2f;
            proj(s, -TG_ROAD_HALF, 0, z);
            float ax = s->tSx, ay = s->tSy;
            proj(s, TG_ROAD_HALF, 0, z);
            float bx = s->tSx, by = s->tSy;
            proj(s, TG_ROAD_HALF, 0, z + gn);
            float cx = s->tSx, cy = s->tSy;
            proj(s, -TG_ROAD_HALF, 0, z + gn);
            rs_quad(r, ax, ay, bx, by, cx, cy, s->tSx, s->tSy);
        }
    }
    PROF_MARK(3);

    /* grass grid lines, the path paints over them */
    rs_set_fill(r, C_GRASS_D);
    {
        float zg = floorf((s->camZ + 8.0f) / 24.0f) * 24.0f;
        for (float z = zg; z < s->camZ + 150.0f; z += 24.0f) {
            if (z - s->camZ < 8.0f) continue;
            proj(s, -90.0f, 0, z);
            float ax = s->tSx, ay = s->tSy;
            proj(s, 90.0f, 0, z);
            rs_line(r, ax, ay, s->tSx, s->tSy, 0.5f);
        }
    }
    PROF_MARK(4);

    /* dirt path plus its white edge stripes */
    rs_set_fill(r, C_PATH);
    path_quad(s, -TG_ROAD_HALF, TG_ROAD_HALF, s->camZ + 6.0f, s->camZ + 160.0f);
    PROF_MARK(5);

    rs_set_fill(r, 0xFFEDE4D0u);
    path_quad(s, -TG_ROAD_HALF, -TG_ROAD_HALF + TG_LINE_W,
              s->camZ + 6.5f, s->camZ + 150.0f);
    path_quad(s, TG_ROAD_HALF - TG_LINE_W, TG_ROAD_HALF,
              s->camZ + 6.5f, s->camZ + 150.0f);

    PROF_MARK(6);
    /* depth split: ahead of the runner, the runner, then behind */
    for (int i = 0; i < TG_N_TREE; i++)
        if (s->g.tz[i] > s->g.pz + 1.0f) draw_tree(s, i);
    for (int i = 0; i < TG_N_OBS; i++)
        if (s->g.obz[i] > s->g.pz + 1.0f) draw_rock(s, i);
    for (int i = 0; i < TG_N_COIN; i++)
        if (s->g.cbz[i] > s->g.pz + 1.0f) draw_coin(s, i);

    PROF_MARK(7);
    draw_collect_fx(s);
    draw_trump(s);
    PROF_MARK(8);

    for (int i = 0; i < TG_N_TREE; i++)
        if (s->g.tz[i] <= s->g.pz + 1.0f && s->g.tz[i] > s->camZ + 8.5f)
            draw_tree(s, i);
    for (int i = 0; i < TG_N_OBS; i++)
        if (s->g.obz[i] <= s->g.pz + 1.0f && s->g.obz[i] > s->camZ + 8.5f)
            draw_rock(s, i);

    rs_restore(r);

    /* crash flash */
    if (s->g.crash) {
        float a = 150.0f * (1.0f - s->g.crashT / 0.9f);
        unsigned al = (unsigned)a;
        if (al > 255) al = 255;
        rs_set_fill(r, (al << 24) | 0xFF2A00u);
        rs_rect(r, 0, 0, GOGO_VW, GOGO_VH);
    }

    PROF_MARK(9);
    draw_coin_count(s);
    draw_dist(s);
    draw_touch_controls(s);
    PROF_MARK(10);
}
