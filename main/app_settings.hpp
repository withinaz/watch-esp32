#pragma once

#include "lvgl.h"
#include "esp_brookesia.hpp"

class AppSettings : public ESP_Brookesia_PhoneApp {
public:
    AppSettings();
    ~AppSettings() override;

    bool run(void) override;
    bool back(void) override;

    /* Callbacks LVGL : appeles aussi par les helpers de construction de
       widgets (fonctions libres du .cpp), donc obligatoirement publics. */
    static void cb_menu(lv_event_t *e);
    static void cb_back(lv_event_t *e);
    static void cb_brightness(lv_event_t *e);
    static void cb_volume(lv_event_t *e);
    static void cb_wifi_scan(lv_event_t *e);
    static void cb_ap_selected(lv_event_t *e);
    static void cb_kb_ready(lv_event_t *e);
    static void cb_kb_cancel(lv_event_t *e);
    static void cb_sync_ntp(lv_event_t *e);
    static void cb_beep(lv_event_t *e);

private:
    /* Pages */
    lv_obj_t *_menu = nullptr;
    lv_obj_t *_sub = nullptr;

    void build_menu(void);
    void open_page(int which);
    void close_page(void);

    void build_wifi(void);
    void build_wifi_list(void);
    void build_wifi_password(const char *ssid);
    void build_display(void);
    void build_sound(void);
    void build_clock(void);
    void build_sensors(void);
    void build_storage(void);
    void build_device(void);
};
