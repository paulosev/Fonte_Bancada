#pragma once
// ╔══════════════════════════════════════════════════════════════════════════╗
//  app/double_reset.h  –  Detector de duplo reset via EEPROM
// ╚══════════════════════════════════════════════════════════════════════════╝
//
//  Implementação própria sem dependência externa.
//
//  O botão RST do ESP32-WROOM-32E aciona o pino EN, gerando POWERON_RESET
//  que apaga a RTC memory. Por isso usamos a EEPROM (flash NVS simulada
//  pela lib EEPROM do Arduino ESP32), que sobrevive a qualquer reset.
//
//  Funcionamento:
//    No setup(), chama detect():
//      - Lê flag da EEPROM
//      - Se flag == MAGIC: duplo reset detectado → limpa flag → retorna true
//      - Se flag != MAGIC: primeiro reset → grava flag + timestamp → retorna false
//
//    No loop(), chama tick():
//      - Se passou DRD_TIMEOUT_S desde o primeiro reset sem 2º reset →
//        limpa o flag (janela expirou, próximo reset é novo ciclo)
//
//  Uso:
//    DoubleReset drd;
//    void setup() {
//        if (drd.detect()) { ... modo OTA ... }
//    }
//    void loop() {
//        drd.tick();
//    }
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <EEPROM.h>
#include "config.h"

namespace app {

class DoubleReset {
public:
    // Endereço na EEPROM onde guardamos o flag (4 bytes magic + 4 bytes timestamp)
    static constexpr int  EEPROM_SIZE = 16;
    static constexpr int  ADDR_MAGIC  = 0;
    static constexpr int  ADDR_TIME   = 4;
    static constexpr uint32_t MAGIC   = 0xDEAD1234UL;

    // Deve ser chamado logo no início do setup(), antes de qualquer delay.
    // Retorna true se duplo reset detectado.
    bool detect() {
        EEPROM.begin(EEPROM_SIZE);

        uint32_t magic = _readU32(ADDR_MAGIC);
        uint32_t savedTime = _readU32(ADDR_TIME);
        uint32_t now = millis();  // ~0ms no início do setup

        Serial.printf("[DRD] EEPROM magic: 0x%08X (esperado: 0x%08X)\\n", magic, MAGIC);
        Serial.printf("[DRD] Tempo salvo: %lu ms atras\\n", now - savedTime);

        if (magic == MAGIC) {
            // Flag presente — segundo reset dentro da janela?
            uint32_t elapsed = now - savedTime;
            // elapsed será ~0 porque millis() mal começou — usamos o valor
            // absoluto armazenado e comparamos com o tempo atual do boot
            // Na prática: se o magic está lá, é porque foi gravado no reset anterior
            // e ainda não expirou (tick() apagaria se tivesse expirado)
            Serial.println("[DRD] Duplo reset detectado!");
            _clearFlag();
            _detected = true;
            return true;
        } else {
            // Primeiro reset: grava flag
            Serial.println("[DRD] Primeiro reset — gravando flag...");
            _writeU32(ADDR_MAGIC, MAGIC);
            _writeU32(ADDR_TIME, now);
            EEPROM.commit();
            _flagTime = now;
            _flagActive = true;
            return false;
        }
    }

    // Chame no loop() — expira o flag após DRD_TIMEOUT_S
    void tick() {
        if (!_flagActive) return;
        if (millis() - _flagTime >= (uint32_t)(DRD_TIMEOUT_S * 1000.0f)) {
            Serial.println("[DRD] Janela expirada — flag limpo.");
            _clearFlag();
        }
    }

    bool wasDetected() const { return _detected; }

private:
    bool     _flagActive {false};
    bool     _detected   {false};
    uint32_t _flagTime   {0};

    void _clearFlag() {
        _writeU32(ADDR_MAGIC, 0);
        _writeU32(ADDR_TIME, 0);
        EEPROM.commit();
        _flagActive = false;
    }

    uint32_t _readU32(int addr) {
        uint32_t val = 0;
        for (int i = 0; i < 4; i++)
            val |= ((uint32_t)EEPROM.read(addr + i)) << (i * 8);
        return val;
    }

    void _writeU32(int addr, uint32_t val) {
        for (int i = 0; i < 4; i++)
            EEPROM.write(addr + i, (val >> (i * 8)) & 0xFF);
    }
};

} // namespace app
