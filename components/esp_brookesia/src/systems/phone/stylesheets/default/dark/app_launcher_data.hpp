/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "systems/phone/widgets/app_launcher/esp_brookesia_app_launcher.hpp"

constexpr ESP_Brookesia_AppLauncherIconData_t ESP_BROOKESIA_PHONE_DEFAULT_DARK_APP_LAUNCHER_ICON_DATA()
{
    return {
        .main = {
            /* 210 px : la grille ne tient qu'une icone par page (410/210 = 1,
               hauteur/210 = 1). Chaque application occupe donc sa propre page,
               et les suivantes sont creees automatiquement. */
            .size = ESP_BROOKESIA_STYLE_SIZE_SQUARE(210),
            .layout_row_pad = 10,
        },
        .image = {
            .default_size = ESP_BROOKESIA_STYLE_SIZE_SQUARE(150),
            .press_size = ESP_BROOKESIA_STYLE_SIZE_SQUARE(136),
        },
        .label = {
            .text_font = ESP_BROOKESIA_STYLE_FONT_SIZE(22),
            .text_color = ESP_BROOKESIA_STYLE_COLOR(0xFFFFFF),
        },
    };
}

constexpr ESP_Brookesia_AppLauncherData_t ESP_BROOKESIA_PHONE_DEFAULT_DARK_APP_LAUNCHER_DATA()
{
    return {
        .main = {
            .y_start = 0,
            .size = ESP_BROOKESIA_STYLE_SIZE_RECT_PERCENT(100, 100),
        },
        .table = {
            /* Une page par application : le nombre de pages suit le nombre
               d'apps installees (plus de page vide en fin de liste). */
            .default_num = 1,
            .size = ESP_BROOKESIA_STYLE_SIZE_RECT_PERCENT(100, 76),
        },
        .indicator = {
            .main_size = ESP_BROOKESIA_STYLE_SIZE_RECT_W_PERCENT(100, 20),
            .main_layout_column_pad = 10,
            .main_layout_bottom_offset = 30,
            .spot_inactive_size = ESP_BROOKESIA_STYLE_SIZE_SQUARE(12),
            .spot_active_size = ESP_BROOKESIA_STYLE_SIZE_RECT(40, 12),
            .spot_inactive_background_color = ESP_BROOKESIA_STYLE_COLOR(0xC6C6C6),
            .spot_active_background_color = ESP_BROOKESIA_STYLE_COLOR(0xFFFFFF),
        },
        .icon = ESP_BROOKESIA_PHONE_DEFAULT_DARK_APP_LAUNCHER_ICON_DATA(),
        .flags = {
            .enable_table_scroll_anim = 0,
        },
    };
}
