#pragma once
// ╔══════════════════════════════════════════════════════════════════════════╗
//  control/feedback.h  –  Equações de controle e cálculo de potência
// ╚══════════════════════════════════════════════════════════════════════════╝
//
// ── PRINCÍPIO: DIVISOR DE TENSÃO VIRTUAL ATIVO ───────────────────────────────
//
//  No XL4015 original, um divisor resistivo (R1, R2) fecha a malha analógica:
//
//    V_out ──┤R1├──┬── FB ──→ comparador interno do XL4015
//            GND ──┤R2├──┘
//
//  O XL4015 ajusta seu PWM para manter FB = Vref = 1,25 V.
//  Com o divisor: FB = V_out × R2/(R1+R2) = 1,25 V  →  V_out = V_set
//
//  Aqui, R1 e R2 foram removidos e substituídos pelo DAC MCP4725.
//  O ESP32 calcula digitalmente qual tensão o pino FB deveria ter
//  para cada V_out medido, reproduzindo o comportamento do divisor:
//
//    V_dac = V_out × K       onde  K = Vref / V_set
//
//  Quando V_out == V_set:  V_dac = V_out × (Vref/V_set) = Vref → equilíbrio
//  Quando V_out  < V_set:  V_dac < Vref → XL4015 aumenta duty cycle
//  Quando V_out  > V_set:  V_dac > Vref → XL4015 reduz duty cycle
//
// ── MODO CV – TENSÃO CONSTANTE ────────────────────────────────────────────────
//
//  Objetivo: regular V_out = V_set (ex: 12,0 V)
//
//  Equação:
//    K     = Vref / V_set      [adimensional – fator de atenuação]
//    V_dac = V_out × K         [V]
//
//  Exemplo com V_set = 12 V, Vref = 1,25 V, V_out medido = 11,8 V:
//    K     = 1,25 / 12 = 0,1042
//    V_dac = 11,8 × 0,1042 = 1,229 V  (< Vref → XL4015 aumenta duty cycle)
//
// ── MODO CC – CORRENTE CONSTANTE ─────────────────────────────────────────────
//
//  Objetivo: regular I_out = I_set (ex: 2,0 A)
//
//  A mesma estrutura de divisor virtual é usada, mas expressa em corrente.
//  O "fator de atenuação" agora tem dimensão de V/A (ganho transresistivo):
//
//  Equação:
//    K_i   = Vref / I_set      [V/A]
//    V_dac = I_out × K_i       [V]
//
//  Quando I_out == I_set:  V_dac = Vref → XL4015 em equilíbrio
//  Quando I_out  > I_set:  V_dac > Vref → XL4015 reduz V_out (corrente cai)
//  Quando I_out  < I_set:  V_dac < Vref → XL4015 aumenta V_out (corrente sobe)
//
//  Exemplo com I_set = 2 A, Vref = 1,25 V, I_out medido = 2,3 A:
//    K_i   = 1,25 / 2 = 0,625
//    V_dac = 2,3 × 0,625 = 1,4375 V  (> Vref → XL4015 reduz V_out)
//
//  NOTA: Este controle é proporcional puro (sem integrador).
//  Para cargas resistivas converge naturalmente. Para cargas com
//  back-EMF (ex: motores), pode ser necessário adicionar um
//  integrador (termo I de um controlador PI) no futuro.
//
// ── PROTEÇÕES CONTRA DIVISÃO POR ZERO ────────────────────────────────────────
//
//  V_set < V_OUT_MIN  →  clampado em V_OUT_MIN (1,3 V mínimo do XL4015)
//  I_set < 0,01 A     →  clampado em 0,01 A    (evita ganho infinito)
//
// ── LIMITAÇÃO DA SAÍDA ────────────────────────────────────────────────────────
//
//  V_dac é limitado a [DAC_V_MIN, DAC_V_MAX] = [0 V, 3,3 V]
//  antes de ser enviado ao DAC. Isso garante que o MCP4725 nunca
//  receba um valor fora de seu range físico.
//
// ─────────────────────────────────────────────────────────────────────────────

#include "config.h"
#include <Arduino.h>

namespace control {

// Modos de operação da fonte
enum class Mode {
    CV,  // Constant Voltage – regula V_out para V_set
    CC   // Constant Current – regula I_out para I_set
};

// ─────────────────────────────────────────────────────────────────────────────
// computeFeedbackVoltage()
//
// Calcula a tensão a ser enviada ao pino FB do XL4015 via DAC MCP4725.
//
// Parâmetros:
//   mode   – modo de operação (CV ou CC)
//   v_out  – tensão de saída medida pelo INA219 [V]
//   i_out  – corrente de saída medida pelo INA219 [A]
//   v_set  – setpoint de tensão definido pelo usuário [V]
//   i_set  – setpoint de corrente definido pelo usuário [A]
//
// Retorna:
//   Tensão calculada para o pino FB [V], limitada a [DAC_V_MIN, DAC_V_MAX]
// ─────────────────────────────────────────────────────────────────────────────
inline float computeFeedbackVoltage(Mode  mode,
                                    float v_out,
                                    float i_out,
                                    float v_set,
                                    float i_set)
{
    float v_dac = 0.0f;

    if (mode == Mode::CV) {
        if (v_set < V_OUT_MIN) v_set = V_OUT_MIN;  // proteção divisão por zero
        const float K = XL4015_VREF / v_set;        // fator de atenuação
        v_dac = v_out * K;

    } else {  // Mode::CC
        if (i_set < 0.01f) i_set = 0.01f;           // proteção divisão por zero
        const float K_i = XL4015_VREF / i_set;      // ganho transresistivo [V/A]
        v_dac = i_out * K_i;
    }

    return constrain(v_dac, DAC_V_MIN, DAC_V_MAX);
}

// ─────────────────────────────────────────────────────────────────────────────
// computePower()
//
// Calcula a potência de saída instantânea da fonte.
//
// P = V_out × I_out
//
// Usa os valores já medidos no ciclo atual – sem custo adicional de I2C.
// O resultado é zerado se negativo para descartar leituras espúrias de
// corrente reversa que possam ocorrer com carga indutiva ou no transitório
// de liga/desliga.
//
// Parâmetros:
//   v_out – tensão de saída medida [V]
//   i_out – corrente de saída medida [A]
//
// Retorna:
//   Potência de saída [W], sempre ≥ 0
// ─────────────────────────────────────────────────────────────────────────────
inline float computePower(float v_out, float i_out)
{
    const float p = v_out * i_out;
    return (p > 0.0f) ? p : 0.0f;
}

} // namespace control
