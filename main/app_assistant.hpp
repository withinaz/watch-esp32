#pragma once

#include "lvgl.h"
#include "esp_brookesia.hpp"

/*
 * AppAssistant - assistant vocal (protocole XiaoZhi).
 *
 * L'appui sur le gros bouton ouvre le micro, l'appui inverse l'envoie au
 * serveur ; la reponse arrive en Opus et se joue toute seule.
 *
 * Un animateur de niveau sonore (barres) occupe la bande sous la carte
 * d'etat : il suit le niveau reel du micro quand on parle et celui du
 * haut-parleur quand l'assistant repond. C'est purement decoratif.
 */
class AppAssistant : public ESP_Brookesia_PhoneApp {
public:
    AppAssistant();
    ~AppAssistant() override;

    bool run(void) override;
    bool back(void) override;

    static void cb_talk(lv_event_t *e);
    static void cb_back(lv_event_t *e);

    /* Nombre de barres de l'animateur. */
    static const int ANIM_BARS = 26;

private:
    void build_ui(void);
    void refresh(void);
    void animate(void);

    static void on_timer(lv_timer_t *t);
    static void on_anim(lv_timer_t *t);

    lv_obj_t *_root = nullptr;
    lv_obj_t *_lbl_state = nullptr;
    lv_obj_t *_lbl_hint = nullptr;
    lv_obj_t *_dot = nullptr;
    lv_obj_t *_lbl_user = nullptr;
    lv_obj_t *_lbl_bot = nullptr;
    lv_obj_t *_btn = nullptr;
    lv_obj_t *_lbl_btn = nullptr;
    lv_timer_t *_timer = nullptr;
    lv_timer_t *_anim = nullptr;

    /* animateur de niveau */
    lv_obj_t *_strip = nullptr;
    lv_obj_t *_bars[ANIM_BARS] = {};
    float _bar_h[ANIM_BARS] = {};
    uint32_t _bar_col = 0xFFFFFFFF;
    uint32_t _phase = 0;          /* ms depuis l'ouverture de l'app */

    uint32_t _last_ver = 0xFFFFFFFF;
    uint32_t _off_since = 0;      /* date du passage en "Hors ligne" (0 = pas hors ligne) */
    int _last_state = -1;
    bool _closing = false;
};
