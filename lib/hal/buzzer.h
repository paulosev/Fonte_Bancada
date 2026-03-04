#pragma once
// ╔══════════════════════════════════════════════════════════════════════════╗
//  hal/buzzer.h  –  Driver do buzzer de sinalização
// ╚══════════════════════════════════════════════════════════════════════════╝
//
// ── FUNCIONAMENTO ─────────────────────────────────────────────────────────────
//
//  O buzzer é acionado em padrão de beep contínuo enquanto uma proteção
//  estiver ativa. O padrão é gerenciado de forma não-bloqueante via tick()
//  chamado periodicamente pelo loop() no Core 0.
//
//  O Core 1 (task de controle) NÃO aciona o buzzer diretamente — apenas
//  sinaliza o estado de proteção via PSU::isProtectionTripped(). O Core 0
//  lê esse estado e chama buzzer.setActive(). Isso evita qualquer operação
//  de GPIO dentro da task de alta prioridade.
//
// ── PADRÃO DE BEEP ────────────────────────────────────────────────────────────
//
//  Ativo: ─■■■───■■■───■■■─  (BEEP_MS ligado, INTERVAL_MS desligado, repete)
//  Inativo: ───────────────  (sempre desligado)
//
// ─────────────────────────────────────────────────────────────────────────────
#include <Arduino.h>
#include "config.h"

namespace hal {

class Buzzer {
public:

    void begin() {
        pinMode(PIN_BUZZER, OUTPUT);
        digitalWrite(PIN_BUZZER, LOW);
    }

    // Ativa ou desativa o padrão de beep.
    // Ao desativar, desliga o buzzer imediatamente.
    void setActive(bool active) {
        if (!active && _active) {
            digitalWrite(PIN_BUZZER, LOW);
            _beepOn = false;
        }
        _active = active;
        if (active && !_wasActive) {
            // Começa beep imediatamente ao ativar
            _lastToggle = millis();
            digitalWrite(PIN_BUZZER, HIGH);
            _beepOn = true;
        }
        _wasActive = active;
    }

    // Deve ser chamado periodicamente no loop() do Core 0.
    // Gerencia o padrão de beep de forma não-bloqueante.
    void tick() {
        if (!_active) return;

        const uint32_t now     = millis();
        const uint32_t elapsed = now - _lastToggle;

        if (_beepOn && elapsed >= BUZZER_BEEP_MS) {
            digitalWrite(PIN_BUZZER, LOW);
            _beepOn     = false;
            _lastToggle = now;
        } else if (!_beepOn && elapsed >= BUZZER_INTERVAL_MS) {
            digitalWrite(PIN_BUZZER, HIGH);
            _beepOn     = true;
            _lastToggle = now;
        }
    }

    bool isActive() const { return _active; }

private:
    bool     _active    {false};
    bool     _wasActive {false};
    bool     _beepOn    {false};
    uint32_t _lastToggle{0};
};

} // namespace hal
