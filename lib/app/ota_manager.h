#pragma once
// ╔══════════════════════════════════════════════════════════════════════════╗
//  app/ota_manager.h  –  OTA via WiFi Access Point + Duplo Reset
// ╚══════════════════════════════════════════════════════════════════════════╝
//
// ── MODO ACCESS POINT ─────────────────────────────────────────────────────────
//
//  O ESP32 cria sua própria rede WiFi — sem roteador, sem credenciais.
//  Basta conectar o notebook na rede "Fonte-OTA" e fazer o upload.
//
//  Vantagens:
//    - Funciona em qualquer lugar, sem depender de rede externa
//    - Sem arquivo de credenciais para gerenciar
//    - Sem risco de expor senha WiFi no código
//
// ── FLUXO ─────────────────────────────────────────────────────────────────────
//
//  1. Duplo reset (2× RST em < DRD_TIMEOUT_S segundos)
//  2. ESP cria rede WiFi "Fonte-OTA" (aberta, sem senha)
//  3. Buzzer: 3 beeps curtos → modo OTA ativo
//  4. Conecte o notebook na rede "Fonte-OTA"
//  5. No PlatformIO: descomente upload_port = 192.168.4.1 e faça upload
//     Ou use Arduino IDE → Ferramentas → Porta → "fonte-bancada (192.168.4.1)"
//  6. Upload concluído → ESP reinicia com novo firmware
//  7. Sem upload em OTA_TIMEOUT_MS → AP desliga, volta ao normal
//
// ── ENDEREÇO IP ───────────────────────────────────────────────────────────────
//
//  No modo AP, o ESP32 usa o IP padrão 192.168.4.1.
//  Este é o IP fixo do ArduinoOTA no modo AP — sem DHCP necessário.
//
// ── INDICAÇÃO PELO BUZZER ─────────────────────────────────────────────────────
//
//  Modo OTA ativo:   3 beeps curtos repetidos a cada 3 s
//  OTA concluído:    reinicia automaticamente (sem beep)
//  Timeout:          1 beep longo → AP desliga
//
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include "config.h"
#include "buzzer.h"

namespace app {

class OTAManager {
public:
    explicit OTAManager(hal::Buzzer& buzzer) : _buzzer(buzzer) {}

    // ─────────────────────────────────────────────────────────────────────────
    // begin()
    //
    // Cria o Access Point, registra o hostname mDNS e inicia o ArduinoOTA.
    // Retorna true se tudo inicializou corretamente.
    // ─────────────────────────────────────────────────────────────────────────
    bool begin() {
        Serial.println("\n[OTA] Duplo reset detectado — iniciando modo OTA");
        Serial.printf("[OTA] Criando rede WiFi: \"%s\"\n", OTA_AP_SSID);

        // Cria AP aberto (sem senha) com IP padrão 192.168.4.1
        WiFi.mode(WIFI_AP);
        if (!WiFi.softAP(OTA_AP_SSID)) {
            Serial.println("[OTA] Falha ao criar AP. Voltando ao normal.");
            WiFi.mode(WIFI_OFF);
            return false;
        }

        Serial.printf("[OTA] AP ativo. IP: %s\n",
                      WiFi.softAPIP().toString().c_str());

        // mDNS: permite acessar como "fonte-bancada.local" em vez do IP
        if (MDNS.begin(OTA_HOSTNAME)) {
            Serial.printf("[OTA] mDNS: %s.local\n", OTA_HOSTNAME);
        }

        // ArduinoOTA sem senha
        ArduinoOTA.setHostname(OTA_HOSTNAME);
        // Sem setPassword() → upload livre na rede AP privada

        ArduinoOTA.onStart([this]() {
            _uploading = true;
            const String type = (ArduinoOTA.getCommand() == U_FLASH)
                                ? "firmware" : "filesystem";
            Serial.printf("\n[OTA] Iniciando upload: %s\n", type.c_str());
        });

        ArduinoOTA.onEnd([]() {
            Serial.println("\n[OTA] Concluido. Reiniciando...");
        });

        ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
            Serial.printf("[OTA] %u%%\r", progress * 100 / total);
        });

        ArduinoOTA.onError([this](ota_error_t error) {
            _uploading = false;
            Serial.printf("\n[OTA] Erro [%u]: ", error);
            if      (error == OTA_AUTH_ERROR)    Serial.println("Autenticacao");
            else if (error == OTA_BEGIN_ERROR)   Serial.println("Inicio");
            else if (error == OTA_CONNECT_ERROR) Serial.println("Conexao");
            else if (error == OTA_RECEIVE_ERROR) Serial.println("Recepcao");
            else if (error == OTA_END_ERROR)     Serial.println("Finalizacao");
        });

        ArduinoOTA.begin();

        _active       = true;
        _startTime    = millis();
        _lastBeepTime = millis();

        // 3 beeps curtos: modo OTA pronto
        _beep(100); delay(150);
        _beep(100); delay(150);
        _beep(100);

        Serial.println("[OTA] Pronto para upload.");
        Serial.println("[OTA] 1. Conecte o notebook na rede \"Fonte-OTA\"");
        Serial.printf("[OTA] 2. Upload via IP: %s  ou  hostname: %s.local\n",
                      WiFi.softAPIP().toString().c_str(), OTA_HOSTNAME);
        Serial.println("[OTA] 3. PlatformIO: descomente upload_port no platformio.ini");
        Serial.printf("[OTA] Timeout em %lu minutos sem upload.\n",
                      OTA_TIMEOUT_MS / 60000UL);

        return true;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // handle()
    //
    // Processa eventos OTA e verifica timeout.
    // Retorna false quando o modo OTA encerrou.
    // ─────────────────────────────────────────────────────────────────────────
    bool handle() {
        if (!_active) return false;

        ArduinoOTA.handle();

        // Beep periódico a cada 3 s enquanto aguarda
        if (!_uploading && millis() - _lastBeepTime >= OTA_BEEP_INTERVAL_MS) {
            _lastBeepTime = millis();
            _beep(80); delay(100);
            _beep(80); delay(100);
            _beep(80);
        }

        // Timeout
        if (!_uploading && millis() - _startTime >= OTA_TIMEOUT_MS) {
            Serial.println("\n[OTA] Timeout. Desligando AP e voltando ao normal.");
            stop();
            return false;
        }

        return true;
    }

    // Para o modo OTA e desliga o AP
    void stop() {
        ArduinoOTA.end();
        MDNS.end();
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
        _active    = false;
        _uploading = false;
        _beep(800);  // beep longo = encerrado
        Serial.println("[OTA] AP desligado. Fonte operando normalmente.");
    }

    bool isActive()    const { return _active; }
    bool isUploading() const { return _uploading; }

private:
    hal::Buzzer& _buzzer;
    bool         _active       {false};
    bool         _uploading    {false};
    uint32_t     _startTime    {0};
    uint32_t     _lastBeepTime {0};

    void _beep(uint32_t ms) {
        digitalWrite(PIN_BUZZER, HIGH);
        delay(ms);
        digitalWrite(PIN_BUZZER, LOW);
    }
};

} // namespace app
