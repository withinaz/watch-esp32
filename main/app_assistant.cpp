/*
 * AppAssistant - assistant vocal (client XiaoZhi) en application du lanceur.
 */
#include <cstdio>
#include <cmath>
#include <string>

#include "esp_log.h"

#include "app_assistant.hpp"
#include "icons/app_icons.h"
#include "xiaozhi_client.hpp"
#include "private/esp_brookesia_utils.h"

#if __has_include("xz_test.h")
#include "xz_test.h"
#endif

static const char *TAG = "ASSISTANT";
static AppAssistant *s_self = nullptr;

#define COL_BG      0x0E0E10
#define COL_CARD    0x1C1C1E
#define COL_TEXT    0xFFFFFF
#define COL_ACCENT  0x00E5A0
#define COL_DIM     0x8A8A8E
#define COL_WARN    0xFF9F0A
#define COL_ERR     0xFF453A

/* Hauteur de la bande de l'animateur, en pixels. */
#define ANIM_STRIP_H 62

/* ------------------------------------------------------------------ */
AppAssistant::AppAssistant() :
    ESP_Brookesia_PhoneApp("Assistant", &app_icon_assistant,
                           true, true, true)
{
    s_self = this;

}


AppAssistant::~AppAssistant()
{
    if (_timer) {
        lv_timer_del(_timer);
        _timer = nullptr;
    }
}

/* ------------------------------------------------------------------ */
/* L'init audio touche le bus I2C (codecs) et cree deux taches. Elle ne doit
   SURTOUT pas tourner sur la tache LVGL : la tache tactile lit le tactile
   sur le MEME bus I2C, et si elle tient le verrou I2C pendant que la tache
   LVGL (qui tient deja le verrou LVGL) l'attend, c'est l'interblocage ->
   ecran fige, image persistante. On la delegue a une tache dediee.        */
static void start_client_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "start_client: begin");
    XzClient &c = XzClient::get();
    c.begin();
    ESP_LOGI(TAG, "start_client: connect");
    c.connectAsync();
    ESP_LOGI(TAG, "start_client: fin");
    vTaskDelete(nullptr);
}

/* ------------------------------------------------------------------ */
/* Repaint force.                                                      */
/*                                                                     */
/* run() est appele AVANT le chargement de l'ecran de l'app : LVGL      */
/* n'invalide pas forcement toute la surface a ce moment-la, et les     */
/* zones statiques peuvent garder les pixels du lanceur (icone visible  */
/* « sous » l'interface). Deux passes completes corrigent le tir.       */
/* ------------------------------------------------------------------ */
static void force_repaint(lv_timer_t *t)
{
    lv_timer_del(t);
    lv_obj_t *scr = lv_scr_act();
    if (scr == nullptr) {
        return;
    }
    /* On marque seulement la surface comme a redessiner : c'est le
       rafraichissement normal de LVGL qui s'en charge, sans bloquer la
       tache graphique. */
    lv_obj_invalidate(scr);
    ESP_LOGI(TAG, "surface marquee a redessiner");
}

bool AppAssistant::run(void)
{
    _closing = false;
    build_ui();

    if (_timer == nullptr) {
        _timer = lv_timer_create(on_timer, 120, this);
    }
    /* L'animateur a son propre rythme : 25 images/s, independant du texte
       qui n'a besoin que de 8 rafraichissements par seconde. */
    if (_anim == nullptr) {
        _anim = lv_timer_create(on_anim, 40, this);
    }
    refresh();

    /* Deux passes completes une fois l'ecran charge (voir force_repaint). */
    lv_timer_create(force_repaint, 350, nullptr);
    lv_timer_create(force_repaint, 1400, nullptr);

    /* Audio + connexion hors de la tache LVGL. 8 Ko : I2C + codecs + init
       Opus, sans profondeur de recursion. */
    xTaskCreate(start_client_task, "xz_start", 8192, this, 5, nullptr);
    ESP_LOGI(TAG, "run OK (pile LVGL %u o)", (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    return true;
}

bool AppAssistant::back(void)
{
    if (_closing) {
        return true;
    }
    _closing = true;

    XzClient::get().disconnect();

    if (_timer) {
        lv_timer_del(_timer);
        _timer = nullptr;
    }
    if (_anim) {
        lv_timer_del(_anim);
        _anim = nullptr;
    }
    if (_root) {
        lv_obj_del(_root);
        _root = nullptr;
    }
    _lbl_state = _lbl_hint = _dot = _lbl_user = _lbl_bot = nullptr;
    _btn = _lbl_btn = nullptr;
    _strip = nullptr;
    for (int i = 0; i < ANIM_BARS; i++) {
        _bars[i] = nullptr;
        _bar_h[i] = 0.0f;
    }
    _bar_col = 0xFFFFFFFF;

    ESP_BROOKESIA_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "notifyCoreClosed a echoue");
    ESP_LOGI(TAG, "back OK : retour au lanceur");
    return true;
}

/* ------------------------------------------------------------------ */
void AppAssistant::build_ui(void)
{
    if (_root) {
        lv_obj_del(_root);
        _root = nullptr;
    }

    /* Fond de l'ecran lui-meme, opaque : aucune zone non couverte par le
       conteneur ne peut laisser apparaitre le lanceur. */
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);

    /* Bandeau superieur : l'ecran de l'app commence a y=50, et les 50 px du
       haut restent ceux de l'ecran precedent. On les repeint avec le fond de
       l'app (voir app_music.cpp pour le detail). */
    {
        lv_obj_t *cap = lv_obj_create(lv_scr_act());
        lv_obj_set_size(cap, LV_PCT(100), 50);
        lv_obj_set_pos(cap, 0, -50);
        lv_obj_set_style_bg_color(cap, lv_color_hex(COL_BG), 0);
        lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(cap, 0, 0);
        lv_obj_set_style_radius(cap, 0, 0);
        lv_obj_set_style_pad_all(cap, 0, 0);
        lv_obj_clear_flag(cap, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(cap, LV_OBJ_FLAG_CLICKABLE);
    }

    /* L'ecran de l'app fourni par le core est DEJA place sur la zone visible
       (sous la barre d'etat) ET le core reduit la resolution de l'affichage a
       la hauteur de cette zone. Tout ce qui depasse la hauteur d'affichage
       n'est ni dessine ni cliquable : il faut donc dimensionner le contenu
       sur ce qui reste reellement adressable. */
    _root = lv_obj_create(lv_scr_act());
    lv_obj_set_pos(_root, 0, 0);
    lv_obj_set_style_bg_color(_root, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_root, 0, 0);
    lv_obj_set_style_radius(_root, 0, 0);
    lv_obj_set_style_pad_all(_root, 12, 0);
    lv_obj_clear_flag(_root, LV_OBJ_FLAG_SCROLLABLE);

    /* Hauteur reellement adressable.
       Le core reduit la resolution d'affichage a la hauteur de la zone visible
       (452 px) PUIS deplace l'ecran de l'app a y=50. Ce deplacement n'est fait
       qu'au chargement de l'ecran, donc lv_obj_get_y() renvoie encore 0 ici :
       on se sert de getVisualArea() pour connaitre le decalage. */
    const lv_area_t &va = getVisualArea();
    int usable_h = 0;
    if (va.y1 > 0) {
        usable_h = lv_area_get_height(&va) - va.y1;
    } else {
        lv_display_t *d = lv_display_get_default();
        usable_h = (d != nullptr) ? (int)lv_display_get_vertical_resolution(d) : 0;
    }
    if (usable_h < 200 || usable_h > 520) {
        usable_h = 402; /* garde-fou */
    }
    lv_obj_set_size(_root, LV_PCT(100), usable_h);
    /* L'ecran de l'app mesure 452 px alors que la zone utile n'en fait que 402
       (affichage reduit de 502 a 452). Sans cette extension, une bande de
       50 px en bas de l'ecran reste peinte par le fond de l'ecran et laisse
       voir le lanceur derriere. Le contenu, lui, reste calibre sur 402. */
    {
        int screen_h = (int)lv_obj_get_height(lv_scr_act());
        if (screen_h > usable_h) {
            lv_obj_set_size(_root, LV_PCT(100), screen_h);
        }
    }
    ESP_LOGI(TAG, "zone visible (%d,%d)-(%d,%d), contenu sur %d px",
             va.x1, va.y1, va.x2, va.y2, usable_h);

    const int scr_h = usable_h;

    lv_obj_t *title = lv_label_create(_root);
    lv_label_set_text(title, "Assistant");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    /* --- carte d'etat --- */
    lv_obj_t *card = lv_obj_create(_root);
    lv_obj_set_size(card, LV_PCT(100), 74);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 42);
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    _dot = lv_obj_create(card);
    lv_obj_set_size(_dot, 12, 12);
    lv_obj_set_style_radius(_dot, 8, 0);
    lv_obj_set_style_bg_color(_dot, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_bg_opa(_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_dot, 0, 0);
    lv_obj_align(_dot, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_clear_flag(_dot, LV_OBJ_FLAG_SCROLLABLE);

    _lbl_state = lv_label_create(card);
    lv_label_set_text(_lbl_state, "Demarrage");
    lv_obj_set_style_text_color(_lbl_state, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(_lbl_state, &lv_font_montserrat_22, 0);
    lv_obj_align(_lbl_state, LV_ALIGN_TOP_LEFT, 40, 10);

    _lbl_hint = lv_label_create(card);
    lv_label_set_text(_lbl_hint, "");
    lv_obj_set_style_text_color(_lbl_hint, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_font(_lbl_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_width(_lbl_hint, 330);
    lv_label_set_long_mode(_lbl_hint, LV_LABEL_LONG_WRAP);
    lv_obj_align(_lbl_hint, LV_ALIGN_TOP_LEFT, 40, 40);

    /* --- animateur de niveau sonore ---
       Bande pleine largeur entre la carte d'etat et la transcription.
       Barres absolument positionnees (pas de flex) : changer la hauteur
       d'une barre n'invalide que son rectangle, pas toute la bande. */
    const int strip_h = ANIM_STRIP_H;
    const int strip_y = 124;
    _strip = lv_obj_create(_root);
    lv_obj_set_size(_strip, LV_PCT(100), strip_h);
    lv_obj_align(_strip, LV_ALIGN_TOP_MID, 0, strip_y);
    lv_obj_set_style_bg_color(_strip, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(_strip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_strip, 0, 0);
    lv_obj_set_style_radius(_strip, 16, 0);
    lv_obj_set_style_pad_all(_strip, 0, 0);
    lv_obj_clear_flag(_strip, LV_OBJ_FLAG_SCROLLABLE);

    {
        const int bar_w = 10;
        const int gap = 3;
        /* La largeur d'un enfant n'est connue qu'apres une passe de layout :
           on la prend sur le conteneur racine, dont la taille est explicite. */
        lv_obj_update_layout(_root);
        int inner = lv_obj_get_content_width(_strip);
        if (inner <= 0) {
            inner = lv_obj_get_content_width(_root);
        }
        if (inner <= 0) {
            inner = 386;
        }
        const int total = ANIM_BARS * bar_w + (ANIM_BARS - 1) * gap;
        int x0 = (inner - total) / 2;
        if (x0 < 0) {
            x0 = 0;
        }
        for (int i = 0; i < ANIM_BARS; i++) {
            lv_obj_t *b = lv_obj_create(_strip);
            lv_obj_set_size(b, bar_w, 6);
            lv_obj_set_pos(b, x0 + i * (bar_w + gap), (strip_h - 6) / 2);
            lv_obj_set_style_bg_color(b, lv_color_hex(COL_DIM), 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(b, 0, 0);
            lv_obj_set_style_radius(b, bar_w / 2, 0);
            lv_obj_set_style_pad_all(b, 0, 0);
            lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
            _bars[i] = b;
            _bar_h[i] = 6.0f;
        }
        ESP_LOGI(TAG, "animateur : %d barres, largeur %d px (x0=%d)",
                 (int)ANIM_BARS, total, x0);
    }

    /* --- transcription ---
       Entre l'animateur (186) et le bouton du bas. */
    const int tr_top = strip_y + strip_h + 8;
    const int btn_h = 60;
    int tr_h = (scr_h - btn_h - 20) - tr_top;
    if (tr_h < 60) {
        tr_h = 60;
    }
    lv_obj_t *tr = lv_obj_create(_root);
    lv_obj_set_size(tr, LV_PCT(100), tr_h);
    lv_obj_align(tr, LV_ALIGN_TOP_MID, 0, tr_top);
    lv_obj_set_style_bg_color(tr, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(tr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tr, 0, 0);
    lv_obj_set_style_radius(tr, 16, 0);
    lv_obj_set_style_pad_all(tr, 14, 0);
    lv_obj_set_scroll_dir(tr, LV_DIR_VER);
    lv_obj_add_flag(tr, LV_OBJ_FLAG_SCROLLABLE);
    /* Colonne : les deux libelles s'empilent quelle que soit leur hauteur. */
    lv_obj_set_flex_flow(tr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(tr, 8, 0);

    _lbl_user = lv_label_create(tr);
    lv_label_set_text(_lbl_user, "");
    lv_obj_set_style_text_color(_lbl_user, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_font(_lbl_user, &lv_font_montserrat_16, 0);
    lv_obj_set_width(_lbl_user, LV_PCT(100));
    lv_label_set_long_mode(_lbl_user, LV_LABEL_LONG_WRAP);

    _lbl_bot = lv_label_create(tr);
    lv_label_set_text(_lbl_bot, "");
    lv_obj_set_style_text_color(_lbl_bot, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(_lbl_bot, &lv_font_montserrat_18, 0);
    lv_obj_set_width(_lbl_bot, LV_PCT(100));
    lv_label_set_long_mode(_lbl_bot, LV_LABEL_LONG_WRAP);

    /* --- bouton d'action, pleine largeur, collé en bas ---
       Pas de bouton retour : pour quitter l'app, on redemarre la montre. */
    _btn = lv_obj_create(_root);
    lv_obj_set_size(_btn, LV_PCT(100), 60);
    lv_obj_align(_btn, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_color(_btn, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_btn, 0, 0);
    lv_obj_set_style_radius(_btn, 18, 0);
    lv_obj_clear_flag(_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_btn, AppAssistant::cb_talk, LV_EVENT_CLICKED, this);

    _lbl_btn = lv_label_create(_btn);
    lv_label_set_text(_lbl_btn, "PARLER");
    lv_obj_set_style_text_color(_lbl_btn, lv_color_hex(0x0E0E10), 0);
    lv_obj_set_style_text_font(_lbl_btn, &lv_font_montserrat_24, 0);
    lv_obj_center(_lbl_btn);

    {
        lv_area_t a;
        lv_obj_get_coords(_btn, &a);
        ESP_LOGI(TAG, "bouton action  x=%d..%d y=%d..%d", a.x1, a.x2, a.y1, a.y2);
    }
}

/* ------------------------------------------------------------------ */
void AppAssistant::on_timer(lv_timer_t *t)
{
    AppAssistant *self = static_cast<AppAssistant *>(t->user_data);
    if (self == nullptr) {
        return;
    }

    /* Trace discrete des appuis : ne s'affiche que si un doigt touche. */
    for (lv_indev_t *i = lv_indev_get_next(nullptr); i != nullptr; i = lv_indev_get_next(i)) {
        if (lv_indev_get_state(i) == LV_INDEV_STATE_PRESSED) {
            lv_point_t p;
            lv_indev_get_point(i, &p);
            static uint32_t last = 0;
            if (lv_tick_get() - last > 500) {
                last = lv_tick_get();
                ESP_LOGI(TAG, "appui %d,%d", (int)p.x, (int)p.y);
            }
        }
    }

    self->refresh();
}

void AppAssistant::refresh(void)
{
    if (_lbl_state == nullptr) {
        return;
    }

    XzClient &c = XzClient::get();
    int st = (int)c.state();

    if (st != _last_state) {
        _last_state = st;

        uint32_t col = COL_DIM;
        const char *txt = "?";
        const char *btn = "PARLER";
        uint32_t btn_col = COL_ACCENT;

        switch (st) {
        case XZ_OFF:
            /* Meme libelle que dans les autres etats : le tactile etant
               inoperant dans les apps, un bouton "RECONNECTER" ne serait de
               toute facon jamais presse. La reconnexion est automatique
               (voir plus bas). */
            col = COL_DIM;  txt = "Hors ligne";     btn = "PARLER";       btn_col = COL_WARN;
            break;
        case XZ_CONNECTING:
            col = COL_WARN; txt = "Connexion";      btn = "PARLER";       btn_col = COL_CARD;
            break;
        case XZ_ACTIVATION:
            col = COL_WARN; txt = "Appairage";      btn = "VERIFIER";     btn_col = COL_CARD;
            break;
        case XZ_READY:
            col = COL_ACCENT; txt = "Pret";         btn = "PARLER";
            break;
        case XZ_LISTENING:
            col = COL_ERR;  txt = "J'ecoute";       btn = "ENVOYER";      btn_col = COL_WARN;
            break;
        case XZ_THINKING:
            col = COL_WARN; txt = "Je reflechis";   btn = "...";          btn_col = COL_CARD;
            break;
        case XZ_SPEAKING:
            col = COL_ACCENT; txt = "Je reponds";   btn = "COUPER";       btn_col = COL_WARN;
            break;
        case XZ_ERROR:
            col = COL_ERR;  txt = "Probleme";       btn = "REESSAYER";    btn_col = COL_WARN;
            break;
        default:
            break;
        }

        lv_label_set_text(_lbl_state, txt);
        lv_obj_set_style_bg_color(_dot, lv_color_hex(col), 0);
        lv_label_set_text(_lbl_btn, btn);
        lv_obj_set_style_bg_color(_btn, lv_color_hex(btn_col), 0);
        lv_obj_set_style_text_color(_lbl_btn,
                                   lv_color_hex(btn_col == COL_ACCENT ? 0x0E0E10 : COL_TEXT), 0);

        /* Le code d'appairage doit rester lisible en gros. */
        std::string code = c.activationCode();
        if (st == XZ_ACTIVATION && !code.empty()) {
            lv_label_set_text(_lbl_bot, ("Code : " + code + "\n(xiaozhi.me)").c_str());
            _last_ver = c.textVersion();
        }
    }

    /* Reconnexion automatique. Si l'app tombe « Hors ligne » (serveur
       injoignable a l'ouverture, coupure WiFi, reouverture apres fermeture),
       elle retente toute seule : sans cela l'etat restait fige
       indefiniment, puisque le bouton n'est pas atteignable au doigt. */
    if (st == XZ_OFF && !_closing) {
        const uint32_t now = lv_tick_get();
        if (_off_since == 0) {
            _off_since = now;
        } else if ((uint32_t)(now - _off_since) > 8000) {
            _off_since = now;
            ESP_LOGI(TAG, "hors ligne depuis 8 s : nouvelle tentative automatique");
            c.connectAsync();
        }
    } else {
        _off_since = 0;
    }

    /* hint + transcription */
    lv_label_set_text(_lbl_hint, c.hint().c_str());

    uint32_t ver = c.textVersion();
    if (ver != _last_ver) {
        _last_ver = ver;
        std::string u = c.userText();
        std::string b = c.botText();
        lv_label_set_text(_lbl_user, u.empty() ? "" : ("Vous : " + u).c_str());
        lv_label_set_text(_lbl_bot, b.empty() ? "" : b.c_str());
        /* garder la fin du dialogue visible */
        if (_lbl_bot->parent) {
            lv_obj_scroll_to_view(_lbl_bot, LV_ANIM_OFF);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Animateur de niveau.                                               */
/*                                                                    */
/* Le niveau vient du VRAI son : crete des trames micro quand on      */
/* parle, crete des trames decodees quand l'assistant repond. Aucun    */
/* son n'est modifie, on ne fait que lire une valeur deja calculee.   */
/* Les etats sans audio (appairage, reflexion) ont leur propre         */
/* mouvement, purement decoratif.                                     */
/* ------------------------------------------------------------------ */
void AppAssistant::animate(void)
{
    if (_strip == nullptr || _bars[0] == nullptr) {
        return;
    }

    XzClient &c = XzClient::get();
    const int st = (int)c.state();

    _phase += 40;                              /* periode du timer */
    const float t = (float)_phase / 1000.0f;

    const int BAR_MIN = 6;
    const int BAR_MAX = ANIM_STRIP_H - 6;

    int level = 0;
    uint32_t col = COL_DIM;
    switch (st) {
    case XZ_LISTENING:
        level = c.micLevel();
        col = COL_ERR;
        break;
    case XZ_SPEAKING:
        level = c.spkLevel();
        col = COL_ACCENT;
        break;
    case XZ_THINKING:
        col = COL_WARN;
        break;
    case XZ_READY:
        col = 0x11996F;                        /* vert sombre : discret */
        break;
    default:
        col = COL_DIM;
        break;
    }

    bool live = (st == XZ_LISTENING || st == XZ_SPEAKING);
#ifdef XZ_ANIMTEST
    /* Banc d'essai : niveau simule pour controler l'animation sans parler. */
    live = true;
    level = 62 + (int)(30.0f * sinf(t * 0.7f));
    col = COL_ACCENT;
#endif

    if (col != _bar_col) {
        _bar_col = col;
        for (int i = 0; i < ANIM_BARS; i++) {
            if (_bars[i] != nullptr) {
                lv_obj_set_style_bg_color(_bars[i], lv_color_hex(col), 0);
            }
        }
    }

    const float mid = (float)(ANIM_BARS - 1) * 0.5f;
    for (int i = 0; i < ANIM_BARS; i++) {
        lv_obj_t *b = _bars[i];
        if (b == nullptr) {
            continue;
        }

        /* Profil : plus haut au centre, plus bas sur les bords. */
        const float d = fabsf((float)i - mid) / mid;
        const float shape = 0.30f + 0.70f * (1.0f - d * d);

        int target = BAR_MIN;
        if (live) {
            /* Spectre : deux sinus qui battent ensemble, un par barre, pour
               eviter l'effet « tout monte et descend en meme temps ». */
            const float mod = 0.55f + 0.45f * sinf(t * 6.9f + (float)i * 0.83f)
                                         * sinf(t * 2.3f + (float)i * 0.37f);
            target = BAR_MIN + (int)((float)level * shape * mod * 0.52f);
        } else if (st == XZ_THINKING) {
            /* Comete qui balaie la bande de gauche a droite. */
            const float pos = fmodf(t * 1.1f, 1.0f) * (float)(ANIM_BARS - 1);
            const float dd = (float)i - pos;
            target = BAR_MIN + (int)(48.0f * expf(-dd * dd / 5.0f));
        } else if (st == XZ_READY) {
            /* Respiration lente : l'app ne parait jamais figee. */
            target = BAR_MIN + (int)(7.0f + 7.0f * sinf(t * 1.7f - (float)i * 0.42f));
        }

        if (target < BAR_MIN) {
            target = BAR_MIN;
        }
        if (target > BAR_MAX) {
            target = BAR_MAX;
        }

        /* Attaque rapide, retombee douce : c'est ce qui donne l'allure
           « analyseur » plutot qu'un scintillement. */
        const float cur = _bar_h[i];
        float next;
        if ((float)target > cur) {
            next = cur + ((float)target - cur) * 0.55f;
        } else {
            next = cur + ((float)target - cur) * 0.20f;
        }
        if (fabsf(next - (float)target) < 0.4f) {
            next = (float)target;
        }
        _bar_h[i] = next;

        const int h = (int)(next + 0.5f);
        lv_obj_set_height(b, h);
        lv_obj_set_y(b, (ANIM_STRIP_H - h) / 2);
    }

#if defined(XZ_ANIMTEST) || defined(XZ_ANIMLOG)
    /* Trace une fois par seconde : prouve que les barres bougent. */
    static uint32_t tick = 0;
    if (++tick % 25 == 0) {
        ESP_LOGI(TAG, "ANIM h=%d/%d/%d/%d/%d lvl=%d", (int)_bar_h[0], (int)_bar_h[6],
                 (int)_bar_h[12], (int)_bar_h[18], (int)_bar_h[25], level);
    }
#endif
}

void AppAssistant::on_anim(lv_timer_t *t)
{
    AppAssistant *self = static_cast<AppAssistant *>(t->user_data);
    if (self != nullptr) {
        self->animate();
    }
}

/* ------------------------------------------------------------------ */
/* Retour : on differe la fermeture hors du callback de l'evenement, pour
   ne pas detruire l'objet qui est en train de le traiter. */
static void close_app_async(void *p)
{
    static_cast<AppAssistant *>(p)->back();
}

void AppAssistant::cb_back(lv_event_t *e)
{
    AppAssistant *self = static_cast<AppAssistant *>(lv_event_get_user_data(e));
    if (self != nullptr) {
        lv_async_call(close_app_async, self);
    }
}

void AppAssistant::cb_talk(lv_event_t *e)
{
    AppAssistant *self = static_cast<AppAssistant *>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    XzClient &c = XzClient::get();

    switch (c.state()) {
    case XZ_READY:
        c.startListening();
        break;
    case XZ_LISTENING:
        c.stopListening();
        break;
    case XZ_SPEAKING:
        c.abortSpeaking();
        break;
    default:
        c.connectAsync();
        break;
    }
    self->refresh();
}
