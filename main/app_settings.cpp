/*
 * AppSettings - application Parametres de la "montre"
 *
 * Reprend les reglages du firmware d'usine : WiFi (choix du reseau +
 * mot de passe), affichage, son, horloge, capteurs, stockage (carte TF),
 * informations appareil.
 */
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <dirent.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "esp_log.h"
#include "esp_check.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "esp_flash.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "esp_vfs_fat.h"
#include "esp_codec_dev.h"
#include "bsp/esp-bsp.h"

#include "app_settings.hpp"
#include "icons/app_icons.h"
#include "private/esp_brookesia_utils.h"

static const char *TAG = "SETTINGS";
static AppSettings *s_self = nullptr;
static esp_codec_dev_handle_t s_spk = nullptr;

#define COL_BG      0x0E0E10
#define COL_CARD    0x1C1C1E
#define COL_TEXT    0xFFFFFF
#define COL_ACCENT  0x00E5A0
#define COL_DIM     0x8A8A8E
#define COL_WARN    0xFF9F0A

/* Identifiants des pages du menu */
enum {
    PAGE_WIFI = 0,
    PAGE_DISPLAY,
    PAGE_SOUND,
    PAGE_CLOCK,
    PAGE_SENSORS,
    PAGE_STORAGE,
    PAGE_DEVICE,
};

/* Etat partage entre les pages WiFi */
static char s_ssids[16][33];
static int  s_ssid_count = 0;
static char s_sel_ssid[33] = {0};
static lv_obj_t *s_kb = nullptr;
static lv_obj_t *s_ta = nullptr;
static lv_obj_t *s_status_lbl = nullptr;
static lv_timer_t *s_sensor_timer = nullptr;
static lv_obj_t *s_lbl_ax = nullptr;
static lv_obj_t *s_lbl_ay = nullptr;
static lv_obj_t *s_lbl_az = nullptr;
static lv_obj_t *s_lbl_gx = nullptr;
static lv_obj_t *s_lbl_gy = nullptr;
static lv_obj_t *s_lbl_gz = nullptr;
static lv_obj_t *s_lbl_tp = nullptr;

/* ------------------------------------------------------------------ */
/* utilitaires UI                                                      */
/* ------------------------------------------------------------------ */
static lv_obj_t *make_screen(lv_obj_t *parent)
{
    lv_obj_t *scr = lv_obj_create(parent);
    lv_obj_set_size(scr, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_radius(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 12, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

static lv_obj_t *make_title(lv_obj_t *parent, const char *text)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 6);
    return lbl;
}

static lv_obj_t *make_row_h(lv_obj_t *parent, const char *text, int id, int height)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_set_size(btn, LV_PCT(100), height);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_pad_left(btn, 16, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *arrow = lv_label_create(btn);
    lv_label_set_text(arrow, ">");
    lv_obj_set_style_text_color(arrow, lv_color_hex(COL_DIM), 0);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -16, 0);

    lv_obj_add_event_cb(btn, AppSettings::cb_menu, LV_EVENT_CLICKED, (void *)(intptr_t)id);
    return btn;
}

static lv_obj_t *make_row(lv_obj_t *parent, const char *text, int id)
{
    return make_row_h(parent, text, id, 64);
}

static lv_obj_t *make_back(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_set_size(btn, 64, 44);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "<");
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT), 0);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, AppSettings::cb_back, LV_EVENT_CLICKED, nullptr);
    return btn;
}

static lv_obj_t *make_text(lv_obj_t *parent, const char *text, int y, uint32_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 8, y);
    return lbl;
}

/* Bouton plein largeur, aligne en bas */
static lv_obj_t *make_action(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_set_size(btn, LV_PCT(100), 56);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    return btn;
}

/* ------------------------------------------------------------------ */
/* QMI8658 : accelerometre + gyroscope                                 */
/* ------------------------------------------------------------------ */
#define QMI8658_ADDR 0x6A
/* Sur cette carte le capteur repond en 0x6B (broche SA0 a l'etat haut) :
   qmi_init() essaie les deux adresses et retient celle qui repond. */
static uint8_t s_qmi_addr = QMI8658_ADDR;
/* CTRL2 = 0x95 : plage +-4 g, soit 8192 LSB par g. Verification faite sur
   la carte : au repos la norme de l'acceleration vaut 1.00 g. */
#define ACC_LSB      8192.0f
#define GYR_LSB      64.0f

static esp_err_t qmi_read(uint8_t reg, uint8_t *buf, size_t n)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) return ESP_ERR_INVALID_STATE;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = s_qmi_addr,
        .scl_speed_hz    = 400000,
    };
    i2c_master_dev_handle_t dev = nullptr;
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &dev);
    if (err != ESP_OK) return err;
    err = i2c_master_transmit_receive(dev, &reg, 1, buf, n, 200);
    i2c_master_bus_rm_device(dev);
    return err;
}

static esp_err_t qmi_write(uint8_t reg, uint8_t val)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) return ESP_ERR_INVALID_STATE;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = s_qmi_addr,
        .scl_speed_hz    = 400000,
    };
    i2c_master_dev_handle_t dev = nullptr;
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &dev);
    if (err != ESP_OK) return err;
    uint8_t tmp[2] = {reg, val};
    err = i2c_master_transmit(dev, tmp, 2, 200);
    i2c_master_bus_rm_device(dev);
    return err;
}

static bool qmi_init(void)
{
    /* Le capteur est en 0x6B sur cette carte, mais on essaie les deux
       adresses : c'est une broche de la carte qui decide. */
    static const uint8_t addrs[2] = {0x6A, 0x6B};
    for (int i = 0; i < 2; i++) {
        s_qmi_addr = addrs[i];
        uint8_t who = 0;
        if (qmi_read(0x00, &who, 1) == ESP_OK && who == 0x05) {
            qmi_write(0x02, 0x40);   /* ADDR_AI */
            qmi_write(0x03, 0x95);   /* accelerometre */
            qmi_write(0x04, 0xD5);   /* gyroscope */
            qmi_write(0x08, 0x03);   /* CTRL7 : active accel + gyro */
            ESP_LOGI(TAG, "QMI8658 trouve a 0x%02X", addrs[i]);
            return true;
        }
    }
    s_qmi_addr = QMI8658_ADDR;
    ESP_LOGW(TAG, "QMI8658 introuvable (ni 0x6A ni 0x6B)");
    return false;
}

/* ------------------------------------------------------------------ */
AppSettings::AppSettings() :
    ESP_Brookesia_PhoneApp("Parametres", &app_icon_settings,
                           true, true, true)
{
    s_self = this;
}

AppSettings::~AppSettings()
{
    if (s_sensor_timer) {
        lv_timer_del(s_sensor_timer);
        s_sensor_timer = nullptr;
    }
}

/* ------------------------------------------------------------------ */
bool AppSettings::run(void)
{
    ESP_LOGI(TAG, "ouverture de l'app Parametres");
    build_menu();
    return true;
}

bool AppSettings::back(void)
{
    if (_sub != nullptr) {
        close_page();
        return true;
    }
    if (_menu != nullptr) {
        lv_obj_del(_menu);
        _menu = nullptr;
    }
    ESP_BROOKESIA_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "notifyCoreClosed a echoue");
    return true;
}

/* ------------------------------------------------------------------ */
void AppSettings::build_menu(void)
{
    _menu = make_screen(lv_scr_act());
    make_title(_menu, "Parametres");

    lv_obj_t *list = lv_obj_create(_menu);
    lv_obj_set_size(list, LV_PCT(100), LV_PCT(78));
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    make_row_h(list, "WiFi",          PAGE_WIFI,    56);
    make_row_h(list, "Affichage",     PAGE_DISPLAY, 56);
    make_row_h(list, "Son",           PAGE_SOUND,   56);
    make_row_h(list, "Horloge",       PAGE_CLOCK,   56);
    make_row_h(list, "Capteurs",      PAGE_SENSORS, 56);
    make_row_h(list, "Stockage",      PAGE_STORAGE, 56);
    make_row_h(list, "Appareil",      PAGE_DEVICE,  56);
}

void AppSettings::close_page(void)
{
    if (s_sensor_timer) {
        lv_timer_del(s_sensor_timer);
        s_sensor_timer = nullptr;
    }
    if (_sub != nullptr) {
        lv_obj_del(_sub);
        _sub = nullptr;
    }
    s_kb = nullptr;
    s_ta = nullptr;
    s_status_lbl = nullptr;
    if (_menu != nullptr) {
        lv_obj_clear_flag(_menu, LV_OBJ_FLAG_HIDDEN);
    }
}

void AppSettings::open_page(int which)
{
    if (_menu != nullptr) {
        lv_obj_add_flag(_menu, LV_OBJ_FLAG_HIDDEN);
    }
    _sub = make_screen(lv_scr_act());
    make_back(_sub);

    switch (which) {
    case PAGE_WIFI:    build_wifi();    break;
    case PAGE_DISPLAY: build_display(); break;
    case PAGE_SOUND:   build_sound();   break;
    case PAGE_CLOCK:   build_clock();   break;
    case PAGE_SENSORS: build_sensors(); break;
    case PAGE_STORAGE: build_storage(); break;
    default:           build_device();  break;
    }
}

/* ------------------------------------------------------------------ */
/* WiFi : etat courant + acces a la liste des reseaux                  */
/* ------------------------------------------------------------------ */
void AppSettings::build_wifi(void)
{
    make_title(_sub, "WiFi");

    wifi_ap_record_t ap;
    bool connected = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
    char buf[384];
    char ipbuf[24];

    if (connected) {
        esp_netif_ip_info_t ip;
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        const char *ipstr = "-";
        if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
            snprintf(ipbuf, sizeof(ipbuf), IPSTR, IP2STR(&ip.ip));
            ipstr = ipbuf;
        }
        snprintf(buf, sizeof(buf), "Connecte : %s\nIP %s\nRSSI %d dBm  canal %d",
                 (char *)ap.ssid, ipstr, ap.rssi, ap.primary);
    } else {
        snprintf(buf, sizeof(buf), "Non connecte\nAucun reseau actif");
    }
    s_status_lbl = make_text(_sub, buf, 90, connected ? COL_TEXT : COL_WARN);

    /* Bouton du haut : choisir un autre reseau */
    lv_obj_t *btn_top = lv_obj_create(_sub);
    lv_obj_set_size(btn_top, LV_PCT(100), 52);
    lv_obj_align(btn_top, LV_ALIGN_BOTTOM_MID, 0, -70);
    lv_obj_set_style_bg_color(btn_top, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(btn_top, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_top, 14, 0);
    lv_obj_set_style_border_width(btn_top, 0, 0);
    lv_obj_t *l1 = lv_label_create(btn_top);
    lv_label_set_text(l1, "Choisir un reseau");
    lv_obj_set_style_text_color(l1, lv_color_hex(COL_TEXT), 0);
    lv_obj_center(l1);
    lv_obj_add_event_cb(btn_top, AppSettings::cb_wifi_scan, LV_EVENT_CLICKED, nullptr);

    make_action(_sub, "Relancer la connexion", AppSettings::cb_sync_ntp);
}

/* ------------------------------------------------------------------ */
void AppSettings::build_wifi_list(void)
{
    if (_sub != nullptr) {
        lv_obj_del(_sub);
        _sub = nullptr;
    }
    _sub = make_screen(lv_scr_act());
    make_back(_sub);
    make_title(_sub, "Reseaux WiFi");

    lv_obj_t *info = make_text(_sub, "Recherche en cours...", 80, COL_DIM);

    lv_obj_t *list = lv_obj_create(_sub);
    lv_obj_set_size(list, LV_PCT(100), LV_PCT(60));
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    wifi_scan_config_t sc = {};
    sc.show_hidden = 1;
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        lv_label_set_text_fmt(info, "Echec du scan (%s)", esp_err_to_name(err));
        return;
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 16) n = 16;
    if (n == 0) {
        lv_label_set_text(info, "Aucun reseau trouve");
        return;
    }

    wifi_ap_record_t *recs = (wifi_ap_record_t *)calloc(n, sizeof(wifi_ap_record_t));
    if (!recs) {
        lv_label_set_text(info, "Memoire insuffisante");
        return;
    }
    uint16_t got = n;
    esp_wifi_scan_get_ap_records(&got, recs);
    s_ssid_count = got;

    for (uint16_t i = 0; i < got; i++) {
        memcpy(s_ssids[i], recs[i].ssid, 32);
        s_ssids[i][32] = 0;

        char line[64];
        bool open_net = (recs[i].authmode == WIFI_AUTH_OPEN);
        /* %.32s : le SSID fait 32 octets au maximum, et borner la precision
           evite un debordement que GCC ne detecte qu'en -O2 (l'app Musique
           impose -O2 sur le composant, voir main/CMakeLists.txt). */
        snprintf(line, sizeof(line), "%.32s  %d dBm%s",
                 s_ssids[i][0] ? s_ssids[i] : "(cache)",
                 recs[i].rssi, open_net ? "  [libre]" : "");

        lv_obj_t *row = lv_obj_create(list);
        lv_obj_set_size(row, LV_PCT(100), 52);
        lv_obj_set_style_bg_color(row, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, line);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 12, 0);

        lv_obj_add_event_cb(row, AppSettings::cb_ap_selected, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
    }
    free(recs);

    lv_label_set_text_fmt(info, "%u reseau(x) - touchez pour vous connecter", got);
}

/* ------------------------------------------------------------------ */
void AppSettings::build_wifi_password(const char *ssid)
{
    if (_sub != nullptr) {
        lv_obj_del(_sub);
        _sub = nullptr;
    }
    _sub = make_screen(lv_scr_act());
    make_back(_sub);
    make_title(_sub, "Mot de passe");

    char buf[96];
    snprintf(buf, sizeof(buf), "Reseau : %s", ssid);
    make_text(_sub, buf, 80, COL_DIM);

    s_ta = lv_textarea_create(_sub);
    lv_textarea_set_one_line(s_ta, true);
    lv_textarea_set_password_mode(s_ta, true);
    lv_textarea_set_max_length(s_ta, 64);
    lv_textarea_set_placeholder_text(s_ta, "mot de passe");
    lv_obj_set_size(s_ta, LV_PCT(100), 56);
    lv_obj_align(s_ta, LV_ALIGN_TOP_MID, 0, 120);
    lv_obj_set_style_text_font(s_ta, &lv_font_montserrat_20, 0);

    s_kb = lv_keyboard_create(_sub);
    lv_obj_set_size(s_kb, LV_PCT(100), LV_PCT(50));
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_kb, s_ta);
    lv_obj_add_event_cb(s_kb, AppSettings::cb_kb_ready, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(s_kb, AppSettings::cb_kb_cancel, LV_EVENT_CANCEL, nullptr);
}

/* ------------------------------------------------------------------ */
void AppSettings::build_display(void)
{
    make_title(_sub, "Affichage");
    make_text(_sub, "Luminosite", 80, COL_TEXT);

    lv_obj_t *slider = lv_slider_create(_sub);
    lv_obj_set_width(slider, LV_PCT(90));
    lv_obj_align(slider, LV_ALIGN_TOP_MID, 0, 120);
    lv_slider_set_range(slider, 10, 100);
    lv_slider_set_value(slider, bsp_display_brightness_get(), LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, AppSettings::cb_brightness, LV_EVENT_VALUE_CHANGED, nullptr);
}

/* ------------------------------------------------------------------ */
void AppSettings::build_sound(void)
{
    make_title(_sub, "Son");
    make_text(_sub, "Volume haut-parleur", 80, COL_TEXT);

    if (s_spk == nullptr) {
        s_spk = bsp_audio_codec_speaker_init();
    }

    lv_obj_t *slider = lv_slider_create(_sub);
    lv_obj_set_width(slider, LV_PCT(90));
    lv_obj_align(slider, LV_ALIGN_TOP_MID, 0, 120);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, 60, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, AppSettings::cb_volume, LV_EVENT_VALUE_CHANGED, nullptr);

    make_text(_sub, s_spk ? "Codec ES8311 pret" : "Codec ES8311 indisponible", 180, COL_DIM);
    make_action(_sub, "Tester le son", AppSettings::cb_beep);
}

/* ------------------------------------------------------------------ */
void AppSettings::build_clock(void)
{
    make_title(_sub, "Horloge");

    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    char buf[160];
    snprintf(buf, sizeof(buf),
             "%02d:%02d:%02d\n%02d/%02d/%04d\n\nNTP : %s",
             tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec,
             tm_now.tm_mday, tm_now.tm_mon + 1, tm_now.tm_year + 1900,
             (now > 1700000000) ? "heure valide" : "non synchronisee");
    make_text(_sub, buf, 90, COL_TEXT);

    make_text(_sub, "Fuseau : Europe/Paris", 230, COL_DIM);
    make_action(_sub, "Synchroniser maintenant", AppSettings::cb_sync_ntp);
}

/* ------------------------------------------------------------------ */
void AppSettings::build_sensors(void)
{
    make_title(_sub, "Capteurs");

    if (!qmi_init()) {
        make_text(_sub, "QMI8658 introuvable sur le bus I2C.", 90, COL_WARN);
        return;
    }

    const int y0 = 88, dy = 34;
    make_text(_sub, "Accelerometre (g)", y0, COL_ACCENT);
    s_lbl_ax = make_text(_sub, "X 0.00", y0 + dy, COL_TEXT);
    s_lbl_ay = make_text(_sub, "Y 0.00", y0 + 2 * dy, COL_TEXT);
    s_lbl_az = make_text(_sub, "Z 0.00", y0 + 3 * dy, COL_TEXT);
    make_text(_sub, "Gyroscope (dps)", y0 + 4 * dy + 6, COL_ACCENT);
    s_lbl_gx = make_text(_sub, "X 0.0", y0 + 5 * dy + 6, COL_TEXT);
    s_lbl_gy = make_text(_sub, "Y 0.0", y0 + 6 * dy + 6, COL_TEXT);
    s_lbl_gz = make_text(_sub, "Z 0.0", y0 + 7 * dy + 6, COL_TEXT);
    s_lbl_tp = make_text(_sub, "Temperature --.- C", y0 + 8 * dy + 12, COL_DIM);

    s_sensor_timer = lv_timer_create([](lv_timer_t *t) {
        (void)t;
        uint8_t raw[12] = {0};
        if (qmi_read(0x35, raw, sizeof(raw)) != ESP_OK) {
            return;
        }
        int16_t ax = (int16_t)((raw[1] << 8) | raw[0]);
        int16_t ay = (int16_t)((raw[3] << 8) | raw[2]);
        int16_t az = (int16_t)((raw[5] << 8) | raw[4]);
        int16_t gx = (int16_t)((raw[7] << 8) | raw[6]);
        int16_t gy = (int16_t)((raw[9] << 8) | raw[8]);
        int16_t gz = (int16_t)((raw[11] << 8) | raw[10]);

        if (s_lbl_ax) lv_label_set_text_fmt(s_lbl_ax, "X %+.2f", ax / ACC_LSB);
        if (s_lbl_ay) lv_label_set_text_fmt(s_lbl_ay, "Y %+.2f", ay / ACC_LSB);
        if (s_lbl_az) lv_label_set_text_fmt(s_lbl_az, "Z %+.2f", az / ACC_LSB);
        if (s_lbl_gx) lv_label_set_text_fmt(s_lbl_gx, "X %+.1f", gx / GYR_LSB);
        if (s_lbl_gy) lv_label_set_text_fmt(s_lbl_gy, "Y %+.1f", gy / GYR_LSB);
        if (s_lbl_gz) lv_label_set_text_fmt(s_lbl_gz, "Z %+.1f", gz / GYR_LSB);

        uint8_t t8[2] = {0};
        if (s_lbl_tp && qmi_read(0x33, t8, 2) == ESP_OK) {
            int16_t traw = (int16_t)((t8[1] << 8) | t8[0]);
            lv_label_set_text_fmt(s_lbl_tp, "Temperature %.1f C", traw / 256.0f);
        }
    }, 300, nullptr);
}

/* ------------------------------------------------------------------ */
void AppSettings::build_storage(void)
{
    make_title(_sub, "Stockage");

    char buf[320];
    esp_err_t err = bsp_sdcard_mount();
    if (err == ESP_OK) {
        uint64_t total = 0, free_b = 0;
        esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &total, &free_b);

        int files = 0;
        DIR *dir = opendir(BSP_SD_MOUNT_POINT);
        if (dir) {
            struct dirent *e;
            while ((e = readdir(dir)) != nullptr) {
                if (e->d_name[0] != '.') files++;
            }
            closedir(dir);
        }
        snprintf(buf, sizeof(buf),
                 "Carte TF montee\nTotal %.2f Go\nLibre %.2f Go\n%d element(s) a la racine",
                 (double)total / (1024.0 * 1024.0 * 1024.0),
                 (double)free_b / (1024.0 * 1024.0 * 1024.0), files);
    } else {
        snprintf(buf, sizeof(buf), "Aucune carte TF detectee\n(code %s)",
                 esp_err_to_name(err));
    }
    make_text(_sub, buf, 90, COL_TEXT);
}

/* ------------------------------------------------------------------ */
void AppSettings::build_device(void)
{
    make_title(_sub, "Appareil");

    esp_chip_info_t ci;
    esp_chip_info(&ci);

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    uint8_t pct = 0;
    uint8_t reg = 0xA4;
    uint8_t bus_addr = 0x34;
    if (bsp_i2c_get_handle()) {
        i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = bus_addr,
            .scl_speed_hz    = 100000,
        };
        i2c_master_dev_handle_t dev = nullptr;
        if (i2c_master_bus_add_device(bus, &cfg, &dev) == ESP_OK) {
            i2c_master_transmit_receive(dev, &reg, 1, &pct, 1, 100);
            i2c_master_bus_rm_device(dev);
        }
    }

    uint32_t flash_size = 0;
    esp_flash_get_size(nullptr, &flash_size);
    size_t psram_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

    char buf[384];
    snprintf(buf, sizeof(buf),
             "ESP32-S3 rev %d  %d coeurs\n"
             "Flash %lu Mo  PSRAM %lu Mo\n"
             "MAC %02X:%02X:%02X:%02X:%02X:%02X\n"
             "Heap libre %lu o\n"
             "Batterie %u %%",
             ci.revision, ci.cores,
             (unsigned long)(flash_size / (1024 * 1024)),
             (unsigned long)(psram_size / (1024 * 1024)),
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             (unsigned long)esp_get_free_heap_size(), pct);

    make_text(_sub, buf, 90, COL_TEXT);
}

/* ------------------------------------------------------------------ */
/* callbacks                                                           */
/* ------------------------------------------------------------------ */
void AppSettings::cb_menu(lv_event_t *e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_self) s_self->open_page(id);
}

void AppSettings::cb_back(lv_event_t *e)
{
    (void)e;
    if (s_self) s_self->close_page();
}

void AppSettings::cb_brightness(lv_event_t *e)
{
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    int v = (int)lv_slider_get_value(slider);
    bsp_display_brightness_set(v);
}

void AppSettings::cb_volume(lv_event_t *e)
{
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    int v = (int)lv_slider_get_value(slider);
    if (s_spk) {
        esp_codec_dev_set_out_vol(s_spk, v);
    }
}

void AppSettings::cb_wifi_scan(lv_event_t *e)
{
    (void)e;
    if (s_self) s_self->build_wifi_list();
}

void AppSettings::cb_ap_selected(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_ssid_count) return;
    strncpy(s_sel_ssid, s_ssids[idx], sizeof(s_sel_ssid) - 1);
    s_sel_ssid[sizeof(s_sel_ssid) - 1] = 0;
    if (s_self) s_self->build_wifi_password(s_sel_ssid);
}

void AppSettings::cb_kb_ready(lv_event_t *e)
{
    (void)e;
    const char *pass = s_ta ? lv_textarea_get_text(s_ta) : "";

    wifi_config_t wc = {};
    memcpy(wc.sta.ssid, s_sel_ssid, strnlen(s_sel_ssid, 32));
    size_t plen = strnlen(pass, 64);
    memcpy(wc.sta.password, pass, plen);
    wc.sta.threshold.authmode = (plen == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "enregistrement du reseau echoue : %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "reseau \"%s\" enregistre, connexion...", s_sel_ssid);
    esp_wifi_disconnect();
    esp_wifi_connect();

    if (s_self) s_self->build_wifi();
}

void AppSettings::cb_kb_cancel(lv_event_t *e)
{
    (void)e;
    if (s_self) s_self->build_wifi_list();
}

void AppSettings::cb_sync_ntp(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "reconnexion WiFi + resynchronisation NTP demandees");
    esp_wifi_disconnect();
    esp_wifi_connect();
    if (s_self && s_self->_sub) {
        s_self->close_page();
        s_self->open_page(PAGE_WIFI);
    }
}

void AppSettings::cb_beep(lv_event_t *e)
{
    (void)e;
    if (!s_spk) {
        s_spk = bsp_audio_codec_speaker_init();
        if (!s_spk) {
            ESP_LOGE(TAG, "codec indisponible");
            return;
        }
    }

    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = 16;
    fs.channel = 1;
    fs.sample_rate = 16000;

    if (esp_codec_dev_open(s_spk, &fs) != ESP_OK) {
        ESP_LOGE(TAG, "ouverture du codec echouee");
        return;
    }
    esp_codec_dev_set_out_vol(s_spk, 70);

    const int n = 16000 * 3 / 10;               /* 300 ms */
    int16_t *pcm = (int16_t *)malloc(n * sizeof(int16_t));
    if (pcm) {
        const double w = 2.0 * M_PI * 440.0 / 16000.0;
        for (int i = 0; i < n; i++) {
            double env = (i < n / 10) ? (double)i / (n / 10) : 1.0;
            pcm[i] = (int16_t)(9000.0 * env * sin(w * i));
        }
        esp_codec_dev_write(s_spk, pcm, n * sizeof(int16_t));
        free(pcm);
        ESP_LOGI(TAG, "bip 440 Hz joue");
    }
    esp_codec_dev_close(s_spk);
}
