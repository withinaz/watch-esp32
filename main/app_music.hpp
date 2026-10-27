#pragma once

#include "lvgl.h"
#include "esp_brookesia.hpp"

/*
 * AppMusic - boite a musique generative (3eme application, page 3).
 *
 * Aucune interaction : l'app compose et joue toute seule des que la page
 * s'ouvre. Un sequenceur pentatonique genere une basse, un accord tenu et
 * une melodie ; un petit moteur de synthese les rend en 24 kHz mono vers
 * le codec ES8311.
 *
 * L'ecran montre un piano roll qui defile (12 colonnes = les 12 derniers
 * huitiemes, 10 lignes = deux octaves de pentatonique), les quatre accords
 * du cycle, et une jauge d'energie qui suit le signal reellement produit.
 */
class AppMusic : public ESP_Brookesia_PhoneApp {
public:
    AppMusic();
    ~AppMusic() override;

    bool run(void) override;
    bool back(void) override;

    /* Grille du piano roll. */
    static const int ROLL_COLS = 12;
    static const int ROLL_ROWS = 10;

private:
    void build_ui(void);
    void refresh(void);
    void update_roll(void);
    void update_chords(void);

    static void on_tick(lv_timer_t *t);

    lv_obj_t *_root = nullptr;
    lv_obj_t *_dot = nullptr;
    lv_obj_t *_lbl_state = nullptr;
    lv_obj_t *_lbl_hint = nullptr;
    lv_obj_t *_strip = nullptr;
    lv_obj_t *_cell[ROLL_COLS][ROLL_ROWS] = {};
    bool _lit[ROLL_COLS][ROLL_ROWS] = {};
    lv_obj_t *_chord_box = nullptr;
    lv_obj_t *_chord_lbl[4] = {};
    lv_obj_t *_energy_bg = nullptr;
    lv_obj_t *_energy_fg = nullptr;
    lv_obj_t *_lbl_foot = nullptr;
    lv_timer_t *_timer = nullptr;

    uint32_t _last_cols_ver = 0;
    int _last_chord = -1;
    int _last_key = -1;
    int _last_bpm = -1;
    bool _closing = false;
};
