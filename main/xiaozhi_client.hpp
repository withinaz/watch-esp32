/*
 * xiaozhi_client - client du protocole "XiaoZhi" (assistant vocal)
 *
 * Le firmware XiaoZhi officiel n'est pas reutilisable ici (il exige ESP-IDF
 * >= 6.0.1 et c'est un firmware complet). On reimplemente donc le cote
 * appareil, qui est simple :
 *
 *   1. POST sur l'URL OTA  -> soit un code d'activation a 6 chiffres,
 *                             soit {websocket:{url,token}}
 *   2. WebSocket           -> "hello" puis trames Opus brutes
 *   3. JSON                -> listen / abort  (montant)
 *                             stt / tts / llm (descendant)
 *
 * Le serveur fait STT -> LLM -> TTS ; la cle du LLM (DeepSeek ou autre)
 * vit dans la console du serveur, jamais sur l'appareil.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Etat courant, lu par l'interface (jamais modifie depuis une task audio) */
enum XzState {
    XZ_OFF = 0,      /* pas connecte                */
    XZ_CONNECTING,   /* OTA + WebSocket en cours    */
    XZ_ACTIVATION,   /* en attente du code sur xiaozhi.me */
    XZ_READY,        /* pret a ecouter              */
    XZ_LISTENING,    /* micro ouvert, on envoie     */
    XZ_THINKING,     /* le serveur reflechit        */
    XZ_SPEAKING,     /* on joue la reponse          */
    XZ_ERROR,
};

/* Resultat de l'appel OTA */
struct XzOta {
    bool ok = false;
    std::string url;
    std::string token;
    std::string code;     /* code d'activation, si non appaire */
    std::string message;  /* message a afficher, ex "xiaozhi.me\n123456" */
    std::string error;
};

XzOta xz_ota_fetch(const char *device_id, const char *client_id);

class XzClient {
public:
    static XzClient &get();

    /* Prepare l'audio (codecs, opus, buffers). Idempotent. */
    bool begin(void);

    /* OTA puis WebSocket, dans une task dediee : ne bloque pas LVGL. */
    void connectAsync(void);
    void disconnect(void);
    bool connected(void) const { return _ws_ready.load(); }

    void startListening(void);
    void stopListening(void);
    void abortSpeaking(void);

    void setVolume(int percent);

    /* Lecture d'etat pour l'UI (thread-safe) */
    XzState state(void) const { return (XzState)_state.load(); }
    std::string hint(void);
    std::string userText(void);
    std::string botText(void);
    std::string activationCode(void);

    /* Numeros de version : l'UI ne redessine que si ca change */
    uint32_t textVersion(void) const { return _text_ver.load(); }

    /* Niveau sonore 0..100 pour l'animateur de l'interface.
       - micLevel() : crete de la derniere trame micro (valide en XZ_LISTENING)
       - spkLevel() : crete de la derniere trame decodee (valide en XZ_SPEAKING)
       Les deux valent 0 quand il n'y a pas d'audio. Lecture seule, sans effet
       sur le son : c'est uniquement de quoi animer des barres. */
    int micLevel(void) const { return _mic_level.load(); }
    int spkLevel(void) const { return _spk_level.load(); }

private:
    XzClient() = default;
    XzClient(const XzClient &) = delete;
    XzClient &operator=(const XzClient &) = delete;

    void setState(XzState s);
    void setHint(const std::string &h);
    void setUserText(const std::string &t);
    void setBotText(const std::string &t);
    void touchText(void);

    bool open_audio(void);
    bool open_mic(void);
    bool ensure_decoder(int sample_rate);
    void mic_task(void);
    void spk_task(void);
    void connect_task(void);

    static void mic_task_entry(void *arg);
    static void spk_task_entry(void *arg);
    static void connect_task_entry(void *arg);
    static void ws_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);

    void on_json(const std::string &text);
    void on_binary(const uint8_t *data, int len);
    bool ws_send_text(const std::string &s);
    bool ws_send_bin(const uint8_t *d, int len);

    std::atomic<int> _state{XZ_OFF};
    std::atomic<bool> _ws_ready{false};
    std::atomic<bool> _mic_run{false};
    std::atomic<bool> _spk_run{false};
    std::atomic<bool> _connected_flag{false};
    std::atomic<uint32_t> _text_ver{0};
    std::atomic<int> _volume{90};
    std::atomic<int> _mic_level{0};   /* 0..100, ecrit par la task micro */
    std::atomic<int> _spk_level{0};   /* 0..100, ecrit par la task haut-parleur */

    std::mutex _m;
    std::string _hint;
    std::string _user;
    std::string _bot;
    std::string _code;

    std::mutex _ws_m;
    void *_ws = nullptr;

    /* audio */
    void *_spk = nullptr;
    void *_mic = nullptr;
    void *_rb = nullptr;
    void *_enc = nullptr;
    void *_dec = nullptr;
    int _dec_rate = 0;
    int _mic_channels = 2;
    bool _audio_ok = false;

    /* Les deux tasks audio sont permanentes (creees une fois dans begin) et
       pilotent leur travail par _mic_run / _spk_run : plus de creation de
       task a chaque appui, donc plus de course. Leurs piles vivent en PSRAM
       parce que libopus empile beaucoup (USE_ALLOCA). */
    void *_mic_stack = nullptr;
    void *_spk_stack = nullptr;
    StaticTask_t _mic_tcb = {};
    StaticTask_t _spk_tcb = {};
    bool _tasks_started = false;

    std::string _session_id;
    int _srv_rate = 24000;
    int _srv_frame_ms = 60;

    std::string _rx;   /* accumulation JSON fragmentaire */
};
