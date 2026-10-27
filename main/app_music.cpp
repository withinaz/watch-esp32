/*
 * AppMusic - boite a musique generative (3eme application, page 3).
 *
 * Le principe : aucun echantillon audio n'est stocke. Un sequenceur tire des
 * notes d'une gamme pentatonique sur une grille de huitiemes, un petit moteur
 * de synthese les rend en sinus (table precalculee) avec enveloppe, et le
 * resultat part vers le codec ES8311 par blocs de 20 ms. Tout est calcule a
 * la volee : la musique ne repete jamais exactement deux fois.
 *
 * Choix techniques :
 *  - 24 kHz mono 16 bits : meme chemin que l'app Assistant, donc deja prouve
 *    sur cette carte (le codec est ouvert avec cette frequence au premier
 *    appel et la memorise).
 *  - Table de sinus de 1024 points + interpolation lineaire : 12 voix
 *    tournent confortablement dans le budget CPU, sans appel a sinf() par
 *    echantillon.
 *  - L'initialisation audio touche le bus I2C (codecs). Comme pour
 *    l'Assistant, elle ne doit PAS tourner sur la tache LVGL : elle est
 *    deleguee a une tache dediee.
 */
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_random.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "bsp/esp-bsp.h"

#include "app_music.hpp"
#include "icons/app_icons.h"
#include "private/esp_brookesia_utils.h"

static const char *TAG = "MUSIQUE";
static AppMusic *s_self = nullptr;

/* ------------------------------------------------------------------ */
/* Palette (reprise de l'app Assistant : le lanceur reste coherent)    */
/* ------------------------------------------------------------------ */
#define COL_BG      0x0E0E10
#define COL_CARD    0x1C1C1E
#define COL_TEXT    0xFFFFFF
#define COL_ACCENT  0x00E5A0
#define COL_DIM     0x8A8A8E
#define COL_OFF     0x232327

/* ================================================================== */
/* 1. Etat partage entre la tache audio et l'interface                */
/* ================================================================== */
/* L'audio ecrit, LVGL lit. Des atomiques suffisent : on ne transporte
   que des entiers, et un affichage legerement en retard d'une image n'a
   aucune importance.                                                    */
static std::atomic<bool>     s_running{false};
static std::atomic<bool>     s_task_alive{false};
static std::atomic<int>      s_ui_step{0};
static std::atomic<int>      s_ui_chord{0};
static std::atomic<int>      s_ui_key{0};
static std::atomic<int>      s_ui_bpm{0};
static std::atomic<int>      s_ui_level{0};
static std::atomic<int>      s_ui_notes{0};
static std::atomic<int>      s_ui_voices{0};
static std::atomic<uint32_t> s_cols_ver{1};

/* Masque des notes par huitieme : index = (numero de huitieme % ROLL_COLS).
   Un bit par ligne du piano roll. */
static uint8_t s_cols[AppMusic::ROLL_COLS] = {};

/* ================================================================== */
/* 2. Moteur de synthese                                              */
/* ================================================================== */
#define MUS_RATE        24000
#define MUS_BLOCK       480          /* 20 ms */
#define MUS_MAX_VOICES  12
#define MUS_LUT_BITS    10
#define MUS_LUT         (1 << MUS_LUT_BITS)
#define MUS_LUT_MASK    (MUS_LUT - 1)
#define MUS_TAU         6.28318530718f

#define MUS_BASE_MIDI   60           /* ligne 0 du roll = do4 */

static float s_lut[MUS_LUT];

/* ------------------------------------------------------------------ */
/* Moteur de synthese : corde pincee, timbre de boite a musique        */
/* ------------------------------------------------------------------ */
struct Voice {
    float ph1, inc1;      /* oscillateur principal */
    float ph2, inc2;      /* oscillateur secondaire (desaccord ou octave) */
    float mix2;           /* part du 2e oscillateur, 0 = inutilise */
    float env;            /* enveloppe courante 0..1 */
    float env_mul;        /* facteur par echantillon */
    float attack;         /* 1 -> 0 pendant l'attaque */
    float attack_step;
    float amp;
    int   row;            /* ligne du piano roll, -1 = pas affiche */
    bool  on;
};

static Voice s_vox[MUS_MAX_VOICES];
static esp_codec_dev_handle_t s_spk = nullptr;
static bool s_audio_ok = false;

/* --- table de sinus ------------------------------------------------ */
static void lut_init(void)
{
    for (int i = 0; i < MUS_LUT; i++) {
        s_lut[i] = sinf(MUS_TAU * (float)i / (float)MUS_LUT);
    }
}

/* Lecture de la table avec interpolation lineaire. */
static inline float wt(float ph)
{
    const float x = ph * (float)MUS_LUT;
    int i = (int)x;
    const float f = x - (float)i;
    i &= MUS_LUT_MASK;
    const int j = (i + 1) & MUS_LUT_MASK;
    return s_lut[i] + (s_lut[j] - s_lut[i]) * f;
}

static inline float midi_hz(int midi)
{
    return 440.0f * powf(2.0f, (float)(midi - 69) / 12.0f);
}

/* --- allocation de voix -------------------------------------------- */
static Voice *voice_take(void)
{
    Voice *weakest = nullptr;
    for (int i = 0; i < MUS_MAX_VOICES; i++) {
        if (!s_vox[i].on) {
            return &s_vox[i];
        }
        if (weakest == nullptr || s_vox[i].env < weakest->env) {
            weakest = &s_vox[i];
        }
    }
    return weakest;   /* vol de voix : on prend la plus eteinte */
}

static void note_on(int midi, float amp, float decay_s, float attack_s,
                    float detune_hz, float mix2, int row)
{
    if (s_spk == nullptr || midi < 20 || midi > 110) {
        return;
    }
    Voice *v = voice_take();
    if (v == nullptr) {
        return;
    }
    const float f = midi_hz(midi);

    v->ph1 = 0.0f;
    v->inc1 = f / (float)MUS_RATE;
    if (v->inc1 >= 1.0f) {
        v->inc1 = 0.999f;
    }
    if (mix2 > 0.0f) {
        v->ph2 = 0.37f;                       /* decorrele les deux osc. */
        v->inc2 = (f + detune_hz) / (float)MUS_RATE;
        if (v->inc2 >= 1.0f) {
            v->inc2 = 0.999f;
        }
        v->mix2 = mix2;
    } else {
        v->inc2 = 0.0f;
        v->mix2 = 0.0f;
    }

    v->amp = amp;
    v->env = 1.0f;
    v->env_mul = expf(-1.0f / (decay_s * (float)MUS_RATE));
    v->attack = (attack_s > 0.0f) ? 1.0f : 0.0f;
    v->attack_step = (attack_s > 0.0f)
                     ? 1.0f / (attack_s * (float)MUS_RATE)
                     : 1.0f;
    v->row = row;
    v->on = true;
}

/* --- rendu d'un echantillon ---------------------------------------- */
static inline float synth_sample(void)
{
    float sum = 0.0f;
    int active = 0;

    for (int i = 0; i < MUS_MAX_VOICES; i++) {
        Voice *v = &s_vox[i];
        if (!v->on) {
            continue;
        }

        float g = v->amp * v->env;

        /* attaque : petite rampe pour eviter le clic */
        if (v->attack > 0.0f) {
            v->attack -= v->attack_step;
            if (v->attack < 0.0f) {
                v->attack = 0.0f;
            }
            g *= 1.0f - v->attack;
        }

        v->env *= v->env_mul;
        if (v->env < 0.0008f) {
            v->on = false;
            continue;
        }
        active++;

        v->ph1 += v->inc1;
        if (v->ph1 >= 1.0f) {
            v->ph1 -= 1.0f;
        }
        float s = wt(v->ph1);

        if (v->mix2 > 0.0f) {
            v->ph2 += v->inc2;
            if (v->ph2 >= 1.0f) {
                v->ph2 -= 1.0f;
            }
            s = s * (1.0f - v->mix2) + wt(v->ph2) * v->mix2;
        }

        sum += s * g;
    }

    /* saturation douce : evite l'ecretage dur quand plusieurs notes tombent
       ensemble, et arrondit le timbre dans les aigus. */
    return tanhf(sum * 0.85f);
}

/* ================================================================== */
/* 3. Sequenceur                                                      */
/* ================================================================== */
/* Gamme pentatonique majeure : aucune intervalle dissonant, donc une
   marche aleatoire dessus reste toujours ecoutable.                     */
static const int PENT[5] = {0, 2, 4, 7, 9};

/* Cycle I - V - vi - IV (le plus consonnant du repertoire populaire). */
static const int PROG_ROOT[4]  = {0, 7, 9, 5};
static const bool PROG_MINOR[4] = {false, false, true, false};

static const char *NOTE_FR[12] = {
    "Do", "Do#", "Re", "Re#", "Mi", "Fa",
    "Fa#", "Sol", "Sol#", "La", "La#", "Si"
};

static int s_key = 0;              /* 0 = do */
static int s_chord = 0;            /* 0..3 */
static int s_step = 0;             /* huitieme courant */
static int s_bpm = 84;
static int s_step_samples = 0;
static int s_last_row = 5;
static uint32_t s_note_count = 0;

static int row_to_midi(int row)
{
    return MUS_BASE_MIDI + s_key + PENT[row % 5] + 12 * (row / 5);
}

static void seq_reset(void)
{
    s_key = (int)(esp_random() % 12);
    s_chord = 0;
    s_step = 0;
    s_bpm = 78 + (int)(esp_random() % 19);           /* 78..96 */
    s_step_samples = (MUS_RATE * 60) / (s_bpm * 2);  /* un huitieme */
    s_last_row = 5;
    s_note_count = 0;

    for (int i = 0; i < AppMusic::ROLL_COLS; i++) {
        s_cols[i] = 0;
    }
    s_ui_step.store(0);
    s_ui_chord.store(0);
    s_ui_key.store(s_key);
    s_ui_bpm.store(s_bpm);
    s_cols_ver.fetch_add(1);
}

/* Marche aleatoire sur les lignes hautes du roll (la melodie). */
static int pick_row(void)
{
    static const int DELTA[6] = {-2, -1, 0, 0, 1, 2};
    int r = s_last_row + DELTA[esp_random() % 6];
    if (r < 2) {
        r = 2;
    }
    if (r > AppMusic::ROLL_ROWS - 1) {
        r = AppMusic::ROLL_ROWS - 1;
    }
    s_last_row = r;
    return r;
}

static void seq_step(void)
{
    const int in_bar = s_step % 8;        /* 8 huitiemes par mesure 4/4 */
    const int beat = in_bar / 2;          /* 0..3 */
    uint8_t mask = 0;

    /* --- basse : temps 1 et 3 ------------------------------------- */
    if (beat == 0 || beat == 2) {
        note_on(row_to_midi(0) - 12, 0.62f, 0.80f, 0.010f, 0.0f, 0.0f, 0);
        mask |= 1u << 0;
        s_note_count++;
    }

    /* --- accord tenu au debut de chaque mesure --------------------- */
    if (in_bar == 0) {
        const int root = 48 + s_key + PROG_ROOT[s_chord];
        const int third = PROG_MINOR[s_chord] ? 3 : 4;
        note_on(root + 0,     0.20f, 1.90f, 0.30f, 0.9f, 0.55f, -1);
        note_on(root + third, 0.17f, 1.90f, 0.32f, 1.1f, 0.55f, -1);
        note_on(root + 7,     0.17f, 1.90f, 0.34f, 0.8f, 0.55f, -1);
        s_note_count += 3;
    }

    /* --- melodie : plus dense sur les temps ----------------------- */
    const int chance = (in_bar % 2 == 0) ? 70 : 44;
    if ((int)(esp_random() % 100) < chance) {
        const int row = pick_row();
        /* une note sur trois est doubllee a l'octave, pour l'eclat */
        const bool octave = (esp_random() % 3) == 0;
        note_on(row_to_midi(row), 0.30f, 0.45f, 0.003f, 12.0f, 0.18f, row);
        if (octave) {
            note_on(row_to_midi(row) + 12, 0.12f, 0.30f, 0.003f, 0.0f, 0.0f, row);
        }
        mask |= 1u << row;
        s_note_count++;
    }

    /* --- trace dans le piano roll --------------------------------- */
    s_cols[s_step % AppMusic::ROLL_COLS] = mask;
    s_ui_step.store(s_step);
    s_cols_ver.fetch_add(1);

    /* --- changement d'accord a chaque mesure ---------------------- */
    if (in_bar == 0) {
        s_ui_chord.store(s_chord);

        /* Toutes les 4 mesures seulement : une trace qui ne noie pas le
           port serie quand l'app tourne des heures. */
        if ((s_step % 32) == 0) {
            ESP_LOGI(TAG, "mesure %d : %s%s, %d BPM, %u notes, niveau %d",
                     s_step / 8 + 1,
                     NOTE_FR[(s_key + PROG_ROOT[s_chord]) % 12],
                     PROG_MINOR[s_chord] ? "m" : "", s_bpm,
                     (unsigned)s_note_count, s_ui_level.load());
        }

        s_chord = (s_chord + 1) % 4;

        /* toutes les 4 mesures, on derive vers une autre tonalite */
        if ((s_step % 32) == 0 && (esp_random() % 100) < 55) {
            s_key = (int)(esp_random() % 12);
            s_ui_key.store(s_key);
            s_bpm = 78 + (int)(esp_random() % 19);
            s_step_samples = (MUS_RATE * 60) / (s_bpm * 2);
            s_ui_bpm.store(s_bpm);
            ESP_LOGI(TAG, "nouvelle tonalite : %s majeur, %d BPM",
                     NOTE_FR[s_key], s_bpm);
        }
    }

    s_step++;
}

/* ================================================================== */
/* 4. Tache audio                                                     */
/* ================================================================== */
static bool audio_open(void)
{
    if (s_audio_ok) {
        return true;
    }

    /* Le BSP memorise la configuration du premier appel : on impose donc
       24 kHz mono des maintenant (c'est aussi ce que fait l'Assistant). */
    i2s_std_config_t cfg = {};
    cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUS_RATE);
    cfg.slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                     I2S_SLOT_MODE_MONO);
    cfg.gpio_cfg.mclk = BSP_I2S_MCLK;
    cfg.gpio_cfg.bclk = BSP_I2S_SCLK;
    cfg.gpio_cfg.ws = BSP_I2S_LCLK;
    cfg.gpio_cfg.dout = BSP_I2S_DOUT;
    cfg.gpio_cfg.din = BSP_I2S_DSIN;

    esp_err_t err = bsp_audio_init(&cfg);
    if (err != ESP_OK) {
        /* ESP_ERR_NOT_FOUND : le canal I2S existe deja (l'app Assistant a
           ette ouverte avant). Ce n'est pas bloquant. */
        ESP_LOGW(TAG, "bsp_audio_init : %s (on continue)", esp_err_to_name(err));
    }

    s_spk = bsp_audio_codec_speaker_init();
    if (s_spk == nullptr) {
        ESP_LOGE(TAG, "haut-parleur indisponible");
        return false;
    }

    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = 16;
    fs.channel = 1;
    fs.sample_rate = MUS_RATE;
    if (esp_codec_dev_open(s_spk, &fs) != ESP_OK) {
        ESP_LOGE(TAG, "ouverture du codec en echec");
        return false;
    }
    esp_codec_dev_set_out_vol(s_spk, 68);

    s_audio_ok = true;
    ESP_LOGI(TAG, "audio pret : %d Hz mono 16 bits, volume 68", MUS_RATE);
    return true;
}

static void mus_task(void *arg)
{
    (void)arg;

    if (!audio_open()) {
        ESP_LOGE(TAG, "pas d'audio : l'app reste muette");
        s_task_alive.store(false);
        vTaskDelete(nullptr);
        return;
    }

    /* Le demarrage se fait hors de la tache LVGL, donc la table peut etre
       construite ici sans concurrence. */
    lut_init();
    memset(s_vox, 0, sizeof(s_vox));
    seq_reset();

    static int16_t buf[MUS_BLOCK];
    int countdown = 0;
    float fade = 0.0f;                 /* fondu d'entree, 200 ms */

    while (s_running.load()) {
        float peak = 0.0f;

        for (int i = 0; i < MUS_BLOCK; i++) {
            if (--countdown <= 0) {
                countdown = s_step_samples;
                seq_step();
            }

            float s = synth_sample();
            if (fade < 1.0f) {
                fade += 1.0f / (0.2f * (float)MUS_RATE);
                if (fade > 1.0f) {
                    fade = 1.0f;
                }
                s *= fade;
            }

            const float a = fabsf(s);
            if (a > peak) {
                peak = a;
            }
            buf[i] = (int16_t)(s * 32767.0f);
        }

        /* Niveau 0..100 : racine carree, pour que les sons faibles restent
           visibles sur la jauge. */
        int lvl = (int)(sqrtf(peak) * 118.0f);
        if (lvl > 100) {
            lvl = 100;
        }
        s_ui_level.store(lvl);

        int act = 0;
        for (int i = 0; i < MUS_MAX_VOICES; i++) {
            if (s_vox[i].on) {
                act++;
            }
        }
        s_ui_voices.store(act);
        s_ui_notes.store((int)s_note_count);

        /* Bloque jusqu'a ce que le DMA ait de la place : c'est ce qui
           cadence la generation. */
        if (esp_codec_dev_write(s_spk, buf, sizeof(buf)) != ESP_OK) {
            ESP_LOGW(TAG, "ecriture codec en echec");
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    /* Arret propre : plus de son, codec ferme. */
    s_ui_level.store(0);
    s_ui_voices.store(0);
    esp_codec_dev_close(s_spk);
    s_audio_ok = false;
    s_task_alive.store(false);
    ESP_LOGI(TAG, "tache audio terminee");
    vTaskDelete(nullptr);
}

static void music_start(void)
{
    if (s_task_alive.exchange(true)) {
        return;                       /* deja lancee */
    }
    s_running.store(true);
    if (xTaskCreate(mus_task, "mus_audio", 8192, nullptr, 5, nullptr) != pdPASS) {
        s_task_alive.store(false);
        s_running.store(false);
        ESP_LOGE(TAG, "creation de la tache audio impossible");
    }
}

/* ================================================================== */
/* 5. Application                                                     */
/* ================================================================== */
AppMusic::AppMusic() :
    ESP_Brookesia_PhoneApp("Musique", &app_icon_music, true, true, true)
{
    s_self = this;
}

AppMusic::~AppMusic()
{
    if (_timer) {
        lv_timer_del(_timer);
        _timer = nullptr;
    }
}

/* ------------------------------------------------------------------ */
/* Repaint force.                                                      */
/*                                                                     */
/* run() est appele AVANT que le coeur ne charge l'ecran de l'app       */
/* (lv_scr_load). L'interface est donc construite sur un ecran qui      */
/* n'est pas encore affiche : LVGL n'invalide pas toute la surface au   */
/* chargement, et les zones STATIQUES gardent les pixels du lanceur —   */
/* l'icone de l'app reste visible « sous » l'interface. Les zones       */
/* animees, elles, se repeignent, ce qui rend le defaut intermittent.   */
/* On force donc deux rafraichissements complets une fois l'ecran en    */
/* place.                                                              */
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

bool AppMusic::run(void)
{
    _closing = false;
    build_ui();

    if (_timer == nullptr) {
        _timer = lv_timer_create(on_tick, 60, this);
    }
    refresh();

    /* Deux passes completes une fois l'ecran charge (voir force_repaint). */
    lv_timer_create(force_repaint, 350, nullptr);
    lv_timer_create(force_repaint, 1400, nullptr);

    /* Audio hors de la tache LVGL : l'init touche l'I2C des codecs. */
    music_start();

    ESP_LOGI(TAG, "run OK (pile LVGL %u o)", (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    return true;
}

bool AppMusic::back(void)
{
    if (_closing) {
        return true;
    }
    _closing = true;

    s_running.store(false);           /* la tache se ferme toute seule */

    if (_timer) {
        lv_timer_del(_timer);
        _timer = nullptr;
    }
    if (_root) {
        lv_obj_del(_root);
        _root = nullptr;
    }
    _dot = _lbl_state = _lbl_hint = _strip = nullptr;
    _chord_box = _lbl_foot = nullptr;
    _energy_bg = _energy_fg = nullptr;
    for (int c = 0; c < ROLL_COLS; c++) {
        for (int r = 0; r < ROLL_ROWS; r++) {
            _cell[c][r] = nullptr;
            _lit[c][r] = false;
        }
    }
    for (int i = 0; i < 4; i++) {
        _chord_lbl[i] = nullptr;
    }

    ESP_BROOKESIA_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "notifyCoreClosed a echoue");
    ESP_LOGI(TAG, "back OK : retour au lanceur");
    return true;
}

/* ------------------------------------------------------------------ */
void AppMusic::build_ui(void)
{
    if (_root) {
        lv_obj_del(_root);
        _root = nullptr;
    }

    /* Fond de l'ecran lui-meme, opaque : aucune zone non couverte par le
       conteneur ne peut laisser apparaitre le lanceur. */
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);

    /* Bandeau superieur. L'ecran de l'app commence a y=50 : les 50 px du haut
       restent ceux de l'ecran precedent, et l'on peut y voir un reste du
       lanceur. On les repeint avec le fond de l'app. Cree avant le conteneur
       pour rester dessous ; une barre d'etat posee sur le calque superieur
       reste au-dessus. */
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

    _root = lv_obj_create(lv_scr_act());
    lv_obj_set_pos(_root, 0, 0);
    lv_obj_set_style_bg_color(_root, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_root, 0, 0);
    lv_obj_set_style_radius(_root, 0, 0);
    lv_obj_set_style_pad_all(_root, 12, 0);
    lv_obj_clear_flag(_root, LV_OBJ_FLAG_SCROLLABLE);

    /* Hauteur reellement adressable (voir app_assistant.cpp : le
       deplacement de l'ecran n'est applique qu'au chargement). */
    const lv_area_t &va = getVisualArea();
    int usable_h = 0;
    if (va.y1 > 0) {
        usable_h = lv_area_get_height(&va) - va.y1;
    } else {
        lv_display_t *d = lv_display_get_default();
        usable_h = (d != nullptr) ? (int)lv_display_get_vertical_resolution(d) : 0;
    }
    if (usable_h < 200 || usable_h > 520) {
        usable_h = 402;
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

    /* Mesure de la zone que l'ecran de l'app couvre reellement : c'est elle
       qui dit si une bande reste a nu (laissant voir le lanceur derriere). */
    {
        lv_display_t *d = lv_display_get_default();
        lv_obj_t *scr = lv_scr_act();
        ESP_LOGI(TAG, "ecran app : affichage %dx%d, scr_act %dx%d a (%d,%d), "
                      "opa_fond %d, zone visible (%d,%d)-(%d,%d)",
                 d ? (int)lv_display_get_horizontal_resolution(d) : -1,
                 d ? (int)lv_display_get_vertical_resolution(d) : -1,
                 (int)lv_obj_get_width(scr), (int)lv_obj_get_height(scr),
                 (int)lv_obj_get_x(scr), (int)lv_obj_get_y(scr),
                 (int)lv_obj_get_style_bg_opa(scr, 0),
                 va.x1, va.y1, va.x2, va.y2);
    }

    /* --- titre ----------------------------------------------------- */
    lv_obj_t *title = lv_label_create(_root);
    lv_label_set_text(title, "Musique");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 2);

    /* --- carte d'etat ---------------------------------------------- */
    lv_obj_t *card = lv_obj_create(_root);
    lv_obj_set_size(card, LV_PCT(100), 62);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    _dot = lv_obj_create(card);
    lv_obj_set_size(_dot, 12, 12);
    lv_obj_set_style_radius(_dot, 8, 0);
    lv_obj_set_style_bg_color(_dot, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_dot, 0, 0);
    lv_obj_align(_dot, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_clear_flag(_dot, LV_OBJ_FLAG_SCROLLABLE);

    _lbl_state = lv_label_create(card);
    lv_label_set_text(_lbl_state, "Composition");
    lv_obj_set_style_text_color(_lbl_state, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(_lbl_state, &lv_font_montserrat_20, 0);
    lv_obj_align(_lbl_state, LV_ALIGN_TOP_LEFT, 40, 8);

    _lbl_hint = lv_label_create(card);
    lv_label_set_text(_lbl_hint, "--- majeur");
    lv_obj_set_style_text_color(_lbl_hint, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_font(_lbl_hint, &lv_font_montserrat_14, 0);
    lv_obj_align(_lbl_hint, LV_ALIGN_TOP_LEFT, 40, 34);

    /* --- piano roll ------------------------------------------------ */
    const int strip_y = 104;
    const int strip_h = 186;
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
        const int cell_w = 23;
        const int cell_h = 15;
        const int gap_x = 5;
        const int gap_y = 3;
        const int grid_w = ROLL_COLS * cell_w + (ROLL_COLS - 1) * gap_x;
        const int grid_h = ROLL_ROWS * cell_h + (ROLL_ROWS - 1) * gap_y;

        lv_obj_update_layout(_root);
        int inner = lv_obj_get_content_width(_strip);
        if (inner <= 0) {
            inner = lv_obj_get_content_width(_root);
        }
        if (inner <= 0) {
            inner = 386;
        }

        const int x0 = (inner - grid_w) / 2;
        const int y0 = (strip_h - grid_h) / 2;

        for (int c = 0; c < ROLL_COLS; c++) {
            for (int r = 0; r < ROLL_ROWS; r++) {
                lv_obj_t *cell = lv_obj_create(_strip);
                lv_obj_set_size(cell, cell_w, cell_h);
                /* ligne 0 = note la plus grave, donc en bas */
                lv_obj_set_pos(cell,
                               x0 + c * (cell_w + gap_x),
                               y0 + (ROLL_ROWS - 1 - r) * (cell_h + gap_y));
                lv_obj_set_style_bg_color(cell, lv_color_hex(COL_OFF), 0);
                lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(cell, 0, 0);
                lv_obj_set_style_radius(cell, 4, 0);
                lv_obj_set_style_pad_all(cell, 0, 0);
                lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_clear_flag(cell, LV_OBJ_FLAG_CLICKABLE);
                _cell[c][r] = cell;
                _lit[c][r] = false;
            }
        }
        ESP_LOGI(TAG, "piano roll : %dx%d cases (%dx%d px), x0=%d y0=%d",
                 (int)ROLL_COLS, (int)ROLL_ROWS, grid_w, grid_h, x0, y0);
    }

    /* --- cycle d'accords ------------------------------------------- */
    const int chord_y = 296;
    _chord_box = lv_obj_create(_root);
    lv_obj_set_size(_chord_box, LV_PCT(100), 32);
    lv_obj_align(_chord_box, LV_ALIGN_TOP_MID, 0, chord_y);
    lv_obj_set_style_bg_opa(_chord_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_chord_box, 0, 0);
    lv_obj_set_style_pad_all(_chord_box, 0, 0);
    lv_obj_set_style_pad_column(_chord_box, 6, 0);
    lv_obj_set_flex_flow(_chord_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(_chord_box, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(_chord_box, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 4; i++) {
        lv_obj_t *pill = lv_obj_create(_chord_box);
        lv_obj_set_size(pill, 88, 32);
        lv_obj_set_style_bg_color(pill, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(pill, 0, 0);
        lv_obj_set_style_radius(pill, 10, 0);
        lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(pill, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *lbl = lv_label_create(pill);
        lv_label_set_text(lbl, NOTE_FR[i]);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COL_DIM), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_center(lbl);

        _chord_lbl[i] = lbl;
    }

    /* --- jauge d'energie ------------------------------------------- */
    const int en_y = 334;
    _energy_bg = lv_obj_create(_root);
    lv_obj_set_size(_energy_bg, LV_PCT(100), 8);
    lv_obj_align(_energy_bg, LV_ALIGN_TOP_MID, 0, en_y);
    lv_obj_set_style_bg_color(_energy_bg, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(_energy_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_energy_bg, 0, 0);
    lv_obj_set_style_radius(_energy_bg, 4, 0);
    lv_obj_set_style_pad_all(_energy_bg, 0, 0);
    lv_obj_clear_flag(_energy_bg, LV_OBJ_FLAG_SCROLLABLE);

    _energy_fg = lv_obj_create(_energy_bg);
    lv_obj_set_size(_energy_fg, 0, 8);
    lv_obj_set_pos(_energy_fg, 0, 0);
    lv_obj_set_style_bg_color(_energy_fg, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(_energy_fg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_energy_fg, 0, 0);
    lv_obj_set_style_radius(_energy_fg, 4, 0);
    lv_obj_set_style_pad_all(_energy_fg, 0, 0);
    lv_obj_clear_flag(_energy_fg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(_energy_fg, LV_OBJ_FLAG_CLICKABLE);

    /* --- pied de page ---------------------------------------------- */
    _lbl_foot = lv_label_create(_root);
    lv_label_set_text(_lbl_foot, "0 notes");
    lv_obj_set_style_text_color(_lbl_foot, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_font(_lbl_foot, &lv_font_montserrat_12, 0);
    lv_obj_align(_lbl_foot, LV_ALIGN_TOP_MID, 0, 348);

    ESP_LOGI(TAG, "zone visible (%d,%d)-(%d,%d), contenu sur %d px ; jauge y=%d ; conteneur %dx%d",
             va.x1, va.y1, va.x2, va.y2, usable_h, en_y,
             (int)lv_obj_get_width(_root), (int)lv_obj_get_height(_root));
}

/* ------------------------------------------------------------------ */
/* Couleur d'une case selon sa hauteur : grave = indigo, aigu = turquoise. */
static uint32_t row_colour(int row)
{
    const int r0 = 0x3D, g0 = 0x6B, b0 = 0xFF;      /* bas */
    const int r1 = 0x00, g1 = 0xE5, b1 = 0xA0;      /* haut */
    const float t = (AppMusic::ROLL_ROWS > 1)
                    ? (float)row / (float)(AppMusic::ROLL_ROWS - 1)
                    : 1.0f;
    const int r = r0 + (int)((r1 - r0) * t);
    const int g = g0 + (int)((g1 - g0) * t);
    const int b = b0 + (int)((b1 - b0) * t);
    return (uint32_t)((r << 16) | (g << 8) | b);
}

/* Fonce une couleur vers le fond : les colonnes anciennes s'effacent. */
static uint32_t fade_colour(uint32_t col, float k)
{
    const int bg = 0x24;
    int r = (int)((col >> 16) & 0xFF);
    int g = (int)((col >> 8) & 0xFF);
    int b = (int)(col & 0xFF);
    r = bg + (int)((r - bg) * k);
    g = bg + (int)((g - bg) * k);
    b = bg + (int)((b - bg) * k);
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    return (uint32_t)((r << 16) | (g << 8) | b);
}

void AppMusic::update_roll(void)
{
    if (_strip == nullptr || _cell[0][0] == nullptr) {
        return;
    }

    const int step = s_ui_step.load();

    /* La colonne la plus a droite est le huitieme courant ; les anciennes
       glissent vers la gauche. L'anneau est indexe par (step % ROLL_COLS). */
    for (int c = 0; c < ROLL_COLS; c++) {
        int idx = (step - (ROLL_COLS - 1) + c) % ROLL_COLS;
        if (idx < 0) {
            idx += ROLL_COLS;
        }
        const uint8_t mask = s_cols[idx];

        /* Les vieilles colonnes s'eteignent progressivement. */
        const float k = 0.42f + 0.58f * (float)c / (float)(ROLL_COLS - 1);

        for (int r = 0; r < ROLL_ROWS; r++) {
            const bool lit = (mask & (1u << r)) != 0;
            if (lit == _lit[c][r]) {
                continue;              /* rien a redessiner */
            }
            _lit[c][r] = lit;
            lv_obj_set_style_bg_color(
                _cell[c][r],
                lv_color_hex(lit ? fade_colour(row_colour(r), k) : COL_OFF), 0);
        }
    }
}

void AppMusic::update_chords(void)
{
    if (_chord_lbl[0] == nullptr) {
        return;
    }
    const int key = s_ui_key.load();
    const int active = s_ui_chord.load();

    if (key != _last_key) {
        _last_key = key;
        for (int i = 0; i < 4; i++) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%s%s",
                     NOTE_FR[(key + PROG_ROOT[i]) % 12],
                     PROG_MINOR[i] ? "m" : "");
            lv_label_set_text(_chord_lbl[i], buf);
        }
    }

    if (active != _last_chord) {
        _last_chord = active;
        for (int i = 0; i < 4; i++) {
            lv_obj_t *pill = lv_obj_get_parent(_chord_lbl[i]);
            const bool on = (i == active);
            lv_obj_set_style_bg_color(pill, lv_color_hex(on ? COL_ACCENT : COL_CARD), 0);
            lv_obj_set_style_text_color(_chord_lbl[i],
                                        lv_color_hex(on ? COL_BG : COL_DIM), 0);
        }
    }
}

void AppMusic::refresh(void)
{
    if (_lbl_state == nullptr) {
        return;
    }

    /* Tonalite et tempo evoluent ensemble : une seule signature suffit a
       savoir s'il faut reecrire la ligne. */
    const int bpm = s_ui_bpm.load();
    const int key_now = s_ui_key.load();
    const int sig = bpm * 100 + key_now;
    if (bpm > 0 && sig != _last_bpm) {
        _last_bpm = sig;
        char buf[48];
        snprintf(buf, sizeof(buf), "%s majeur . %d BPM", NOTE_FR[key_now % 12], bpm);
        lv_label_set_text(_lbl_hint, buf);
    }

    update_chords();

    uint32_t ver = s_cols_ver.load();
    if (ver != _last_cols_ver) {
        _last_cols_ver = ver;
        update_roll();
    }

    /* Jauge d'energie : largeur proportionnelle au niveau du signal.
       La largeur utile ne bouge pas : on la mesure une seule fois. */
    if (_energy_fg != nullptr) {
        static int full = 0;
        if (full <= 0) {
            lv_obj_update_layout(_energy_bg);
            full = lv_obj_get_content_width(_energy_bg);
            if (full <= 0) {
                full = 386;
            }
        }
        const int lvl = s_ui_level.load();
        int w = (full * lvl) / 100;
        if (w < 0) {
            w = 0;
        }
        if (w > full) {
            w = full;
        }
        lv_obj_set_width(_energy_fg, w);
    }

    if (_lbl_foot != nullptr) {
        const int notes = s_ui_notes.load();
        const int voices = s_ui_voices.load();
        lv_label_set_text_fmt(_lbl_foot, "%d notes . %d voix", notes, voices);
    }

    /* Le point d'etat respire au rythme du niveau : l'app parait vivante. */
    if (_dot != nullptr) {
        const int lvl = s_ui_level.load();
        const uint32_t c = (lvl > 30) ? COL_ACCENT : 0x11996F;
        lv_obj_set_style_bg_color(_dot, lv_color_hex(c), 0);
    }
}

void AppMusic::on_tick(lv_timer_t *t)
{
    AppMusic *self = static_cast<AppMusic *>(t->user_data);
    if (self != nullptr) {
        self->refresh();
    }
}
