/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "systems/phone/widgets/status_bar/esp_brookesia_status_bar.hpp"
#include "systems/phone/assets/esp_brookesia_phone_assets.h"

constexpr ESP_Brookesia_StatusBarAreaData_t ESP_BROOKESIA_PHONE_DEFAULT_DARK_STATUS_BAR_AREA_DATA(
    int w_percent, ESP_Brookesia_StatusBarAreaAlign_t align, int start_offset)
{
    return {
        .size = ESP_BROOKESIA_STYLE_SIZE_RECT_PERCENT(w_percent, 100),
        .layout_column_align = align,
        .layout_column_start_offset = start_offset,
        .layout_column_pad = 3,
    };
}

constexpr ESP_Brookesia_StatusBarData_t ESP_BROOKESIA_PHONE_DEFAULT_DARK_STATUS_BAR_DATA()
{
    return {
        .main = {
            .size = ESP_BROOKESIA_STYLE_SIZE_RECT_PERCENT(100, 10),
            .size_min = ESP_BROOKESIA_STYLE_SIZE_RECT_W_PERCENT(100, 24),
            .size_max = ESP_BROOKESIA_STYLE_SIZE_RECT_W_PERCENT(100, 50),
            /* Orange : la barre etait grise (0x38393A) */
            .background_color = ESP_BROOKESIA_STYLE_COLOR(0xE8590C),
            .text_font = ESP_BROOKESIA_STYLE_FONT_HEIGHT_PERCENT(60),
            .text_color = ESP_BROOKESIA_STYLE_COLOR(0xFFFFFF),
        },
        .area = {
            .num = 2,
            .data = {
                /* Zone gauche elargie a 60 % : au demarrage les libelles de
                   l'horloge contiennent un texte provisoire plus large que
                   l'heure finale, et Brookesia masque l'horloge si elle
                   depasse sa zone (marge de 65 px + ~150 px de texte). */
                ESP_BROOKESIA_PHONE_DEFAULT_DARK_STATUS_BAR_AREA_DATA(60, ESP_BROOKESIA_STATUS_BAR_AREA_ALIGN_START, 65),
                ESP_BROOKESIA_PHONE_DEFAULT_DARK_STATUS_BAR_AREA_DATA(40, ESP_BROOKESIA_STATUS_BAR_AREA_ALIGN_END, 65),
            },
        },
        .icon_common_size = ESP_BROOKESIA_STYLE_SIZE_SQUARE_PERCENT(60),
        .battery = {
            .area_index = 1,
            .icon_data = {
                .icon = {
                    .image_num = 5,
                    .images = {
                        ESP_BROOKESIA_STYLE_IMAGE_RECOLOR_WHITE(&esp_brookesia_image_large_status_bar_battery_level1_36_36),
                        ESP_BROOKESIA_STYLE_IMAGE_RECOLOR_WHITE(&esp_brookesia_image_large_status_bar_battery_level2_36_36),
                        ESP_BROOKESIA_STYLE_IMAGE_RECOLOR_WHITE(&esp_brookesia_image_large_status_bar_battery_level3_36_36),
                        ESP_BROOKESIA_STYLE_IMAGE_RECOLOR_WHITE(&esp_brookesia_image_large_status_bar_battery_level4_36_36),
                        ESP_BROOKESIA_STYLE_IMAGE_RECOLOR_WHITE(&esp_brookesia_image_large_status_bar_battery_charge_36_36),
                    },
                },
            },
        },
        .wifi = {
            .area_index = 1,
            .icon_data = {
                .icon = {
                    .image_num = 4,
                    .images = {
                        ESP_BROOKESIA_STYLE_IMAGE_RECOLOR_WHITE(&esp_brookesia_image_large_status_bar_wifi_close_36_36),
                        ESP_BROOKESIA_STYLE_IMAGE_RECOLOR_WHITE(&esp_brookesia_image_large_status_bar_wifi_level1_36_36),
                        ESP_BROOKESIA_STYLE_IMAGE_RECOLOR_WHITE(&esp_brookesia_image_large_status_bar_wifi_level2_36_36),
                        ESP_BROOKESIA_STYLE_IMAGE_RECOLOR_WHITE(&esp_brookesia_image_large_status_bar_wifi_level3_36_36),
                    },
                },
            },
        },
        .clock = {
            .area_index = 0,
        },
        .flags = {
            .enable_main_size_min = 1,
            .enable_main_size_max = 1,
            .enable_battery_icon = 1,
            .enable_battery_icon_common_size = 1,
            .enable_battery_label = 0,
            .enable_wifi_icon = 1,
            .enable_wifi_icon_common_size = 1,
            .enable_clock = 1,
        },
    };
}
