/*
 * montre - firmware personnalise ESP32-S3-Touch-AMOLED-2.06
 *
 * Base : esp-brookesia 0.5 (meme shell que le firmware d'usine)
 *        + BSP Waveshare (ecran CO5300, tactile FT3168, audio, SD, I2C)
 *
 * Lanceur 2x2 (4 icones par page) + barre d'etat batterie/WiFi.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "bsp/esp-bsp.h"
#include "esp_brookesia.hpp"
#include "private/esp_brookesia_utils.h"

#include <ctime>
#include <cstdlib>
#include <cstring>
#include <cstdio>

#include "app_settings.hpp"
#include "app_assistant.hpp"
#include "app_music.hpp"
#include "app_perplexe.hpp"

#if __has_include("xz_test.h")
#include "xz_test.h"
#include "xiaozhi_client.hpp"
#endif

static ESP_Brookesia_Phone *s_phone = nullptr;
static lv_obj_t *s_home_scr = nullptr;      /* ecran du lanceur, capture au demarrage */

/* ------------------------------------------------------------------ */
/* Garde du lanceur.                                                   */
/*                                                                    */
/* Un vrai doigt declenche LV_EVENT_PRESSED, qui AGRANDIT l'icone      */
/* (press_size) et force un recalcul de mise en page ; RELEASED la     */
/* remet. Un appui simule par programme (autotest) ne passe pas par    */
/* la : c'est pour cela que le defaut ne se voyait que sur la montre.  */
/* On neutralise donc la cause : des qu'une app est ouverte, les icones */
/* sont remises a leur taille normale puis masquees ; elles            */
/* reapparaissent au retour au lanceur.                                */
/* ------------------------------------------------------------------ */
#define ICON_DEFAULT_SIZE 150      /* icon.image.default_size du stylesheet */

static void launcher_icons_show(lv_obj_t *parent, bool show)
{
    uint32_t n = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *ch = lv_obj_get_child(parent, i);
        if (lv_obj_check_type(ch, &lv_image_class)) {
            lv_area_t a;
            lv_obj_get_coords(ch, &a);
            if (lv_area_get_width(&a) >= 80) {
                /* Remet l'echelle et la taille d'origine avant de masquer :
                   une icone restee agrandie par un PRESSED sans RELEASED
                   reapparaitrait trop grosse. */
                lv_image_set_scale(ch, LV_SCALE_NONE);
                lv_obj_set_size(ch, ICON_DEFAULT_SIZE, ICON_DEFAULT_SIZE);
                lv_obj_refr_size(ch);
                if (show) {
                    lv_obj_clear_flag(ch, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(ch, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
        launcher_icons_show(ch, show);
    }
}

/* ------------------------------------------------------------------ */
/* Repeint force de la bande du haut (lignes 0 a 49).                  */
/*                                                                    */
/* Mesure (compteur d'ecritures par ligne) : quand une app est         */
/* ouverte, AUCUNE ligne 0..49 n'est ecrite, jamais. Cette bande        */
/* appartient a la barre d'etat, sur la couche systeme, et LVGL ne la   */
/* redessine que si son contenu change. Tout ce que le lanceur a laisse */
/* dans ces 50 lignes y reste donc fige.                              */
/*                                                                    */
/* D'ou le defaut : le doigt agrandit l'icone (LV_EVENT_PRESSED), son   */
/* bord haut passe de quelques pixels au-dessus de y=50, dans la bande  */
/* gelee, et ces 2 ou 3 lignes y restent collees sous l'app.            */
/* ------------------------------------------------------------------ */
#define TOP_BAND_HEIGHT 50

static void repaint_top_band(void)
{
    lv_obj_t *sys = lv_layer_sys();
    if (sys == nullptr) {
        return;
    }
    lv_area_t band;
    band.x1 = 0;
    band.y1 = 0;
    band.x2 = lv_display_get_horizontal_resolution(lv_display_get_default()) - 1;
    band.y2 = TOP_BAND_HEIGHT - 1;
    lv_obj_invalidate_area(sys, &band);
}

/* L'icone « statut » que brookesia ajoute pour l'app ouverte est une image
   de 150x150 posee dans un emplacement de 30x30 de la barre : elle est
   rognee et laisse un morceau de l'icone de l'app dans le haut de l'ecran.
   On la masque (seule une image de cette taille y est concernee). */
static void hide_status_app_icon(lv_obj_t *parent)
{
    uint32_t n = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *ch = lv_obj_get_child(parent, i);
        if (lv_obj_check_type(ch, &lv_image_class)) {
            lv_area_t a;
            lv_obj_get_coords(ch, &a);
            if (lv_area_get_width(&a) >= 80 && a.y1 < TOP_BAND_HEIGHT && a.y2 >= -TOP_BAND_HEIGHT) {
                lv_obj_add_flag(ch, LV_OBJ_FLAG_HIDDEN);
            }
        }
        hide_status_app_icon(ch);
    }
}

/* Emplacement reserve aux sondes de developpement : vide en production. */
static void openprobe_arm(void) {}

static void launcher_guard_cb(lv_timer_t *t)
{
    if (s_phone == nullptr || s_home_scr == nullptr) {
        return;
    }
    static bool app_open_prev = false;
    const bool app_open = (lv_scr_act() != s_home_scr);
    if (app_open) {
        /* a chaque tour : brookesia peut ajouter l'icone apres le changement
           d'ecran. Masquer une icone deja masquee ne coute rien. */
        hide_status_app_icon(lv_layer_sys());
    }
    if (app_open == app_open_prev) {
        return;
    }
    app_open_prev = app_open;

    launcher_icons_show(s_home_scr, !app_open);
    lv_obj_invalidate(s_home_scr);
    repaint_top_band();
    ESP_LOGI("MONTRE", "icones du lanceur %s", app_open ? "masquees (app ouverte)"
                                                   : "reaffichees");
    if (app_open) {
        openprobe_arm();
    }
}

#ifdef XZ_AUTOTEST
/* Page du lanceur ouverte par l'autotest (0 = Parametres, 1 = Assistant,
   2 = Musique) et delai avant l'ouverture. */
#ifndef XZ_AUTOTEST_PAGE
#define XZ_AUTOTEST_PAGE 1
#endif
#ifndef XZ_AUTOTEST_MS
#define XZ_AUTOTEST_MS 10000
#endif

/* ------------------------------------------------------------------ */
/* Reproduit un appui sur l'icone "Assistant" sans toucher l'ecran :
   on retrouve l'objet image du lanceur et on lui envoie LV_EVENT_CLICKED,
   exactement ce que fait le doigt.                                     */
static lv_obj_t *find_big_image(lv_obj_t *parent, int min_w, int min_center_x)
{
    uint32_t n = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(parent, i);
        if (lv_obj_check_type(c, &lv_image_class)) {
            lv_area_t a;
            lv_obj_get_coords(c, &a);
            if (lv_area_get_width(&a) >= min_w && (a.x1 + a.x2) / 2 >= min_center_x) {
                return c;
            }
        }
        lv_obj_t *r = find_big_image(c, min_w, min_center_x);
        if (r != nullptr) {
            return r;
        }
    }
    return nullptr;
}

/* Declenche l'ouverture de l'app DEPUIS la tache LVGL (un timer LVGL tourne
   dans lv_timer_handler) et reproduit la SEQUENCE DU DOIGT : PRESSED (qui
   agrandit l'icone et force un recalcul de mise en page), puis RELEASED et
   CLICKED. Un simple CLICKED ne passait pas par l'agrandissement, d'ou des
   essais qui ne reproduisaient pas le defaut de la montre. */
static lv_obj_t *s_press_icon = nullptr;

static void autotest_release(lv_timer_t *t)
{
    lv_timer_del(t);
    if (s_press_icon == nullptr) {
        return;
    }
    lv_obj_send_event(s_press_icon, LV_EVENT_RELEASED, nullptr);
    lv_obj_send_event(s_press_icon, LV_EVENT_CLICKED, nullptr);
    s_press_icon = nullptr;
}

static void autotest_open_app(lv_timer_t *t)
{
    lv_timer_del(t);
    if (s_phone == nullptr) {
        return;
    }
    /* Une app par page : on va d'abord sur la page demandee. */
    s_phone->getHome().getAppLauncher()->scrollToPage(XZ_AUTOTEST_PAGE);

    lv_obj_t *icon = find_big_image(lv_scr_act(), 80, 0);
    if (icon == nullptr) {
        ESP_LOGE("AUTOTEST", "icone introuvable sur la page %d", (int)XZ_AUTOTEST_PAGE);
        return;
    }
    lv_area_t a;
    lv_obj_get_coords(icon, &a);
    ESP_LOGI("AUTOTEST", "appui (tache LVGL) x=%d y=%d", (a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
    s_press_icon = icon;
    lv_obj_send_event(icon, LV_EVENT_PRESSED, nullptr);
    lv_timer_create(autotest_release, 120, nullptr);
}

#ifdef XZ_AUTOTEST_BACK
/* Retrouve le parent d'un libelle dont le texte vaut `text` (le bouton retour
   contient le libelle "<"). */
static lv_obj_t *find_label_parent(lv_obj_t *parent, const char *text)
{
    uint32_t n = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(parent, i);
        if (lv_obj_check_type(c, &lv_label_class)) {
            const char *t = lv_label_get_text(c);
            if (t != nullptr && strcmp(t, text) == 0) {
                return lv_obj_get_parent(c);
            }
        }
        lv_obj_t *r = find_label_parent(c, text);
        if (r != nullptr) {
            return r;
        }
    }
    return nullptr;
}

/* Appuie sur le bouton retour, toujours depuis la tache LVGL. */
static void autotest_press_back(lv_timer_t *t)
{
    lv_timer_del(t);
    lv_obj_t *btn = find_label_parent(lv_scr_act(), "RETOUR AU LANCEUR");
    if (btn == nullptr) {
        ESP_LOGE("AUTOTEST", "bouton retour introuvable");
        return;
    }
    lv_area_t a;
    lv_obj_get_coords(btn, &a);
    ESP_LOGI("AUTOTEST", "appui retour x=%d..%d y=%d..%d", a.x1, a.x2, a.y1, a.y2);
    lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
}
#endif /* XZ_AUTOTEST_BACK */
#endif

#if defined(XZ_SNAPSHOT) || defined(XZ_ICONSNAP) || defined(XZ_MUSSNAP)
/* ------------------------------------------------------------------ */
/* Capture d'ecran de diagnostic : rend l'ecran dans un buffer LVGL et
   l'envoie en base64 sur le port serie (voir tools/snap.py).            */
static const char *B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void dump_line(const unsigned char *buf, int len)
{
    char out[1400];
    int o = 0;
    int i = 0;
    for (; i + 2 < len; i += 3) {
        unsigned v = (buf[i] << 16) | (buf[i + 1] << 8) | buf[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = B64[(v >> 6) & 63];
        out[o++] = B64[v & 63];
    }
    if (i < len) {
        unsigned v = buf[i] << 16;
        if (i + 1 < len) {
            v |= buf[i + 1] << 8;
        }
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = i + 1 < len ? B64[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o++] = '\n';
    fwrite(out, 1, (size_t)o, stdout);
    fflush(stdout);
}

static void snap_dump(const char *tag)
{
    lv_draw_buf_t *snap = lv_snapshot_take(lv_scr_act(), LV_COLOR_FORMAT_RGB565);
    if (snap == nullptr) {
        printf("SNAP FAIL %s\n", tag);
        return;
    }
    const int step = 3;
    int w = snap->header.w, h = snap->header.h;
    int ow = w / step, oh = h / step;
    esp_log_level_set("*", ESP_LOG_NONE);
    printf("SNAP BEGIN %s %d %d\n", tag, ow, oh);
    static unsigned char row[1200];
    for (int y = 0; y < oh; y++) {
        const unsigned char *line = (const unsigned char *)snap->data + (size_t)(y * step) * snap->header.stride;
        for (int x = 0; x < ow; x++) {
            const unsigned char *p = line + (size_t)(x * step) * 2;
            row[x * 2] = p[0];
            row[x * 2 + 1] = p[1];
        }
        dump_line(row, ow * 2);
    }
    printf("SNAP END\n");
    fflush(stdout);
    esp_log_level_set("*", ESP_LOG_INFO);
    lv_draw_buf_destroy(snap);
}

#ifdef XZ_SNAPSHOT
static void snapshot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(9000));

    bsp_display_lock(0);
    /* Une app par page : on va d'abord sur la page de l'Assistant. */
    if (s_phone != nullptr) {
        s_phone->getHome().getAppLauncher()->scrollToPage(1);
    }
    bsp_display_unlock();
    vTaskDelay(pdMS_TO_TICKS(700));

    bsp_display_lock(0);
    lv_obj_t *icon = find_big_image(lv_scr_act(), 80, 0);
    if (icon == nullptr) {
        ESP_LOGE("AUTOTEST", "icone introuvable sur la page 2");
    } else {
        lv_area_t a;
        lv_obj_get_coords(icon, &a);
        ESP_LOGI("AUTOTEST", "clic en x=%d y=%d", (a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
        lv_obj_send_event(icon, LV_EVENT_CLICKED, nullptr);
    }
    bsp_display_unlock();

    /* laisser la connexion aboutir (OTA + websocket) */
    vTaskDelay(pdMS_TO_TICKS(8000));

    bsp_display_lock(0);
    snap_dump("APP");
    bsp_display_unlock();

    vTaskDelete(nullptr);
}
#endif

#ifdef XZ_ICONSNAP
/* Verification des icones du lanceur : une capture par page, sans rien
   ouvrir. Les trois pages tiennent largement dans les 12 s qui precedent
   l'ouverture automatique de l'app. */
static void iconsnap_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(5000));

    bsp_display_lock(0);
    snap_dump("PAGE1");
    bsp_display_unlock();

    for (int page = 1; page <= 2; page++) {
        bsp_display_lock(0);
        if (s_phone != nullptr) {
            s_phone->getHome().getAppLauncher()->scrollToPage(page);
        }
        bsp_display_unlock();
        vTaskDelay(pdMS_TO_TICKS(1200));

        bsp_display_lock(0);
        snap_dump(page == 1 ? "PAGE2" : "PAGE3");
        bsp_display_unlock();
    }

    vTaskDelete(nullptr);
}
#endif

#ifdef XZ_MUSSNAP
/* L'app Musique a besoin de temps pour demarrer l'audio : on capture trois
   fois, a ~4 s d'intervalle, pour voir le piano roll se remplir et la
   jauge d'energie bouger. A associer a XZ_AUTOTEST_PAGE=2. */
static void mussnap_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(14000));

    bsp_display_lock(0);
    snap_dump("MUS1");
    bsp_display_unlock();
    vTaskDelay(pdMS_TO_TICKS(4000));

    bsp_display_lock(0);
    snap_dump("MUS2");
    bsp_display_unlock();
    vTaskDelay(pdMS_TO_TICKS(4000));

    bsp_display_lock(0);
    snap_dump("MUS3");
    bsp_display_unlock();

    vTaskDelete(nullptr);
}
#endif
#endif /* captures de diagnostic */

#define LVGL_PORT_INIT_CONFIG() \
    {                           \
        .task_priority = 4,     \
        .task_stack = 16 * 1024,\
        .task_affinity = -1,    \
        .task_max_sleep_ms = 500,\
        .timer_period_ms = 5,   \
    }

static const char *TAG = "MONTRE";

/* ------------------------------------------------------------------ */
/* I2C : acces generique (AXP2101 alimentation, PCF85063 horloge)      */
/* ------------------------------------------------------------------ */
#define AXP2101_ADDR  0x34
#define PCF85063_ADDR 0x51

static esp_err_t i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) return ESP_ERR_INVALID_STATE;

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = 100000,
    };
    i2c_master_dev_handle_t dev = nullptr;
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &dev);
    if (err != ESP_OK) return err;
    err = i2c_master_transmit_receive(dev, &reg, 1, buf, len, 200);
    i2c_master_bus_rm_device(dev);
    return err;
}

static esp_err_t i2c_write_reg(uint8_t addr, uint8_t reg, const uint8_t *buf, size_t len)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) return ESP_ERR_INVALID_STATE;

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = 100000,
    };
    i2c_master_dev_handle_t dev = nullptr;
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &dev);
    if (err != ESP_OK) return err;

    uint8_t tmp[8];
    if (len + 1 > sizeof(tmp)) {
        i2c_master_bus_rm_device(dev);
        return ESP_ERR_INVALID_SIZE;
    }
    tmp[0] = reg;
    for (size_t i = 0; i < len; i++) tmp[1 + i] = buf[i];
    err = i2c_master_transmit(dev, tmp, len + 1, 200);
    i2c_master_bus_rm_device(dev);
    return err;
}

/* ------------------------------------------------------------------ */
/* AXP2101 : pourcentage de batterie                                   */
/* ------------------------------------------------------------------ */
static esp_err_t axp_read(uint8_t reg, uint8_t *val)
{
    return i2c_read_reg(AXP2101_ADDR, reg, val, 1);
}

/* ------------------------------------------------------------------ */
/* PCF85063ATL : horloge temps reel (l'heure survit a une coupure)     */
/* ------------------------------------------------------------------ */
static inline uint8_t bcd2dec(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static inline uint8_t dec2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static bool rtc_get(struct tm *out)
{
    uint8_t r[7];
    /* 0x04 = secondes ; les OSC bits de poids fort doivent etre masques */
    if (i2c_read_reg(PCF85063_ADDR, 0x04, r, sizeof(r)) != ESP_OK) return false;

    struct tm t = {};
    t.tm_sec  = bcd2dec(r[0] & 0x7F);
    t.tm_min  = bcd2dec(r[1] & 0x7F);
    t.tm_hour = bcd2dec(r[2] & 0x3F);
    t.tm_mday = bcd2dec(r[3] & 0x3F);
    t.tm_wday = (int)(r[4] & 0x07);
    t.tm_mon  = (int)bcd2dec(r[5] & 0x1F) - 1;
    t.tm_year = 2000 + (int)bcd2dec(r[6]) - 1900;
    t.tm_isdst = -1;

    if (t.tm_year < 120 || t.tm_mon < 0 || t.tm_mon > 11 ||
        t.tm_mday < 1 || t.tm_mday > 31 || t.tm_hour > 23 ||
        t.tm_min > 59 || t.tm_sec > 59) {
        return false;
    }
    *out = t;
    return true;
}

static void rtc_set(const struct tm *in)
{
    uint8_t w[7];
    w[0] = dec2bcd((uint8_t)in->tm_sec);
    w[1] = dec2bcd((uint8_t)in->tm_min);
    w[2] = dec2bcd((uint8_t)in->tm_hour);
    w[3] = dec2bcd((uint8_t)in->tm_mday);
    w[4] = (uint8_t)in->tm_wday;
    w[5] = dec2bcd((uint8_t)(in->tm_mon + 1));
    w[6] = dec2bcd((uint8_t)(in->tm_year + 1900 - 2000));

    uint8_t ctrl1 = 0;
    if (i2c_read_reg(PCF85063_ADDR, 0x00, &ctrl1, 1) == ESP_OK) {
        ctrl1 &= (uint8_t)~0x20;      /* STOP = 0 : le compteur tourne */
        i2c_write_reg(PCF85063_ADDR, 0x00, &ctrl1, 1);
    }
    i2c_write_reg(PCF85063_ADDR, 0x04, w, sizeof(w));
}


/* ------------------------------------------------------------------ */
/* WiFi : connexion automatique + resynchronisation a chaque coupure   */
/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* Appele quand NTP a fourni l'heure : on la grave dans le PCF85063    */
/* ------------------------------------------------------------------ */
static void sntp_synced_cb(struct timeval *tv)
{
    time_t now = tv ? tv->tv_sec : time(nullptr);
    struct tm tm_local;
    localtime_r(&now, &tm_local);

    ESP_LOGI(TAG, "NTP : heure synchronisee -> %02d:%02d:%02d le %02d/%02d/%04d",
             tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec,
             tm_local.tm_mday, tm_local.tm_mon + 1, tm_local.tm_year + 1900);

    rtc_set(&tm_local);
    ESP_LOGI(TAG, "RTC PCF85063 : heure ecrite");
}

static void on_got_ip(void)
{
    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_set_time_sync_notification_cb(sntp_synced_cb);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_init();
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        wifi_config_t wc = {};
        if (esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK && wc.sta.ssid[0] != 0) {
            /* canal 0 + pas de BSSID memorise => scan complet a chaque fois */
            wc.sta.channel = 0;
            wc.sta.bssid_set = 0;
            esp_wifi_set_config(WIFI_IF_STA, &wc);
            ESP_LOGI(TAG, "WiFi : connexion a \"%s\"", (char *)wc.sta.ssid);
        } else {
            ESP_LOGW(TAG, "WiFi : aucun reseau enregistre en NVS");
        }
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "WiFi : deconnecte (raison %d), nouvelle tentative", d->reason);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "WiFi : connecte, IP " IPSTR, IP2STR(&e->ip_info.ip));
        on_got_ip();
    }
}

/* ------------------------------------------------------------------ */
/* Services systeme : NVS, netif, WiFi en mode station                 */
/* ------------------------------------------------------------------ */
static void services_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr, nullptr));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "services: NVS + netif + WiFi STA demarres");
}

/* ------------------------------------------------------------------ */
/* Date dans la barre d'etat (Brookesia 0.5 n'affiche que l'heure)     */
/* ------------------------------------------------------------------ */
static lv_obj_t *s_date_label = nullptr;
static int s_date_last = -1;

static void add_date_to_status_bar(void)
{
    lv_obj_t *bar = s_phone->getHome().getStatusBar()->getMainObj();
    if (!bar) return;
    /* Zone 0 = celle de l'horloge ; le label se place apres elle */
    lv_obj_t *area0 = lv_obj_get_child(bar, 0);
    if (!area0) return;

    lv_obj_t *lbl = lv_label_create(area0);
    if (!lbl) return;
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_pad_left(lbl, 6, 0);
    lv_label_set_text(lbl, "--/--");
    s_date_label = lbl;
}

/* ------------------------------------------------------------------ */
/* Mise a jour periodique de la barre d'etat                           */
/* ------------------------------------------------------------------ */
static void status_timer_cb(lv_timer_t *t)
{
    ESP_Brookesia_Phone *phone = (ESP_Brookesia_Phone *)t->user_data;
    if (!phone) return;

    time_t now;
    struct tm tm_now;
    time(&now);
    localtime_r(&now, &tm_now);

    /* Confirmation unique : l'heure est-elle synchronisee ? */
    static bool logged_time = false;
    if (!logged_time && now > 1700000000) {
        logged_time = true;
        ESP_LOGI(TAG, "heure locale %02d:%02d:%02d le %02d/%02d/%04d (TZ Europe/Paris)",
                 tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec,
                 tm_now.tm_mday, tm_now.tm_mon + 1, tm_now.tm_year + 1900);
    }

    phone->getHome().getStatusBar()->setClock(tm_now.tm_hour, tm_now.tm_min);

    /* Date (mise a jour une fois par jour) */
    int day_key = tm_now.tm_mday * 100 + tm_now.tm_mon;
    if (s_date_label && day_key != s_date_last) {
        s_date_last = day_key;
        lv_label_set_text_fmt(s_date_label, "%02d/%02d", tm_now.tm_mday, tm_now.tm_mon + 1);
    }

    /* Batterie : AXP2101, registre 0xA4 (pourcentage) */
    uint8_t pct = 0;
    esp_err_t berr = axp_read(0xA4, &pct);
    static bool logged_batt = false;
    if (!logged_batt) {
        logged_batt = true;
        ESP_LOGI(TAG, "batterie : lecture AXP2101 0xA4 -> %s, valeur %u",
                 esp_err_to_name(berr), pct);
    }
    if (berr == ESP_OK && pct > 0 && pct <= 100) {
        phone->getHome().getStatusBar()->setBatteryPercent(false, pct);
    }

    /* WiFi : icone = nombre de barres selon le RSSI */
    using WifiState = ESP_Brookesia_StatusBar::WifiState;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        WifiState st = (ap.rssi > -55) ? WifiState::SIGNAL_3
                       : (ap.rssi > -70) ? WifiState::SIGNAL_2
                       : WifiState::SIGNAL_1;
        phone->getHome().getStatusBar()->setWifiIconState(st);
    } else {
        phone->getHome().getStatusBar()->setWifiIconState(WifiState::DISCONNECTED);
    }
}

/* ------------------------------------------------------------------ */
extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== montre : demarrage ===");

    /* Fuseau horaire applique des le boot : l'heure affichee est localisee
       meme avant la synchro Internet. */
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();

    services_init();

    /* Reprise de l'heure depuis le PCF85063 en attendant le reseau */
    struct tm rtc_tm;
    if (rtc_get(&rtc_tm)) {
        struct timeval tv = {.tv_sec = mktime(&rtc_tm), .tv_usec = 0};
        settimeofday(&tv, nullptr);
        ESP_LOGI(TAG, "RTC PCF85063 : reprise %02d:%02d le %02d/%02d/%04d",
                 rtc_tm.tm_hour, rtc_tm.tm_min, rtc_tm.tm_mday,
                 rtc_tm.tm_mon + 1, rtc_tm.tm_year + 1900);
    } else {
        ESP_LOGW(TAG, "RTC PCF85063 : pas d'heure valide, on attend le reseau");
    }

    /* Ecran + LVGL.
       IMPORTANT : le BSP Waveshare ignore buffer_size / double_buffer /
       flags passes ici — il ne lit que lvgl_port_cfg puis construit sa
       propre config d'ecran depuis le Kconfig (BSP_DISPLAY_LVGL_BUF_HEIGHT,
       etc.). La taille de pile de la tache LVGL, elle, est bien prise en
       compte (16 Ko : il ne restait que 2,7 Ko de marge pendant run()). */
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = LVGL_PORT_INIT_CONFIG(),
    };
    lv_display_t *disp = bsp_display_start_with_config(&cfg);
    if (!disp) {
        ESP_LOGE(TAG, "bsp_display_start_with_config a echoue");
        return;
    }
    bsp_display_brightness_set(80);
    bsp_display_backlight_on();

    ESP_LOGI(TAG, "ecran %dx%d pret", BSP_LCD_H_RES, BSP_LCD_V_RES);
    ESP_LOGI(TAG, "RAM interne libre : %u o (plus gros bloc %u o)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    bsp_display_lock(0);

    s_phone = new ESP_Brookesia_Phone(disp);
    ESP_BROOKESIA_CHECK_NULL_EXIT(s_phone, "Creation du phone impossible");

    /* Pas de stylesheet specifique : la feuille "default" (en pourcentages)
       s'adapte a 410x502 et donne 2 colonnes. */
    ESP_BROOKESIA_CHECK_FALSE_EXIT(
        s_phone->setTouchDevice(bsp_display_get_input_dev()), "setTouchDevice a echoue");
    s_phone->registerLvLockCallback((ESP_Brookesia_GUI_LockCallback_t)(bsp_display_lock), 0);
    s_phone->registerLvUnlockCallback((ESP_Brookesia_GUI_UnlockCallback_t)(bsp_display_unlock));
    ESP_BROOKESIA_CHECK_FALSE_EXIT(s_phone->begin(), "phone->begin() a echoue");

    /* Reference de l'ecran du lanceur : sert a la garde du lanceur. */
    s_home_scr = lv_scr_act();


    /* Applications installees dans le lanceur */
    AppSettings *app = new AppSettings();
    ESP_BROOKESIA_CHECK_NULL_EXIT(app, "Creation de l'app Parametres impossible");
    ESP_BROOKESIA_CHECK_FALSE_EXIT(
        (s_phone->installApp(app) >= 0), "Installation de l'app Parametres echouee");

    AppAssistant *assistant = new AppAssistant();
    ESP_BROOKESIA_CHECK_NULL_EXIT(assistant, "Creation de l'app Assistant impossible");
    ESP_BROOKESIA_CHECK_FALSE_EXIT(
        (s_phone->installApp(assistant) >= 0), "Installation de l'app Assistant echouee");

    AppMusic *music = new AppMusic();
    ESP_BROOKESIA_CHECK_NULL_EXIT(music, "Creation de l'app Musique impossible");
    ESP_BROOKESIA_CHECK_FALSE_EXIT(
        (s_phone->installApp(music) >= 0), "Installation de l'app Musique echouee");

    AppPerplexe *perplexe = new AppPerplexe();
    ESP_BROOKESIA_CHECK_NULL_EXIT(perplexe, "Creation de l'app Perplexe impossible");
    ESP_BROOKESIA_CHECK_FALSE_EXIT(
        (s_phone->installApp(perplexe) >= 0), "Installation de l'app Perplexe echouee");

    /* Le lanceur tient une icone par page : chaque app installee occupe donc
       sa propre page (Parametres page 1, Assistant page 2, Musique page 3,
       Perplexe page 4). */

    lv_timer_create(status_timer_cb, 1000, s_phone);

#ifdef XZ_AUTOTEST
    lv_timer_create(autotest_open_app, XZ_AUTOTEST_MS, nullptr);
#endif
#ifdef XZ_AUTOTEST_BACK
    lv_timer_create(autotest_press_back, 22000, nullptr);
#endif

#ifdef XZ_SNAPSHOT
    xTaskCreate(snapshot_task, "xz_snap", 8192, assistant, 3, nullptr);
#endif

#ifdef XZ_ICONSNAP
    xTaskCreate(iconsnap_task, "xz_icon", 8192, nullptr, 3, nullptr);
#endif

#ifdef XZ_MUSSNAP
    xTaskCreate(mussnap_task, "mus_snap", 8192, nullptr, 3, nullptr);
#endif

#ifdef XZ_LAYERDIAG
    lv_timer_create(layerdiag_cb, 15000, nullptr);
#endif

    /* Surveillance du lanceur : 200 ms, et n'agit qu'aux transitions. */
    lv_timer_create(launcher_guard_cb, 200, nullptr);

    /* Date ajoutee dans la barre, apres l'horloge */
    add_date_to_status_bar();

    bsp_display_unlock();

    ESP_LOGI(TAG, "=== montre : interface prete ===");
}
