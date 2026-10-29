/*
 * AppWar - « Guerre » : carte du monde pixel art, missiles sans fin.
 * Cinquieme application, page 5 du lanceur.
 *
 * Le principe : deux continents se tirent dessus, l'un apres l'autre, jusqu'a
 * ce qu'on ferme l'application. Les crateres restent sur la carte : plus l'app
 * tourne, plus le monde est abime.
 *
 * Choix techniques (tous payes par des essais) :
 *  - le canevas ne couvre QUE la bande de la carte (410 x 198). Il est
 *    revalide en entier a chaque image : sur cette carte, invalider une
 *    partie d'un canevas fait apparaitre une ligne corrompue en haut de la
 *    zone repeinte (verifie a l'ecran, quel que soit l'ancrage du canevas).
 *    Une bande de 81 000 pixels tient ~28 images/s ; l'ecran entier, non.
 *  - les jauges de degats sont des objets LVGL ordinaires : elles changent
 *    rarement et ne coutent rien.
 *  - les tampons du canevas sont en PSRAM (2 x 162 Ko).
 *  - pas de son (l'audio a deja degrade l'affichage en v4.1, piege 25), pas
 *    d'ecriture en nvs, pas d'I2C.
 *  - la barre d'etat orange est conservee : rien n'est dessine au-dessus de
 *    y=50, la carte commence a y=56.
 */
#include <cmath>
#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"

#include "app_war.hpp"
#include "icons/app_icons.h"
#include "private/esp_brookesia_utils.h"
#include "worldmap_war.h"

static const char *TAG = "GUERRE";

/* ------------------------------------------------------------------ */
/* Geometrie                                                           */
/* ------------------------------------------------------------------ */
#define CW          410              /* largeur de la carte            */
#define CH          WAR_MAP_H        /* hauteur du canevas (198)       */
#define MAP_SCREEN_Y 56              /* position du canevas a l'ecran  */

#define BAR_N       6
#define BAR_W       58
#define BAR_GAP     8
#define BAR_X0      10
#define BAR_MAXH    126
#define BAR_Y0      (MAP_SCREEN_Y + WAR_MAP_H + 14)   /* 268 */
#define BAR_BASE    (BAR_Y0 + BAR_MAXH)               /* 394 */

#define QUIT_X      (410 - 38)
#define QUIT_Y      (BAR_BASE + 16)                   /* 410 */
#define QUIT_S      28

/* ------------------------------------------------------------------ */
/* Couleurs                                                            */
/* ------------------------------------------------------------------ */
#define COL_OCEAN   0x0A1826
#define COL_TROUGH  0x11222D
#define COL_TROUGHT 0x244C60
#define COL_QUIT_BG 0x14242F
#define COL_QUIT_BD 0x2E4E60
#define COL_QUIT_X  0xDDEBF2
#define COL_FLASH   0xFFFFFF
#define COL_FIRE1   0xFFE066
#define COL_FIRE2   0xFF8A3C
#define COL_FIRE3   0xE0452A
#define COL_CRATER  0x2E1712

#ifndef WAR_END_PCT
#define WAR_END_PCT 90               /* % du monde detruit -> « THE END » */
#endif
#ifndef WAR_END_MS
#define WAR_END_MS  3000             /* duree du carton */
#endif

#define MAX_SHOTS   12
#define MAX_BOOMS   12
#define BOOM_MS     430
#define SHOT_MS_MIN 950

static int rnd_upto(int n)
{
    static uint32_t s = 0x9E3779B9u;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return (int)(s % (uint32_t)n);
}

static inline uint16_t to565(uint32_t c)
{
    uint32_t r = (c >> 16) & 0xFF;
    uint32_t g = (c >> 8) & 0xFF;
    uint32_t b = c & 0xFF;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static uint32_t shade(uint32_t c, float f)
{
    int r = (int)(((c >> 16) & 0xFF) * f);
    int g = (int)(((c >> 8) & 0xFF) * f);
    int b = (int)((c & 0xFF) * f);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static inline float clampf(float v, float a, float b)
{
    return v < a ? a : (v > b ? b : v);
}

/* ------------------------------------------------------------------ */
/* Construction                                                        */
/* ------------------------------------------------------------------ */
AppWar::AppWar() :
    ESP_Brookesia_PhoneApp("Guerre", &app_icon_guerre, true, true, true)
{
}

AppWar::~AppWar()
{
    if (_timer) {
        lv_timer_del(_timer);
        _timer = nullptr;
    }
}

void AppWar::put(int x, int y, uint16_t c)
{
    if (x < 0 || y < 0 || x >= CW || y >= CH) {
        return;
    }
    _px[y * CW + x] = c;
}

void AppWar::dirty(int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > CW) x1 = CW;
    if (y1 > CH) y1 = CH;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    if (!_has_dirty) {
        _dx0 = x0; _dy0 = y0; _dx1 = x1; _dy1 = y1;
        _has_dirty = true;
        return;
    }
    if (x0 < _dx0) _dx0 = x0;
    if (y0 < _dy0) _dy0 = y0;
    if (x1 > _dx1) _dx1 = x1;
    if (y1 > _dy1) _dy1 = y1;
}

void AppWar::restore(int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > CW) x1 = CW;
    if (y1 > CH) y1 = CH;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    for (int y = y0; y < y1; y++) {
        memcpy(_px + y * CW + x0, _bg + y * CW + x0, (size_t)(x1 - x0) * 2u);
    }
}

void AppWar::paint_ocean(void)
{
    uint16_t ocean = to565(COL_OCEAN);
    for (int i = 0; i < CW * CH; i++) {
        _bg[i] = ocean;
    }
}

void AppWar::paint_map(void)
{
    for (int r = 0; r < WAR_MAP_ROWS; r++) {
        for (int c = 0; c < WAR_MAP_COLS; c++) {
            int code = WAR_AT(r, c);
            if (code == 0) {
                continue;
            }
            uint32_t base = war_palette[code];
            bool coast = false;
            static const int dr[4] = {-1, 1, 0, 0};
            static const int dc[4] = {0, 0, -1, 1};
            for (int k = 0; k < 4; k++) {
                int rr = r + dr[k], cc = c + dc[k];
                if (rr < 0 || cc < 0 || rr >= WAR_MAP_ROWS || cc >= WAR_MAP_COLS ||
                        WAR_AT(rr, cc) != code) {
                    coast = true;
                    break;
                }
            }
            uint32_t col;
            if (coast) {
                col = shade(base, 1.22f);
            } else if (((r * 3 + c * 5) % 7) == 0) {
                col = shade(base, 0.90f);
            } else {
                col = base;
            }
            uint16_t c565 = to565(col);
            for (int y = 0; y < WAR_MAP_CY; y++) {
                uint16_t *row = _bg + (r * WAR_MAP_CY + y) * CW + c * WAR_MAP_CX;
                for (int x = 0; x < WAR_MAP_CX; x++) {
                    row[x] = c565;
                }
            }
        }
    }
}

void AppWar::paint_background(void)
{
    paint_ocean();
    paint_map();
    memcpy(_px, _bg, (size_t)CW * CH * 2u);
}

/* ------------------------------------------------------------------ */
/* Index des cases                                                     */
/* ------------------------------------------------------------------ */
void AppWar::free_cells(void)
{
    for (int i = 0; i < 7; i++) {
        if (_cells[i]) {
            free(_cells[i]);
            _cells[i] = nullptr;
        }
        if (_coast[i]) {
            free(_coast[i]);
            _coast[i] = nullptr;
        }
        _ncells[i] = 0;
        _ncoast[i] = 0;
    }
}

void AppWar::index_cells(void)
{
    for (int i = 0; i < 7; i++) {
        _ncells[i] = 0;
        _ncoast[i] = 0;
    }
    for (int r = 0; r < WAR_MAP_ROWS; r++) {
        for (int c = 0; c < WAR_MAP_COLS; c++) {
            int code = WAR_AT(r, c);
            if (code == 0) {
                continue;
            }
            _ncells[code]++;
            static const int dr[4] = {-1, 1, 0, 0};
            static const int dc[4] = {0, 0, -1, 1};
            for (int k = 0; k < 4; k++) {
                int rr = r + dr[k], cc = c + dc[k];
                if (rr < 0 || cc < 0 || rr >= WAR_MAP_ROWS || cc >= WAR_MAP_COLS ||
                        WAR_AT(rr, cc) != code) {
                    _ncoast[code]++;
                    break;
                }
            }
        }
    }
    for (int i = 1; i < 7; i++) {
        if (_ncells[i] > 0) {
            _cells[i] = (uint16_t *)malloc(sizeof(uint16_t) * (size_t)_ncells[i]);
        }
        if (_ncoast[i] > 0) {
            _coast[i] = (uint16_t *)malloc(sizeof(uint16_t) * (size_t)_ncoast[i]);
        }
        _ncells[i] = 0;
        _ncoast[i] = 0;
    }
    for (int r = 0; r < WAR_MAP_ROWS; r++) {
        for (int c = 0; c < WAR_MAP_COLS; c++) {
            int code = WAR_AT(r, c);
            if (code == 0) {
                continue;
            }
            uint16_t idx = (uint16_t)(r * WAR_MAP_COLS + c);
            _cells[code][_ncells[code]++] = idx;
            static const int dr[4] = {-1, 1, 0, 0};
            static const int dc[4] = {0, 0, -1, 1};
            for (int k = 0; k < 4; k++) {
                int rr = r + dr[k], cc = c + dc[k];
                if (rr < 0 || cc < 0 || rr >= WAR_MAP_ROWS || cc >= WAR_MAP_COLS ||
                        WAR_AT(rr, cc) != code) {
                    _coast[code][_ncoast[code]++] = idx;
                    break;
                }
            }
        }
    }
    _land_total = 0;
    for (int i = 1; i < 7; i++) {
        _land_total += _ncells[i];
        ESP_LOGI(TAG, "continent %d : %d cases, %d cotes", i, _ncells[i], _ncoast[i]);
    }
    ESP_LOGI(TAG, "monde : %d cases de terre (90 %% = %d)", _land_total,
             (_land_total * WAR_END_PCT) / 100);
}

void AppWar::target_point(int cont, float *x, float *y)
{
    if (cont < 1 || cont > 6 || _ncells[cont] <= 0) {
        *x = CW * 0.5f;
        *y = CH * 0.5f;
        return;
    }
    uint16_t idx = _cells[cont][rnd_upto(_ncells[cont])];
    int r = idx / WAR_MAP_COLS;
    int c = idx % WAR_MAP_COLS;
    *x = (float)(c * WAR_MAP_CX + WAR_MAP_CX / 2 + rnd_upto(5) - 2);
    *y = (float)(r * WAR_MAP_CY + WAR_MAP_CY / 2 + rnd_upto(5) - 2);
}

void AppWar::launch_point(int cont, float tx, float ty, float *x, float *y)
{
    if (cont < 1 || cont > 6 || _ncoast[cont] <= 0) {
        *x = CW * 0.5f;
        *y = 20.0f;
        return;
    }
    /* on tire depuis la cote : la meilleure de quelques candidates, pour que
       le tir parte du bon cote du continent sans etre toujours identique */
    float best = 1e9f;
    int best_r = 0, best_c = 0;
    int tries = _ncoast[cont] < 14 ? _ncoast[cont] : 14;
    for (int k = 0; k < tries; k++) {
        uint16_t idx = _coast[cont][rnd_upto(_ncoast[cont])];
        int r = idx / WAR_MAP_COLS;
        int c = idx % WAR_MAP_COLS;
        float px = (float)(c * WAR_MAP_CX + WAR_MAP_CX / 2);
        float py = (float)(r * WAR_MAP_CY + WAR_MAP_CY / 2);
        float d = (px - tx) * (px - tx) + (py - ty) * (py - ty);
        if (d < best) {
            best = d;
            best_r = r;
            best_c = c;
        }
    }
    *x = (float)(best_c * WAR_MAP_CX + WAR_MAP_CX / 2);
    *y = (float)(best_r * WAR_MAP_CY + WAR_MAP_CY / 2);
}

/* ------------------------------------------------------------------ */
/* Guerre                                                              */
/* ------------------------------------------------------------------ */
void AppWar::reset_war(void)
{
    memset(_shots, 0, sizeof(_shots));
    memset(_booms, 0, sizeof(_booms));
    for (int i = 0; i < 7; i++) {
        _damage[i] = 0.0f;
    }
    _prev_a = -1;
    _prev_b = -1;
    _salvo = 0;
    _strikes = 0;
    _step = ST_INTRO;
    _step_t0 = esp_timer_get_time();
}

void AppWar::pick_pair(void)
{
    for (int guard = 0; guard < 64; guard++) {
        int a = rnd_upto(6) + 1;
        int b = rnd_upto(6) + 1;
        if (a == b) {
            continue;
        }
        if (_ncells[a] == 0 || _ncells[b] == 0) {
            continue;
        }
        bool same = ((a == _prev_a && b == _prev_b) || (a == _prev_b && b == _prev_a));
        if (same) {
            continue;
        }
        _a = a;
        _b = b;
        _prev_a = a;
        _prev_b = b;
        return;
    }
    _a = 1;
    _b = 5;
    _prev_a = _a;
    _prev_b = _b;
}

void AppWar::launch_one(int from, int to)
{
    int slot = -1;
    for (int i = 0; i < MAX_SHOTS; i++) {
        if (!_shots[i].on) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return;
    }
    float tx, ty, sx, sy;
    target_point(to, &tx, &ty);
    launch_point(from, tx, ty, &sx, &sy);

    Shot *s = &_shots[slot];
    s->on = true;
    s->done = false;
    s->from = from;
    s->to = to;
    s->x0 = sx;
    s->y0 = sy;
    s->x1 = tx;
    s->y1 = ty;

    float dx = tx - sx;
    float dy = ty - sy;
    float d = sqrtf(dx * dx + dy * dy);
    if (d < 1.0f) {
        d = 1.0f;
    }
    /* point de controle : au-dessus du segment, un peu aleatoire, pour un arc
       de tir balistique et non une ligne droite */
    float mx = (sx + tx) * 0.5f;
    float my = (sy + ty) * 0.5f;
    float nx = -dy / d;
    float ny = dx / d;
    float lift = d * (0.22f + 0.12f * (float)rnd_upto(100) / 100.0f);
    if (ny > 0.0f) {
        nx = -nx;
        ny = -ny;
    }
    s->cx = mx + nx * lift;
    s->cy = my + ny * lift - 12.0f;

    s->t0 = esp_timer_get_time();
    s->dur = (SHOT_MS_MIN + (int)(d * 0.9f) + rnd_upto(260)) * 1000;

    _strikes++;
}

void AppWar::boom_at(int x, int y, int cont)
{
    for (int i = 0; i < MAX_BOOMS; i++) {
        if (!_booms[i].on) {
            _booms[i].on = true;
            _booms[i].x = x;
            _booms[i].y = y;
            _booms[i].cont = cont;
            _booms[i].t0 = esp_timer_get_time();
            return;
        }
    }
}

void AppWar::crater(int x, int y)
{
    /* le cratere est ecrit dans le FOND : il reste tant que l'app tourne */
    for (int dy = -6; dy <= 6; dy++) {
        for (int dx = -6; dx <= 6; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 > 36) {
                continue;
            }
            int px = x + dx;
            int py = y + dy;
            if (px < 0 || py < 0 || px >= CW || py >= CH) {
                continue;
            }
            int c = px / WAR_MAP_CX;
            int r = py / WAR_MAP_CY;
            if (r < 0 || r >= WAR_MAP_ROWS || c < 0 || c >= WAR_MAP_COLS) {
                continue;
            }
            if (WAR_AT(r, c) == 0) {
                continue;
            }
            if (!_dead[r][c]) {
                _dead[r][c] = 1;
                _dead_count++;
            }
            uint32_t col = d2 < 10 ? 0x1A0C08 : COL_CRATER;
            _bg[py * CW + px] = to565(col);
        }
    }
    dirty(x - 7, y - 7, x + 7, y + 7);
}

/* ------------------------------------------------------------------ */
/* Sprites                                                             */
/* ------------------------------------------------------------------ */
void AppWar::draw_shots(void)
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < MAX_SHOTS; i++) {
        Shot *s = &_shots[i];
        if (!s->on) {
            continue;
        }
        float t = (float)(now - s->t0) / (float)s->dur;
        if (t < 0.0f) {
            t = 0.0f;
        }
        if (t > 1.0f) {
            t = 1.0f;
        }
        uint32_t col = war_palette[s->from];
        for (int k = 9; k >= 1; k--) {
            float tt = t - 0.018f * (float)k;
            if (tt < 0.0f) {
                continue;
            }
            float u = 1.0f - tt;
            int tx = (int)(u * u * s->x0 + 2.0f * u * tt * s->cx + tt * tt * s->x1);
            int ty = (int)(u * u * s->y0 + 2.0f * u * tt * s->cy + tt * tt * s->y1);
            uint16_t tc = to565(shade(col, 0.30f + 0.070f * (float)(9 - k)));
            put(tx, ty, tc);
            put(tx + 1, ty, tc);
            put(tx, ty + 1, tc);
            put(tx + 1, ty + 1, tc);
        }
        float u = 1.0f - t;
        int hx = (int)(u * u * s->x0 + 2.0f * u * t * s->cx + t * t * s->x1);
        int hy = (int)(u * u * s->y0 + 2.0f * u * t * s->cy + t * t * s->y1);
        uint16_t halo = to565(shade(col, 1.45f));
        uint16_t core = to565(COL_FLASH);
        for (int dy = -3; dy <= 3; dy++) {
            for (int dx = -3; dx <= 3; dx++) {
                if (dx * dx + dy * dy <= 10) {
                    put(hx + dx, hy + dy, halo);
                }
            }
        }
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                put(hx + dx, hy + dy, core);
            }
        }
    }
}

void AppWar::draw_booms(void)
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < MAX_BOOMS; i++) {
        Boom *b = &_booms[i];
        if (!b->on) {
            continue;
        }
        float t = (float)(now - b->t0) / (float)(BOOM_MS * 1000);
        if (t < 0.0f) {
            t = 0.0f;
        }
        if (t > 1.0f) {
            t = 1.0f;
        }
        uint32_t col;
        if (t < 0.22f) {
            col = COL_FLASH;
        } else if (t < 0.5f) {
            col = COL_FIRE1;
        } else if (t < 0.78f) {
            col = COL_FIRE2;
        } else {
            col = COL_FIRE3;
        }
        float rad = 3.0f + 30.0f * t;
        uint16_t c = to565(col);
        for (int k = 0; k < 44; k++) {
            float a = (float)k * 6.2831853f / 44.0f;
            for (int e = 0; e < 3; e++) {
                float rr = rad - (float)e;
                put(b->x + (int)(cosf(a) * rr), b->y + (int)(sinf(a) * rr), c);
            }
        }
        if (t < 0.40f) {
            int core = (int)(10.0f * (1.0f - t / 0.40f));
            uint16_t white = to565(COL_FLASH);
            for (int dy = -core; dy <= core; dy++) {
                for (int dx = -core; dx <= core; dx++) {
                    if (dx * dx + dy * dy <= core * core) {
                        put(b->x + dx, b->y + dy, white);
                    }
                }
            }
        }
        for (int k = 0; k < 8; k++) {
            float a = (float)((k * 47 + i * 17) % 360) * 6.2831853f / 360.0f;
            float rr = rad * 1.30f;
            int sx = b->x + (int)(cosf(a) * rr);
            int sy = b->y + (int)(sinf(a) * rr);
            put(sx, sy, c);
            put(sx + 1, sy, c);
        }
        if ((now - b->t0) > BOOM_MS * 1000) {
            b->on = false;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Boucle                                                              */
/* ------------------------------------------------------------------ */
void AppWar::update_bars(void)
{
    for (int i = 1; i < 7; i++) {
        if (_damage[i] > 0.0f) {
            _damage[i] = clampf(_damage[i] - 0.015f * _dt, 0.0f, 1.0f);
        }
        int h = (int)(_damage[i] * (float)BAR_MAXH);
        if (h != _bar_h[i] && _fill[i - 1] != nullptr) {
            _bar_h[i] = h;
            if (h <= 0) {
                lv_obj_add_flag(_fill[i - 1], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_remove_flag(_fill[i - 1], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(_fill[i - 1], 1, BAR_MAXH - h - 1);
                lv_obj_set_size(_fill[i - 1], BAR_W - 2, h);
            }
        }
    }
}

void AppWar::end_trigger(int64_t now)
{
    _ending = true;
    _end_t0 = now;
    if (_lbl_end != nullptr) {
        lv_obj_remove_flag(_lbl_end, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(_lbl_end);
    }
    ESP_LOGW(TAG, "THE END : %d cases detruites sur %d", _dead_count, _land_total);
}

void AppWar::end_world(void)
{
    if (_lbl_end != nullptr) {
        lv_obj_add_flag(_lbl_end, LV_OBJ_FLAG_HIDDEN);
    }
    memset(_dead, 0, sizeof(_dead));
    _dead_count = 0;
    for (int i = 0; i < 7; i++) {
        _damage[i] = 0.0f;
        _bar_h[i] = -1;
    }
    paint_background();
    lv_obj_invalidate(_canvas);
    _prev_a = -1;
    _prev_b = -1;
    _salvo = 0;
    _ending = false;
    _step = ST_INTRO;
    _step_t0 = esp_timer_get_time();
    ESP_LOGI(TAG, "monde reinitialise : carte neuve, jauges a zero");
}

void AppWar::tick(void)
{
    int64_t now = esp_timer_get_time();
    float dt = (float)(now - _last_frame) / 1000000.0f;
    if (dt < 0.0f || dt > 0.25f) {
        dt = 0.033f;
    }
    _last_frame = now;
    _dt = dt;

    /* Pendant « THE END » on ne repeint plus rien : la carte reste figee et
       le carton ne coute aucune image (le redessiner a chaque image faisait
       tomber le rythme a ~12 i/s). */
    if (_ending) {
        if (now - _end_t0 > (int64_t)WAR_END_MS * 1000) {
            end_world();
        }
        return;
    }

    /* ------------------------------------------------ sprites a effacer */
    for (int i = 0; i < MAX_SHOTS; i++) {
        Shot *s = &_shots[i];
        if (!s->on) {
            continue;
        }
        float t = (float)(now - s->t0) / (float)s->dur;
        if (t >= 1.0f && !s->done) {
            s->done = true;
            s->on = false;
            boom_at((int)s->x1, (int)s->y1, s->to);
            int dmg = 5 + rnd_upto(5);
            _damage[s->to] = clampf(_damage[s->to] + 0.020f * (float)dmg, 0.0f, 1.0f);
            crater((int)s->x1, (int)s->y1);
            continue;
        }
        if (t < 0.0f) {
            t = 0.0f;
        }
        if (t > 1.0f) {
            t = 1.0f;
        }
        float u = 1.0f - t;
        int hx = (int)(u * u * s->x0 + 2.0f * u * t * s->cx + t * t * s->x1);
        int hy = (int)(u * u * s->y0 + 2.0f * u * t * s->cy + t * t * s->y1);
        dirty(hx - 14, hy - 14, hx + 15, hy + 15);
    }
    for (int i = 0; i < MAX_BOOMS; i++) {
        Boom *b = &_booms[i];
        if (!b->on) {
            continue;
        }
        dirty(b->x - 46, b->y - 46, b->x + 47, b->y + 47);
    }

    update_bars();

    if (_has_dirty) {
        restore(_dx0, _dy0, _dx1, _dy1);
    }

    /* ------------------------------------------------ chef d'orchestre */
    switch (_ending ? ST_PAUSED : _step) {
    case ST_PAUSED:
        if (now - _end_t0 > (int64_t)WAR_END_MS * 1000) {
            end_world();
        }
        break;
    case ST_INTRO:
        if (now - _step_t0 > 900000) {
            pick_pair();
            _salvo = 0;
            _next_launch = now;
            _step = ST_FIRE_A;
            ESP_LOGI(TAG, "guerre : %d attaque %d", _a, _b);
        }
        break;
    case ST_FIRE_A:
        if (_salvo < 3 && now >= _next_launch) {
            launch_one(_a, _b);
            _salvo++;
            _next_launch = now + 210000;
        }
        if (_salvo >= 3) {
            _step = ST_WAIT_A;
        }
        break;
    case ST_WAIT_A: {
        bool busy = false;
        for (int i = 0; i < MAX_SHOTS; i++) {
            if (_shots[i].on) {
                busy = true;
                break;
            }
        }
        if (!busy) {
            _salvo = 0;
            _next_launch = now + 450000;
            _step = ST_FIRE_B;
            ESP_LOGI(TAG, "riposte : %d attaque %d", _b, _a);
        }
        break;
    }
    case ST_FIRE_B:
        if (_salvo < 3 && now >= _next_launch) {
            launch_one(_b, _a);
            _salvo++;
            _next_launch = now + 210000;
        }
        if (_salvo >= 3) {
            _step = ST_WAIT_B;
        }
        break;
    case ST_WAIT_B: {
        bool busy = false;
        for (int i = 0; i < MAX_SHOTS; i++) {
            if (_shots[i].on) {
                busy = true;
                break;
            }
        }
        if (!busy) {
            _step = ST_GAP;
            _step_t0 = now;
        }
        break;
    }
    case ST_GAP:
        if (now - _step_t0 > 1300000) {
            pick_pair();
            _salvo = 0;
            _next_launch = now;
            _step = ST_FIRE_A;
            ESP_LOGI(TAG, "guerre : %d attaque %d", _a, _b);
        }
        break;
    default:
        _step = ST_INTRO;
        _step_t0 = now;
        break;
    }

    if (!_ending && _land_total > 0 && _dead_count * 100 >= _land_total * WAR_END_PCT) {
        end_trigger(now);
    }

    /* ------------------------------------------------ dessin */
    draw_shots();
    draw_booms();

    /* On revalide TOUTE la bande : sur cette carte, invalider une partie d'un
       canevas laisse une ligne corrompue en haut de la zone repeinte. */
    lv_obj_invalidate(_canvas);
    _has_dirty = false;

    _frames++;
    uint32_t ms = (uint32_t)(now / 1000);
    if (ms - _fps_t0 >= 5000) {
        ESP_LOGI(TAG, "%u images/s, %u tirs, salle %d", _frames * 1000 / (ms - _fps_t0), _strikes, _step);
        _frames = 0;
        _fps_t0 = ms;
    }
}

void AppWar::on_tick(lv_timer_t *t)
{
    AppWar *self = static_cast<AppWar *>(t->user_data);
    if (self != nullptr && !self->_closing) {
        self->tick();
    }
}

/* ------------------------------------------------------------------ */
/* Interface                                                           */
/* ------------------------------------------------------------------ */
void AppWar::build(void)
{
    int screen_h = (int)lv_obj_get_height(lv_scr_act());

    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(COL_OCEAN), 0);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);

    _root = lv_obj_create(lv_scr_act());
    lv_obj_set_size(_root, CW, screen_h);
    lv_obj_set_pos(_root, 0, 0);
    lv_obj_set_style_pad_all(_root, 0, 0);
    lv_obj_set_style_border_width(_root, 0, 0);
    lv_obj_set_style_radius(_root, 0, 0);
    lv_obj_set_style_bg_color(_root, lv_color_hex(COL_OCEAN), 0);
    lv_obj_set_style_bg_opa(_root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(_root, LV_OBJ_FLAG_SCROLLABLE);

    size_t bytes = (size_t)CW * CH * 2u;
    _px = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    _bg = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (_px == nullptr || _bg == nullptr) {
        ESP_LOGE(TAG, "PSRAM insuffisante (%u o demandes)", (unsigned)bytes);
        return;
    }

    index_cells();
    paint_background();

    _canvas = lv_canvas_create(_root);
    lv_canvas_set_buffer(_canvas, _px, CW, CH, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(_canvas, 0, MAP_SCREEN_Y);
    lv_obj_remove_flag(_canvas, LV_OBJ_FLAG_SCROLLABLE);

    /* jauges : un creux et un remplissage par continent, objets LVGL normaux */
    for (int i = 0; i < BAR_N; i++) {
        int x = BAR_X0 + i * (BAR_W + BAR_GAP);
        _trough[i] = lv_obj_create(_root);
        lv_obj_set_size(_trough[i], BAR_W, BAR_MAXH);
        lv_obj_set_pos(_trough[i], x, BAR_Y0);
        lv_obj_set_style_radius(_trough[i], 3, 0);
        lv_obj_set_style_bg_color(_trough[i], lv_color_hex(COL_TROUGH), 0);
        lv_obj_set_style_bg_opa(_trough[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(_trough[i], 1, 0);
        lv_obj_set_style_border_color(_trough[i], lv_color_hex(COL_TROUGHT), 0);
        lv_obj_set_style_pad_all(_trough[i], 0, 0);
        lv_obj_remove_flag(_trough[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(_trough[i], LV_OBJ_FLAG_CLICKABLE);

        _fill[i] = lv_obj_create(_trough[i]);
        lv_obj_set_size(_fill[i], BAR_W - 2, 0);
        lv_obj_set_pos(_fill[i], 1, BAR_MAXH - 1);
        lv_obj_set_style_radius(_fill[i], 3, 0);
        lv_obj_set_style_bg_color(_fill[i], lv_color_hex(war_palette[i + 1]), 0);
        lv_obj_set_style_bg_opa(_fill[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(_fill[i], 0, 0);
        lv_obj_set_style_pad_all(_fill[i], 0, 0);
        lv_obj_add_flag(_fill[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(_fill[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(_fill[i], LV_OBJ_FLAG_CLICKABLE);
        _bar_h[i + 1] = -1;
    }

    /* « THE END » : au-dessus du canevas, au milieu de la carte, gras et blanc.
       Le gras vient d'un contour de 3 px dans la meme couleur. */
    _lbl_end = lv_label_create(_root);
    lv_label_set_text(_lbl_end, "THE END");
    lv_obj_set_style_text_font(_lbl_end, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(_lbl_end, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_outline_stroke_width(_lbl_end, 3, 0);
    lv_obj_set_style_text_outline_stroke_color(_lbl_end, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_outline_stroke_opa(_lbl_end, LV_OPA_COVER, 0);
    lv_obj_align(_lbl_end, LV_ALIGN_TOP_MID, 0, MAP_SCREEN_Y + WAR_MAP_H / 2 - 24);
    lv_obj_add_flag(_lbl_end, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(_lbl_end, LV_OBJ_FLAG_CLICKABLE);

    /* petite croix de fermeture */
    _btn_quit = lv_obj_create(_root);
    lv_obj_set_size(_btn_quit, QUIT_S, QUIT_S);
    lv_obj_set_pos(_btn_quit, QUIT_X, QUIT_Y);
    lv_obj_set_style_radius(_btn_quit, 8, 0);
    lv_obj_set_style_bg_color(_btn_quit, lv_color_hex(COL_QUIT_BG), 0);
    lv_obj_set_style_bg_opa(_btn_quit, LV_OPA_70, 0);
    lv_obj_set_style_border_width(_btn_quit, 1, 0);
    lv_obj_set_style_border_color(_btn_quit, lv_color_hex(COL_QUIT_BD), 0);
    lv_obj_set_style_pad_all(_btn_quit, 0, 0);
    lv_obj_remove_flag(_btn_quit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_btn_quit, LV_OBJ_FLAG_CLICKABLE);

    static lv_point_precise_t p1[2];
    static lv_point_precise_t p2[2];
    p1[0].x = 8;  p1[0].y = 8;   p1[1].x = 20; p1[1].y = 20;
    p2[0].x = 20; p2[0].y = 8;   p2[1].x = 8;  p2[1].y = 20;
    for (int i = 0; i < 2; i++) {
        lv_obj_t *ln = lv_line_create(_btn_quit);
        lv_line_set_points(ln, i == 0 ? p1 : p2, 2);
        lv_obj_set_style_line_width(ln, 2, 0);
        lv_obj_set_style_line_color(ln, lv_color_hex(COL_QUIT_X), 0);
        lv_obj_set_style_line_rounded(ln, true, 0);
    }
    lv_obj_add_event_cb(_btn_quit, on_quit, LV_EVENT_CLICKED, this);

    _built = true;
    _last_frame = esp_timer_get_time();
    _fps_t0 = (uint32_t)(_last_frame / 1000);
    reset_war();

    ESP_LOGI(TAG, "carte %dx%d a y=%d, canevas %dx%d en PSRAM, pile %u o",
             WAR_MAP_W, WAR_MAP_H, MAP_SCREEN_Y, CW, CH,
             (unsigned)uxTaskGetStackHighWaterMark(nullptr));
}

void AppWar::on_quit(lv_event_t *e)
{
    AppWar *self = static_cast<AppWar *>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    ESP_LOGI(TAG, "croix : fermeture");
    lv_timer_create(do_quit, 40, self);
}

void AppWar::do_quit(lv_timer_t *t)
{
    AppWar *self = static_cast<AppWar *>(t->user_data);
    lv_timer_del(t);
    if (self != nullptr) {
        self->back();
    }
}

bool AppWar::run(void)
{
    _closing = false;
    if (!_built) {
        build();
    }
    if (_timer == nullptr) {
        _timer = lv_timer_create(on_tick, 33, this);
    }
    ESP_LOGI(TAG, "run OK : ecran %dx%d",
             (int)lv_obj_get_width(lv_scr_act()),
             (int)lv_obj_get_height(lv_scr_act()));
    return true;
}

bool AppWar::back(void)
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
    _btn_quit = nullptr;
    for (int i = 0; i < BAR_N; i++) {
        _trough[i] = nullptr;
        _fill[i] = nullptr;
    }

    if (_px) {
        free(_px);
        _px = nullptr;
    }
    if (_bg) {
        free(_bg);
        _bg = nullptr;
    }
    free_cells();
    _built = false;

    ESP_BROOKESIA_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "notifyCoreClosed a echoue");
    ESP_LOGI(TAG, "back OK : retour au lanceur");
    return true;
}
