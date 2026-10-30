/*
 * AppFight - « Street Fury », jeu de combat 16 bits (6eme app, page 6).
 *
 * Principes de rendu :
 *  - un seul canevas 410 x 396 place a y=56 (sous la barre d'etat orange) ;
 *  - le decor complet est peint une fois dans _bg (PSRAM), puis recopie dans
 *    le canevas ;
 *  - a chaque image d'attaque, seule la bande des combattants est restauree
 *    depuis _bg, redessinee, puis invalidee : une seule zone, ~110 Ko ;
 *  - au repos, rien n'est redessine du tout ;
 *  - pas de son (piege 25), pas de nvs, pas d'I2C.
 */
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "app_fight.hpp"
#include "icons/app_icons.h"
#include "private/esp_brookesia_utils.h"

static const char *TAG = "FIGHT";

/* ------------------------------------------------------------------ */
/* Geometrie                                                           */
/* ------------------------------------------------------------------ */
#define CW          410
#define CH          396
#define CANVAS_Y    56              /* haut du decor a l'ecran        */

#define ART_AS      3               /* 1 pixel d'art = 3 pixels ecran  */
#define ART_OX0     46              /* origine art du combattant gauche */
#define ART_OX1     166             /* origine art du combattant droit  */
#define ART_OY      72
#define ART_W       66
#define ART_H       88

/* bande redessinee a chaque image d'attaque (coordonnees canevas) */
#define SCENE_X0    16
#define SCENE_Y0    86
#define SCENE_X1    396
#define SCENE_Y1    356

#define FRAMES      12              /* images d'une attaque (40 ms)   */
#define IMPACT_F    6

#define HP_MAX      240
#define BAR_W       178
#define BAR_H       16

/* zones touchees, en pixels d'art du combattant touche */
#define P_HEAD      0
#define P_JAW       1
#define P_CHIN      2
#define P_TORSO     3
#define P_BELLY     4
#define P_LEG       5

/* genres d'attaque */
#define K_PUNCH_N   0
#define K_PUNCH_F   1
#define K_KICK      2
#define K_SPECIAL   3

static const int PART_AX[6] = {33, 37, 35, 33, 33, 28};
static const int PART_AY[6] = {22, 26, 29, 42, 50, 72};

struct Atk {
    const char *name;
    uint8_t dmg;
    uint8_t var;
    uint8_t part;
    uint8_t kind;
};

static const Atk ATK[2][5] = {
    {   /* Ryo : poings et pied */
        {"JAB",        7, 1, P_HEAD,  K_PUNCH_N},
        {"CROCHET",   11, 2, P_JAW,   K_PUNCH_F},
        {"PIED HAUT", 14, 2, P_TORSO, K_KICK},
        {"UPPERCUT",  17, 2, P_CHIN,  K_PUNCH_F},
        {"HADOKEN",   22, 3, P_TORSO, K_SPECIAL},
    },
    {   /* Kai : griffes, genou, balayage */
        {"GRIFFE",     7, 1, P_HEAD,  K_PUNCH_N},
        {"GENOU",     11, 2, P_BELLY, K_KICK},
        {"BALAYAGE",  14, 2, P_LEG,   K_KICK},
        {"DOUBLE",    17, 2, P_HEAD,  K_PUNCH_F},
        {"ONDE",      22, 3, P_TORSO, K_SPECIAL},
    },
};

/* ------------------------------------------------------------------ */
static inline uint16_t to565(uint32_t c)
{
    return (uint16_t)(((c >> 8) & 0xF800u) | ((c >> 5) & 0x07E0u) | ((c >> 3) & 0x001Fu));
}

static inline uint32_t lerp_c(uint32_t a, uint32_t b, int t)
{
    int r = (int)((a >> 16) & 0xFF) + (((int)((b >> 16) & 0xFF) - (int)((a >> 16) & 0xFF)) * t) / 256;
    int g = (int)((a >> 8) & 0xFF) + (((int)((b >> 8) & 0xFF) - (int)((a >> 8) & 0xFF)) * t) / 256;
    int bl = (int)(a & 0xFF) + (((int)(b & 0xFF) - (int)(a & 0xFF)) * t) / 256;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

static inline int lerp_i(int a, int b, int t, int n)
{
    if (n <= 0) {
        return b;
    }
    return a + (b - a) * t / n;
}

/* ------------------------------------------------------------------ */
AppFight::AppFight() :
    ESP_Brookesia_PhoneApp("Fight", &app_icon_fight, true, true, true)
{
}

AppFight::~AppFight()
{
    if (_timer != nullptr) {
        lv_timer_del(_timer);
        _timer = nullptr;
    }
}

/* ------------------------------------------------------------------ */
/* acces pixel bas niveau                                              */
/* ------------------------------------------------------------------ */
void AppFight::fp(int side, int ax, int ay, uint16_t c)
{
    const int cx = (side == 0) ? (ART_OX0 + ax * ART_AS) : (ART_OX1 + (ART_W - 1 - ax) * ART_AS);
    const int cy = ART_OY + ay * ART_AS;
    for (int dy = 0; dy < ART_AS; dy++) {
        const int y = cy + dy;
        if (y < 0 || y >= CH) {
            continue;
        }
        uint16_t *row = _px + (size_t)y * CW;
        for (int dx = 0; dx < ART_AS; dx++) {
            const int x = cx + dx;
            if (x < 0 || x >= CW) {
                continue;
            }
            row[x] = c;
        }
    }
}

void AppFight::frect(int side, int x0, int y0, int x1, int y1, uint16_t c)
{
    if (x1 < x0) {
        int t = x0; x0 = x1; x1 = t;
    }
    if (y1 < y0) {
        int t = y0; y0 = y1; y1 = t;
    }
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            fp(side, x, y, c);
        }
    }
}

void AppFight::fcirc(int side, int cx, int cy, int r, uint16_t c)
{
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r * r) {
                fp(side, cx + dx, cy + dy, c);
            }
        }
    }
}

void AppFight::fline(int side, int x0, int y0, int x1, int y1, int t, uint16_t c)
{
    int dx = abs(x1 - x0);
    int sx = (x0 < x1) ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;
    const int h = t / 2;
    for (;;) {
        for (int j = -h; j <= h; j++) {
            for (int i = -h; i <= h; i++) {
                fp(side, x0 + i, y0 + j, c);
            }
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* membre en deux segments, avec une cassure (coude ou genou) */
void AppFight::limb(int side, int x0, int y0, int x1, int y1, uint16_t fill, uint16_t out, int bend,
                     int t)
{
    int mx = (x0 + x1) / 2;
    int my = (y0 + y1) / 2;
    if (bend != 0) {
        const int dx = x1 - x0;
        const int dy = y1 - y0;
        float len = sqrtf((float)(dx * dx + dy * dy));
        if (len < 1.0f) {
            len = 1.0f;
        }
        mx += (int)((float)(-dy) * (float)bend / len);
        my += (int)((float)(dx) * (float)bend / len);
    }
    fline(side, x0, y0, mx, my, t, out);
    fline(side, x0, y0, mx, my, t - 2 > 2 ? t - 2 : 2, fill);
    fline(side, mx, my, x1, y1, t, out);
    fline(side, mx, my, x1, y1, t - 2 > 2 ? t - 2 : 2, fill);
}

void AppFight::arm(int side, int sx, int sy, int hx, int hy, uint16_t sleeve, uint16_t skin,
                  uint16_t out, int bend)
{
    int ex = (sx + hx) / 2;
    int ey = (sy + hy) / 2;
    const int dx = hx - sx;
    const int dy = hy - sy;
    float len = sqrtf((float)(dx * dx + dy * dy));
    if (len < 1.0f) {
        len = 1.0f;
    }
    ex += (int)((float)(-dy) * (float)bend / len);
    ey += (int)((float)(dx) * (float)bend / len);

    fline(side, sx, sy, ex, ey, 6, out);
    fline(side, sx, sy, ex, ey, 4, sleeve);
    fline(side, ex, ey, hx, hy, 5, out);
    fline(side, ex, ey, hx, hy, 3, skin);
    fcirc(side, hx, hy, 3, out);
    fcirc(side, hx, hy, 2, skin);
}

void AppFight::box(int side, int x0, int y0, int x1, int y1, uint16_t fill, uint16_t out)
{
    frect(side, x0 - 1, y0 - 1, x1 + 1, y1 + 1, out);
    frect(side, x0, y0, x1, y1, fill);
}

/* ------------------------------------------------------------------ */
/* poses                                                               */
/* ------------------------------------------------------------------ */
AppFight::Pose AppFight::guard_pose(void) const
{
    Pose p;
    p.lean = 1;
    p.fnx = 43; p.fny = 26;
    p.ffx = 37; p.ffy = 29;
    p.fkx = 42; p.fky = 85;
    p.head_dy = 0;
    p.arm_up = 0;
    return p;
}

AppFight::Pose AppFight::winner_pose(void) const
{
    Pose p = guard_pose();
    p.fnx = 48; p.fny = 4;
    p.ffx = 20; p.ffy = 6;
    p.arm_up = 1;
    return p;
}

AppFight::Pose AppFight::loser_pose(void) const
{
    Pose p = guard_pose();
    p.lean = -5;
    p.head_dy = 5;
    p.fnx = 33; p.fny = 56;
    p.ffx = 30; p.ffy = 58;
    p.fkx = 40; p.fky = 85;
    return p;
}

void AppFight::to_art(int side, int cx, int cy, int *ax, int *ay) const
{
    if (side == 0) {
        *ax = (cx - ART_OX0) / ART_AS;
    } else {
        *ax = (ART_W - 1) - (cx - ART_OX1) / ART_AS;
    }
    *ay = (cy - ART_OY) / ART_AS;
}

/* ------------------------------------------------------------------ */
/* dessin d'un combattant                                              */
/* ------------------------------------------------------------------ */
void AppFight::draw_fighter(int side, const Pose &p, int flash)
{
    const uint16_t OUT  = to565(flash ? 0xFFD8D8 : 0x0E0A18);
    const uint16_t skin = to565(flash ? 0xFFFFFF : (side == 0 ? 0xFFC79A : 0xC98A55));
    const uint16_t hair = to565(flash ? 0xFFFFFF : (side == 0 ? 0x241A2A : 0xF2D24A));
    const uint16_t band = to565(flash ? 0xFFFFFF : (side == 0 ? 0xF2C33C : 0x46E0D0));
    const uint16_t gi   = to565(flash ? 0xFFFFFF : (side == 0 ? 0xE0362E : 0x2C7BE8));
    const uint16_t pant = to565(flash ? 0xFFFFFF : (side == 0 ? 0x2C2A6E : 0xD8DCEA));
    const uint16_t belt = to565(flash ? 0xFFFFFF : (side == 0 ? 0xF2C33C : 0x46E0D0));

    const int L = p.lean;
    const int hx = 33 + L;
    const int hy = 20 + p.head_dy;

    /* jambe arriere */
    limb(side, 30 + L, 51, 27, 69, pant, OUT, -2, 8);
    limb(side, 27, 69, 24, 85, pant, OUT, 0, 6);

    /* jambe avant, ou jambe de coup de pied */
    {
        const int kx = (p.fkx + 36 + L) / 2;
        const int ky = (p.fky + 51) / 2;
        fline(side, 36 + L, 51, kx, ky, 7, OUT);
        fline(side, 36 + L, 51, kx, ky, 5, pant);
        fline(side, kx, ky, p.fkx, p.fky, 6, OUT);
        fline(side, kx, ky, p.fkx, p.fky, 4, pant);
        fcirc(side, p.fkx, p.fky, 3, OUT);
        fcirc(side, p.fkx, p.fky, 2, pant);
    }

    /* torse : trapeze, epaules larges et taille fine */
    for (int i = 0; i < 18; i++) {
        const int hw = 8 - i / 4;
        frect(side, 33 + L - hw, 30 + i, 33 + L + hw, 30 + i, gi);
    }
    frect(side, 24 + L, 29, 42 + L, 30, OUT);
    frect(side, 24 + L, 31, 24 + L, 46, OUT);
    frect(side, 42 + L, 31, 42 + L, 46, OUT);
    box(side, 28 + L, 46, 38 + L, 51, belt, OUT);
    frect(side, 31 + L, 30, 35 + L, 33, skin);         /* col en V */

    /* tete : cheveux, bandeau, yeux, bouche */
    fcirc(side, hx, hy, 8, OUT);
    fcirc(side, hx, hy, 7, hair);
    fcirc(side, hx + 1, hy + 3, 5, skin);
    frect(side, hx - 7, hy - 1, hx + 8, hy + 1, band);
    frect(side, hx + 2, hy + 3, hx + 3, hy + 3, to565(0x1A1020));
    frect(side, hx + 6, hy + 3, hx + 7, hy + 3, to565(0x1A1020));
    frect(side, hx + 1, hy + 1, hx + 3, hy + 1, hair);
    fp(side, hx + 4, hy + 6, to565(0x8A4A3A));

    /* bras arriere puis bras avant */
    arm(side, 25 + L, 33, p.ffx, p.ffy, gi, skin, OUT, 4);
    arm(side, 41 + L, 33, p.fnx, p.fny, gi, skin, OUT, -4);
}

/* ------------------------------------------------------------------ */
/* decor                                                               */
/* ------------------------------------------------------------------ */
void AppFight::paint_bg(void)
{
    struct Stop {
        int y;
        uint32_t c;
    };
    static const Stop stops[6] = {
        {  0, 0x1C0A38 }, { 60, 0x511264 }, { 110, 0x8E1F72 },
        { 160, 0xD94460 }, { 210, 0xFF8A3C }, { 250, 0xFFC85E },
    };

    /* ciel */
    for (int y = 0; y < 250; y++) {
        int i = 0;
        while (i < 4 && y > stops[i + 1].y) {
            i++;
        }
        const int y0 = stops[i].y;
        const int y1 = stops[i + 1].y;
        const int t = (y1 > y0) ? ((y - y0) * 256 / (y1 - y0)) : 256;
        const uint16_t c = to565(lerp_c(stops[i].c, stops[i + 1].c, t));
        uint16_t *row = _bg + (size_t)y * CW;
        for (int x = 0; x < CW; x++) {
            row[x] = c;
        }
    }

    /* soleil a rayures */
    for (int dy = -44; dy <= 44; dy++) {
        const int y = 148 + dy;
        if (y < 0 || y >= 250) {
            continue;
        }
        if (y > 152 && ((y - 152) % 9) > 4) {
            continue;
        }
        const int half = (int)sqrtf(44.0f * 44.0f - (float)(dy * dy));
        const uint16_t c = to565(y < 150 ? 0xFFE873 : 0xFFB33C);
        uint16_t *row = _bg + (size_t)y * CW;
        for (int x = 295 - half; x <= 295 + half; x++) {
            if (x >= 0 && x < CW) {
                row[x] = c;
            }
        }
    }

    /* immeubles et fenetres allumees */
    {
        uint32_t seed = 0x1F35C7u;
        int x = -12;
        while (x < CW) {
            seed = seed * 1664525u + 1013904223u;
            const int w = 16 + (int)((seed >> 16) % 30u);
            seed = seed * 1664525u + 1013904223u;
            const int h = 26 + (int)((seed >> 16) % 74u);
            const int top = 250 - h;
            for (int yy = top; yy < 250; yy++) {
                uint16_t *row = _bg + (size_t)yy * CW;
                for (int xx = x; xx < x + w; xx++) {
                    if (xx >= 0 && xx < CW) {
                        row[xx] = to565(0x2A1240);
                    }
                }
            }
            for (int wy = top + 6; wy < 246; wy += 9) {
                uint16_t *row = _bg + (size_t)wy * CW;
                for (int wx = x + 3; wx < x + w - 3; wx += 7) {
                    seed = seed * 1664525u + 1013904223u;
                    if (((seed >> 20) % 100u) < 42u && wx >= 0 && wx < CW) {
                        row[wx] = to565(0xFFD87A);
                    }
                }
            }
            seed = seed * 1664525u + 1013904223u;
            x += w + 3 + (int)((seed >> 16) % 8u);
        }
    }

    /* sol */
    for (int y = 250; y < CH; y++) {
        const int t = (y - 250) * 256 / (CH - 250);
        const uint16_t c = to565(lerp_c(0x1A0F2A, 0x2A1038, t));
        uint16_t *row = _bg + (size_t)y * CW;
        for (int x = 0; x < CW; x++) {
            row[x] = c;
        }
    }

    /* lueur d'horizon */
    for (int y = 246; y < 256; y++) {
        uint16_t *row = _bg + (size_t)y * CW;
        for (int x = 0; x < CW; x++) {
            row[x] = to565(0xFF6A3C);
        }
    }

    /* lignes de fuite */
    {
        const uint16_t line = to565(0x6A1C50);
        for (int k = -9; k <= 9; k++) {
            int x0 = 205 + k * 3;
            int x1 = 205 + k * 46;
            int y0 = 250, y1 = CH - 1;
            int dx = abs(x1 - x0);
            int sx = (x0 < x1) ? 1 : -1;
            int dy = -abs(y1 - y0);
            int sy = (y0 < y1) ? 1 : -1;
            int err = dx + dy;
            for (;;) {
                if (x0 >= 0 && x0 < CW && y0 >= 252 && y0 < CH) {
                    _bg[(size_t)y0 * CW + x0] = line;
                }
                if (x0 == x1 && y0 == y1) {
                    break;
                }
                const int e2 = 2 * err;
                if (e2 >= dy) {
                    err += dy;
                    x0 += sx;
                }
                if (e2 <= dx) {
                    err += dx;
                    y0 += sy;
                }
            }
        }
        for (int i = 1; i <= 9; i++) {
            const int y = 250 + i * i * 2;
            if (y < CH) {
                uint16_t *row = _bg + (size_t)y * CW;
                for (int x = 0; x < CW; x++) {
                    row[x] = line;
                }
            }
        }
    }

    /* foule : des tetes en bas */
    {
        uint32_t seed = 0x51A3u;
        for (int x = 2; x < CW; x += 15) {
            seed = seed * 1664525u + 1013904223u;
            const int cx = x + (int)((seed >> 18) % 5u);
            seed = seed * 1664525u + 1013904223u;
            const int cy = 378 + (int)((seed >> 18) % 12u);
            for (int dy = -6; dy <= 6; dy++) {
                for (int dx = -6; dx <= 6; dx++) {
                    if (dx * dx + dy * dy > 36) {
                        continue;
                    }
                    const int px = cx + dx;
                    const int py = cy + dy;
                    if (px < 0 || px >= CW || py < 0 || py >= CH) {
                        continue;
                    }
                    _bg[(size_t)py * CW + px] = to565(dy < -3 ? 0x3A1030 : 0x0E0718);
                }
            }
        }
    }

    /* ombres sous les pieds */
    for (int s = 0; s < 2; s++) {
        const int fx = (s == 0) ? (ART_OX0 + 44 * ART_AS) : (ART_OX1 + 21 * ART_AS);
        for (int dy = -7; dy <= 7; dy++) {
            const int half = (int)(sqrtf(1.0f - (float)(dy * dy) / 49.0f) * 36.0f);
            for (int dx = -half; dx <= half; dx++) {
                const int px = fx + dx;
                const int py = 316 + dy;
                if (px < 0 || px >= CW || py < 0 || py >= CH) {
                    continue;
                }
                _bg[(size_t)py * CW + px] = to565(0x120A1E);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* image                                                               */
/* ------------------------------------------------------------------ */
void AppFight::restore_band(void)
{
    for (int y = SCENE_Y0; y < SCENE_Y1; y++) {
        memcpy(_px + (size_t)y * CW + SCENE_X0,
               _bg + (size_t)y * CW + SCENE_X0,
               (size_t)(SCENE_X1 - SCENE_X0) * 2u);
    }
}

void AppFight::invalidate_band(void)
{
    lv_area_t a;
    a.x1 = SCENE_X0;
    a.y1 = SCENE_Y0 + CANVAS_Y;
    a.x2 = SCENE_X1 - 1;
    a.y2 = SCENE_Y1 - 1 + CANVAS_Y;
    lv_obj_invalidate_area(_canvas, &a);
}

static inline void setpx(uint16_t *px, int x, int y, uint16_t c)
{
    if (x < 0 || x >= CW || y < 0 || y >= CH) {
        return;
    }
    px[(size_t)y * CW + x] = c;
}

void AppFight::draw_ring(int cx, int cy, int r, uint16_t c)
{
    for (int a = 0; a < 720; a++) {
        const float t = (float)a * 3.14159265f / 360.0f;
        const int x = cx + (int)(cosf(t) * (float)r);
        const int y = cy + (int)(sinf(t) * (float)r);
        setpx(_px, x, y, c);
        setpx(_px, x + 1, y, c);
        setpx(_px, x, y + 1, c);
    }
}

void AppFight::draw_dot(void)
{
    if (_dot_frames <= 0) {
        return;
    }
    const int grow = 3 - _dot_frames;
    const int r = 7 + grow;
    draw_ring(_dot_x, _dot_y, r + 6, to565(0xFF3C8C));
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r * r) {
                setpx(_px, _dot_x + dx, _dot_y + dy, to565(0xFFFFFF));
            }
        }
    }
}

void AppFight::draw_orbs(void)
{
    if (!_busy || _frame < 3 || _frame > 5) {
        return;
    }
    const Atk &a = ATK[_side][_atk[_side]];
    if (a.kind != K_SPECIAL) {
        return;
    }
    const int sx = (_side == 0) ? (ART_OX0 + 60 * ART_AS) : (ART_OX1 + (ART_W - 1 - 60) * ART_AS);
    const int sy = ART_OY + 36 * ART_AS;
    const int t = _frame - 3;
    const int cx = sx + (_dot_x - sx) * t / 2;
    const int cy = sy + (_dot_y - sy) * t / 2;
    const int r = 5 + t;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            const int d2 = dx * dx + dy * dy;
            if (d2 > r * r) {
                continue;
            }
            setpx(_px, cx + dx, cy + dy, to565(d2 > (r - 2) * (r - 2) ? 0x4AC8FF : 0xE8FFFF));
        }
    }
}

void AppFight::draw_frame(void)
{
    restore_band();

    Pose p[2];
    p[0] = guard_pose();
    p[1] = guard_pose();

    int flash[2] = {_flash[0], _flash[1]};

    if (_state == 1 && _winner >= 0) {
        p[_winner] = winner_pose();
        p[1 - _winner] = loser_pose();
    } else if (_busy) {
        const int a = _side;
        const int v = 1 - _side;
        p[a] = attack_pose(a);
        if (_frame >= IMPACT_F) {
            Pose pv = guard_pose();
            pv.lean = -5;
            pv.head_dy = 3;
            pv.fnx = 33; pv.fny = 20;
            pv.ffx = 27; pv.ffy = 22;
            p[v] = pv;
        }
    }

    draw_fighter(0, p[0], flash[0]);
    draw_fighter(1, p[1], flash[1]);
    draw_orbs();
    draw_dot();

    invalidate_band();
    _redraws++;
}

/* ------------------------------------------------------------------ */
AppFight::Pose AppFight::attack_pose(int side)
{
    Pose p = guard_pose();
    const Atk &a = ATK[side][_atk[side]];
    int tx = 0, ty = 0;
    to_art(side, _dot_x, _dot_y, &tx, &ty);
    const int f = _frame;
    const int gnx = 43, gny = 26;    /* poing avant au repos */
    const int gfx = 37, gfy = 29;    /* poing arriere au repos */

    switch (a.kind) {
    case K_PUNCH_N: {
        const int cx = 34, cy = 40;
        if (f <= 2) {
            p.fnx = lerp_i(gnx, cx, f, 2);
            p.fny = lerp_i(gny, cy, f, 2);
            p.lean = -1;
        } else if (f <= 5) {
            const int t = f - 2;
            p.fnx = lerp_i(cx, tx, t, 3);
            p.fny = lerp_i(cy, ty, t, 3);
            p.lean = t + 1;
        } else if (f == 6) {
            p.fnx = tx;
            p.fny = ty;
            p.lean = 4;
        } else if (f <= 9) {
            const int t = f - 6;
            p.fnx = lerp_i(tx, gnx, t, 3);
            p.fny = lerp_i(ty, gny, t, 3);
            p.lean = 4 - t;
        }
        break;
    }
    case K_PUNCH_F: {
        const int cx = 30, cy = 50;
        if (f <= 2) {
            p.ffx = lerp_i(gfx, cx, f, 2);
            p.ffy = lerp_i(gfy, cy, f, 2);
            p.lean = -1;
        } else if (f <= 5) {
            const int t = f - 2;
            p.ffx = lerp_i(cx, tx, t, 3);
            p.ffy = lerp_i(cy, ty, t, 3);
            p.lean = t + 1;
        } else if (f == 6) {
            p.ffx = tx;
            p.ffy = ty;
            p.lean = 4;
        } else if (f <= 9) {
            const int t = f - 6;
            p.ffx = lerp_i(tx, gfx, t, 3);
            p.ffy = lerp_i(ty, gfy, t, 3);
            p.lean = 4 - t;
        }
        break;
    }
    case K_KICK: {
        const int cx = 42, cy = 68;
        if (f <= 2) {
            p.fkx = lerp_i(42, cx, f, 2);
            p.fky = lerp_i(85, cy, f, 2);
            p.lean = -2;
        } else if (f <= 5) {
            const int t = f - 2;
            p.fkx = lerp_i(cx, tx, t, 3);
            p.fky = lerp_i(cy, ty, t, 3);
            p.lean = -2 - t;
        } else if (f == 6) {
            p.fkx = tx;
            p.fky = ty;
            p.lean = -5;
        } else if (f <= 9) {
            const int t = f - 6;
            p.fkx = lerp_i(tx, 42, t, 3);
            p.fky = lerp_i(ty, 85, t, 3);
            p.lean = -5 + t;
        }
        break;
    }
    default: {  /* K_SPECIAL */
        if (f <= 2) {
            p.fnx = lerp_i(gnx, 36, f, 2);
            p.fny = lerp_i(gny, 50, f, 2);
            p.ffx = lerp_i(gfx, 31, f, 2);
            p.ffy = lerp_i(gfy, 52, f, 2);
            p.lean = -2;
        } else if (f <= 5) {
            const int t = f - 2;
            p.fnx = lerp_i(36, 60, t, 3);
            p.fny = lerp_i(50, 34, t, 3);
            p.ffx = 31;
            p.ffy = 50;
            p.lean = t + 2;
        } else if (f == 6) {
            p.fnx = 60;
            p.fny = 34;
            p.ffx = 31;
            p.ffy = 50;
            p.lean = 6;
        } else if (f <= 9) {
            const int t = f - 6;
            p.fnx = lerp_i(60, gnx, t, 3);
            p.fny = lerp_i(34, gny, t, 3);
            p.lean = 6 - t;
        }
        break;
    }
    }
    return p;
}

/* ------------------------------------------------------------------ */
/* logique                                                             */
/* ------------------------------------------------------------------ */
void AppFight::launch(int side)
{
    _side = side;
    _atk[side] = (int)(esp_random() % 5u);
    _busy = true;
    _frame = 0;
    _taps++;

    const int v = 1 - side;
    const int part = ATK[side][_atk[side]].part;
    const int ax = PART_AX[part];
    const int ay = PART_AY[part];
    _dot_x = (v == 0) ? (ART_OX0 + ax * ART_AS) : (ART_OX1 + (ART_W - 1 - ax) * ART_AS);
    _dot_y = ART_OY + ay * ART_AS;

    ESP_LOGI(TAG, "%s : %s (%d degats) sur %s",
             side == 0 ? "Ryo" : "Kai", ATK[side][_atk[side]].name,
             (int)ATK[side][_atk[side]].dmg, v == 0 ? "Ryo" : "Kai");
}

void AppFight::apply_hit(void)
{
    const int v = 1 - _side;
    const Atk &a = ATK[_side][_atk[_side]];
    const int var = (int)(esp_random() % (uint32_t)(2 * a.var + 1)) - a.var;
    int dmg = (int)a.dmg + var;
    if (dmg < 1) {
        dmg = 1;
    }
    _hp[v] -= dmg;
    if (_hp[v] < 0) {
        _hp[v] = 0;
    }
    _flash[v] = 2;
    _dot_frames = 3;
    update_bars();
}

void AppFight::end_turn(void)
{
    _used[_side]++;
    update_pips();
    update_round();
    if (_hp[0] <= 0 || _hp[1] <= 0) {
        decide();
        return;
    }
    if (_used[0] >= 10 && _used[1] >= 10) {
        decide();
    }
}

void AppFight::decide(void)
{
    _over = true;
    _state = 1;
    if (_hp[0] == _hp[1]) {
        _winner = (int)(esp_random() % 2u);
    } else {
        _winner = (_hp[0] > _hp[1]) ? 0 : 1;
    }
    const int wc = (_winner == 0) ? (ART_OX0 + ART_W * ART_AS / 2) : (ART_OX1 + ART_W * ART_AS / 2);
    lv_obj_set_pos(_plate_win, wc - 84, 96);
    lv_obj_clear_flag(_plate_win, LV_OBJ_FLAG_HIDDEN);
    if (_hp[0] <= 0 || _hp[1] <= 0) {
        lv_obj_clear_flag(_lbl_ko, LV_OBJ_FLAG_HIDDEN);
    }
    _over_t0 = esp_timer_get_time();
    _blink_t0 = _over_t0;
    draw_frame();
    ESP_LOGI(TAG, "fin du match : %d / %d -> gagnant %s (K.O. %s)",
             _hp[0], _hp[1], _winner == 0 ? "Ryo" : "Kai",
             (_hp[0] <= 0 || _hp[1] <= 0) ? "oui" : "non");
}

void AppFight::reset_match(void)
{
    _hp[0] = HP_MAX;
    _hp[1] = HP_MAX;
    _used[0] = 0;
    _used[1] = 0;
    _over = false;
    _busy = false;
    _frame = 0;
    _state = 0;
    _winner = -1;
    _flash[0] = 0;
    _flash[1] = 0;
    _dot_frames = 0;
    lv_obj_add_flag(_plate_win, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_lbl_ko, LV_OBJ_FLAG_HIDDEN);
    update_bars();
    update_pips();
    update_round();
    draw_frame();
    ESP_LOGI(TAG, "nouveau match");
}

/* ------------------------------------------------------------------ */
/* interface                                                           */
/* ------------------------------------------------------------------ */
void AppFight::update_bars(void)
{
    for (int s = 0; s < 2; s++) {
        int w = (BAR_W - 4) * _hp[s] / HP_MAX;
        if (w < 0) {
            w = 0;
        }
        lv_obj_set_width(_bar_fill[s], w);
        lv_obj_set_x(_bar_fill[s], (s == 0) ? 2 : (BAR_W - 2 - w));
        uint32_t c = (s == 0)
                     ? (_hp[s] > 45 ? 0xFF4A3C : 0xFF2A6A)
                     : (_hp[s] > 45 ? 0x3CA8FF : 0xFF2A6A);
        lv_obj_set_style_bg_color(_bar_fill[s], lv_color_hex(c), 0);
    }
}

void AppFight::update_pips(void)
{
    for (int s = 0; s < 2; s++) {
        for (int i = 0; i < 10; i++) {
            uint32_t c = (i < _used[s])
                         ? (s == 0 ? 0xFF4A3C : 0x3CA8FF)
                         : 0x241634;
            lv_obj_set_style_bg_color(_pip[s][i], lv_color_hex(c), 0);
        }
    }
}

void AppFight::update_round(void)
{
    int m = (_used[0] < _used[1]) ? _used[0] : _used[1];
    int r = m + 1;
    if (r > 10) {
        r = 10;
    }
    char buf[24];
    snprintf(buf, sizeof(buf), "ROUND %d/10", r);
    lv_label_set_text(_lbl_round, buf);
}

void AppFight::build(void)
{
    const int screen_h = (int)lv_obj_get_height(lv_scr_act());

    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x0B0614), 0);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);

    _root = lv_obj_create(lv_scr_act());
    lv_obj_set_size(_root, CW, screen_h);
    lv_obj_set_pos(_root, 0, 0);
    lv_obj_set_style_pad_all(_root, 0, 0);
    lv_obj_set_style_border_width(_root, 0, 0);
    lv_obj_set_style_radius(_root, 0, 0);
    lv_obj_set_style_bg_color(_root, lv_color_hex(0x0B0614), 0);
    lv_obj_set_style_bg_opa(_root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(_root, LV_OBJ_FLAG_SCROLLABLE);

    const size_t bytes = (size_t)CW * CH * 2u;
    _px = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    _bg = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (_px == nullptr || _bg == nullptr) {
        ESP_LOGE(TAG, "PSRAM insuffisante (%u o)", (unsigned)bytes);
        return;
    }
    paint_bg();
    memcpy(_px, _bg, bytes);

    _canvas = lv_canvas_create(_root);
    lv_canvas_set_buffer(_canvas, _px, CW, CH, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(_canvas, 0, CANVAS_Y);
    lv_obj_remove_flag(_canvas, LV_OBJ_FLAG_SCROLLABLE);
    /* Sur cette carte un objet ne recoit le doigt que s'il est CLICKABLE
       (voir Perplexe, piege 24) ; l'appui part des le toucher. */
    lv_obj_add_flag(_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_canvas, on_tap, LV_EVENT_PRESSED, this);
    lv_obj_add_flag(_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_root, on_tap, LV_EVENT_PRESSED, this);

    /* noms */
    _lbl_name[0] = lv_label_create(_root);
    lv_label_set_text(_lbl_name[0], "RYO");
    lv_obj_set_style_text_color(_lbl_name[0], lv_color_hex(0xFF7A5A), 0);
    lv_obj_set_style_text_font(_lbl_name[0], &lv_font_montserrat_14, 0);
    lv_obj_set_pos(_lbl_name[0], 12, 2);
    lv_obj_remove_flag(_lbl_name[0], LV_OBJ_FLAG_CLICKABLE);

    _lbl_name[1] = lv_label_create(_root);
    lv_label_set_text(_lbl_name[1], "KAI");
    lv_obj_set_style_text_color(_lbl_name[1], lv_color_hex(0x6AC8FF), 0);
    lv_obj_set_style_text_font(_lbl_name[1], &lv_font_montserrat_14, 0);
    lv_obj_set_pos(_lbl_name[1], 398 - 40, 2);
    lv_obj_remove_flag(_lbl_name[1], LV_OBJ_FLAG_CLICKABLE);

    _lbl_round = lv_label_create(_root);
    lv_label_set_text(_lbl_round, "ROUND 1/10");
    lv_obj_set_style_text_color(_lbl_round, lv_color_hex(0xFFD87A), 0);
    lv_obj_set_style_text_font(_lbl_round, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(_lbl_round, 205 - 46, 2);
    lv_obj_remove_flag(_lbl_round, LV_OBJ_FLAG_CLICKABLE);

    /* barres de vie */
    for (int s = 0; s < 2; s++) {
        const int x = (s == 0) ? 12 : (220);
        _bar_bg[s] = lv_obj_create(_root);
        lv_obj_set_size(_bar_bg[s], BAR_W, BAR_H);
        lv_obj_set_pos(_bar_bg[s], x, 22);
        lv_obj_set_style_pad_all(_bar_bg[s], 0, 0);
        lv_obj_set_style_radius(_bar_bg[s], 3, 0);
        lv_obj_set_style_bg_color(_bar_bg[s], lv_color_hex(0x1A1020), 0);
        lv_obj_set_style_bg_opa(_bar_bg[s], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(_bar_bg[s], 2, 0);
        lv_obj_set_style_border_color(_bar_bg[s], lv_color_hex(0x3A2050), 0);
        lv_obj_remove_flag(_bar_bg[s], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(_bar_bg[s], LV_OBJ_FLAG_CLICKABLE);

        _bar_fill[s] = lv_obj_create(_bar_bg[s]);
        lv_obj_set_size(_bar_fill[s], BAR_W - 4, BAR_H - 4);
        lv_obj_set_pos(_bar_fill[s], 2, 2);
        lv_obj_set_style_pad_all(_bar_fill[s], 0, 0);
        lv_obj_set_style_radius(_bar_fill[s], 2, 0);
        lv_obj_set_style_bg_color(_bar_fill[s], lv_color_hex(s == 0 ? 0xFF4A3C : 0x3CA8FF), 0);
        lv_obj_set_style_bg_opa(_bar_fill[s], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(_bar_fill[s], 0, 0);
        lv_obj_remove_flag(_bar_fill[s], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(_bar_fill[s], LV_OBJ_FLAG_CLICKABLE);
    }

    /* pastilles de rounds */
    for (int s = 0; s < 2; s++) {
        for (int i = 0; i < 10; i++) {
            const int x = (s == 0) ? (12 + i * 8) : (398 - 80 + i * 8);
            _pip[s][i] = lv_obj_create(_root);
            lv_obj_set_size(_pip[s][i], 6, 6);
            lv_obj_set_pos(_pip[s][i], x, 42);
            lv_obj_set_style_pad_all(_pip[s][i], 0, 0);
            lv_obj_set_style_radius(_pip[s][i], 1, 0);
            lv_obj_set_style_bg_color(_pip[s][i], lv_color_hex(0x241634), 0);
            lv_obj_set_style_bg_opa(_pip[s][i], LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(_pip[s][i], 0, 0);
            lv_obj_remove_flag(_pip[s][i], LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(_pip[s][i], LV_OBJ_FLAG_CLICKABLE);
        }
    }

    /* WINNER / K.O. */
    _plate_win = lv_obj_create(_root);
    lv_obj_set_size(_plate_win, 168, 40);
    lv_obj_set_pos(_plate_win, 120, 96);
    lv_obj_set_style_radius(_plate_win, 7, 0);
    lv_obj_set_style_bg_color(_plate_win, lv_color_hex(0x1A0A20), 0);
    lv_obj_set_style_bg_opa(_plate_win, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_plate_win, 2, 0);
    lv_obj_set_style_border_color(_plate_win, lv_color_hex(0xFFB03C), 0);
    lv_obj_set_style_pad_all(_plate_win, 0, 0);
    lv_obj_remove_flag(_plate_win, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(_plate_win, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(_plate_win, LV_OBJ_FLAG_HIDDEN);

    _lbl_win = lv_label_create(_plate_win);
    lv_label_set_text(_lbl_win, "WINNER");
    lv_obj_set_style_text_align(_lbl_win, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(_lbl_win, lv_color_hex(0xFFF05A), 0);
    lv_obj_set_style_text_font(_lbl_win, &lv_font_montserrat_28, 0);
    lv_obj_center(_lbl_win);
    lv_obj_remove_flag(_lbl_win, LV_OBJ_FLAG_CLICKABLE);

    _lbl_ko = lv_label_create(_root);
    lv_label_set_text(_lbl_ko, "K.O.");
    lv_obj_set_style_text_color(_lbl_ko, lv_color_hex(0xFF3030), 0);
    lv_obj_set_style_text_font(_lbl_ko, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_outline_stroke_width(_lbl_ko, 3, 0);
    lv_obj_set_style_text_outline_stroke_color(_lbl_ko, lv_color_hex(0x1A0010), 0);
    lv_obj_set_pos(_lbl_ko, 205 - 26, 108);
    lv_obj_remove_flag(_lbl_ko, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(_lbl_ko, LV_OBJ_FLAG_HIDDEN);

    /* croix de fermeture */
    _btn_quit = lv_obj_create(_root);
    lv_obj_set_size(_btn_quit, 26, 26);
    lv_obj_set_pos(_btn_quit, CW - 34, screen_h - 34);
    lv_obj_set_style_radius(_btn_quit, 13, 0);
    lv_obj_set_style_bg_color(_btn_quit, lv_color_hex(0x2A1038), 0);
    lv_obj_set_style_bg_opa(_btn_quit, LV_OPA_70, 0);
    lv_obj_set_style_border_width(_btn_quit, 1, 0);
    lv_obj_set_style_border_color(_btn_quit, lv_color_hex(0xFF6A9A), 0);
    lv_obj_set_style_pad_all(_btn_quit, 0, 0);
    lv_obj_remove_flag(_btn_quit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(_btn_quit, on_quit, LV_EVENT_CLICKED, this);

    lv_obj_t *x = lv_label_create(_btn_quit);
    lv_label_set_text(x, "x");
    lv_obj_set_style_text_color(x, lv_color_hex(0xFFD0E0), 0);
    lv_obj_set_style_text_font(x, &lv_font_montserrat_14, 0);
    lv_obj_center(x);
    lv_obj_remove_flag(x, LV_OBJ_FLAG_CLICKABLE);

    _built = true;
    reset_match();
}

/* ------------------------------------------------------------------ */
/* boucle                                                              */
/* ------------------------------------------------------------------ */
void AppFight::tick(void)
{
    if (_state == 1) {
        const int64_t now = esp_timer_get_time();
        if (now - _blink_t0 > 280000) {
            _blink_t0 = now;
            _blink_on = !_blink_on;
            lv_obj_set_style_text_color(_lbl_win,
                                        lv_color_hex(_blink_on ? 0xFFF05A : 0xFF5A9A), 0);
        }
        if (now - _over_t0 > 4200000) {
            reset_match();
        }
        return;
    }

    if (!_busy) {
        return;
    }

    _frame++;
    if (_frame == IMPACT_F) {
        apply_hit();
    }
    draw_frame();

    if (_flash[0] > 0) {
        _flash[0]--;
    }
    if (_flash[1] > 0) {
        _flash[1]--;
    }
    if (_dot_frames > 0) {
        _dot_frames--;
    }

    if (_frame >= FRAMES - 1) {
        _busy = false;
        _dot_frames = 0;
        _flash[0] = 0;
        _flash[1] = 0;
        end_turn();
        draw_frame();
    }
}

void AppFight::tap_at(int x, int y)
{
    if (_over || _busy) {
        return;
    }
    if (y < CANVAS_Y || x < 0 || x >= CW) {
        ESP_LOGI(TAG, "appui hors jeu (%d,%d)", x, y);
        return;
    }
    const int side = (x < CW / 2) ? 0 : 1;
    ESP_LOGI(TAG, "doigt (%d,%d) -> %s", x, y, side == 0 ? "Ryo" : "Kai");
    if (_used[side] >= 10) {
        ESP_LOGI(TAG, "%s a epuise ses dix attaques", side == 0 ? "Ryo" : "Kai");
        return;
    }
    launch(side);
    draw_frame();
}

void AppFight::on_tick(lv_timer_t *t)
{
    AppFight *self = static_cast<AppFight *>(t->user_data);
    if (self != nullptr && !self->_closing) {
        self->tick();
    }
}

void AppFight::on_tap(lv_event_t *e)
{
    AppFight *self = static_cast<AppFight *>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    if (indev == nullptr) {
        indev = lv_indev_get_next(nullptr);
    }
    if (indev == nullptr) {
        ESP_LOGW(TAG, "appui sans peripherique d'entree");
        return;
    }
    lv_point_t p = {0, 0};
    lv_indev_get_point(indev, &p);
    self->tap_at((int)p.x, (int)p.y);
}

void AppFight::on_quit(lv_event_t *e)
{
    AppFight *self = static_cast<AppFight *>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    ESP_LOGI(TAG, "croix : fermeture");
    lv_timer_create(on_quit_later, 40, self);
}

void AppFight::on_quit_later(lv_timer_t *t)
{
    AppFight *self = static_cast<AppFight *>(t->user_data);
    lv_timer_del(t);
    if (self != nullptr) {
        self->back();
    }
}

/* ------------------------------------------------------------------ */
bool AppFight::run(void)
{
    _closing = false;
    if (!_built) {
        build();
    }
    if (_timer == nullptr) {
        _timer = lv_timer_create(on_tick, 45, this);
    }
    ESP_LOGI(TAG, "run OK : ecran %dx%d", (int)lv_obj_get_width(lv_scr_act()),
             (int)lv_obj_get_height(lv_scr_act()));
    return true;
}

bool AppFight::back(void)
{
    if (_closing) {
        return true;
    }
    _closing = true;

    if (_timer) {
        lv_timer_del(_timer);
        _timer = nullptr;
    }
    if (_root) {
        lv_obj_del(_root);
        _root = nullptr;
    }
    _canvas = nullptr;
    _lbl_round = nullptr;
    _plate_win = nullptr;
    _lbl_win = nullptr;
    _lbl_ko = nullptr;
    _btn_quit = nullptr;
    for (int s = 0; s < 2; s++) {
        _lbl_name[s] = nullptr;
        _bar_bg[s] = nullptr;
        _bar_fill[s] = nullptr;
        for (int i = 0; i < 10; i++) {
            _pip[s][i] = nullptr;
        }
    }

    if (_px) {
        free(_px);
        _px = nullptr;
    }
    if (_bg) {
        free(_bg);
        _bg = nullptr;
    }
    _built = false;

    ESP_LOGI(TAG, "back OK : %u appuis, %u images", (unsigned)_taps, (unsigned)_redraws);
    ESP_BROOKESIA_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "notifyCoreClosed a echoue");
    return true;
}
