#pragma once
// ╔══════════════════════════════════════════════════════════════════════════╗
//  hal/dac.h  –  Driver do DAC MCP4725 (tensão de feedback para XL4015)
// ╚══════════════════════════════════════════════════════════════════════════╝
//
// ── SOBRE O MCP4725 ───────────────────────────────────────────────────────────
//
//  DAC I2C de 12 bits com saída de tensão proporcional à alimentação (VDD).
//  Alimentado a 3,3 V, gera tensões de 0 V a 3,3 V com resolução de
//  3300 mV / 4096 = ~0,8 mV por passo.
//
//  Esta saída é conectada ao pino FB do XL4015 para implementar o
//  "divisor de tensão virtual": V_dac controla indiretamente V_out
//  através da malha analógica interna do XL4015.
//
// ── PROTOCOLO FAST WRITE ──────────────────────────────────────────────────────
//
//  O MCP4725 suporta dois modos de escrita (datasheet §6.2):
//
//  Normal Write (3 bytes):
//    [ADDR+W] [C2 C1 PD1 PD0 x x x x] [D11..D4] [D3..D0 0 0 0 0]
//    Permite também gravar na EEPROM interna. Tempo: ~27 µs a 1 MHz.
//
//  Fast Write (2 bytes):  ← USADO AQUI
//    [ADDR+W] [PD1 PD0 D11 D10 D9 D8] [D7 D6 D5 D4 D3 D2 D1 D0]
//    Apenas atualiza o registrador de saída (sem EEPROM).
//    PD1:PD0 = 00 → modo normal (saída ativa).
//    Tempo: ~18 µs a 1 MHz (33% mais rápido que Normal Write).
//
//  Como o DAC é escrito a cada ciclo de controle (~700 µs), nunca
//  usamos gravação em EEPROM (desgastaria a memória em horas).
//
// ── CONVERSÃO DE TENSÃO PARA RAW ─────────────────────────────────────────────
//
//  raw = round(voltage / DAC_VREF × (DAC_RESOLUTION - 1))
//
//  Exemplo: 1,25 V com VDD = 3,3 V:
//    raw = 1,25 / 3,3 × 4095 = 1552
//
// ─────────────────────────────────────────────────────────────────────────────

#include <Wire.h>
#include "config.h"
#include <Arduino.h>

namespace hal {

class DAC {
public:

    // ─────────────────────────────────────────────────────────────────────────
    // begin()
    //
    // Verifica presença do MCP4725 no barramento I2C.
    // Retorna true se o dispositivo responder com ACK.
    // ─────────────────────────────────────────────────────────────────────────
    bool begin() {
        Wire.beginTransmission(MCP4725_I2C_ADDR);
        return (Wire.endTransmission() == 0);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // writeVoltage()
    //
    // Converte a tensão desejada [V] para valor RAW 12-bit e envia ao DAC
    // via Fast Write. O valor é limitado ao intervalo [DAC_V_MIN, DAC_V_MAX]
    // para proteger o XL4015 de receber tensão de feedback fora do esperado.
    //

    // ─────────────────────────────────────────────────────────────────────────
    void writeVoltage(float voltage) {
        voltage = constrain(voltage, DAC_V_MIN, DAC_V_MAX);
        const uint16_t raw = static_cast<uint16_t>(
            (voltage / DAC_VREF) * (DAC_RESOLUTION - 1)
        );
        _fastWrite(raw);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // shutdown()
    //
    // Escreve 0 no DAC, levando V_dac a 0 V.
    // Com FB = 0 V (abaixo de Vref), o XL4015 tentaria elevar o duty cycle
    // ao máximo, mas sem o divisor externo a malha não fecha normalmente.
    // Na prática, o XL4015 reduz V_out ao mínimo possível (próximo de 0 V),
    // efetivamente desligando a saída regulada.
    //
    // Chamado ao desabilitar a saída ou ao acionar proteção OVP/OCP.
    // ─────────────────────────────────────────────────────────────────────────
    void shutdown() {
        _fastWrite(0);
    }

private:

    // ─────────────────────────────────────────────────────────────────────────
    // _fastWrite()  –  Fast Write MCP4725 (2 bytes)
    //
    // Formato dos bytes enviados (datasheet §6.2.1):
    //
    //   Byte 1: [ 0 ][ 0 ][ D11 ][ D10 ][ D9 ][ D8 ][ 0 ][ 0 ]
    //            PD1  PD0   MSBs do valor de 12 bits       (últimos 2 bits ignorados)
    //
    //   Byte 2: [ D7 ][ D6 ][ D5 ][ D4 ][ D3 ][ D2 ][ D1 ][ D0 ]
    //            LSBs do valor de 12 bits
    //
    //   PD1:PD0 = 00 → Normal operation (saída ativa)
    //   PD1:PD0 = 01/10/11 → Power-down modes (não usados aqui)
    //
    // A 1 MHz, esta transação leva ~18 µs (START + 2 bytes + STOP).
    // ─────────────────────────────────────────────────────────────────────────
    void _fastWrite(uint16_t raw) {
        Wire.beginTransmission(MCP4725_I2C_ADDR);
        Wire.write(static_cast<uint8_t>((raw >> 8) & 0x0F)); // 4 MSBs; PD=00
        Wire.write(static_cast<uint8_t>(raw & 0xFF));        // 8 LSBs
        Wire.endTransmission();
    }
};

} // namespace hal
