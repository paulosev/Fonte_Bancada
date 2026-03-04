#pragma once
// ╔══════════════════════════════════════════════════════════════════════════╗
//  control/crossover.h  –  Detecção automática de crossover CV↔CC
// ╚══════════════════════════════════════════════════════════════════════════╝
//
// ── O QUE É CROSSOVER ────────────────────────────────────────────────────────
//
//  Em fontes de bancada profissionais, CV e CC não são modos exclusivos
//  escolhidos pelo usuário — são dois "reguladores" que competem pela saída.
//  O que estiver mais próximo de seu limite assume o controle.
//
//  O usuário define DOIS limites simultaneamente:
//    V_set = tensão máxima desejada
//    I_set = corrente máxima desejada
//
//  A fonte opera automaticamente em:
//    CV  →  quando a carga é leve o suficiente para I_out < I_set
//    CC  →  quando a carga forçaria I_out > I_set (tensão cede)
//
// ── DIAGRAMA DE OPERAÇÃO ──────────────────────────────────────────────────────
//
//  V_out
//  V_set ──────────────────╮  ← CV ativo (tensão regulada)
//                          │  ← ponto de crossover: V_set / I_set
//                          ╰──────────────────  ← CC ativo (corrente regulada)
//                                            → I_out
//                          I_set
//
//  Exemplo: V_set=12V, I_set=2A
//    Carga 100Ω → I=0,12A < 2A → CV mantém 12V
//    Carga   6Ω → I=2,0A  = 2A → crossover (12V / 2A)
//    Carga   2Ω → sem CC: I=6A → COM CC: mantém 2A, V_out cai para 4V
//
// ── LÓGICA DE TRANSIÇÃO ───────────────────────────────────────────────────────
//
//  CV → CC:
//    Quando I_out ≥ I_set × (1 - DEADBAND)
//    A corrente está tocando o limite → CC deve assumir para proteger a carga.
//
//  CC → CV:
//    Quando I_out < I_set × (1 - DEADBAND)
//    E V_out < V_set × (1 - DEADBAND)
//    A carga diminuiu (corrente caiu), CV pode voltar a regular a tensão.
//
//  A banda morta (DEADBAND) evita oscilação rápida entre modos quando a
//  carga opera exatamente no ponto de crossover. Com DEADBAND=0.02 (2%),
//  a transição CC→CV só ocorre quando a corrente cai 2% abaixo de I_set.
//
// ── HISTERESE DE TEMPO ────────────────────────────────────────────────────────
//
//  Um contador de ciclos exige que a condição persista por MIN_CYCLES
//  ciclos consecutivos antes de trocar o modo. Isso filtra ruído de
//  medição que poderia causar transições espúrias.
//
//  Com CONTROL_PERIOD_US=700µs e MIN_CYCLES=5: histerese de 3,5 ms.
//
// ── INTEGRAÇÃO ────────────────────────────────────────────────────────────────
//
//  Crossover crossover;
//
//  // No ciclo de controle, após ler V e I:
//  Mode mode = crossover.evaluate(v_out, i_out, v_set_ema, i_set_ema, mode);
//
//  // O modo retornado substitui o modo atual para o cálculo de V_dac.
//  // O modo interno também é atualizado e pode ser lido via getMode().
//
// ── MODO MANUAL ───────────────────────────────────────────────────────────────
//
//  O crossover pode ser desabilitado, respeitando o modo definido pelo usuário.
//  Útil para forçar CC em cargas que precisam de corrente constante pura
//  (ex: carga de bateria) ou CV em bancadas de teste precisas.
//
//    crossover.setEnabled(false);  // modo manual
//    crossover.setEnabled(true);   // crossover automático
//
// ─────────────────────────────────────────────────────────────────────────────

#include "config.h"

namespace control {

class Crossover {
public:

    // Habilita ou desabilita a detecção automática de crossover.
    // Quando desabilitado, evaluate() retorna sempre o modo passado como
    // parâmetro (comportamento idêntico ao da v18 sem crossover).
    void setEnabled(bool enabled) {
        _enabled = enabled;
        _cvToCcCount = 0;
        _ccToCvCount = 0;
    }

    bool isEnabled() const { return _enabled; }

    // Retorna o modo atual detectado (CV ou CC).
    Mode getMode() const { return _mode; }

    // ─────────────────────────────────────────────────────────────────────────
    // evaluate()  –  Avalia condição de crossover e retorna o modo correto.
    //
    // Deve ser chamado a cada ciclo de controle com os valores filtrados
    // pelo EMA (v_set_ema, i_set_ema), não com os setpoints brutos.
    // Usar o EMA garante que o crossover não oscile durante rampas de partida.
    //
    // Parâmetros:
    //   v_out      – tensão de saída medida [V]
    //   i_out      – corrente de saída medida [A]
    //   v_set_ema  – setpoint de tensão filtrado pelo EMA [V]
    //   i_set_ema  – setpoint de corrente filtrado pelo EMA [A]
    //   user_mode  – modo selecionado pelo usuário (respeitado se !_enabled)
    //
    // Retorna o modo a usar no cálculo de V_dac neste ciclo.
    // ─────────────────────────────────────────────────────────────────────────
    Mode evaluate(float v_out, float i_out,
                  float v_set_ema, float i_set_ema,
                  Mode  user_mode)
    {
        if (!_enabled) {
            _mode = user_mode;
            return _mode;
        }

        // Limiares com banda morta de 2%
        const float i_threshold = i_set_ema * (1.0f - CROSSOVER_DEADBAND);
        const float v_threshold = v_set_ema * (1.0f - CROSSOVER_DEADBAND);

        if (_mode == Mode::CV) {
            // ── CV → CC ──────────────────────────────────────────────────────
            // Corrente tocou o limite → CC deve assumir
            if (i_out >= i_threshold) {
                _cvToCcCount++;
                _ccToCvCount = 0;
                if (_cvToCcCount >= CROSSOVER_MIN_CYCLES) {
                    _mode = Mode::CC;
                    _cvToCcCount = 0;
                }
            } else {
                _cvToCcCount = 0;
            }

        } else {
            // ── CC → CV ──────────────────────────────────────────────────────
            // Corrente caiu abaixo do limite E tensão ainda não chegou ao set
            // → carga diminuiu, CV pode voltar a regular
            if (i_out < i_threshold && v_out < v_threshold) {
                _ccToCvCount++;
                _cvToCcCount = 0;
                if (_ccToCvCount >= CROSSOVER_MIN_CYCLES) {
                    _mode = Mode::CV;
                    _ccToCvCount = 0;
                }
            } else {
                _ccToCvCount = 0;
            }
        }

        return _mode;
    }

    // Força o modo interno (chamado quando o usuário troca manualmente).
    void forceMode(Mode mode) {
        _mode = mode;
        _cvToCcCount = 0;
        _ccToCvCount = 0;
    }

private:
    bool    _enabled    {true};
    Mode    _mode       {Mode::CV};
    uint8_t _cvToCcCount{0};
    uint8_t _ccToCvCount{0};
};

} // namespace control
