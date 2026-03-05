#pragma once
// ╔══════════════════════════════════════════════════════════════════════════╗
//  app/ota_manager.h  –  OTA via página web (ElegantOTA + AsyncWebServer)
// ╚══════════════════════════════════════════════════════════════════════════╝
//
// ── MODO DE USO ───────────────────────────────────────────────────────────────
//
//  1. Duplo reset (2× RST) OU botão na tela TFT
//  2. ESP cria rede WiFi "Fonte-OTA" (sem senha)
//  3. Conecte o notebook na rede "Fonte-OTA"
//  4. Abra o navegador em:  http://192.168.4.1/update
//  5. Selecione o arquivo .bin gerado pelo PlatformIO:
//       .pio/build/esp32dev/firmware.bin
//  6. Clique Upload — barra de progresso aparece na página
//  7. ESP reinicia automaticamente com o novo firmware
//
// ── GERAR O .BIN NO PLATFORMIO ────────────────────────────────────────────────
//
//  Menu: Project Tasks → esp32dev → General → Build
//  O arquivo fica em: .pio/build/esp32dev/firmware.bin
//  Ou pelo terminal:  pio run
//
// ── VANTAGENS SOBRE ArduinoOTA ────────────────────────────────────────────────
//
//  - Interface web no navegador — sem PlatformIO conectado
//  - Barra de progresso visual
//  - Funciona em qualquer dispositivo com navegador (celular, tablet)
//  - Não precisa de porta serial nem de configuração no platformio.ini
//
// ── ARQUITETURA ───────────────────────────────────────────────────────────────
//
//  AsyncWebServer roda em background (task interna do AsyncTCP).
//  ElegantOTA.loop() é chamado no Core 0 a cada tick do UIManager.
//  O controle no Core 1 não é afetado durante o upload.
//
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ElegantOTA.h>
#include <WebSerial.h>
#include "config.h"
#include "buzzer.h"

namespace app {

class OTAManager {
public:
    explicit OTAManager(hal::Buzzer& buzzer)
        : _buzzer(buzzer), _server(80) {}

    // ─────────────────────────────────────────────────────────────────────────
    // begin()  –  Cria AP, sobe servidor web e registra ElegantOTA
    // ─────────────────────────────────────────────────────────────────────────
    bool begin() {
        Serial.println("\n[OTA] Iniciando modo OTA via página web");
        Serial.printf("[OTA] Criando rede WiFi: \"%s\"\n", OTA_AP_SSID);

        WiFi.mode(WIFI_AP);
        if (!WiFi.softAP(OTA_AP_SSID)) {
            Serial.println("[OTA] Falha ao criar AP.");
            WiFi.mode(WIFI_OFF);
            return false;
        }

        Serial.printf("[OTA] AP ativo. IP: %s\n",
                      WiFi.softAPIP().toString().c_str());

        // Página raiz: redireciona para /update
        _server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
            req->redirect("/update");
        });

        // ElegantOTA registra a rota /update no servidor
        ElegantOTA.begin(&_server);

        // Callbacks de progresso e conclusão
        ElegantOTA.onStart([this]() {
            _uploading = true;
            _progress  = 0;
            Serial.println("\n[OTA] Upload iniciado...");
        });

        ElegantOTA.onProgress([this](size_t current, size_t total) {
            _progress = static_cast<uint8_t>(current * 100 / total);
            Serial.printf("[OTA] %u%%\r", _progress);
        });

        ElegantOTA.onEnd([this](bool success) {
            _uploading = false;
            _finished  = true;
            if (success) {
                Serial.println("\n[OTA] Upload concluido! Reiniciando...");
            } else {
                Serial.println("\n[OTA] Falha no upload.");
                _finished = false;
            }
        });

        // WebSerial — monitor serial pelo navegador em /webserial
        WebSerial.begin(&_server);

        _server.begin();

        _active    = true;
        _startTime = millis();
        _lastBeep  = millis();

        // 3 beeps curtos: modo OTA ativo
        _beep(100); delay(150);
        _beep(100); delay(150);
        _beep(100);

        Serial.println("[OTA] Servidor web ativo.");
        Serial.printf("[OTA] Firmware:  http://%s/update\n", WiFi.softAPIP().toString().c_str());
        Serial.printf("[OTA] Monitor:   http://%s/webserial\n", WiFi.softAPIP().toString().c_str());
        Serial.printf("[OTA] Timeout:   %lu min\n", OTA_TIMEOUT_MS / 60000UL);

        return true;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // handle()  –  Chamado no loop() do Core 0
    //
    // ElegantOTA v3 com AsyncWebServer não precisa de loop() explícito,
    // mas mantemos para o timeout e beep periódico.
    // Retorna false quando encerrado (timeout).
    // ─────────────────────────────────────────────────────────────────────────
    bool handle() {
        if (!_active) return false;

        ElegantOTA.loop();  // necessário para ElegantOTA v2; no-op na v3

        // Beep periódico enquanto aguarda
        if (!_uploading && millis() - _lastBeep >= OTA_BEEP_INTERVAL_MS) {
            _lastBeep = millis();
            _beep(80); delay(100);
            _beep(80); delay(100);
            _beep(80);
        }

        // Timeout
        if (!_uploading && millis() - _startTime >= OTA_TIMEOUT_MS) {
            Serial.println("\n[OTA] Timeout. Desligando AP.");
            stop();
            return false;
        }

        return true;
    }

    // log(): envia para Serial + WebSerial simultaneamente
    void log(const char* msg) {
        Serial.println(msg);
        if (_active) WebSerial.println(msg);
    }
    void log(const String& msg) { log(msg.c_str()); }

    void stop() {
        _server.end();
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
        _active    = false;
        _uploading = false;
        _beep(800);
        Serial.println("[OTA] AP desligado.");
    }

    bool     isActive()      const { return _active; }
    bool     isUploading()   const { return _uploading; }
    uint8_t  getProgress()   const { return _progress; }
    uint32_t getStartTime()  const { return _startTime; }

private:
    hal::Buzzer&      _buzzer;
    AsyncWebServer    _server;
    bool              _active    {false};
    bool              _uploading {false};
    bool              _finished  {false};
    uint8_t           _progress  {0};
    uint32_t          _startTime {0};
    uint32_t          _lastBeep  {0};

    void _beep(uint32_t ms) {
        digitalWrite(PIN_BUZZER, HIGH);
        delay(ms);
        digitalWrite(PIN_BUZZER, LOW);
    }
};

} // namespace app
