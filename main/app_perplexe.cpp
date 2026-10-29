/*
 * AppPerplexe - labyrinthe a bille (4eme application, page 4).
 *
 * Le principe : un plateau vu de dessus, des murs, des trous, une arrivee.
 * On incline la montre et la bille roule dans ce sens ; le doigt pose sur le
 * plateau la freine ; une secousse la ramene au dernier point de passage.
 * Dix niveaux, 100 points par niveau moins 10 par chute.
 *
 * Choix techniques :
 *  - L'accelerometre du QMI8658 est lu par une tache dediee a 30 Hz, JAMAIS
 *    par la tache LVGL : le bus I2C est partage avec le tactile, et un acces
 *    I2C depuis la tache graphique a deja fige la montre (voir le piege 7 de
 *    la base de reference). La tache publie l'inclinaison dans des atomiques,
 *    la boucle de jeu ne fait que les lire.
 *  - Pas de tache de rendu : la bille est un objet LVGL de 10 px deplace par
 *    lv_obj_set_pos, ce qui n'invalide que son ancien et son nouveau
 *    rectangle. Les murs sont dessines une fois par niveau, en fusionnant les
 *    segments horizontaux (environ 70 objets, pas 300).
 *  - La barre d'etat orange est conservee : l'app garde use_status_bar, son
 *    ecran commence a y=50 et rien n'est dessine au-dessus.
 */
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"

#include "app_perplexe.hpp"
#include "icons/app_icons.h"
#include "private/esp_brookesia_utils.h"
#include "levels_perplexe.h"

static const char *TAG = "PERPLEXE";

/* ------------------------------------------------------------------ */
/* Palette : bleu-vert profond, la famille de l'icone                  */
/* ------------------------------------------------------------------ */
#define COL_BG      0x0B1416
#define COL_FLOOR   0x152A2E
#define COL_WALL    0x2C5F5A
#define COL_WALL_HI 0x478A80
#define COL_BALL    0xF4FFFB
#define COL_HOLE    0x000000
#define COL_HOLE_RI 0x1B3A3C
#define COL_GOAL    0x2BE8B0
#define COL_CP      0xE8C15A
#define COL_CP_OFF  0x6B5A2A
#define COL_TEXT    0xEAF6F4
#define COL_DIM     0x7C9C9C
#define COL_CARD    0x0A1B1D

/* ------------------------------------------------------------------ */
/* Geometrie : 19 x 17 cases de 20 px = 380 x 340 px                   */
/* ------------------------------------------------------------------ */
#define PER_CELL    20
#define BD_W        (PERPLEXE_COLS * PER_CELL)
#define BD_H        (PERPLEXE_ROWS * PER_CELL)
#define BD_X        15
#define BD_Y        32

#define BALL_HALF   4.6f     /* demi-largeur de collision, en px */
#define BALL_DRAW   10       /* taille dessinee                  */

#define PER_ACC     1150.0f  /* px/s2 pour une inclinaison de 1 g */
#define PER_VMAX    210.0f   /* px/s                              */
#define PER_DAMP    5.2f     /* amortissement au rouleau          */
#define PER_DAMP_BRK 24.0f   /* amortissement doigt pose          */

/* ================================================================== */
/* 1. Etat partage entre la tache capteur et la boucle de jeu          */
/* ================================================================== */
static std::atomic<int>  s_tilt_x{0};       /* milli-g */
static std::atomic<int>  s_tilt_y{0};
static std::atomic<bool> s_shake{false};
static std::atomic<bool> s_imu_ok{false};
static std::atomic<bool> s_sensor_run{false};
static std::atomic<bool> s_task_alive{false};

/* Grille du niveau courant : copie de la chaine, 'S' et '*' remplaces. */
static char s_grid[PERPLEXE_ROWS][PERPLEXE_COLS];

static inline bool solid(int cx, int cy)
{
    if (cx < 0 || cy < 0 || cx >= PERPLEXE_COLS || cy >= PERPLEXE_ROWS) {
        return true;
    }
    return s_grid[cy][cx] == '#';
}

/* ================================================================== */
/* 2. Capteur : tache dediee, 30 Hz                                    */
/* ================================================================== */
/* Passage du repere du capteur a celui de l'ecran. Le QMI8658 est monte
   tourne d'un quart de tour sur cette carte : son axe X descend l'ecran
   et son axe Y va vers la gauche (releve sur la montre : incliner le bord
   droit vers le bas faisait monter la bille). Les deux endroits qui
   utilisent l'inclinaison passent par ici, pour ne pas diverger. */
static inline void imu_to_screen(float rx, float ry, float *ox, float *oy)
{
    *ox = -ry;      /* + = vers la droite de l'ecran */
    *oy = rx;       /* + = vers le bas de l'ecran    */
}
/* Sensibilite de l'accelerometre : CTRL2 = 0x95 selectionne la plage
   +-4 g, soit 8192 LSB par g. Mesure sur la carte : au repos |a| vaut
   1.00 g avec 8192, alors qu'avec 4096 (plage +-8 g) on lisait 2.00 g.
   Le gyroscope est en 64 LSB/(deg/s) ; il ne sert qu'au journal. */
#define ACC_LSB 8192.0f
#define GYR_LSB 64.0f

static esp_err_t qmi_rd(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(dev, &reg, 1, buf, n, 60);
}

static esp_err_t qmi_wr(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t t[2] = {reg, val};
    return i2c_master_transmit(dev, t, 2, 60);
}

/* Sonde une adresse : ouvre le peripherique, lit WHO_AM_I, referme si ce
   n'est pas le QMI8658. L'adresse peut etre 0x6A ou 0x6B selon la carte. */
static bool imu_probe(i2c_master_bus_handle_t bus, uint8_t addr,
                      i2c_master_dev_handle_t *out)
{
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address  = addr;
    cfg.scl_speed_hz    = 400000;

    i2c_master_dev_handle_t dev = nullptr;
    if (i2c_master_bus_add_device(bus, &cfg, &dev) != ESP_OK || dev == nullptr) {
        ESP_LOGW(TAG, "0x%02X : ajout au bus impossible", addr);
        return false;
    }

    uint8_t who = 0;
    const esp_err_t err = qmi_rd(dev, 0x00, &who, 1);
    ESP_LOGI(TAG, "sonde 0x%02X : err=%d WHO_AM_I=0x%02X", addr, (int)err, who);

    if (err == ESP_OK && who == 0x05) {
        *out = dev;
        return true;
    }
    i2c_master_bus_rm_device(dev);
    return false;
}

/* Cherche le capteur et l'initialise. Renvoie true si l'accelerometre et le
   gyroscope tournent. */
static bool imu_start(i2c_master_bus_handle_t bus, i2c_master_dev_handle_t *out)
{
    static const uint8_t addrs[2] = {0x6A, 0x6B};
    for (int i = 0; i < 2; i++) {
        i2c_master_dev_handle_t dev = nullptr;
        if (!imu_probe(bus, addrs[i], &dev)) {
            continue;
        }
        qmi_wr(dev, 0x02, 0x40);   /* ADDR_AI */
        qmi_wr(dev, 0x03, 0x95);   /* accelerometre */
        qmi_wr(dev, 0x04, 0xD5);   /* gyroscope */
        qmi_wr(dev, 0x08, 0x03);   /* CTRL7 : les deux actifs */
        *out = dev;
        ESP_LOGI(TAG, "QMI8658 pret a 0x%02X (accel 0x35, gyro 0x3B)", addrs[i]);
        return true;
    }
    return false;
}

static void sensor_task(void *arg)
{
    (void)arg;

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    i2c_master_dev_handle_t dev = nullptr;
    bool ok = false;

    if (bus == nullptr) {
        ESP_LOGE(TAG, "bus I2C indisponible : pas de capteur");
    } else {
        /* Le capteur met parfois quelques centaines de millisecondes a
           repondre apres l'ouverture de l'app : on insiste. */
        for (int attempt = 0; attempt < 6 && !ok; attempt++) {
            ok = imu_start(bus, &dev);
            if (!ok) {
                vTaskDelay(pdMS_TO_TICKS(250));
            }
        }
    }
    s_imu_ok.store(ok);

    float fx = 0.0f;
    float fy = 0.0f;
    int64_t last_shake = 0;
    int tick = 0;
    int retry = 0;

    while (s_sensor_run.load()) {
        if (!ok) {
            /* Une nouvelle tentative par seconde, sans cesser de dormir. */
            if (bus != nullptr && ++retry >= 30) {
                retry = 0;
                ok = imu_start(bus, &dev);
                s_imu_ok.store(ok);
            }
        }
        if (ok) {
            retry = 0;
            uint8_t raw[12] = {0};
            if (qmi_rd(dev, 0x35, raw, sizeof(raw)) == ESP_OK) {
                const int16_t rx = (int16_t)((raw[1] << 8) | raw[0]);
                const int16_t ry = (int16_t)((raw[3] << 8) | raw[2]);
                const int16_t rz = (int16_t)((raw[5] << 8) | raw[4]);
                const int16_t gx = (int16_t)((raw[7] << 8) | raw[6]);
                const int16_t gy = (int16_t)((raw[9] << 8) | raw[8]);
                const int16_t gz = (int16_t)((raw[11] << 8) | raw[10]);

                const float ax_g = rx / ACC_LSB;
                const float ay_g = ry / ACC_LSB;
                const float az_g = rz / ACC_LSB;

                /* Passe-bas : la main tremble, la bille doit rester calme. */
                fx += 0.40f * (ax_g - fx);
                fy += 0.40f * (ay_g - fy);
                s_tilt_x.store((int)(fx * 1000.0f));
                s_tilt_y.store((int)(fy * 1000.0f));

                /* Secousse : soit un a-coup (|a| tres loin de 1 g), soit une
                   rotation rapide. Un delai d'une seconde evite les rafales. */
                const float an = sqrtf(ax_g * ax_g + ay_g * ay_g + az_g * az_g);
                const float gdps = sqrtf((float)gx * gx + (float)gy * gy + (float)gz * gz) / GYR_LSB;
                const int64_t now = esp_timer_get_time();
                if (fabsf(an - 1.0f) > 0.80f &&
                    (now - last_shake) > 1200000) {
                    last_shake = now;
                    s_shake.store(true);
                }

                /* Une ligne toutes les deux secondes : de quoi verifier le
                   sens de l'inclinaison dans le journal. */
                if (++tick >= 60) {
                    tick = 0;
                    float ex = 0.0f;
                    float ey = 0.0f;
                    imu_to_screen(ax_g, ay_g, &ex, &ey);
                    ESP_LOGI(TAG,
                             "accel brut %+.2f %+.2f %+.2f g (|a|=%.2f) ; ecran %+.2f %+.2f ; gyro %.0f dps",
                             ax_g, ay_g, az_g, an, ex, ey, gdps);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(33));
    }

    if (dev != nullptr) {
        i2c_master_bus_rm_device(dev);
    }
    s_imu_ok.store(false);
    s_task_alive.store(false);
    ESP_LOGI(TAG, "tache capteur terminee");
    vTaskDelete(nullptr);
}

static void sensor_start(void)
{
    s_shake.store(false);
    s_sensor_run.store(true);
    if (s_task_alive.exchange(true)) {
        return;                       /* deja lancee : elle repart seule */
    }
    if (xTaskCreate(sensor_task, "per_imu", 4096, nullptr, 3, nullptr) != pdPASS) {
        s_task_alive.store(false);
        s_sensor_run.store(false);
        ESP_LOGE(TAG, "creation de la tache capteur impossible");
    }
}

static void sensor_stop(void)
{
    s_sensor_run.store(false);
    s_shake.store(false);
}

/* ================================================================== */
/* 3. Petits utilitaires d'interface                                   */
/* ================================================================== */
static lv_obj_t *make_label(lv_obj_t *parent, const char *txt, int x, int y,
                            const lv_font_t *font, uint32_t col)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *make_pill(lv_obj_t *parent, int x, int y, int w, int h,
                           const char *txt, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(COL_WALL_HI), 0);
    lv_obj_set_style_radius(b, h / 2, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_center(l);

    if (cb != nullptr) {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    }
    return b;
}

/* ================================================================== */
/* 4. Application                                                      */
/* ================================================================== */
AppPerplexe::AppPerplexe() :
    ESP_Brookesia_PhoneApp("Perplexe", &app_icon_perplexe, true, true, true)
{
}

AppPerplexe::~AppPerplexe()
{
    if (_timer) {
        lv_timer_del(_timer);
        _timer = nullptr;
    }
}

/* ------------------------------------------------------------------ */
void AppPerplexe::build_ui(void)
{
    if (_root) {
        lv_obj_del(_root);
        _root = nullptr;
    }

    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);

    /* Hauteur reellement adressable : l'ecran de l'app fait 452 px mais le
       coeur le decale de 50 px, la zone utile s'arrete donc a 402. */
    const lv_area_t &va = getVisualArea();
    int usable_h = 402;
    if (va.y1 > 0) {
        usable_h = lv_area_get_height(&va) - va.y1;
    }
    if (usable_h < 200 || usable_h > 520) {
        usable_h = 402;
    }

    _root = lv_obj_create(lv_scr_act());
    lv_obj_set_pos(_root, 0, 0);
    lv_obj_set_size(_root, LV_PCT(100), usable_h);
    lv_obj_set_style_bg_color(_root, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_root, 0, 0);
    lv_obj_set_style_radius(_root, 0, 0);
    lv_obj_set_style_pad_all(_root, 0, 0);
    lv_obj_clear_flag(_root, LV_OBJ_FLAG_SCROLLABLE);

    /* Le conteneur couvre tout l'ecran (452) pour qu'aucune bande ne reste
       peinte par le fond de l'ecran et laisse voir le lanceur (piege 17).
       Le contenu, lui, reste cale sur 402. */
    {
        const int screen_h = (int)lv_obj_get_height(lv_scr_act());
        if (screen_h > usable_h) {
            lv_obj_set_size(_root, LV_PCT(100), screen_h);
        }
    }

    /* --- bandeau de score ------------------------------------------ */
    _lbl_lvl = make_label(_root, "N 1/10", 14, 7, &lv_font_montserrat_14, COL_DIM);
    _lbl_score = make_label(_root, "SCORE 0", 0, 7, &lv_font_montserrat_14, COL_TEXT);
    lv_obj_align(_lbl_score, LV_ALIGN_TOP_MID, 0, 7);
    _lbl_time = make_label(_root, "0:00", 0, 7, &lv_font_montserrat_14, COL_DIM);
    lv_obj_align(_lbl_time, LV_ALIGN_TOP_RIGHT, -14, 7);

    /* --- plateau ---------------------------------------------------- */
    _board = lv_obj_create(_root);
    lv_obj_set_size(_board, BD_W, BD_H);
    lv_obj_set_pos(_board, BD_X, BD_Y);
    lv_obj_set_style_bg_color(_board, lv_color_hex(COL_FLOOR), 0);
    lv_obj_set_style_bg_opa(_board, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_board, 0, 0);
    lv_obj_set_style_radius(_board, 0, 0);
    lv_obj_set_style_pad_all(_board, 0, 0);
    lv_obj_clear_flag(_board, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_board, LV_OBJ_FLAG_CLICKABLE);
    /* Le doigt, n'importe ou sur le plateau : frein. */
    lv_obj_add_event_cb(_board, on_brake, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(_board, on_brake, LV_EVENT_RELEASED, this);
    lv_obj_add_event_cb(_board, on_brake, LV_EVENT_PRESS_LOST, this);

    /* --- la bille (creee avant les murs, remontee au premier plan a
           chaque chargement de niveau) ---------------------------------- */
    _ball = lv_obj_create(_board);
    lv_obj_set_size(_ball, BALL_DRAW, BALL_DRAW);
    lv_obj_set_style_radius(_ball, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(_ball, lv_color_hex(COL_BALL), 0);
    lv_obj_set_style_bg_opa(_ball, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_ball, 0, 0);
    lv_obj_set_style_pad_all(_ball, 0, 0);
    lv_obj_set_style_shadow_width(_ball, 6, 0);
    lv_obj_set_style_shadow_opa(_ball, 90, 0);
    lv_obj_clear_flag(_ball, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(_ball, LV_OBJ_FLAG_CLICKABLE);

    /* --- bandeau de message (par-dessus le plateau) ------------------ */
    _card = lv_obj_create(_root);
    lv_obj_set_size(_card, 262, 84);
    lv_obj_align(_card, LV_ALIGN_CENTER, 0, -24);
    lv_obj_set_style_bg_color(_card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(_card, 236, 0);
    lv_obj_set_style_border_width(_card, 2, 0);
    lv_obj_set_style_border_color(_card, lv_color_hex(COL_GOAL), 0);
    lv_obj_set_style_radius(_card, 16, 0);
    lv_obj_set_style_pad_all(_card, 4, 0);
    lv_obj_clear_flag(_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(_card, LV_OBJ_FLAG_CLICKABLE);

    _lbl_msg = lv_label_create(_card);
    lv_label_set_text(_lbl_msg, "");
    lv_obj_set_style_text_color(_lbl_msg, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(_lbl_msg, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(_lbl_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(_lbl_msg);
    lv_obj_add_flag(_card, LV_OBJ_FLAG_HIDDEN);

    /* --- pied : deux boutons et le compteur d'appuis ------------------ */
    _btn_restart = make_pill(_root, 12, 376, 92, 24, "REJOUER", on_restart, this);
    make_pill(_root, 306, 376, 92, 24, "QUITTER", on_quit, this);
    _lbl_touch = make_label(_root, "APPUI 0", 0, 380, &lv_font_montserrat_12, COL_DIM);
    lv_obj_align(_lbl_touch, LV_ALIGN_TOP_MID, 0, 380);

    ESP_LOGI(TAG, "interface : zone visible (%d,%d)-(%d,%d), plateau %dx%d en (%d,%d), conteneur %dx%d",
             va.x1, va.y1, va.x2, va.y2, BD_W, BD_H, BD_X, BD_Y,
             (int)lv_obj_get_width(_root), (int)lv_obj_get_height(_root));
}

/* ------------------------------------------------------------------ */
void AppPerplexe::clear_level(void)
{
    if (_board == nullptr) {
        return;
    }
    const int n = (int)lv_obj_get_child_count(_board);
    for (int i = n - 1; i >= 0; i--) {
        lv_obj_t *c = lv_obj_get_child(_board, i);
        if (c != nullptr && c != _ball) {
            lv_obj_del(c);
        }
    }
    for (int i = 0; i < 4; i++) {
        _cp_obj[i] = nullptr;
        _cp_taken[i] = false;
    }
    _nholes = 0;
    _ncps = 0;
}

void AppPerplexe::load_level(int idx)
{
    if (idx < 0 || idx >= PERPLEXE_LEVELS) {
        idx = 0;
    }
    if (_board == nullptr) {
        return;
    }

    _level = idx;
    _falls = 0;
    _elapsed = 0;
    _state = ST_BANNER;
    _wait = 1500;
    _brake = false;
    _sec_shown = -1;
    _msg_ms = -1;
    clear_level();

    const char *src = PERPLEXE_LEVEL[idx];
    int sx = 1;
    int sy = 1;
    int gx = PERPLEXE_COLS - 2;
    int gy = PERPLEXE_ROWS - 2;

    for (int y = 0; y < PERPLEXE_ROWS; y++) {
        for (int x = 0; x < PERPLEXE_COLS; x++) {
            char c = src[y * PERPLEXE_COLS + x];
            if (c == 'S') {
                sx = x;
                sy = y;
                c = '.';
            } else if (c == '*') {
                if (_ncps < 4) {
                    _cps[_ncps].x = x * PER_CELL + PER_CELL / 2.0f;
                    _cps[_ncps].y = y * PER_CELL + PER_CELL / 2.0f;
                    _ncps++;
                }
                c = '.';
            } else if (c == 'G') {
                gx = x;
                gy = y;
            } else if (c == 'o') {
                if (_nholes < 16) {
                    _holes[_nholes].x = x * PER_CELL + PER_CELL / 2.0f;
                    _holes[_nholes].y = y * PER_CELL + PER_CELL / 2.0f;
                    _nholes++;
                }
            }
            s_grid[y][x] = c;
        }
    }

    /* Murs : un rectangle par segment horizontal, pas un par case. */
    int walls = 0;
    for (int y = 0; y < PERPLEXE_ROWS; y++) {
        int x = 0;
        while (x < PERPLEXE_COLS) {
            if (s_grid[y][x] != '#') {
                x++;
                continue;
            }
            const int x0 = x;
            while (x < PERPLEXE_COLS && s_grid[y][x] == '#') {
                x++;
            }
            lv_obj_t *w = lv_obj_create(_board);
            lv_obj_set_pos(w, x0 * PER_CELL, y * PER_CELL);
            lv_obj_set_size(w, (x - x0) * PER_CELL, PER_CELL);
            lv_obj_set_style_bg_color(w, lv_color_hex(COL_WALL), 0);
            lv_obj_set_style_bg_opa(w, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(w, 0, 0);
            lv_obj_set_style_radius(w, 3, 0);
            lv_obj_set_style_pad_all(w, 0, 0);
            lv_obj_clear_flag(w, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(w, LV_OBJ_FLAG_CLICKABLE);
            walls++;
        }
    }

    /* Trous : un disque noir, plus sombre que le sol. */
    for (int i = 0; i < _nholes; i++) {
        lv_obj_t *h = lv_obj_create(_board);
        lv_obj_set_size(h, 14, 14);
        lv_obj_set_pos(h, (int)(_holes[i].x - 7.0f), (int)(_holes[i].y - 7.0f));
        lv_obj_set_style_radius(h, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(h, lv_color_hex(COL_HOLE), 0);
        lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(h, 1, 0);
        lv_obj_set_style_border_color(h, lv_color_hex(COL_HOLE_RI), 0);
        lv_obj_set_style_pad_all(h, 0, 0);
        lv_obj_clear_flag(h, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(h, LV_OBJ_FLAG_CLICKABLE);
    }

    /* Points de passage. */
    for (int i = 0; i < _ncps; i++) {
        lv_obj_t *c = lv_obj_create(_board);
        lv_obj_set_size(c, 9, 9);
        lv_obj_set_pos(c, (int)(_cps[i].x - 4.5f), (int)(_cps[i].y - 4.5f));
        lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(c, lv_color_hex(COL_CP), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(c, 0, 0);
        lv_obj_set_style_pad_all(c, 0, 0);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
        _cp_obj[i] = c;
        _cp_taken[i] = false;
    }

    /* Arrivee : un anneau et son coeur. */
    _goal.x = gx * PER_CELL + PER_CELL / 2.0f;
    _goal.y = gy * PER_CELL + PER_CELL / 2.0f;
    {
        lv_obj_t *ring = lv_obj_create(_board);
        lv_obj_set_size(ring, 24, 24);
        lv_obj_set_pos(ring, (int)(_goal.x - 12.0f), (int)(_goal.y - 12.0f));
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(ring, 3, 0);
        lv_obj_set_style_border_color(ring, lv_color_hex(COL_GOAL), 0);
        lv_obj_set_style_pad_all(ring, 0, 0);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *core = lv_obj_create(_board);
        lv_obj_set_size(core, 8, 8);
        lv_obj_set_pos(core, (int)(_goal.x - 4.0f), (int)(_goal.y - 4.0f));
        lv_obj_set_style_radius(core, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(core, lv_color_hex(COL_GOAL), 0);
        lv_obj_set_style_bg_opa(core, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(core, 0, 0);
        lv_obj_set_style_pad_all(core, 0, 0);
        lv_obj_clear_flag(core, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(core, LV_OBJ_FLAG_CLICKABLE);
    }

    /* Depart. */
    _start.x = sx * PER_CELL + PER_CELL / 2.0f;
    _start.y = sy * PER_CELL + PER_CELL / 2.0f;
    _cp = _start;
    place_ball(_start.x, _start.y);
    lv_obj_move_foreground(_ball);

    char a[24];
    snprintf(a, sizeof(a), "NIVEAU %d", idx + 1);
    msg(a, nullptr, -1);

    hud(true);
    ESP_LOGI(TAG, "niveau %d : %d trous, %d points de passage, %d murs, depart (%d,%d)",
             idx + 1, _nholes, _ncps, walls, sx, sy);
}

/* ------------------------------------------------------------------ */
void AppPerplexe::place_ball(float x, float y)
{
    _bx = x;
    _by = y;
    _vx = 0.0f;
    _vy = 0.0f;
    _px = (int)(x - BALL_DRAW / 2.0f);
    _py = (int)(y - BALL_DRAW / 2.0f);
    if (_ball != nullptr) {
        lv_obj_set_pos(_ball, _px, _py);
    }
}

void AppPerplexe::slide_x(float d)
{
    if (d == 0.0f) {
        return;
    }
    _bx += d;
    const int cy0 = (int)floorf((_by - BALL_HALF) / PER_CELL);
    const int cy1 = (int)floorf((_by + BALL_HALF) / PER_CELL);
    const int cx0 = (int)floorf((_bx - BALL_HALF) / PER_CELL);
    const int cx1 = (int)floorf((_bx + BALL_HALF) / PER_CELL);
    for (int cy = cy0; cy <= cy1; cy++) {
        for (int cx = cx0; cx <= cx1; cx++) {
            if (!solid(cx, cy)) {
                continue;
            }
            if (d > 0.0f) {
                _bx = (float)(cx * PER_CELL) - BALL_HALF - 0.01f;
            } else {
                _bx = (float)((cx + 1) * PER_CELL) + BALL_HALF + 0.01f;
            }
            _vx = 0.0f;
            return;
        }
    }
}

void AppPerplexe::slide_y(float d)
{
    if (d == 0.0f) {
        return;
    }
    _by += d;
    const int cx0 = (int)floorf((_bx - BALL_HALF) / PER_CELL);
    const int cx1 = (int)floorf((_bx + BALL_HALF) / PER_CELL);
    const int cy0 = (int)floorf((_by - BALL_HALF) / PER_CELL);
    const int cy1 = (int)floorf((_by + BALL_HALF) / PER_CELL);
    for (int cy = cy0; cy <= cy1; cy++) {
        for (int cx = cx0; cx <= cx1; cx++) {
            if (!solid(cx, cy)) {
                continue;
            }
            if (d > 0.0f) {
                _by = (float)(cy * PER_CELL) - BALL_HALF - 0.01f;
            } else {
                _by = (float)((cy + 1) * PER_CELL) + BALL_HALF + 0.01f;
            }
            _vy = 0.0f;
            return;
        }
    }
}

void AppPerplexe::step(float dt)
{
    /* Le capteur est monte tourne d'un quart de tour par rapport a l'ecran :
       imu_to_screen() remet les axes dans le repere du plateau. */
    const float sx = s_tilt_x.load() / 1000.0f;
    const float sy = s_tilt_y.load() / 1000.0f;
    float ax = 0.0f;
    float ay = 0.0f;
    imu_to_screen(sx, sy, &ax, &ay);

    /* Zone morte : une main posee n'est jamais parfaitement plate. */
    if (fabsf(ax) < 0.05f) {
        ax = 0.0f;
    }
    if (fabsf(ay) < 0.05f) {
        ay = 0.0f;
    }

    if (_brake) {
        ax *= 0.25f;
        ay *= 0.25f;
    }

    _vx += ax * PER_ACC * dt;
    _vy += ay * PER_ACC * dt;

    const float damp = _brake ? PER_DAMP_BRK : PER_DAMP;
    float f = 1.0f - damp * dt;
    if (f < 0.0f) {
        f = 0.0f;
    }
    _vx *= f;
    _vy *= f;

    if (_vx > PER_VMAX) {
        _vx = PER_VMAX;
    } else if (_vx < -PER_VMAX) {
        _vx = -PER_VMAX;
    }
    if (_vy > PER_VMAX) {
        _vy = PER_VMAX;
    } else if (_vy < -PER_VMAX) {
        _vy = -PER_VMAX;
    }

    /* Sous-pas : a 210 px/s et 40 ms, un pas ferait 8 px et traverserait
       un mur de 20 px. Quatre sous-pas ramenent le deplacement a 2 px. */
    const int sub = 4;
    for (int i = 0; i < sub; i++) {
        slide_x(_vx * dt / (float)sub);
        slide_y(_vy * dt / (float)sub);
    }

    /* Un trou avale la bille. */
    for (int i = 0; i < _nholes; i++) {
        const float dx = _bx - _holes[i].x;
        const float dy = _by - _holes[i].y;
        if (dx * dx + dy * dy < 30.0f) {
            on_fall();
            return;
        }
    }

    /* Arrivee. */
    {
        const float dx = _bx - _goal.x;
        const float dy = _by - _goal.y;
        if (dx * dx + dy * dy < 110.0f) {
            on_goal();
            return;
        }
    }

    /* Points de passage : le dernier touche devient le point de reprise. */
    for (int i = 0; i < _ncps; i++) {
        if (_cp_taken[i]) {
            continue;
        }
        const float dx = _bx - _cps[i].x;
        const float dy = _by - _cps[i].y;
        if (dx * dx + dy * dy < 70.0f) {
            _cp_taken[i] = true;
            _cp = _cps[i];
            if (_cp_obj[i] != nullptr) {
                lv_obj_set_style_bg_color(_cp_obj[i], lv_color_hex(COL_CP_OFF), 0);
            }
            ESP_LOGI(TAG, "point de passage %d atteint", i + 1);
        }
    }

    const int px = (int)(_bx - BALL_DRAW / 2.0f);
    const int py = (int)(_by - BALL_DRAW / 2.0f);
    if (px != _px || py != _py) {
        _px = px;
        _py = py;
        lv_obj_set_pos(_ball, px, py);
    }
}

/* ------------------------------------------------------------------ */
void AppPerplexe::on_fall(void)
{
    _falls++;
    _state = ST_FALL;
    _wait = 750;
    msg("CHUTE", nullptr, -1);
    ESP_LOGI(TAG, "chute %d (niveau %d)", _falls, _level + 1);
}

void AppPerplexe::respawn(void)
{
    place_ball(_cp.x, _cp.y);
    _state = ST_PLAY;
    msg_off();
    _msg_ms = 0;
}

void AppPerplexe::on_goal(void)
{
    int pts = 100 - 10 * _falls;
    if (pts < 20) {
        pts = 20;
    }
    _score += pts;

    char a[24];
    char b[40];
    snprintf(a, sizeof(a), "NIVEAU %d", _level + 1);
    snprintf(b, sizeof(b), "+%d POINTS", pts);
    msg(a, b, 1700);
    _state = ST_DONE;
    _wait = 1700;
    ESP_LOGI(TAG, "niveau %d termine en %u ms, %d chute(s), +%d points (total %d)",
             _level + 1, (unsigned)_elapsed, _falls, pts, _score);
}

void AppPerplexe::advance(void)
{
    if (_level + 1 < PERPLEXE_LEVELS) {
        load_level(_level + 1);
        return;
    }

    if (_score > _best) {
        _best = _score;
    }
    char b[40];
    snprintf(b, sizeof(b), "SCORE %d", _score);
    msg("PARTIE FINIE", b, -1);
    _state = ST_END;
    _wait = 6000;
    ESP_LOGI(TAG, "partie finie : %d points (record de la session : %d)", _score, _best);
}

/* ------------------------------------------------------------------ */
void AppPerplexe::msg(const char *a, const char *b, int life_ms)
{
    if (_card == nullptr) {
        return;
    }
    if (b != nullptr && b[0] != '\0') {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s\n%s", a, b);
        lv_label_set_text(_lbl_msg, buf);
    } else {
        lv_label_set_text(_lbl_msg, a);
    }
    lv_obj_clear_flag(_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_card);
    _msg_ms = life_ms;
}

void AppPerplexe::msg_off(void)
{
    if (_card != nullptr) {
        lv_obj_add_flag(_card, LV_OBJ_FLAG_HIDDEN);
    }
    _msg_ms = 0;
}

/* ------------------------------------------------------------------ */
void AppPerplexe::hud(bool force)
{
    if (_lbl_lvl == nullptr) {
        return;
    }
    if (force || _level != _lvl_shown) {
        _lvl_shown = _level;
        lv_label_set_text_fmt(_lbl_lvl, "N %d/%d", _level + 1, PERPLEXE_LEVELS);
    }
    if (force || _score != _score_shown) {
        _score_shown = _score;
        lv_label_set_text_fmt(_lbl_score, "SCORE %d", _score);
    }
    if (force || _state == ST_PLAY) {
        const int s = (int)(_elapsed / 1000);
        if (force || s != _sec_shown) {
            _sec_shown = s;
            lv_label_set_text_fmt(_lbl_time, "%d:%02d", s / 60, s % 60);
        }
    }
    if (force || _taps != _taps_shown) {
        _taps_shown = _taps;
        lv_label_set_text_fmt(_lbl_touch, "APPUI %d", _taps);
    }
}

void AppPerplexe::tick(void)
{
    if (_root == nullptr) {
        return;
    }

    const uint32_t now = lv_tick_get();
    uint32_t d = now - _last_tick;
    _last_tick = now;
    if (d < 4) {
        d = 4;
    }
    if (d > 40) {
        d = 40;
    }

    /* Secousse : retour au dernier point de passage. */
    if (s_shake.exchange(false)) {
        if (_state == ST_PLAY) {
            place_ball(_cp.x, _cp.y);
            msg("SECOUSSE", "point de reprise", 900);
            ESP_LOGI(TAG, "secousse : bille ramenee en (%.0f,%.0f)", _cp.x, _cp.y);
        }
    }

    if (_msg_ms > 0) {
        _msg_ms -= (int)d;
        if (_msg_ms <= 0) {
            msg_off();
        }
    }

    switch (_state) {
    case ST_BANNER:
        _wait -= (int)d;
        if (_wait <= 0) {
            _state = ST_PLAY;
            msg_off();
        }
        break;
    case ST_PLAY:
        _elapsed += d;
        step(d / 1000.0f);
        break;
    case ST_FALL:
        _wait -= (int)d;
        if (_wait <= 0) {
            respawn();
        }
        break;
    case ST_DONE:
        _wait -= (int)d;
        if (_wait <= 0) {
            advance();
        }
        break;
    case ST_END:
        _wait -= (int)d;
        if (_wait <= 0) {
            _score = 0;
            load_level(0);
        }
        break;
    default:
        break;
    }

    hud(false);
}

void AppPerplexe::on_tick(lv_timer_t *t)
{
    AppPerplexe *self = static_cast<AppPerplexe *>(t->user_data);
    if (self != nullptr) {
        self->tick();
    }
}

/* ------------------------------------------------------------------ */
void AppPerplexe::on_brake(lv_event_t *e)
{
    AppPerplexe *self = static_cast<AppPerplexe *>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        self->_brake = true;
        self->_taps++;
        lv_point_t p = {0, 0};
        lv_indev_t *ind = lv_indev_active();
        if (ind != nullptr) {
            lv_indev_get_point(ind, &p);
        }
        ESP_LOGI(TAG, "doigt : appui %d en (%d,%d) -> frein",
                 self->_taps, (int)p.x, (int)p.y);
        self->hud(false);
    } else {
        self->_brake = false;
    }
}

void AppPerplexe::on_restart(lv_event_t *e)
{
    AppPerplexe *self = static_cast<AppPerplexe *>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    ESP_LOGI(TAG, "bouton REJOUER : niveau %d relance", self->_level + 1);
    self->load_level(self->_level);
}

void AppPerplexe::on_quit(lv_event_t *e)
{
    AppPerplexe *self = static_cast<AppPerplexe *>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    ESP_LOGI(TAG, "bouton QUITTER");
    /* On ne detruit pas l'interface dans le callback de l'objet qu'on est en
       train de traiter : on repousse d'un tour de boucle. */
    lv_timer_create(do_quit, 40, self);
}

void AppPerplexe::do_quit(lv_timer_t *t)
{
    AppPerplexe *self = static_cast<AppPerplexe *>(t->user_data);
    lv_timer_del(t);              /* t->user_data lu AVANT de detruire le timer */
    if (self != nullptr) {
        self->back();
    }
}

/* ------------------------------------------------------------------ */
bool AppPerplexe::run(void)
{
    _closing = false;
    build_ui();
    _last_tick = lv_tick_get();
    load_level(0);

    if (_timer == nullptr) {
        _timer = lv_timer_create(on_tick, 16, this);
    }

    sensor_start();

    lv_indev_t *ind = lv_indev_get_next(nullptr);
    ESP_LOGI(TAG, "run OK : tactile %s, ecran %dx%d, pile LVGL %u o",
             (ind != nullptr) ? "present" : "ABSENT",
             (int)lv_obj_get_width(lv_scr_act()),
             (int)lv_obj_get_height(lv_scr_act()),
             (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    return true;
}

bool AppPerplexe::back(void)
{
    if (_closing) {
        return true;
    }
    _closing = true;

    sensor_stop();

    if (_timer) {
        lv_timer_del(_timer);
        _timer = nullptr;
    }
    if (_root) {
        lv_obj_del(_root);
        _root = nullptr;
    }
    _board = nullptr;
    _ball = nullptr;
    _card = nullptr;
    _lbl_msg = nullptr;
    _lbl_lvl = nullptr;
    _lbl_score = nullptr;
    _lbl_time = nullptr;
    _lbl_touch = nullptr;
    _btn_restart = nullptr;
    for (int i = 0; i < 4; i++) {
        _cp_obj[i] = nullptr;
    }

    ESP_BROOKESIA_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "notifyCoreClosed a echoue");
    ESP_LOGI(TAG, "back OK : retour au lanceur");
    return true;
}
