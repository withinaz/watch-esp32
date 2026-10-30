#pragma once

#include "lvgl.h"
#include "esp_brookesia.hpp"

/*
 * AppFight - « Street Fury », jeu de combat 16 bits (6eme app, page 6).
 *
 * Une rue au coucher du soleil, deux combattants pixel art face a face. Chaque
 * appui sur un combattant declenche une de ses cinq attaques, tiree au hasard.
 * L'attaque part d'une garde, frappe, marque le point de choc d'un rond blanc,
 * puis revient. Chaque combattant dispose de dix attaques ; le match se joue en
 * dix rounds (un round = un echange) et le vainqueur est celui qui a le plus de
 * vie restante. Si une barre tombe a zero avant la fin, c'est K.O.
 *
 * Rendu : un seul canevas de 410 x 396 en PSRAM (le decor complet), restaure
 * par bande depuis un fond immuable, puis les deux combattants sont redessines
 * par-dessus. Seule la bande des combattants est invalidee, et rien n'est
 * redessine au repos : l'ecran est statique entre deux attaques.
 *
 * Pas de son (l'audio a deja casse l'affichage en v4.1), pas de nvs, pas d'I2C.
 * La barre d'etat orange est conservee : le jeu commence a y=56.
 */
class AppFight : public ESP_Brookesia_PhoneApp {
public:
    AppFight();
    ~AppFight() override;

    bool run(void) override;
    bool back(void) override;

    /* Avance une attaque comme le ferait un appui (banc d'essai). */
    void tap_at(int x, int y);
    bool finished(void) const { return _over; }

private:
    /* Une pose = les quatre extremites qui bougent, en pixels d'art. */
    struct Pose {
        int lean;               /* decalage du corps                     */
        int fnx, fny;           /* poing avant                           */
        int ffx, ffy;           /* poing arriere                         */
        int fkx, fky;           /* pied avant                            */
        int head_dy;            /* inclinaison de la tete                */
        int arm_up;             /* 0 = garde, 1 = bras leves (victoire)  */
    };

    void build(void);
    void paint_bg(void);

    void reset_match(void);
    void launch(int side);
    void apply_hit(void);
    void end_turn(void);
    void decide(void);

    void draw_frame(void);
    void draw_fighter(int side, const Pose &p, int flash);
    void draw_orbs(void);
    void draw_dot(void);
    void draw_ring(int cx, int cy, int r, uint16_t c);

    void fp(int side, int ax, int ay, uint16_t c);
    void fline(int side, int x0, int y0, int x1, int y1, int t, uint16_t c);
    void frect(int side, int x0, int y0, int x1, int y1, uint16_t c);
    void fcirc(int side, int cx, int cy, int r, uint16_t c);
    void limb(int side, int x0, int y0, int x1, int y1, uint16_t fill, uint16_t out, int bend,
              int t = 6);
    void arm(int side, int sx, int sy, int hx, int hy, uint16_t sleeve, uint16_t skin,
             uint16_t out, int bend);
    void box(int side, int x0, int y0, int x1, int y1, uint16_t fill, uint16_t out);

    Pose guard_pose(void) const;
    Pose attack_pose(int side);
    Pose winner_pose(void) const;
    Pose loser_pose(void) const;

    void to_art(int side, int cx, int cy, int *ax, int *ay) const;
    void restore_band(void);
    void invalidate_band(void);
    void update_bars(void);
    void update_pips(void);
    void update_round(void);

    void tick(void);

    static void on_tick(lv_timer_t *t);
    static void on_tap(lv_event_t *e);
    static void on_quit(lv_event_t *e);
    static void on_quit_later(lv_timer_t *t);

    lv_obj_t *_root = nullptr;
    lv_obj_t *_canvas = nullptr;
    lv_obj_t *_lbl_round = nullptr;
    lv_obj_t *_lbl_name[2] = {};
    lv_obj_t *_bar_bg[2] = {};
    lv_obj_t *_bar_fill[2] = {};
    lv_obj_t *_pip[2][10] = {};
    lv_obj_t *_plate_win = nullptr;
    lv_obj_t *_lbl_win = nullptr;
    lv_obj_t *_lbl_ko = nullptr;
    lv_obj_t *_btn_quit = nullptr;
    lv_timer_t *_timer = nullptr;

    uint16_t *_px = nullptr;        /* tampon du canevas (PSRAM)       */
    uint16_t *_bg = nullptr;        /* decor immuable (PSRAM)          */

    int _hp[2] = {150, 150};
    int _used[2] = {0, 0};
    int _atk[2] = {0, 0};
    int _side = 0;
    int _frame = 0;
    int _flash[2] = {0, 0};
    int _dot_frames = 0;
    int _dot_x = 0, _dot_y = 0;
    int _winner = -1;
    int _state = 0;                 /* 0 combat, 1 fin, 2 remise a zero */
    int64_t _over_t0 = 0;
    int64_t _blink_t0 = 0;
    bool _over = false;
    bool _blink_on = false;
    bool _busy = false;
    bool _built = false;
    bool _closing = false;
    uint32_t _taps = 0;
    uint32_t _redraws = 0;
};
