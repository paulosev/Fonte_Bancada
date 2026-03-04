#pragma once
// ╔══════════════════════════════════════════════════════════════════════════╗
//  control/ema.h  –  Filtro de Média Móvel Exponencial (EMA)
// ╚══════════════════════════════════════════════════════════════════════════╝
//
// ── O QUE É E POR QUE USAR ────────────────────────────────────────────────────
//
//  Quando o usuário muda o setpoint (ex: 5 V → 12 V) ou liga a fonte,
//  passar o novo valor diretamente na equação de feedback cria um degrau:
//
//    K = Vref / V_set  →  com V_set pulando de 5→12, K cai abruptamente
//    V_dac = V_out × K →  XL4015 recebe feedback muito baixo, força duty
//                          cycle ao máximo de uma vez → pico de tensão.
//
//  O filtro EMA suaviza a transição ao longo de vários ciclos:
//
//    V_ema[n] = α × V_set + (1 − α) × V_ema[n−1]
//
// ── VISUALIZAÇÃO ──────────────────────────────────────────────────────────────
//
//  V_set (entrada):  5V ─────────────────┐ 12V
//  V_ema (saída):    5V ─────────────────╱‾‾‾‾‾‾‾‾ 12V   (rampa suave)
//  Sem EMA:          5V ─────────────────┘                (degrau → pico)
//
// ── α COMO PARÂMETRO DE TEMPLATE ──────────────────────────────────────────────
//
//  α é passado como parâmetro de template (não depende de config.h).
//  Isso resolve o problema de visibilidade de constantes entre libs do PlatformIO
//  e permite instanciar filtros com alphas diferentes no mesmo projeto.
//
//  Constante de tempo: τ ≈ período_ciclo / α
//  Com CONTROL_PERIOD_US = 700 µs:
//
//    α = 0.005f  →  τ ≈ 140 ms  →  99% do valor final em ~700 ms  (padrão)
//    α = 0.001f  →  τ ≈ 700 ms  →  99% em ~3,5 s  (mais lento)
//    α = 0.010f  →  τ ≈  70 ms  →  99% em ~350 ms (mais rápido)
//
// ── CUSTO COMPUTACIONAL ───────────────────────────────────────────────────────
//
//  Uma multiplicação + uma adição + uma subtração float por ciclo.
//  Na FPU Xtensa LX6 do ESP32: < 1 µs total. Negligenciável.
//
// ── USO ───────────────────────────────────────────────────────────────────────
//
//  // Declara com alpha = 0.005 embutido no tipo
//  EMA<0.005f> ema_v(0.0f);
//
//  // No ciclo de controle:
//  float v_smooth = ema_v.update(v_set_target);
//
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>

namespace control {

// Alpha como parâmetro de template não-tipo (float, C++20).
// No C++17/14 do Xtensa usamos uma abordagem compatível: alpha como
// constante de membro estático inicializada no template.
template<int AlphaMillis>
// AlphaMillis = alpha × 1000 (ex: alpha=0.005 → AlphaMillis=5)
// Evita float como parâmetro de template (não suportado antes do C++20).
class EMA {
public:
    // alpha real = AlphaMillis / 1000.0f
    static constexpr float ALPHA = AlphaMillis / 1000.0f;

    explicit EMA(float initial = 0.0f) : _value(initial) {}

    // Avança o filtro um passo. Retorna o valor suavizado.
    float update(float target) {
        _value = ALPHA * target + (1.0f - ALPHA) * _value;
        return _value;
    }

    // Força o filtro para um valor imediato (sem rampa).
    void reset(float value = 0.0f) { _value = value; }

    float value() const { return _value; }

    // Retorna true se convergiu para o alvo dentro da tolerância.
    bool hasSettled(float target, float tolerance = 0.01f) const {
        const float diff = _value - target;
        return (diff > -tolerance) && (diff < tolerance);
    }

private:
    float _value;
};

// ── Aliases prontos para uso ──────────────────────────────────────────────────
// EMA_FAST  : α = 0.010 → τ ≈  70 ms
// EMA_MED   : α = 0.005 → τ ≈ 140 ms  ← padrão da fonte
// EMA_SLOW  : α = 0.001 → τ ≈ 700 ms
using EMA_FAST = EMA<10>;
using EMA_MED  = EMA<5>;
using EMA_SLOW = EMA<1>;

} // namespace control
