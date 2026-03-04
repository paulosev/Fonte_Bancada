#pragma once
// ╔══════════════════════════════════════════════════════════════════════════╗
//  hal/ina219.h  –  Driver do sensor INA219 (tensão + corrente)
// ╚══════════════════════════════════════════════════════════════════════════╝
//
// ── SOBRE O INA219 ────────────────────────────────────────────────────────────
//
//  O INA219 é um sensor I2C que mede tensão no barramento (V_bus) e tensão
//  no shunt (V_shunt), calculando corrente via registrador CURRENT quando
//  calibrado. Possui ADC de 12 bits e suporta 1 MHz no barramento I2C.
//
//  Registradores principais:
//    0x00  CONFIG      – configuração do ADC (resolução, modo, range)
//    0x01  SHUNT_V     – tensão no resistor shunt (proporcional à corrente)
//    0x02  BUS_V       – tensão no barramento de saída
//    0x03  POWER       – potência calculada internamente (não usado aqui)
//    0x04  CURRENT     – corrente calculada com base na calibração
//    0x05  CALIBRATION – define o LSB de corrente (ver equação abaixo)
//
// ── CALIBRAÇÃO ────────────────────────────────────────────────────────────────
//
//  Cal = trunc(0,04096 / (I_LSB × R_shunt))
//
//  Com R_shunt = 0,1 Ω e I_LSB escolhido = 100 µA:
//    Cal = trunc(0,04096 / (0,0001 × 0,1)) = 4096
//
//  Isso resulta em:
//    Corrente [mA] = reg_CURRENT × I_LSB_MA  (I_LSB_MA = 0,1 mA/LSB)
//    Corrente [A]  = reg_CURRENT × 0,0001
//
//  Para shunt diferente (ex: 0,05 Ω para 5 A):
//    Ajuste CAL_VALUE e I_LSB_MA proporcionalmente.
//
// ── MODO DE CONVERSÃO: SINGLE-SHOT POR CANAL ──────────────────────────────────
//
//  O INA219 suporta três modos single-shot:
//    0b001  →  apenas I_shunt (corrente)
//    0b010  →  apenas V_bus   (tensão)
//    0b011  →  ambos sequencialmente
//
//  Neste driver, o ESP32 dispara apenas o canal necessário para a equação
//  de controle do ciclo atual (canal "primário"), permitindo que o hardware
//  converta em paralelo com o processamento da CPU.
//
//  O canal secundário (proteção/monitoramento) é disparado separadamente
//  a cada SECONDARY_RATIO ciclos, sem competir com o primário.
//
// ── PIPELINE DE CONVERSÃO ─────────────────────────────────────────────────────
//
//  t=0 µs    triggerPrimary()   – ESP32 escreve CONFIG; INA219 inicia conversão
//  t=0+      CPU segue sem bloquear (proteções, setpoints, etc.)
//  t≈532 µs  hardware termina; seta bit CNVR no registrador BUS_V
//  t≈552 µs  readPrimary() detecta CNVR=1 via polling; lê registrador
//
//  A CPU nunca usa delayMicroseconds() – trabalho útil preenche o tempo.
//
// ── BIT CNVR (Conversion Ready) ───────────────────────────────────────────────
//
//  Bit [1] do registrador BUS_V (0x02).
//  Sobe para '1' quando a conversão atual termina.
//  É limpo automaticamente quando CONFIG é escrito (novo trigger).
//  Usar polling de CNVR é mais preciso que delay fixo e evita leituras
//  de dados desatualizados.
//
// ─────────────────────────────────────────────────────────────────────────────

#include <Wire.h>
#include "config.h"

namespace hal {

// ── Mapa de registradores (datasheet INA219 §8.6) ──────────────────────────
namespace reg {
    constexpr uint8_t CONFIG      = 0x00;
    constexpr uint8_t SHUNT_V     = 0x01;
    constexpr uint8_t BUS_V       = 0x02;
    constexpr uint8_t POWER       = 0x03;
    constexpr uint8_t CURRENT     = 0x04;
    constexpr uint8_t CALIBRATION = 0x05;
}

// ── Palavras de configuração (datasheet §8.6.1) ──────────────────────────────
//
//  Bit 13     BRNG  – bus voltage range: 1 = 32 V
//  Bits 12:11 PG    – shunt PGA gain:   11 = ÷8 → ±320 mV full scale
//  Bits 10:7  BADC  – bus ADC:          0011 = 12-bit / 532 µs
//  Bits  6:3  SADC  – shunt ADC:        0011 = 12-bit / 532 µs
//  Bits  2:0  MODE  – operation mode
namespace cfg {
    constexpr uint16_t BRNG_32V   = (1u     << 13);
    constexpr uint16_t PG_320MV   = (0b11u  << 11);
    constexpr uint16_t ADC_12BIT  = (0b0011u << 7);   // bus ADC  12-bit
    constexpr uint16_t SADC_12BIT = (0b0011u << 3);   // shunt ADC 12-bit
    constexpr uint16_t BASE       = BRNG_32V | PG_320MV | ADC_12BIT | SADC_12BIT;

    // Modos single-shot (bits [2:0])
    constexpr uint16_t SHOT_SHUNT = BASE | 0b001u;  // apenas corrente
    constexpr uint16_t SHOT_BUS   = BASE | 0b010u;  // apenas tensão
    constexpr uint16_t SHOT_BOTH  = BASE | 0b011u;  // ambos (usado no begin)
}

// ── Constantes de calibração ─────────────────────────────────────────────────
namespace cal {
    constexpr uint16_t VALUE     = 4096u;   // Cal = 0,04096 / (100µA × 0,1Ω)
    constexpr float    I_LSB_MA  = 0.1f;    // mA por LSB do registrador CURRENT
    constexpr float    V_BUS_LSB = 0.004f;  // V por LSB do registrador BUS_V (4 mV)
    constexpr uint8_t  V_BUS_SHR = 3;       // bits [15:3] = tensão; [1]=CNVR, [0]=OVF
    constexpr uint16_t CNVR_MASK = 0x0002u; // bit [1] do BUS_V = Conversion Ready
}

// ── Número de ciclos primários entre cada atualização do canal secundário ────
// 10 ciclos × 700 µs = 7 ms → ~143 Hz (suficiente para OVP/OCP e telemetria)
constexpr uint8_t SECONDARY_RATIO = 10u;

// ─────────────────────────────────────────────────────────────────────────────
class INA219 {
public:

    // ─────────────────────────────────────────────────────────────────────────
    // begin()
    //
    // Escreve calibração e dispara a primeira conversão dupla.
    // A calibração DEVE ser escrita antes de CONFIG, conforme datasheet §8.5.1.
    // O delayMicroseconds(1200) garante que o primeiro dado nos registradores
    // é válido antes de o loop de controle começar a ler.
    // ─────────────────────────────────────────────────────────────────────────
    bool begin() {
        // 1. Calibração define o fator de escala do registrador CURRENT
        _writeReg16(reg::CALIBRATION, cal::VALUE);

        // 2. Primeira conversão dupla para "aquecer" os registradores.
        //    Sem isso, a primeira leitura retornaria 0 ou dado inválido.
        _writeReg16(reg::CONFIG, cfg::SHOT_BOTH);

        // 3. Aguarda 2 ciclos completos: 2 × (V + I) × 532 µs = ~1,1 ms + margem
        delayMicroseconds(1200);

        // 4. Lê e descarta para garantir CNVR em estado limpo
        _readReg16(reg::BUS_V);
        _readReg16(reg::CURRENT);

        return _checkPresence();
    }

    // ─────────────────────────────────────────────────────────────────────────
    // triggerPrimary()  –  Dispara conversão do canal prioritário
    //
    // Escreve a palavra CONFIG correspondente ao modo de operação.
    // Ao receber a escrita, o INA219 inicia a conversão imediatamente.
    // Esta função retorna em ~20 µs (1 transação I2C de 3 bytes a 1 MHz).
    // O hardware converte de forma autônoma durante os próximos ~532 µs.
    //
    //   cv_mode = true  →  SHOT_BUS   (V_bus,  usado no modo CV)
    //   cv_mode = false →  SHOT_SHUNT (I_shunt, usado no modo CC)
    // ─────────────────────────────────────────────────────────────────────────
    void triggerPrimary(bool cv_mode) {
        _cvMode = cv_mode;
        _writeReg16(reg::CONFIG, cv_mode ? cfg::SHOT_BUS : cfg::SHOT_SHUNT);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // readPrimary()  –  Lê o resultado do canal prioritário
    //
    // Chama _waitCNVR() para aguardar o bit de conversão pronta e então
    // lê o registrador correto (BUS_V ou CURRENT) conforme o modo.
    //
    // Na maioria dos ciclos a conversão já terminou antes desta chamada
    // (o tempo de proteções + setpoints já consome > 532 µs), então
    // o polling retorna na primeira tentativa.
    //
    // Retorna o valor em unidades físicas (V ou A) pronto para uso na equação.
    // ─────────────────────────────────────────────────────────────────────────
    float readPrimary() {
        _waitCNVR();
        return _cvMode ? _readBusVoltage() : _readCurrent();
    }

    // ─────────────────────────────────────────────────────────────────────────
    // tickSecondary()  –  Gerencia o canal secundário (proteção/telemetria)
    //
    // Chamado uma vez por ciclo primário, APÓS readPrimary().
    // Usa um contador interno para distribuir o trabalho em dois semi-ciclos:
    //
    //   Ciclo SECONDARY_RATIO/2:  dispara conversão do canal secundário.
    //     O trigger é dado no meio do período para que a conversão termine
    //     antes do próximo tick completo.
    //
    //   Ciclo SECONDARY_RATIO:    lê o resultado e armazena em _vSecondary
    //     ou _iSecondary. O dado fica disponível via getSecondaryV/I().
    //
    // Esta divisão garante que primário e secundário nunca convertem
    // simultaneamente (o INA219 tem apenas um ADC interno).
    // ─────────────────────────────────────────────────────────────────────────
    void tickSecondary() {
        _secCounter++;

        if (_secCounter == SECONDARY_RATIO / 2) {
            // Dispara conversão do canal oposto ao primário
            _writeReg16(reg::CONFIG,
                        _cvMode ? cfg::SHOT_SHUNT : cfg::SHOT_BUS);
        }
        else if (_secCounter >= SECONDARY_RATIO) {
            _secCounter = 0;
            // Lê o resultado (conversão já terminou – tempo decorrido ≈ 5×532 µs)
            if (_cvMode) {
                _iSecondary = _readCurrent();       // corrente para OCP
            } else {
                _vSecondary = _readBusVoltage();    // tensão para OVP
            }
        }
    }

    // Getters do canal secundário – usados pelo _controlLoop() para
    // compor o par (V, I) e pela proteção
    float getSecondaryV() const { return _vSecondary; }
    float getSecondaryI() const { return _iSecondary; }

private:
    bool    _cvMode{true};       // modo atual: true = CV, false = CC
    uint8_t _secCounter{0};      // contador de ciclos do canal secundário
    float   _vSecondary{0.0f};   // último V_bus do canal secundário [V]
    float   _iSecondary{0.0f};   // último I_shunt do canal secundário [A]

    // ── Leitura dos registradores em unidades físicas ─────────────────────────

    float _readBusVoltage() {
        const int16_t raw = _readReg16(reg::BUS_V);
        // Bits [15:3] contêm a tensão; bits [1:0] são flags de status.
        // Deslocamento de 3 bits elimina os flags; LSB = 4 mV.
        return (static_cast<uint16_t>(raw) >> cal::V_BUS_SHR) * cal::V_BUS_LSB;
    }

    float _readCurrent() {
        const int16_t raw = _readReg16(reg::CURRENT);
        // Valor com sinal: corrente negativa indica fluxo reverso.
        // Conversão: raw × I_LSB_MA [mA] → ÷ 1000 → [A]
        return (raw * cal::I_LSB_MA) / 1000.0f;
    }

    // ── Polling do bit CNVR ───────────────────────────────────────────────────
    //
    // Lê o registrador BUS_V repetidamente até CNVR=1 ou timeout.
    // Timeout de 10 tentativas × ~20 µs = ~200 µs.
    // Na operação normal (ciclo ≥ 700 µs), o hardware termina antes desta
    // chamada e o polling retorna na primeira tentativa.
    // O timeout evita travamento em falha I2C ou configuração errada.
    void _waitCNVR() {
        for (uint8_t attempt = 0; attempt < 10; attempt++) {
            const uint16_t busRaw = static_cast<uint16_t>(_readReg16(reg::BUS_V));
            if (busRaw & cal::CNVR_MASK) return;  // conversão pronta
            delayMicroseconds(20);                 // aguarda 20 µs e tenta de novo
        }
        // Timeout: dado pode estar desatualizado em até 1 ciclo (700 µs),
        // mas a proteção com histérese absorve variações pontuais.
    }

    // ── I2C raw (sem biblioteca de terceiros) ─────────────────────────────────
    //
    // Escrever direto em Wire é ~30% mais rápido que usar a biblioteca
    // Adafruit_INA219, pois elimina verificações redundantes e permite
    // usar endTransmission(false) para repeated start.

    void _writeReg16(uint8_t r, uint16_t v) {
        Wire.beginTransmission(INA219_I2C_ADDR);
        Wire.write(r);
        Wire.write(static_cast<uint8_t>(v >> 8));    // MSB primeiro
        Wire.write(static_cast<uint8_t>(v & 0xFF));  // LSB
        Wire.endTransmission();
    }

    int16_t _readReg16(uint8_t r) {
        // Repeated start: aponta o registrador sem liberar o barramento,
        // evitando que outro master (se houver) insira transação entre
        // o write do ponteiro e o read dos dados.
        Wire.beginTransmission(INA219_I2C_ADDR);
        Wire.write(r);
        Wire.endTransmission(false);  // false = repeated start (sem STOP)
        Wire.requestFrom(static_cast<uint8_t>(INA219_I2C_ADDR),
                         static_cast<uint8_t>(2),
                         static_cast<uint8_t>(1));  // 1 = sendStop
        const uint8_t hi = Wire.read();
        const uint8_t lo = Wire.read();
        return static_cast<int16_t>((hi << 8) | lo);
    }

    bool _checkPresence() {
        Wire.beginTransmission(INA219_I2C_ADDR);
        return Wire.endTransmission() == 0;
    }
};

} // namespace hal
