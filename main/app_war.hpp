#pragma once

#include "lvgl.h"
#include "esp_brookesia.hpp"

/*
 * AppWar - « Guerre », fond d'ecran anime (5eme application, page 5).
 *
 * Une carte du monde a plat, en pixel art, un continent par couleur. Deux
 * continents se frappent l'un apres l'autre, sans fin : l'attaquant tire une
 * salve, la cible riposte, puis une nouvelle paire est tiree au sort. Aucune
 * intervention humaine n'est necessaire ; l'app tourne jusqu'a ce qu'on la
 * ferme (petite croix en bas a droite).
 *
 * Le decor (ocean + carte) vit dans un canevas de 410 x 198 en PSRAM, revalide
 * en entier a chaque image : le redessin partiel d'un canevas s'est revele
 * defectueux sur cette carte (ligne corrompue en haut de la zone repeinte).
 * Les jauges de degats sont des objets LVGL ordinaires.
 */
class AppWar : public ESP_Brookesia_PhoneApp {
public:
    AppWar();
    ~AppWar() override;

    bool run(void) override;
    bool back(void) override;

private:
    /* Une salve = trois missiles, lances a 210 ms d'intervalle. */
    struct Shot {
        bool on;
        bool done;          /* l'explosion a deja ete declenchee */
        int from, to;       /* continents */
        float x0, y0;       /* depart */
        float cx, cy;       /* point de controle de la courbe */
        float x1, y1;       /* impact */
        int64_t t0;         /* depart, en us */
        int dur;            /* duree du vol, en us */
    };

    struct Boom {
        bool on;
        int x, y;
        int cont;
        int64_t t0;
    };

    enum Step {
        ST_INTRO = 0,       /* la carte s'installe */
        ST_FIRE_A,          /* l'attaquant tire sa salve */
        ST_WAIT_A,          /* on attend que tout retombe */
        ST_FIRE_B,          /* la cible riposte */
        ST_WAIT_B,
        ST_GAP,             /* respiration avant la paire suivante */
        ST_PAUSED,          /* « THE END » : plus aucune attaque */
    };

    void build(void);
    void paint_background(void);
    void paint_map(void);
    void paint_ocean(void);
    void free_cells(void);
    void index_cells(void);

    void put(int x, int y, uint16_t c);
    void dirty(int x0, int y0, int x1, int y1);
    void restore(int x0, int y0, int x1, int y1);

    void reset_war(void);
    void pick_pair(void);
    void launch_one(int from, int to);
    void target_point(int cont, float *x, float *y);
    void launch_point(int cont, float tx, float ty, float *x, float *y);
    void boom_at(int x, int y, int cont);
    void crater(int x, int y);

    void tick(void);
    void end_trigger(int64_t now);
    void end_world(void);
    void update_bars(void);
    void draw_shots(void);
    void draw_booms(void);

    static void on_tick(lv_timer_t *t);
    static void on_quit(lv_event_t *e);
    static void do_quit(lv_timer_t *t);

    lv_obj_t *_root = nullptr;
    lv_obj_t *_canvas = nullptr;
    lv_obj_t *_btn_quit = nullptr;
    lv_obj_t *_lbl_end = nullptr;
    lv_obj_t *_trough[6] = {};
    lv_obj_t *_fill[6] = {};
    lv_timer_t *_timer = nullptr;

    uint16_t *_px = nullptr;      /* tampon affiche (canevas)       */
    uint16_t *_bg = nullptr;      /* fond immuable (restauration)   */

    /* rectangle sale de l'image en cours */
    int _dx0, _dy0, _dx1, _dy1;
    bool _has_dirty;

    /* cases utilisables, par continent : indices r * WAR_MAP_COLS + c */
    uint16_t *_cells[7] = {};
    int _ncells[7] = {};
    int _ncoast[7] = {};
    uint16_t *_coast[7] = {};

    Shot _shots[12] = {};
    Boom _booms[12] = {};
    float _damage[7] = {};
    int _bar_h[7] = {-1, -1, -1, -1, -1, -1, -1};
    uint8_t _dead[33][82] = {};      /* cases deja creusees, pour le 90 % */
    int _dead_count = 0;
    int _land_total = 0;
    bool _ending = false;
    int64_t _end_t0 = 0;

    int _step = ST_INTRO;
    int _a = 0, _b = 1;
    int _prev_a = -1, _prev_b = -1;
    int _salvo = 0;
    int64_t _step_t0 = 0;
    int64_t _next_launch = 0;
    int64_t _last_frame = 0;
    float _dt = 0.033f;

    uint32_t _frames = 0;
    uint32_t _fps_t0 = 0;
    uint32_t _strikes = 0;
    bool _built = false;
    bool _closing = false;
};
