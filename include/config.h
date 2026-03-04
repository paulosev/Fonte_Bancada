#pragma once
// ╔══════════════════════════════════════════════════════════════════════════╗
//  config.h  –  Constantes globais, limites físicos e parâmetros de sistema
//
//  Este arquivo é o único ponto de configuração do projeto.
//  Altere aqui para adaptar a outros ranges de tensão/corrente,
//  pinos de I2C, frequência de controle ou limiares de proteção.
// ╚══════════════════════════════════════════════════════════════════════════╝

// ── LIMITES FÍSICOS DA FONTE ──────────────────────────────────────────────────
//
//  Definem o range operacional válido da fonte.
//  Valores fora deste range são rejeitados pelos setters de PSU.
//
//  V_OUT_MIN = 1,3 V: limitado pela tensão de referência interna do XL4015.
//    Com V_dac < Vref (1,25 V), o XL4015 não consegue regular abaixo disso.
constexpr float V_OUT_MAX = 24.0f;    // [V] tensão máxima de saída
// V_OUT_MIN não é um limite físico de saída — é apenas o valor mínimo
// aceito como setpoint para evitar divisão por zero em K = Vref / V_set.
// 0,0001 V é tratado como "zero prático": abaixo disso o XL4015 já estará
// no mínimo absoluto de saída independentemente do controle digital.
constexpr float V_OUT_MIN = 0.0001f; // [V] zero prático (evita div/0)
constexpr float I_OUT_MAX =  5.0f;   // [A] corrente máxima de saída
constexpr float I_OUT_MIN =  0.0f;   // [A] corrente mínima (limite inferior)

// ── LIMIARES DE PROTEÇÃO ──────────────────────────────────────────────────────
//
//  OVP e OCP com histérese para evitar chattering no limiar.
//  Ver protection.h para detalhes do comportamento com histérese.
constexpr float OVP_THRESHOLD  = 25.0f;  // [V] – dispara OVP acima deste valor
constexpr float OCP_THRESHOLD  =  5.2f;  // [A] – dispara OCP acima deste valor
constexpr float OVP_HYSTERESIS =  0.5f;  // [V] – banda morta do comparador OVP
constexpr float OCP_HYSTERESIS =  0.1f;  // [A] – banda morta do comparador OCP

// ── REFERÊNCIA INTERNA DO XL4015 ─────────────────────────────────────────────
//
//  O XL4015 mantém o pino FB em Vref = 1,25 V na condição de equilíbrio.
//  Esta constante é usada nas equações de controle CV e CC para calcular
//  o fator de atenuação K = Vref / V_set.
constexpr float XL4015_VREF = 1.25f;    // [V] tensão de referência do pino FB

// ── DAC MCP4725 ───────────────────────────────────────────────────────────────
//
//  Endereço I2C padrão (A0=GND, A1=GND → 0x60).
//  DAC_VREF = tensão de alimentação do MCP4725 (VDD = 3,3 V no ESP32).
//  A saída do DAC vai de 0 V a DAC_VREF com resolução de 12 bits.
constexpr uint8_t  MCP4725_I2C_ADDR = 0x60;
constexpr float    DAC_VREF         = 3.3f;    // [V] alimentação do MCP4725
constexpr uint16_t DAC_RESOLUTION   = 4096u;   // 2^12 passos
constexpr float    DAC_V_MAX        = DAC_VREF; // [V] saída máxima do DAC
constexpr float    DAC_V_MIN        = 0.0f;     // [V] saída mínima do DAC

// ── SENSOR INA219 ─────────────────────────────────────────────────────────────
//
//  Endereço I2C padrão (A0=GND, A1=GND → 0x40).
//  Para outros endereços: A0=VCC → 0x41, A1=VCC → 0x44, ambos → 0x45.
constexpr uint8_t INA219_I2C_ADDR = 0x40;

// ── I2C FAST MODE PLUS (1 MHz) ────────────────────────────────────────────────
//
//  Frequência máxima suportada por ESP32, INA219 e MCP4725.
//  Reduz o tempo por transação de ~500 µs (100 kHz padrão) para ~100 µs,
//  diminuindo a latência total do ciclo leitura→cálculo→escrita de ~1,5 ms
//  para ~590 µs (≈ 2,5× mais rápido).
//
//  Configurado em main.cpp via Wire.setClock(I2C_FREQ_HZ).
constexpr int      PIN_SDA     = 21;
constexpr int      PIN_SCL     = 22;
constexpr uint32_t I2C_FREQ_HZ = 1000000UL;  // 1 MHz (Fast Mode Plus)

// ── TASK DE CONTROLE (FreeRTOS, Core 1) ──────────────────────────────────────
//
//  CONTROL_PERIOD_US: intervalo do timer de hardware que dispara o ciclo.
//    Mínimo teórico: 532 µs (tempo de conversão INA219 12-bit single-shot)
//    + ~70 µs (overhead I2C + cálculo) = ~602 µs.
//    Usando 700 µs dá ~100 µs de folga confortável.
//    Taxa resultante: 1 / 700 µs ≈ 1,43 kHz.
//
//  CONTROL_TASK_PRIORITY: configMAX_PRIORITIES - 1 é a maior prioridade
//    possível no FreeRTOS, garantindo que o ciclo de controle preempta
//    qualquer outra task (Serial, WiFi, etc.) imediatamente.
//
//  CONTROL_TASK_CORE: Core 1 dedicado ao controle; Core 0 para UI/Arduino.
constexpr uint32_t    CONTROL_PERIOD_US     = 700UL;               // [µs]
constexpr uint32_t    CONTROL_TASK_STACK    = 4096u;               // [bytes]
constexpr UBaseType_t CONTROL_TASK_PRIORITY = configMAX_PRIORITIES - 1;
constexpr BaseType_t  CONTROL_TASK_CORE     = 1;

// ── SERIAL / UI (Core 0, loop()) ─────────────────────────────────────────────
//
//  Intervalo de impressão periódica de telemetria na Serial.
//  Valor alto o suficiente para não poluir o monitor; baixo o suficiente
//  para feedback visual em tempo real ao usuário.
constexpr uint32_t SERIAL_PRINT_MS = 200u;  // [ms] → 5 Hz

// ── VALORES PADRÃO AO LIGAR ───────────────────────────────────────────────────
//
//  A saída começa DESLIGADA (setOutput(false)) independentemente destes valores.
//  Ao ligar com "on", estes setpoints são usados.
constexpr float DEFAULT_V_SET = 5.0f;   // [V]
constexpr float DEFAULT_I_SET = 1.0f;   // [A]

// ── BUZZER ────────────────────────────────────────────────────────────────────
// GPIO de saída para buzzer passivo/ativo de sinalização de proteção.
// O buzzer é acionado quando OVP ou OCP dispara e silenciado ao resetar.
constexpr int      PIN_BUZZER        = 18;
constexpr uint32_t BUZZER_BEEP_MS    = 200u;  // duração de cada beep [ms]
constexpr uint32_t BUZZER_INTERVAL_MS= 400u;  // intervalo entre beeps [ms]
