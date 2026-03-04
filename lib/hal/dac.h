#pragma once
// ╔══════════════════════════════════════════════════════════════════════════╗
//  hal/dac.h  –  Driver do DAC MCP4725
// ╚══════════════════════════════════════════════════════════════════════════╝
//
// ── SOBRE O MCP4725 ───────────────────────────────────────────────────────────
//
//  DAC I2C de 12 bits com saída 0–VDD (3,3 V no ESP32).
//  Resolução: 3300 mV / 4096 = ~0,8 mV por passo.
//  Alimentado a 3,3 V → VDD = 3,3 V → DAC máximo (4095) = ~3,3 V no FB.
//
//  A saída é conectada ao pino FB do XL4015. Com FB >> Vref (1,25 V),
//  o XL4015 reduz duty cycle ao mínimo → V_out ≈ 0 V ("fonte desligada").
//
// ── ESTRATÉGIA DE INICIALIZAÇÃO SEM PICO DE TENSÃO ───────────────────────────
//
//  Problema:
//    O XL4015 é analógico e inicia instantaneamente ao ser energizado.
//    O ESP32 demora ~200–500 ms no bootloader antes de executar o setup().
//    Nesse intervalo, o pino FB do XL4015 fica flutuante ou em valor
//    indefinido → XL4015 pode forçar V_out ao máximo → pico de tensão
//    na saída mesmo com a fonte "desligada" pelo software.
//
//  Solução — EEPROM do MCP4725:
//    O MCP4725 possui 12 bits de EEPROM interna. Ao ser energizado,
//    ele carrega automaticamente o valor da EEPROM para sua saída,
//    ANTES de qualquer comunicação I2C. Se a EEPROM contiver 4095,
//    o DAC já inicia com FB = 3,3 V → XL4015 bloqueado desde o início.
//
//    Gravação deve ser feita UMA ÚNICA VEZ (EEPROM tem ~1 milhão de ciclos,
//    mas gravar em todo boot a esgotaria em ~10 anos de uso contínuo).
//    Use o comando serial "burn" apenas na primeira instalação do firmware.
//
//  Fluxo ao energizar o circuito:
//
//    t=0 ms    Alimentação sobe
//    t=~1 ms   MCP4725 carrega EEPROM → DAC saída = 4095 → FB = 3,3 V
//    t=~2 ms   XL4015 inicia, lê FB = 3,3 V >> Vref → V_out = 0 V  ✔
//    t=~300 ms ESP32 termina bootloader, entra no setup()
//    t=~310 ms Wire.begin() + psu.begin() → forceOff() confirma DAC = 4095
//    t=...     setOutput(true) → EMA rampa de 0 → V_set
//
// ── PROTOCOLO I2C DO MCP4725 ──────────────────────────────────────────────────
//
//  Fast Write (2 bytes) — apenas registrador de saída, sem EEPROM:
//    [ADDR+W] [0 0 D11..D8] [D7..D0]
//    Tempo a 1 MHz: ~18 µs  ← usado em writeVoltage() e forceOff()
//
//  Normal Write com EEPROM (3 bytes) — grava registrador E EEPROM:
//    [ADDR+W] [0 1 1 PD1 PD0 x x x] [D11..D4] [D3..D0 0 0 0 0]
//    Bits [6:5] = 11 → Write DAC and EEPROM
//    Tempo de escrita na EEPROM: ~25–50 ms (datasheet §1.0 Table 1-2)
//    ← usado APENAS em burnEEPROM(), chamado uma única vez
//
// ─────────────────────────────────────────────────────────────────────────────

#include <Wire.h>
#include "config.h"
#include <Arduino.h>

namespace hal {

class DAC {
public:

    // ─────────────────────────────────────────────────────────────────────────
    // begin()  –  Verifica presença do MCP4725 no barramento I2C.
    // ─────────────────────────────────────────────────────────────────────────
    bool begin() {
        Wire.beginTransmission(MCP4725_I2C_ADDR);
        return (Wire.endTransmission() == 0);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // writeVoltage()  –  Escreve tensão no registrador de saída (Fast Write).
    //
    // Uso normal no ciclo de controle (~18 µs a 1 MHz).
    // NÃO grava EEPROM — sem desgaste.
    // ─────────────────────────────────────────────────────────────────────────
    void writeVoltage(float voltage) {
        voltage = constrain(voltage, DAC_V_MIN, DAC_V_MAX);
        const uint16_t raw = static_cast<uint16_t>(
            (voltage / DAC_VREF) * (DAC_RESOLUTION - 1)
        );
        _fastWrite(raw);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // forceOff()  –  Força DAC ao máximo (raw 4095 → ~3,3 V no FB).
    //
    // Com FB >> Vref, XL4015 reduz duty cycle ao mínimo → V_out ≈ 0 V.
    // Chamado ao desligar a saída ou ao acionar proteção OVP/OCP.
    // NÃO grava EEPROM.
    // ─────────────────────────────────────────────────────────────────────────
    void forceOff() {
        _fastWrite(DAC_RESOLUTION - 1u);  // 4095
    }

    // ─────────────────────────────────────────────────────────────────────────
    // shutdown()  –  Zera saída do DAC (raw 0 → 0 V no FB).
    // ─────────────────────────────────────────────────────────────────────────
    void shutdown() {
        _fastWrite(0);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // burnEEPROM()  –  Grava raw 4095 na EEPROM interna do MCP4725.
    //
    // ╔═══════════════════════════════════════════════════════════════════════╗
    // ║  ATENÇÃO: CHAMAR APENAS UMA VEZ na primeira instalação do firmware.  ║
    // ║  A EEPROM do MCP4725 suporta ~1.000.000 ciclos de escrita.           ║
    // ║  Gravar em todo boot esgotaria a memória em anos de uso.             ║
    // ║  Use o comando serial "burn" e jamais chame esta função no loop.     ║
    // ╚═══════════════════════════════════════════════════════════════════════╝
    //
    //  Efeito após gravar:
    //    Ao energizar o circuito, o MCP4725 carrega 4095 da EEPROM e
    //    coloca ~3,3 V no pino FB do XL4015 automaticamente, antes
    //    mesmo do ESP32 iniciar. Isso bloqueia o XL4015 desde o início,
    //    eliminando o pico de tensão na saída durante o boot.
    //
    //  Retorna true se a gravação foi confirmada (leitura de volta = 4095).
    //  A gravação da EEPROM leva ~50 ms; aguarda com polling do ACK.
    // ─────────────────────────────────────────────────────────────────────────
    bool burnEEPROM() {
        const uint16_t raw = DAC_RESOLUTION - 1u;  // 4095

        // Formato Normal Write com EEPROM (datasheet §5.3.2):
        //   Byte 0: [C2=0][C1=1][C0=1][PD1=0][PD0=0][x][x][x]
        //           C2:C0 = 011 → Write DAC Register and EEPROM
        //   Byte 1: D11..D4  (MSBs)
        //   Byte 2: D3..D0 + 0000 (LSBs com padding)
        Wire.beginTransmission(MCP4725_I2C_ADDR);
        Wire.write(0x60);                                          // C1=1, C0=1 → EEPROM
        Wire.write(static_cast<uint8_t>(raw >> 4));               // D11..D4
        Wire.write(static_cast<uint8_t>((raw & 0x0F) << 4));      // D3..D0 + 0000
        const uint8_t err = Wire.endTransmission();
        if (err != 0) return false;

        // Aguarda a EEPROM terminar a gravação (até 50 ms).
        // O MCP4725 não responde com ACK enquanto grava internamente.
        const uint32_t start = millis();
        while (millis() - start < 100) {
            Wire.beginTransmission(MCP4725_I2C_ADDR);
            if (Wire.endTransmission() == 0) break;  // ACK → gravação concluída
            delay(5);
        }

        // Verifica: lê os 3 bytes de status do MCP4725 e confirma EEPROM = 4095
        Wire.requestFrom(static_cast<uint8_t>(MCP4725_I2C_ADDR),
                         static_cast<uint8_t>(5),
                         static_cast<uint8_t>(1));
        if (Wire.available() < 5) return false;

        Wire.read();                                    // byte 0: status
        Wire.read();                                    // byte 1: DAC MSB
        Wire.read();                                    // byte 2: DAC LSB
        const uint8_t eepMSB = Wire.read();             // byte 3: EEPROM MSB
        const uint8_t eepLSB = Wire.read();             // byte 4: EEPROM LSB

        // EEPROM armazena nos bits [15:4] do word de 16 bits
        const uint16_t stored = ((static_cast<uint16_t>(eepMSB) << 4) |
                                  (eepLSB >> 4));
        return (stored == raw);  // true = gravado corretamente
    }

private:

    // Fast Write: 2 bytes, atualiza apenas o registrador de saída (~18 µs a 1 MHz)
    void _fastWrite(uint16_t raw) {
        Wire.beginTransmission(MCP4725_I2C_ADDR);
        Wire.write(static_cast<uint8_t>((raw >> 8) & 0x0F)); // 4 MSBs; C=00 (fast write)
        Wire.write(static_cast<uint8_t>(raw & 0xFF));        // 8 LSBs
        Wire.endTransmission();
    }
};

} // namespace hal
