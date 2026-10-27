/*
 * xiaozhi_client.cpp - implementation cote appareil du protocole XiaoZhi.
 *
 * Voir xiaozhi_client.hpp pour le pourquoi. Tout ce qui touche a LVGL est
 * evite ici : les tasks audio et WebSocket ne font que mettre a jour un
 * etat et des chaines protegees par mutex, que l'UI vient lire.
 */
#include "xiaozhi_client.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "esp_random.h"
#include "esp_idf_version.h"
#include "esp_websocket_client.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"

#include "esp_codec_dev.h"
#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"

#include "opus.h"

#if __has_include("xz_test.h")
#include "xz_test.h"
#endif

static const char *TAG = "XIAOZHI";

/* Le serveur officiel. Surchargeable plus tard par un reglage. */
#define XZ_OTA_URL "https://api.tenclass.net/xiaozhi/ota/"

/* 24 kHz des deux cotes : c'est la frequence native de cette carte
   (boards/.../config.h de XiaoZhi : AUDIO_INPUT/OUTPUT_SAMPLE_RATE = 24000)
   et cela evite tout resampler. Trame de 60 ms = 1440 echantillons. */
#define XZ_RATE        24000
#define XZ_FRAME_MS    60
#define XZ_FRAME_SAMP  (XZ_RATE / 1000 * XZ_FRAME_MS)

/* Gain logiciel applique apres lecture du micro, en plus des 37.5 dB
   materiels de l'ES7210 (×2 = +6 dB). Baisser a 1 si ca sature. */
#define XZ_MIC_SW_GAIN 2

/* ------------------------------------------------------------------ */
/* Niveau sonore pour l'animateur de l'interface.                     */
/* Purement decoratif : rien n'est renvoye au serveur, le PCM n'est    */
/* pas modifie. Une lecture de 1440 echantillons toutes les 60 ms.    */
/* ------------------------------------------------------------------ */
static int frame_peak(const int16_t *pcm, int n)
{
    int peak = 0;
    for (int i = 0; i < n; i++) {
        int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (v > peak) {
            peak = v;
        }
    }
    return peak;
}

/* Crete -> 0..100, courbe racine carree : les fins de phrase restent
   visibles sur les barres sans que les pics ecrasent tout. */
static int peak_to_level(int peak)
{
    if (peak <= 0) {
        return 0;
    }
    if (peak >= 32767) {
        return 100;
    }
    int lv = (int)(sqrtf((float)peak / 32767.0f) * 118.0f);
    return lv > 100 ? 100 : lv;
}

static XzClient *s_inst = nullptr;

/* ------------------------------------------------------------------ */
/* identite de l'appareil                                             */
/* ------------------------------------------------------------------ */

static std::string device_mac(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[18];
    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string(buf);
}

/* UUID stable, genere une fois et garde en NVS. Le serveur s'en sert comme
   identite du client ; il doit donc survivre aux redemarrages. */
static std::string device_uuid(void)
{
    nvs_handle_t h;
    char buf[40] = {0};
    size_t len = sizeof(buf);
    if (nvs_open("xz", NVS_READONLY, &h) == ESP_OK) {
        esp_err_t err = nvs_get_str(h, "uuid", buf, &len);
        nvs_close(h);
        if (err == ESP_OK && strlen(buf) >= 32) {
            return std::string(buf);
        }
    }

    uint8_t r[16];
    esp_fill_random(r, sizeof(r));
    r[6] = (r[6] & 0x0F) | 0x40;
    r[8] = (r[8] & 0x3F) | 0x80;
    snprintf(buf, sizeof(buf),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
             r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);

    if (nvs_open("xz", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "uuid", buf);
        nvs_commit(h);
        nvs_close(h);
    }
    return std::string(buf);
}

/* ------------------------------------------------------------------ */
/* enregistrement OTA                                                 */
/* ------------------------------------------------------------------ */

/* Vrai des qu'une adresse IP est attribuee a l'interface WiFi. */
static bool net_up(void)
{
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (n == nullptr) {
        return false;
    }
    esp_netif_ip_info_t ip = {};
    if (esp_netif_get_ip_info(n, &ip) != ESP_OK) {
        return false;
    }
    return ip.ip.addr != 0;
}

/* esp_http_client_perform() consomme le corps de la reponse : sans
   gestionnaire d'evenement, les donnees sont perdues. On les capture ici. */
struct HttpAcc {
    char *buf;
    int cap;
    int len;
};

static esp_err_t http_evt(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA && e->user_data != nullptr) {
        HttpAcc *acc = static_cast<HttpAcc *>(e->user_data);
        int room = acc->cap - 1 - acc->len;
        if (room > 0) {
            int n = e->data_len < room ? e->data_len : room;
            memcpy(acc->buf + acc->len, e->data, n);
            acc->len += n;
            acc->buf[acc->len] = 0;
        }
    }
    return ESP_OK;
}

XzOta xz_ota_fetch(const char *device_id, const char *client_id)
{
    XzOta out;

#ifdef XZ_TEST_WS
    /* Mode test : on saute l'enregistrement et on attaque le serveur local. */
    (void)device_id;
    (void)client_id;
    out.ok = true;
    out.url = XZ_TEST_WS;
    out.token = "";
    ESP_LOGW(TAG, "MODE TEST : connexion directe a %s", XZ_TEST_WS);
    return out;
#endif

    char body[420];
    snprintf(body, sizeof(body),
             "{\"version\":2,\"mac_address\":\"%s\",\"uuid\":\"%s\","
             "\"chip_model_name\":\"esp32s3\","
             "\"application\":{\"name\":\"montre\",\"version\":\"1.0.0\","
             "\"idf_version\":\"%s\"},"
             "\"board\":{\"type\":\"waveshare-esp32-s3-touch-amoled-2.06\","
             "\"name\":\"Waveshare ESP32-S3-Touch-AMOLED-2.06\"}}",
             device_id, client_id, esp_get_idf_version());

    static char rbuf[2048];
    rbuf[0] = 0;
    HttpAcc acc = {rbuf, (int)sizeof(rbuf), 0};

    esp_http_client_config_t cfg = {};
    cfg.url = XZ_OTA_URL;
    cfg.timeout_ms = 15000;
    cfg.method = HTTP_METHOD_POST;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size = 1024;
    cfg.event_handler = http_evt;
    cfg.user_data = &acc;

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == nullptr) {
        out.error = "init http";
        return out;
    }

    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "Activation-Version", "1");
    esp_http_client_set_header(c, "Device-Id", device_id);
    esp_http_client_set_header(c, "Client-Id", client_id);
    esp_http_client_set_header(c, "User-Agent", "montre/1.0.0");

    if (esp_http_client_set_post_field(c, body, strlen(body)) != ESP_OK) {
        out.error = "post field";
        esp_http_client_cleanup(c);
        return out;
    }

    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);

    ESP_LOGI(TAG, "OTA http: %s, status %d", esp_err_to_name(err), status);

    if (err != ESP_OK || status != 200) {
        out.error = "HTTP " + std::to_string(status);
        esp_http_client_cleanup(c);
        return out;
    }

    esp_http_client_cleanup(c);

    ESP_LOGI(TAG, "OTA reponse (%d o): %s", acc.len, rbuf);

    cJSON *root = cJSON_Parse(rbuf);
    if (root == nullptr) {
        out.error = "JSON invalide";
        return out;
    }

    cJSON *ws = cJSON_GetObjectItem(root, "websocket");
    if (cJSON_IsObject(ws)) {
        cJSON *u = cJSON_GetObjectItem(ws, "url");
        cJSON *t = cJSON_GetObjectItem(ws, "token");
        if (cJSON_IsString(u)) {
            out.url = u->valuestring;
        }
        if (cJSON_IsString(t)) {
            out.token = t->valuestring;
        }
    }

    cJSON *act = cJSON_GetObjectItem(root, "activation");
    if (cJSON_IsObject(act)) {
        cJSON *code = cJSON_GetObjectItem(act, "code");
        cJSON *msg = cJSON_GetObjectItem(act, "message");
        if (cJSON_IsString(code)) {
            out.code = code->valuestring;
        }
        if (cJSON_IsString(msg)) {
            out.message = msg->valuestring;
        }
    }

    out.ok = !out.url.empty();
    cJSON_Delete(root);
    return out;
}

/* ------------------------------------------------------------------ */
/* singleton                                                          */
/* ------------------------------------------------------------------ */

XzClient &XzClient::get(void)
{
    if (s_inst == nullptr) {
        s_inst = new XzClient();
    }
    return *s_inst;
}

void XzClient::setState(XzState s)
{
    int old = _state.exchange((int)s);
    if (old != (int)s) {
        ESP_LOGI(TAG, "etat %d -> %d", old, (int)s);
    }
}

static void set_str(std::mutex &m, std::string &dst, const std::string &v)
{
    std::lock_guard<std::mutex> g(m);
    dst = v;
}

void XzClient::setHint(const std::string &h) { set_str(_m, _hint, h); }

void XzClient::setUserText(const std::string &t)
{
    set_str(_m, _user, t);
    touchText();
}

void XzClient::setBotText(const std::string &t)
{
    set_str(_m, _bot, t);
    touchText();
}

void XzClient::touchText(void) { _text_ver.fetch_add(1); }

std::string XzClient::hint(void)
{
    std::lock_guard<std::mutex> g(_m);
    return _hint;
}

std::string XzClient::userText(void)
{
    std::lock_guard<std::mutex> g(_m);
    return _user;
}

std::string XzClient::botText(void)
{
    std::lock_guard<std::mutex> g(_m);
    return _bot;
}

std::string XzClient::activationCode(void)
{
    std::lock_guard<std::mutex> g(_m);
    return _code;
}

/* ------------------------------------------------------------------ */
/* audio                                                              */
/* ------------------------------------------------------------------ */

bool XzClient::open_audio(void)
{
    if (_audio_ok) {
        return true;
    }

    /* Le BSP cree le canal I2S duplex et le configure en 22050 Hz par
       defaut ; on impose 24000 des le premier appel puisque son
       initialisation est paresseuse (elle memorise le premier config). */
    i2s_std_config_t cfg = {};
    cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(XZ_RATE);
    cfg.slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_MONO);
    cfg.gpio_cfg.mclk = BSP_I2S_MCLK;
    cfg.gpio_cfg.bclk = BSP_I2S_SCLK;
    cfg.gpio_cfg.ws = BSP_I2S_LCLK;
    cfg.gpio_cfg.dout = BSP_I2S_DOUT;
    cfg.gpio_cfg.din = BSP_I2S_DSIN;

    esp_err_t err = bsp_audio_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_init: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "audio: i2s ok");

    _spk = bsp_audio_codec_speaker_init();
    if (_spk == nullptr) {
        ESP_LOGE(TAG, "init haut-parleur echouee");
        return false;
    }
    ESP_LOGI(TAG, "audio: codec hp cree");
    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = 16;
    fs.channel = 1;
    fs.sample_rate = XZ_RATE;
    /* Le codec peut etre encore tenu par l'autre application audio (Musique)
       pendant quelques dizaines de millisecondes : on retente une fois plutot
       que de laisser l'assistant hors ligne. */
    esp_err_t oerr = esp_codec_dev_open((esp_codec_dev_handle_t)_spk, &fs);
    if (oerr != ESP_OK) {
        ESP_LOGW(TAG, "ouverture hp : %s, nouvel essai", esp_err_to_name(oerr));
        vTaskDelay(pdMS_TO_TICKS(300));
        oerr = esp_codec_dev_open((esp_codec_dev_handle_t)_spk, &fs);
        if (oerr != ESP_OK) {
            ESP_LOGE(TAG, "ouverture hp impossible : %s", esp_err_to_name(oerr));
        }
    }
    esp_codec_dev_set_out_vol((esp_codec_dev_handle_t)_spk, _volume.load());
    ESP_LOGI(TAG, "audio: hp ouvert");

    if (!open_mic()) {
        return false;
    }
    ESP_LOGI(TAG, "audio: micro ouvert");

    _rb = xRingbufferCreate(16 * 1024, RINGBUF_TYPE_NOSPLIT);
    if (_rb == nullptr) {
        ESP_LOGE(TAG, "ringbuffer echoue");
        return false;
    }

    int e = 0;
    OpusEncoder *enc = opus_encoder_create(XZ_RATE, 1, OPUS_APPLICATION_VOIP, &e);
    if (enc == nullptr || e != OPUS_OK) {
        ESP_LOGE(TAG, "opus_encoder_create: %d", e);
        return false;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(24000));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(0));   /* CPU : priorite au temps reel */
    opus_encoder_ctl(enc, OPUS_SET_VBR(1));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    _enc = enc;

    /* Tasks audio permanentes, piles allouees en PSRAM : libopus compile
       avec USE_ALLOCA consomme plus de 16 Ko de pile par appel, ce qui ne
       tient pas en RAM interne a cote du reste. */
    if (!_tasks_started) {
        const size_t mic_stack_sz = 64 * 1024;
        const size_t spk_stack_sz = 48 * 1024;
        _mic_stack = heap_caps_malloc(mic_stack_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        _spk_stack = heap_caps_malloc(spk_stack_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (_mic_stack == nullptr || _spk_stack == nullptr) {
            ESP_LOGE(TAG, "piles audio PSRAM indisponibles");
            return false;
        }
        if (xTaskCreateStaticPinnedToCore(mic_task_entry, "xz_mic", mic_stack_sz, this, 7,
                                          (StackType_t *)_mic_stack, &_mic_tcb, 1) == nullptr) {
            ESP_LOGE(TAG, "task micro non creee");
            return false;
        }
        if (xTaskCreateStaticPinnedToCore(spk_task_entry, "xz_spk", spk_stack_sz, this, 6,
                                          (StackType_t *)_spk_stack, &_spk_tcb, 1) == nullptr) {
            ESP_LOGE(TAG, "task haut-parleur non creee");
            return false;
        }
        _tasks_started = true;
        ESP_LOGI(TAG, "tasks audio demarrees (piles PSRAM %u/%u o)",
                 (unsigned)mic_stack_sz, (unsigned)spk_stack_sz);
    }

    _audio_ok = true;
    ESP_LOGI(TAG, "audio pret : %d Hz, trame %d ms, micro %d canal(aux)",
             XZ_RATE, XZ_FRAME_MS, _mic_channels);
    return true;
}

/* L'ES7210 de cette carte delivre le micro + un canal de reference. On ouvre
   en stereo et on garde le canal qui porte du signal ; si l'ouverture
   stereo echoue, on retombe en mono. */
bool XzClient::open_mic(void)
{
    _mic = bsp_audio_codec_microphone_init();
    if (_mic == nullptr) {
        ESP_LOGE(TAG, "init micro echouee");
        return false;
    }

    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = 16;
    fs.channel = 2;
    fs.sample_rate = XZ_RATE;

    if (esp_codec_dev_open((esp_codec_dev_handle_t)_mic, &fs) != ESP_OK) {
        ESP_LOGW(TAG, "micro stereo refuse, essai mono");
        fs.channel = 1;
        if (esp_codec_dev_open((esp_codec_dev_handle_t)_mic, &fs) != ESP_OK) {
            ESP_LOGE(TAG, "ouverture micro echouee");
            return false;
        }
        _mic_channels = 1;
    } else {
        _mic_channels = 2;
    }

    /* 37.5 dB est le maximum du gain materiel de l'ES7210. Le reste du
       gain est applique en logiciel dans la tache micro (XZ_MIC_SW_GAIN). */
    esp_codec_dev_set_in_gain((esp_codec_dev_handle_t)_mic, 37.5f);
    return true;
}

bool XzClient::ensure_decoder(int sample_rate)
{
    if (sample_rate <= 0) {
        sample_rate = XZ_RATE;
    }
    if (_dec != nullptr && _dec_rate == sample_rate) {
        return true;
    }
    if (_dec != nullptr) {
        opus_decoder_destroy((OpusDecoder *)_dec);
        _dec = nullptr;
    }
    int e = 0;
    OpusDecoder *d = opus_decoder_create(sample_rate, 1, &e);
    if (d == nullptr || e != OPUS_OK) {
        ESP_LOGE(TAG, "opus_decoder_create(%d): %d", sample_rate, e);
        return false;
    }
    _dec = d;
    _dec_rate = sample_rate;

    /* Aligne la sortie sur la frequence annoncee par le serveur. */
    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = 16;
    fs.channel = 1;
    fs.sample_rate = sample_rate;
    esp_codec_dev_close((esp_codec_dev_handle_t)_spk);
    esp_codec_dev_open((esp_codec_dev_handle_t)_spk, &fs);
    esp_codec_dev_set_out_vol((esp_codec_dev_handle_t)_spk, _volume.load());
    ESP_LOGI(TAG, "decodeur opus pret a %d Hz", sample_rate);
    return true;
}

bool XzClient::begin(void)
{
    return open_audio();
}

void XzClient::setVolume(int percent)
{
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    _volume.store(percent);
    if (_spk != nullptr) {
        esp_codec_dev_set_out_vol((esp_codec_dev_handle_t)_spk, percent);
    }
}

/* ------------------------------------------------------------------ */
/* tasks                                                              */
/* ------------------------------------------------------------------ */

void XzClient::mic_task_entry(void *arg) { static_cast<XzClient *>(arg)->mic_task(); }
void XzClient::spk_task_entry(void *arg) { static_cast<XzClient *>(arg)->spk_task(); }

void XzClient::mic_task(void)
{
    const int n = XZ_FRAME_SAMP;
    int16_t *pcm = (int16_t *)malloc(n * sizeof(int16_t));
    int16_t *raw = (int16_t *)malloc(n * 2 * sizeof(int16_t));
    uint8_t *enc = (uint8_t *)malloc(2000);
    OpusEncoder *e = (OpusEncoder *)_enc;

    if (pcm == nullptr || raw == nullptr || enc == nullptr || e == nullptr) {
        ESP_LOGE(TAG, "memoire micro insuffisante");
        free(pcm);
        free(raw);
        free(enc);
        _mic_run.store(false);
        vTaskDelete(nullptr);
        return;
    }

    bool first_frame = true;
    int tx_count = 0;
    bool running = false;
    for (;;) {
        if (!_mic_run.load()) {
            if (running) {
                ESP_LOGI(TAG, "micro ferme");
                running = false;
                first_frame = true;
                tx_count = 0;
            }
            _mic_level.store(0);
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }
        if (!running) {
            ESP_LOGI(TAG, "micro ouvert");
            running = true;
        }

        if (_mic_channels == 2) {
            if (esp_codec_dev_read((esp_codec_dev_handle_t)_mic, raw,
                                   n * 2 * sizeof(int16_t)) != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            for (int i = 0; i < n; i++) {
                pcm[i] = raw[i * 2];
            }
        } else {
            if (esp_codec_dev_read((esp_codec_dev_handle_t)_mic, pcm,
                                   n * sizeof(int16_t)) != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
        }

        /* Gain logiciel : le gain materiel de l'ES7210 plafonne a 37.5 dB. */
        for (int i = 0; i < n; i++) {
            int v = (int)pcm[i] * XZ_MIC_SW_GAIN;
            pcm[i] = v > 32767 ? 32767 : (v < -32768 ? -32768 : (int16_t)v);
        }

        /* Niveau pour l'animateur de l'interface (le PCM n'est pas modifie). */
        int peak = frame_peak(pcm, n);
        _mic_level.store(peak_to_level(peak));

        if (first_frame) {
            ESP_LOGI(TAG, "premiere trame micro : crete %d / 32767", peak);
            first_frame = false;
        }

        int len = opus_encode(e, pcm, n, enc, 2000);
        if (len > 1) {
            if (++tx_count <= 3) {
                ESP_LOGI(TAG, "trame %d encodee : %d o", tx_count, len);
            }
            ws_send_bin(enc, len);
        } else if (len < 0) {
            ESP_LOGW(TAG, "opus_encode: %d", len);
        }
    }
}

void XzClient::spk_task(void)
{
    int16_t *pcm = (int16_t *)malloc(5760 * sizeof(int16_t));
    if (pcm == nullptr) {
        _spk_run.store(false);
        vTaskDelete(nullptr);
        return;
    }

    for (;;) {
        size_t got = 0;
        void *p = xRingbufferReceive((RingbufHandle_t)_rb, &got, pdMS_TO_TICKS(80));
        if (p == nullptr) {
            _spk_level.store(0);
            continue;
        }
        if (_spk_run.load() && _dec != nullptr && got > 0) {
            int n = opus_decode((OpusDecoder *)_dec, (const unsigned char *)p, (opus_int32)got,
                                pcm, 5760, 0);
            if (n > 0) {
                /* Niveau pour l'animateur de l'interface, avant le codec. */
                _spk_level.store(peak_to_level(frame_peak(pcm, n)));
                esp_codec_dev_write((esp_codec_dev_handle_t)_spk, pcm, n * (int)sizeof(int16_t));
            }
        }
        vRingbufferReturnItem((RingbufHandle_t)_rb, p);
    }
    free(pcm);
    vTaskDelete(nullptr);
}

/* ------------------------------------------------------------------ */
/* WebSocket                                                          */
/* ------------------------------------------------------------------ */

bool XzClient::ws_send_text(const std::string &s)
{
    std::lock_guard<std::mutex> g(_ws_m);
    if (_ws == nullptr) {
        return false;
    }
    return esp_websocket_client_send_text((esp_websocket_client_handle_t)_ws, s.c_str(),
                                          (int)s.size(), pdMS_TO_TICKS(2000)) >= 0;
}

bool XzClient::ws_send_bin(const uint8_t *d, int len)
{
    std::lock_guard<std::mutex> g(_ws_m);
    if (_ws == nullptr) {
        return false;
    }
    return esp_websocket_client_send_bin((esp_websocket_client_handle_t)_ws, (const char *)d,
                                         len, pdMS_TO_TICKS(2000)) >= 0;
}

void XzClient::ws_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)base;
    XzClient *self = static_cast<XzClient *>(arg);
    auto *ev = static_cast<esp_websocket_event_data_t *>(data);

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "websocket connecte");
        self->_connected_flag.store(true);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "websocket deconnecte");
        self->_connected_flag.store(false);
        self->_ws_ready.store(false);
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "websocket erreur");
        self->_connected_flag.store(false);
        self->_ws_ready.store(false);
        break;

    case WEBSOCKET_EVENT_DATA:
        if (ev == nullptr || ev->data_len <= 0) {
            break;
        }
        if (ev->op_code == 0x02) {
            self->on_binary((const uint8_t *)ev->data_ptr, ev->data_len);
        } else if (ev->op_code == 0x01) {
            self->_rx.append(ev->data_ptr, ev->data_len);
            /* Les messages du serveur sont petits et non fragmentes en
               pratique ; on tente le parsing et on garde le reste si
               jamais il en manque un morceau. */
            cJSON *probe = cJSON_Parse(self->_rx.c_str());
            if (probe != nullptr) {
                cJSON_Delete(probe);
                std::string msg;
                msg.swap(self->_rx);
                self->on_json(msg);
            }
        }
        break;

    default:
        break;
    }
}

void XzClient::on_binary(const uint8_t *data, int len)
{
    if (_rb == nullptr) {
        return;
    }
    xRingbufferSend((RingbufHandle_t)_rb, data, len, pdMS_TO_TICKS(100));
}

void XzClient::on_json(const std::string &text)
{
    cJSON *root = cJSON_Parse(text.c_str());
    if (root == nullptr) {
        return;
    }
    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }

    const char *t = type->valuestring;

    if (strcmp(t, "hello") == 0) {
        cJSON *sid = cJSON_GetObjectItem(root, "session_id");
        if (cJSON_IsString(sid)) {
            _session_id = sid->valuestring;
        }
        cJSON *ap = cJSON_GetObjectItem(root, "audio_params");
        if (cJSON_IsObject(ap)) {
            cJSON *sr = cJSON_GetObjectItem(ap, "sample_rate");
            cJSON *fd = cJSON_GetObjectItem(ap, "frame_duration");
            if (cJSON_IsNumber(sr)) {
                _srv_rate = sr->valueint;
            }
            if (cJSON_IsNumber(fd)) {
                _srv_frame_ms = fd->valueint;
            }
        }
        ensure_decoder(_srv_rate);
        _ws_ready.store(true);
        setState(XZ_READY);
        setHint("Pret");
    } else if (strcmp(t, "stt") == 0) {
        cJSON *txt = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(txt)) {
            setUserText(txt->valuestring);
        }
        setState(XZ_THINKING);
        setHint("Je reflechis");
    } else if (strcmp(t, "tts") == 0) {
        cJSON *st = cJSON_GetObjectItem(root, "state");
        const char *s = cJSON_IsString(st) ? st->valuestring : "";
        if (strcmp(s, "start") == 0) {
            setBotText("");
            setState(XZ_SPEAKING);
            setHint("Je parle");
        } else if (strcmp(s, "sentence_start") == 0) {
            cJSON *txt = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(txt)) {
                std::string cur = botText();
                if (!cur.empty()) {
                    cur += " ";
                }
                cur += txt->valuestring;
                setBotText(cur);
            }
        } else if (strcmp(s, "stop") == 0) {
            setState(XZ_READY);
            setHint("Pret");
        }
    } else if (strcmp(t, "llm") == 0) {
        /* emotion : sans objet ici */
    } else if (strcmp(t, "alert") == 0) {
        cJSON *msg = cJSON_GetObjectItem(root, "message");
        if (cJSON_IsString(msg)) {
            setHint(msg->valuestring);
        }
    } else if (strcmp(t, "error") == 0) {
        cJSON *msg = cJSON_GetObjectItem(root, "message");
        setHint(cJSON_IsString(msg) ? msg->valuestring : "erreur serveur");
        setState(XZ_ERROR);
    }

    cJSON_Delete(root);
}

/* ------------------------------------------------------------------ */
/* connexion                                                          */
/* ------------------------------------------------------------------ */

void XzClient::connect_task_entry(void *arg) { static_cast<XzClient *>(arg)->connect_task(); }

void XzClient::connectAsync(void)
{
    if (!open_audio()) {
        setState(XZ_ERROR);
        setHint("Audio indisponible");
        return;
    }
    if (state() == XZ_CONNECTING || state() == XZ_ACTIVATION) {
        return;
    }
    setState(XZ_CONNECTING);
    setHint("Connexion");
    xTaskCreatePinnedToCore(connect_task_entry, "xz_connect", 12288, this, 5, nullptr, 0);
}

void XzClient::connect_task(void)
{
    std::string dev = device_mac();
    std::string uuid = device_uuid();

    for (;;) {
        if (state() == XZ_OFF) {
            break;
        }

        /* Au demarrage la carte n'est pas encore associee : attendre une
           adresse IP evite de bruler une tentative pour rien. */
        for (int i = 0; i < 300; i++) {
            if (net_up()) {
                break;
            }
            if (state() == XZ_OFF) {
                vTaskDelete(nullptr);
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        XzOta ota = xz_ota_fetch(dev.c_str(), uuid.c_str());

        if (!ota.code.empty()) {
            {
                std::lock_guard<std::mutex> g(_m);
                _code = ota.code;
            }
            setState(XZ_ACTIVATION);
            setHint("Code " + ota.code + " sur xiaozhi.me");
            ESP_LOGI(TAG, "activation requise : %s", ota.code.c_str());
            /* On re-interroge : des que le code est saisi, le serveur
               renvoie l'URL websocket definitive. */
            for (int i = 0; i < 10; i++) {
                vTaskDelay(pdMS_TO_TICKS(500));
                if (state() == XZ_OFF) {
                    vTaskDelete(nullptr);
                    return;
                }
            }
            continue;
        }

        if (!ota.ok) {
            setState(XZ_ERROR);
            setHint(ota.error.empty() ? "Serveur injoignable" : ota.error);
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        /* WebSocket */
        std::string hdrs;
        if (!ota.token.empty()) {
            std::string tok = ota.token;
            if (tok.find(' ') == std::string::npos) {
                tok = "Bearer " + tok;
            }
            hdrs += "Authorization: " + tok + "\r\n";
        }
        hdrs += "Protocol-Version: 1\r\n";
        hdrs += "Device-Id: " + dev + "\r\n";
        hdrs += "Client-Id: " + uuid + "\r\n";

        esp_websocket_client_config_t cfg = {};
        cfg.uri = ota.url.c_str();
        cfg.headers = hdrs.c_str();
        cfg.buffer_size = 4096;
        cfg.task_stack = 8192;
        cfg.network_timeout_ms = 12000;
        cfg.reconnect_timeout_ms = 0;
        cfg.disable_auto_reconnect = true;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;

        esp_websocket_client_handle_t ws = esp_websocket_client_init(&cfg);
        if (ws == nullptr) {
            setState(XZ_ERROR);
            setHint("WS init");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, ws_event_handler, this);
        {
            std::lock_guard<std::mutex> g(_ws_m);
            _ws = ws;
        }
        esp_websocket_client_start(ws);

        bool up = false;
        for (int i = 0; i < 100; i++) {
            if (_connected_flag.load()) {
                up = true;
                break;
            }
            if (state() == XZ_OFF) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        if (!up) {
            ESP_LOGE(TAG, "connexion websocket echouee");
            {
                std::lock_guard<std::mutex> g(_ws_m);
                _ws = nullptr;
            }
            esp_websocket_client_destroy(ws);
            setState(XZ_ERROR);
            setHint("Connexion impossible");
            vTaskDelay(pdMS_TO_TICKS(8000));
            continue;
        }

        /* hello montant */
        char hello[320];
        snprintf(hello, sizeof(hello),
                 "{\"type\":\"hello\",\"version\":1,"
                 "\"features\":{\"mcp\":false},"
                 "\"transport\":\"websocket\","
                 "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":%d,"
                 "\"channels\":1,\"frame_duration\":%d}}",
                 XZ_RATE, XZ_FRAME_MS);
        ws_send_text(hello);
        ESP_LOGI(TAG, "hello envoye");

        for (int i = 0; i < 120; i++) {
            if (_ws_ready.load()) {
                break;
            }
            if (state() == XZ_OFF) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        if (!_ws_ready.load()) {
            ESP_LOGE(TAG, "pas de hello serveur (code d'activation non valide ?)");
            setState(XZ_ERROR);
            setHint("Serveur muet : appaire l'appareil");
            vTaskDelay(pdMS_TO_TICKS(8000));
            {
                std::lock_guard<std::mutex> g(_ws_m);
                _ws = nullptr;
            }
            esp_websocket_client_destroy(ws);
            continue;
        }

        /* Canal audio pret : on lance la lecture. */
        _spk_run.store(true);
        setState(XZ_READY);
        setHint("Pret");

#ifdef XZ_TEST_AUTO_LISTEN
        vTaskDelay(pdMS_TO_TICKS(1200));
        ESP_LOGW(TAG, "TEST : ecoute automatique 4 s");
        startListening();
        vTaskDelay(pdMS_TO_TICKS(4000));
        stopListening();
#endif
        while (state() != XZ_OFF) {
            vTaskDelay(pdMS_TO_TICKS(500));
            if (!_connected_flag.load()) {
                ESP_LOGW(TAG, "liaison perdue");
                break;
            }
        }

        _spk_run.store(false);
        {
            std::lock_guard<std::mutex> g(_ws_m);
            _ws = nullptr;
        }
        esp_websocket_client_destroy(ws);
        if (state() == XZ_OFF) {
            break;
        }
        setState(XZ_CONNECTING);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    _spk_run.store(false);
    _mic_run.store(false);
    vTaskDelete(nullptr);
}

void XzClient::disconnect(void)
{
    setState(XZ_OFF);
    _mic_run.store(false);
    _spk_run.store(false);
    _ws_ready.store(false);
}

/* ------------------------------------------------------------------ */
/* dialogue                                                           */
/* ------------------------------------------------------------------ */

void XzClient::startListening(void)
{
    if (state() != XZ_READY) {
        return;
    }
    if (!ensure_decoder(_srv_rate)) {
        return;
    }
    setState(XZ_LISTENING);
    setHint("J'ecoute");

    char msg[160];
    snprintf(msg, sizeof(msg),
             "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"start\",\"mode\":\"manual\"}",
             _session_id.c_str());
    ws_send_text(msg);

    _mic_run.store(true);
}

void XzClient::stopListening(void)
{
    if (state() != XZ_LISTENING) {
        return;
    }
    _mic_run.store(false);

    char msg[160];
    snprintf(msg, sizeof(msg),
             "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"stop\"}",
             _session_id.c_str());
    ws_send_text(msg);

    setState(XZ_THINKING);
    setHint("Je reflechis");
}

void XzClient::abortSpeaking(void)
{
    char msg[160];
    snprintf(msg, sizeof(msg),
             "{\"session_id\":\"%s\",\"type\":\"abort\",\"reason\":\"wake_word_detected\"}",
             _session_id.c_str());
    ws_send_text(msg);
    /* vide la file de lecture */
    if (_rb != nullptr) {
        size_t got = 0;
        void *p;
        while ((p = xRingbufferReceive((RingbufHandle_t)_rb, &got, 0)) != nullptr) {
            vRingbufferReturnItem((RingbufHandle_t)_rb, p);
        }
    }
    setState(XZ_READY);
    setHint("Pret");
}
