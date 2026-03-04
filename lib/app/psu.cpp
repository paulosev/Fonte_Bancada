// ─────────────────────────────────────────────────────────────────────────────
//  app/psu.cpp
//
//  Definições que precisam estar fora do header:
//
//  1. PSU::_instance  (membro estático – ODR do C++11/14)
//
//  2. PSU::_timerISR  (ISR com IRAM_ATTR)
//     Manter o corpo no header causa 'dangerous relocation: l32r' no
//     linker Xtensa: funções IRAM_ATTR inlined arrastam literais de
//     funções vizinhas para além da janela de 256 KB do L32R.
//     Compilando aqui, isolada, o linker posiciona os literais corretamente.
// ─────────────────────────────────────────────────────────────────────────────

#include "psu.h"

// ── Membro estático ───────────────────────────────────────────────────────────
// Inicializado como nullptr; preenchido por PSU::registerInstance() em setup().
app::PSU* app::PSU::_instance = nullptr;

// ── ISR do timer de hardware ──────────────────────────────────────────────────
// IRAM_ATTR: executa da RAM para latência determinística (sem cache miss).
// Única responsabilidade: acordar a task de controle via notificação FreeRTOS.
// Não faz I2C, não acessa periféricos, não bloqueia.
void IRAM_ATTR app::PSU::_timerISR() {
    if (!_instance) return;
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(_instance->_taskHandle, &woken);
    // portYIELD_FROM_ISR não é chamado: macro com comportamento inconsistente
    // no ESP32-Arduino framework. A task acorda no próximo tick do FreeRTOS
    // (≤ 1 ms com configTICK_RATE_HZ=1000), dentro do período de 700 µs
    // com atraso máximo de uma iteração — aceitável para este design.
    (void)woken;
}
