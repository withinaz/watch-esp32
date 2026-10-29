#pragma once

#include "lvgl.h"
#include "esp_brookesia.hpp"

/*
 * AppPerplexe - labyrinthe a bille (4eme application, page 4).
 *
 * Un plateau vu de dessus : des murs, des trous, un depart et une arrivee.
 * La bille roule quand on incline la montre -- l'accelerometre du QMI8658
 * donne la direction de la gravite, c'est exactement ce qu'il faut pour un
 * plateau. Le doigt, pose n'importe ou sur le plateau, freine la bille ;
 * une secousse la ramene au dernier point de passage.
 *
 * contraintes respectees :
 *  - barre d'etat orange : l'app garde use_status_bar, rien n'est dessine
 *    au-dessus de y=50 (le plateau commence a y=32 DANS l'ecran de l'app,
 *    qui est deja decale de 50 px) ;
 *  - aucun acces I2C sur la tache LVGL : le capteur est lu par une tache
 *    dediee a 30 Hz, qui publie l'inclinaison dans des atomiques ;
 *  - aucun bouton retour : on sort par le bouton QUITTER (au doigt) ou en
 *    redemarrant.
 */
class AppPerplexe : public ESP_Brookesia_PhoneApp {
public:
    AppPerplexe();
    ~AppPerplexe() override;

    bool run(void) override;
    bool back(void) override;

private:
    struct Pt {
        float x, y;
    };
    enum State {
        ST_BANNER = 0,   /* decompte avant le depart */
        ST_PLAY,
        ST_FALL,         /* la bille est tombee, on la repose */
        ST_DONE,         /* niveau termine */
        ST_END,          /* les dix niveaux sont finis */
    };

    void build_ui(void);
    void load_level(int idx);
    void clear_level(void);
    void respawn(void);
    void on_fall(void);
    void on_goal(void);
    void advance(void);
    void place_ball(float x, float y);
    void step(float dt);
    void slide_x(float d);
    void slide_y(float d);
    void tick(void);
    void hud(bool force);
    void msg(const char *a, const char *b, int life_ms);
    void msg_off(void);

    static void on_tick(lv_timer_t *t);
    static void on_brake(lv_event_t *e);
    static void on_restart(lv_event_t *e);
    static void on_quit(lv_event_t *e);
    static void do_quit(lv_timer_t *t);

    lv_obj_t *_root = nullptr;
    lv_obj_t *_board = nullptr;
    lv_obj_t *_ball = nullptr;
    lv_obj_t *_card = nullptr;
    lv_obj_t *_lbl_msg = nullptr;
    lv_obj_t *_lbl_lvl = nullptr;
    lv_obj_t *_lbl_score = nullptr;
    lv_obj_t *_lbl_time = nullptr;
    lv_obj_t *_lbl_touch = nullptr;
    lv_obj_t *_btn_restart = nullptr;
    lv_obj_t *_cp_obj[4] = {};
    bool _cp_taken[4] = {};
    lv_timer_t *_timer = nullptr;

    Pt _holes[16] = {};
    int _nholes = 0;
    Pt _cps[4] = {};
    int _ncps = 0;
    Pt _start = {};
    Pt _goal = {};
    Pt _cp = {};

    int _level = 0;
    int _score = 0;
    int _best = 0;
    int _falls = 0;
    int _taps = 0;
    int _state = ST_BANNER;
    int _wait = 0;
    int _msg_ms = 0;
    uint32_t _elapsed = 0;
    uint32_t _last_tick = 0;
    int _lvl_shown = -1;
    int _score_shown = -1;
    int _taps_shown = -1;
    int _sec_shown = -1;

    float _bx = 0.0f;
    float _by = 0.0f;
    float _vx = 0.0f;
    float _vy = 0.0f;
    int _px = -999;
    int _py = -999;
    bool _brake = false;
    bool _closing = false;
};
