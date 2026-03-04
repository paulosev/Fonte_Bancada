#pragma once
// ╔══════════════════════════════════════════════════════════════════════════╗
//  control/protection.h  –  Proteções OVP e OCP com histérese
// ╚══════════════════════════════════════════════════════════════════════════╝
//
// ── PROTEÇÕES IMPLEMENTADAS ───────────────────────────────────────────────────
//
//  OVP – Over Voltage Protection (proteção contra sobretensão)
//    Desliga a saída quando V_out ≥ OVP_THRESHOLD (25 V).
//    Protege a carga contra tensão excessiva por falha de controle.
//
//  OCP – Over Current Protection (proteção contra sobrecorrente)
//    Desliga a saída quando I_out ≥ OCP_THRESHOLD (5,2 A).
//    Protege a fonte e a carga contra corrente excessiva.
//
// ── HISTÉRESE ─────────────────────────────────────────────────────────────────
//
//  Sem histérese, um sinal ruidoso próximo ao limiar causaria oscilação
//  rápida entre "protegido" e "normal" (chattering), o que geraria
//  chaveamentos rápidos no DAC e instabilidade na saída.
//
//  Com histérese, o comportamento é assimétrico:
//    Ativa  quando valor ≥ THRESHOLD                    (limiar superior)
//    Desativa quando valor < THRESHOLD - HYSTERESIS     (limiar inferior)
//
//  Exemplo OVP (THRESHOLD=25 V, HYSTERESIS=0,5 V):
//
//    V_out
//    26 V ─────────────────────────────────────────────
//    25 V ────────────────────── ← ativa OVP aqui
//    24.5V ─────────────────────────── ← desativa OVP aqui (auto-reset)
//    24 V ─────────────────────────────────────────────
//
//  Nota: o auto-reset ocorre quando a condição de falha se resolve
//  (ex: carga desconectada). Para reset manual após intervenção do
//  usuário, use protection.reset().
//
// ── USO ───────────────────────────────────────────────────────────────────────
//
//  Chamada no loop de controle (Core 1) com as medições do ciclo anterior:
//
//    _protection.update(v_out, i_out);
//    if (_protection.isTripped()) { _dac.shutdown(); continue; }
//
// ─────────────────────────────────────────────────────────────────────────────

#include "config.h"

namespace control {

class Protection {
public:

    // ─────────────────────────────────────────────────────────────────────────
    // update()
    //
    // Avalia as condições de proteção com as medições atuais.
    // Deve ser chamado uma vez por ciclo de controle.
    //
    // Implementa comparador com histérese para cada proteção:
    //   - Arma  (_tripped = true)  quando valor ≥ threshold
    //   - Desarma (_tripped = false) quando valor < threshold - hysteresis
    //   - Mantém estado (_tripped) na zona de histérese (entre os dois limiares)
    // ─────────────────────────────────────────────────────────────────────────
    void update(float v_out, float i_out) {

        // ── OVP ──────────────────────────────────────────────────────────────
        if (!_ovpTripped && v_out >= OVP_THRESHOLD) {
            _ovpTripped = true;   // tensão atingiu o limiar superior → dispara
        } else if (_ovpTripped && v_out < (OVP_THRESHOLD - OVP_HYSTERESIS)) {
            _ovpTripped = false;  // tensão caiu abaixo do limiar inferior → reseta
        }

        // ── OCP ──────────────────────────────────────────────────────────────
        if (!_ocpTripped && i_out >= OCP_THRESHOLD) {
            _ocpTripped = true;   // corrente atingiu o limiar superior → dispara
        } else if (_ocpTripped && i_out < (OCP_THRESHOLD - OCP_HYSTERESIS)) {
            _ocpTripped = false;  // corrente caiu abaixo do limiar inferior → reseta
        }
    }

    // Retorna true se qualquer proteção está ativa (saída deve ser desligada)
    bool isTripped() const { return _ovpTripped || _ocpTripped; }
    bool isOVP()     const { return _ovpTripped; }
    bool isOCP()     const { return _ocpTripped; }

    // ─────────────────────────────────────────────────────────────────────────
    // reset()
    //
    // Força o desarme de todas as proteções.
    // Use apenas após verificar que V_out e I_out estão dentro dos limites
    // normais de operação. O próximo ciclo de update() reavaliará as
    // condições; se a falha persistir, a proteção será rearmada imediatamente.
    // ─────────────────────────────────────────────────────────────────────────
    void reset() {
        _ovpTripped = false;
        _ocpTripped = false;
    }

private:
    bool _ovpTripped{false};  // true = sobretensão detectada
    bool _ocpTripped{false};  // true = sobrecorrente detectada
};

} // namespace control
