#pragma once
// ╔══════════════════════════════════════════════════════════════════════════╗
//  app/psu.h  –  Orquestrador principal da fonte de bancada
// ╚══════════════════════════════════════════════════════════════════════════╝
//
// ── RESPONSABILIDADE ──────────────────────────────────────────────────────────
//
//  Esta classe é o "cérebro" da fonte. Ela conecta todas as camadas:
//    HAL  (ina219, dac)  →  control (feedback, protection)  →  app (psu)
//
//  Expõe uma API limpa para o loop() do Core 0 (setpoints, getters, status)
//  e executa o ciclo de controle de alta prioridade no Core 1.
//
// ── ARQUITETURA DUAL-CORE ─────────────────────────────────────────────────────
//
//  ┌─ Core 0 (Arduino loop, prioridade normal) ──────────────────────────────┐
//  │  setVoltage(), setCurrent(), setMode(), setOutput()  ← escrita          │
//  │  getVout(), getIout(), getPout()                     ← leitura          │
//  │  printStatus()                                       ← UI               │
//  └─────────────────────────────────────────────────────────────────────────┘
//           ↕  portENTER_CRITICAL / portEXIT_CRITICAL  (spinlock)
//  ┌─ Core 1 (Task "ctrl", prioridade máxima) ────────────────────────────────┐
//  │  Timer HW → ISR → vTaskNotifyGiveFromISR() → _controlLoop()             │
//  │                                                                          │
//  │  Ciclo (~590 µs a cada 700 µs):                                         │
//  │    ① triggerPrimary()   ~20 µs  INA219 inicia conversão de hardware    │
//  │    ── hardware converte 532 µs; CPU usa o tempo para: ──────────────    │
//  │       • ler setpoints atomicamente                                       │
//  │       • atualizar proteções com medição anterior                        │
//  │    ② readPrimary()      ~20 µs  polling CNVR → lê registrador          │
//  │    ③ computeFeedback()   ~1 µs  cálculo float (V_dac)                  │
//  │    ④ writeVoltage()     ~18 µs  Fast Write I2C → MCP4725 → XL4015 FB  │
//  │    ⑤ tickSecondary()    ~20 µs  canal secundário (proteção/telemetria) │
//  └─────────────────────────────────────────────────────────────────────────┘
//
// ── CANAL PRIORITÁRIO POR MODO ────────────────────────────────────────────────
//
//  O INA219 converte um canal por vez (single-shot 12-bit, 532 µs/canal).
//  O canal que alimenta a equação de controle recebe prioridade máxima;
//  o outro canal atualiza a ~143 Hz para proteção e telemetria.
//
//    Modo CV  →  primário: V_bus (controle)    secundário: I_shunt (OCP)
//    Modo CC  →  primário: I_shunt (controle)  secundário: V_bus   (OVP)
//
// ── SINCRONIZAÇÃO ENTRE CORES ──────────────────────────────────────────────────
//
//  Variáveis compartilhadas (_vOut, _iOut, _pOut, _vSet, _iSet, _mode):
//    - Float não é atômico em ARM Cortex-M; sem proteção, uma leitura
//      de 4 bytes pode ser interrompida no meio pelo outro core.
//    - Usamos portENTER_CRITICAL (spinlock por core) que desabilita
//      preempção localmente sem desabilitar interrupções globalmente.
//    - A seção crítica é mantida o menor tempo possível (apenas a
//      cópia da variável), nunca envolve I2C ou operações lentas.
//
// ── TIMER DE HARDWARE E ISR ────────────────────────────────────────────────────
//
//  O timer (grupo 0, timer 0) tem prescaler 80 → 1 tick = 1 µs.
//  A ISR é marcada IRAM_ATTR (roda da RAM, não da flash) para
//  garantir latência determinística mesmo com cache miss.
//  A ISR não faz I2C (proibido: driver I2C usa semáforos FreeRTOS).
//  Ela apenas acorda a task com vTaskNotifyGiveFromISR(), que é
//  uma operação atômica e segura em contexto de interrupção.
//
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <Wire.h>
#include "config.h"
#include "ina219.h"
#include "dac.h"
#include "feedback.h"
#include "protection.h"
#include "buzzer.h"
#include "ema.h"
#include "crossover.h"

namespace app {

class PSU {
public:

    // ─────────────────────────────────────────────────────────────────────────
    // begin()
    //
    // Inicializa periféricos, cria a task FreeRTOS de controle e arma o
    // timer de hardware. Deve ser chamado uma única vez no setup(), após
    // Wire.begin() e PSU::registerInstance().
    //
    // Retorna false se o DAC MCP4725 não for encontrado no barramento I2C
    // (sem DAC não há controle possível; o INA219 ausente gera apenas aviso).
    // ─────────────────────────────────────────────────────────────────────────
    bool begin() {
        if (!_dac.begin()) {
            Serial.println("[PSU] ERRO: DAC MCP4725 não encontrado no I2C!");
            return false;
        }
        if (!_ina.begin()) {
            Serial.println("[PSU] AVISO: INA219 não respondeu – sem leitura de V/I.");
        }

        // Garante saída desligada antes de qualquer coisa
        _dac.forceOff();
        _buzzer.begin();
        _spinlock = portMUX_INITIALIZER_UNLOCKED;

        // ── Task de controle (Core 1, prioridade máxima) ──────────────────
        // Passa 'this' como parâmetro para que a função estática possa
        // chamar o método de instância _controlLoop().
        xTaskCreatePinnedToCore(
            _controlTaskEntry,        // função estática de entrada
            "ctrl",                   // nome da task (visível no monitor FreeRTOS)
            CONTROL_TASK_STACK,       // stack em bytes (4096 é suficiente para float + I2C)
            this,                     // parâmetro → ponteiro para esta instância
            CONTROL_TASK_PRIORITY,    // configMAX_PRIORITIES - 1  (máxima)
            &_taskHandle,             // handle usado pela ISR para notificar
            CONTROL_TASK_CORE         // Core 1 dedicado ao controle
        );

        // ── Timer de hardware (grupo 0, timer 0) ──────────────────────────
        // Prescaler 80 → clock do timer = 80 MHz / 80 = 1 MHz → 1 tick = 1 µs
        // Auto-reload: o alarme se recarrega automaticamente a cada período.
        _timer = timerBegin(0, 80, true);
        timerAttachInterrupt(_timer, _timerISR, true);     // edge trigger
        timerAlarmWrite(_timer, CONTROL_PERIOD_US, true);  // período em µs
        timerAlarmEnable(_timer);

        Serial.printf("[PSU] Controle iniciado: periodo %u us (~%.0f Hz), Core %d, prio %d\n",
                      (unsigned)CONTROL_PERIOD_US,
                      1e6f / CONTROL_PERIOD_US,
                      (int)CONTROL_TASK_CORE,
                      (int)CONTROL_TASK_PRIORITY);
        return true;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Setpoints  (escritos pelo Core 0, lidos pelo Core 1)
    //
    // Cada setter usa portENTER_CRITICAL para garantir que a escrita de
    // 4 bytes (float) não seja interrompida pelo Core 1 no meio.
    // constrain() é chamado antes de armazenar para garantir que valores
    // fora dos limites físicos nunca cheguem ao cálculo de V_dac.
    // ─────────────────────────────────────────────────────────────────────────
    void setVoltage(float v) {
        portENTER_CRITICAL(&_spinlock);
        _vSet = constrain(v, V_OUT_MIN, V_OUT_MAX);
        portEXIT_CRITICAL(&_spinlock);
    }

    void setCurrent(float i) {
        portENTER_CRITICAL(&_spinlock);
        _iSet = constrain(i, I_OUT_MIN, I_OUT_MAX);
        portEXIT_CRITICAL(&_spinlock);
    }

    void setMode(control::Mode mode) {
        portENTER_CRITICAL(&_spinlock);
        _mode = mode;
        portEXIT_CRITICAL(&_spinlock);
        // Sincroniza o estado interno do crossover para evitar que ele
        // reverta imediatamente o modo que o usuário acabou de definir.
        _crossover.forceMode(mode);
    }

    // Habilita/desabilita crossover automático CV↔CC.
    // Quando desabilitado, o modo definido por setMode() é respeitado
    // fixamente (comportamento da v18).
    void setCrossoverEnabled(bool enabled) {
        _crossover.setEnabled(enabled);
        Serial.printf("[PSU] Crossover automatico: %s\n",
                      enabled ? "HABILITADO" : "DESABILITADO");
    }

    bool isCrossoverEnabled() const { return _crossover.isEnabled(); }

    // Habilita/desabilita a saída.
    // Ao desabilitar, zera o DAC imediatamente (sem esperar o próximo ciclo)
    // para garantir resposta rápida ao comando "off".
    // Liga (true) ou desliga (false) a saída.
    // OFF: DAC vai ao máximo (4095) → FB >> Vref → XL4015 reduz V_out a ~0 V.
    // ON:  EMA é resetado para 0 → partida suave do zero até V_set/I_set.
    void setOutput(bool on) {
        portENTER_CRITICAL(&_spinlock);
        _outputEnabled = on;
        portEXIT_CRITICAL(&_spinlock);
        if (on) {
            // Reseta EMA para 0: o filtro vai rampar suavemente até o setpoint
            // atual, evitando pico de tensão na partida.
            _emaV.reset(0.0f);
            _emaI.reset(0.0f);
        } else {
            _dac.forceOff();
            _buzzer.setActive(false);
        }
    }

    // Reseta os flags de OVP/OCP após o usuário corrigir a condição de falha.
    // Deve ser chamado somente quando V_out e I_out estiverem dentro dos limites.
    void resetProtection() {
        _protection.reset();  // flags bool – acesso implicitamente atômico em 32-bit
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Getters de telemetria  (lidos pelo Core 0, escritos pelo Core 1)
    //
    // Seção crítica garante leitura consistente de variáveis float.
    // ─────────────────────────────────────────────────────────────────────────
    float getVout() {
        portENTER_CRITICAL(&_spinlock);
        float v = _vOut;
        portEXIT_CRITICAL(&_spinlock);
        return v;
    }

    float getIout() {
        portENTER_CRITICAL(&_spinlock);
        float i = _iOut;
        portEXIT_CRITICAL(&_spinlock);
        return i;
    }

    // Potência calculada como P = V_out × I_out, sempre ≥ 0.
    // Atualizada no mesmo ciclo e bloco crítico que V_out e I_out,
    // garantindo que os três valores são sempre do mesmo instante.
    float getPout() {
        portENTER_CRITICAL(&_spinlock);
        float p = _pOut;
        portEXIT_CRITICAL(&_spinlock);
        return p;
    }

    // Getters de setpoints – lidos apenas pelo Core 0 (sem seção crítica
    // necessária, pois só o Core 0 escreve e lê estes valores na UI).
    float         getVset() const { return _vSet; }
    float         getIset() const { return _iSet; }
    control::Mode getMode() const { return _mode; }
    bool isOutputEnabled()  const { return _outputEnabled; }
    bool isOVP()            const { return _protection.isOVP(); }
    bool isOCP()            const { return _protection.isOCP(); }
    bool isProtectionTripped() const { return _protection.isTripped(); }

    // ─────────────────────────────────────────────────────────────────────────
    // printStatus()
    //
    // Imprime uma linha de telemetria formatada na Serial.
    // Exemplo de saída:
    //   [PSU] Mode:CV | Vset: 5.00V Iset:1.000A | Vout: 4.98V Iout:0.995A Pout:  4.95W | Prot:OK
    // ─────────────────────────────────────────────────────────────────────────
    void printStatus() {
        const float v = getVout(), i = getIout(), p = getPout();
        // Exibe o modo ATIVO (pode diferir do setpoint do usuário quando
        // o crossover automático está habilitado e trocou o modo).
        const char* modeStr = (_crossover.getMode() == control::Mode::CV)
                              ? "CV" : "CC";
        const char* xoStr   = _crossover.isEnabled() ? "auto" : "man";
        const char* protStr = _protection.isTripped()
            ? (_protection.isOVP() ? "OVP!" : "OCP!")
            : "OK";
        Serial.printf("[PSU] Mode:%-2s(%s) | Vset:%5.2fV Iset:%4.2fA | "
                      "Vout:%5.2fV Iout:%4.3fA Pout:%6.2fW | Prot:%s\n",
                      modeStr, xoStr, _vSet, _iSet, v, i, p, protStr);
    }

    // Registra o ponteiro da instância para uso na ISR (que é estática).
    // Deve ser chamado antes de begin().
    // Deve ser chamado periodicamente no loop() do Core 0.
    // Gerencia o padrão de beep do buzzer de forma não-bloqueante.
    void tickBuzzer() {
        // Espelha o estado de proteção no buzzer
        _buzzer.setActive(_protection.isTripped());
        _buzzer.tick();
    }

    // ─────────────────────────────────────────────────────────────────────────
    // burnDACEEPROM()  –  Grava raw 4095 na EEPROM do MCP4725.
    //
    // Expõe dac.burnEEPROM() para o main.cpp sem quebrar o encapsulamento.
    // Deve ser chamado apenas via comando serial "burn" na primeira instalação.
    // Ver dac.h para detalhes completos do protocolo e advertências.
    // ─────────────────────────────────────────────────────────────────────────
    bool burnDACEEPROM() {
        return _dac.burnEEPROM();
    }

    static void registerInstance(PSU* inst) { _instance = inst; }

private:

    // ── Periféricos (HAL) ────────────────────────────────────────────────────
    hal::INA219         _ina;
    hal::Buzzer         _buzzer;        // sensor de tensão e corrente
    hal::DAC            _dac;        // DAC que alimenta o pino FB do XL4015
    control::Protection _protection; // lógica OVP + OCP com histérese

    // ── Estado compartilhado entre cores ─────────────────────────────────────
    portMUX_TYPE   _spinlock = portMUX_INITIALIZER_UNLOCKED;

    // Telemetria: escritos pelo Core 1, lidos pelo Core 0
    volatile float _vOut{0.0f};  // tensão de saída medida [V]
    volatile float _iOut{0.0f};  // corrente de saída medida [A]
    volatile float _pOut{0.0f};  // potência de saída calculada [W]

    // Setpoints: escritos pelo Core 0, lidos pelo Core 1
    float          _vSet{DEFAULT_V_SET};
    float          _iSet{DEFAULT_I_SET};
    control::Mode  _mode{control::Mode::CV};

    // Filtros EMA para rampa suave de setpoint.
    // Inicializados em 0: ao ligar, o EMA sobe suavemente de 0 até V_set/I_set,
    // eliminando o pico de tensão na partida e nas mudanças de setpoint.
    // EMA_MED: α = 0.005 → τ ≈ 140 ms → 99% do valor em ~700 ms
    // Troque por EMA_FAST ou EMA_SLOW para ajustar a velocidade da rampa.
    control::EMA_MED  _emaV{0.0f};      // rampa suave do setpoint de tensão
    control::EMA_MED  _emaI{0.0f};      // rampa suave do setpoint de corrente
    control::Crossover _crossover;       // detector de crossover CV↔CC
    bool           _outputEnabled{false};

    // ── FreeRTOS / timer de hardware ─────────────────────────────────────────
    TaskHandle_t _taskHandle{nullptr};
    hw_timer_t*  _timer{nullptr};

    // Ponteiro estático para acesso na ISR (que não tem 'this')
    static PSU* _instance;

    // ─────────────────────────────────────────────────────────────────────────
    // _timerISR()  –  ISR do timer de hardware
    //
    // Executa na IRAM para latência determinística (sem cache miss de flash).
    // Única responsabilidade: acordar a task de controle via notificação
    // FreeRTOS. Não faz I2C, não acessa periféricos, não bloqueia.
    //
    // vTaskNotifyGiveFromISR() é equivalente a um semáforo ultra-leve;
    // portYIELD_FROM_ISR() garante que o Core 1 preempta imediatamente
    // se a task de controle tiver prioridade maior que a task atual.
    // ─────────────────────────────────────────────────────────────────────────
    // Implementação em psu.cpp — corpo fora do header evita
    // 'dangerous relocation: l32r' no linker Xtensa (literais de
    // funções IRAM_ATTR inlined extrapolam a janela de 256 KB do L32R).
    static void IRAM_ATTR _timerISR();

    // Wrapper estático necessário pois xTaskCreatePinnedToCore requer
    // um ponteiro de função estático (sem 'this' implícito).
    static void _controlTaskEntry(void* param) {
        static_cast<PSU*>(param)->_controlLoop();
    }

    // ─────────────────────────────────────────────────────────────────────────
    // _controlLoop()  –  Ciclo de controle principal (Core 1)
    //
    // Estrutura do ciclo (ver diagrama no cabeçalho deste arquivo):
    //
    //  ① Dispara conversão do canal prioritário no INA219.
    //     O hardware começa a converter imediatamente de forma autônoma.
    //     O ESP32 NÃO bloqueia aguardando; segue para ②.
    //
    //  [Tempo livre ~532 µs: CPU usa para proteções e leitura de setpoints]
    //
    //  ② Lê o resultado via polling do bit CNVR.
    //     Na maioria dos ciclos a conversão já terminou; o poll retorna
    //     na primeira tentativa (~20 µs de I2C).
    //
    //  ③ Aplica a equação de feedback (CV ou CC) → V_dac.
    //
    //  ④ Escreve V_dac no MCP4725 via Fast Write (2 bytes, ~18 µs).
    //     O XL4015 ajusta seu duty cycle para que V_out → V_set.
    //
    //  ⑤ Avança o canal secundário (alterna entre disparar e ler a
    //     cada SECONDARY_RATIO ciclos, sem interferir com o primário).
    // ─────────────────────────────────────────────────────────────────────────
    void _controlLoop() {
        while (true) {
            // Bloqueia com consumo zero de CPU até o timer notificar.
            // ulTaskNotifyTake com pdTRUE limpa o contador de notificações,
            // descartando notificações acumuladas se o ciclo atrasou.
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

            // ── Lê setpoints atomicamente ────────────────────────────────
            // Cópia local evita múltiplos acessos à seção crítica e garante
            // consistência (modo e setpoints do mesmo instante).
            control::Mode mode;
            float vSet, iSet;
            portENTER_CRITICAL(&_spinlock);
            mode = _mode;
            vSet = _vSet;
            iSet = _iSet;
            portEXIT_CRITICAL(&_spinlock);

            const bool cvMode = (mode == control::Mode::CV);

            // ① Dispara conversão do canal prioritário.
            //   Modo CV → dispara V_bus; Modo CC → dispara I_shunt.
            //   A partir daqui o INA219 converte de forma autônoma (~532 µs).
            _ina.triggerPrimary(cvMode);

            // ── Usa o tempo de conversão do INA219 ──────────────────────
            // Em vez de delayMicroseconds(532), executa trabalho útil:
            // atualiza proteções com as medições do ciclo ANTERIOR.
            // Isso é seguro: _vOut/_iOut são volatile e já foram salvos
            // atomicamente no ciclo anterior.
            const float vPrev = _vOut;
            const float iPrev = _iOut;
            _protection.update(vPrev, iPrev);

            // Se proteção ativada ou saída desligada: zera DAC e pula.
            // Drena a conversão em andamento para não deixar o INA219
            // com estado inconsistente no próximo ciclo.
            if (_protection.isTripped() || !_outputEnabled) {
                // Desliga saída: DAC máximo → FB >> Vref → V_out → ~0 V
                _dac.forceOff();
                _ina.readPrimary();    // descarta resultado; limpa flag CNVR
                _ina.tickSecondary();  // mantém o canal secundário ativo
                continue;
            }

            // ② Lê resultado do canal prioritário.
            //   _waitCNVR() faz polling do bit CNVR; na maioria dos ciclos
            //   a conversão já terminou e o dado é lido imediatamente.
            const float primary = _ina.readPrimary();

            // Distribui o valor lido para V e I conforme o modo.
            // O canal secundário fornece o valor do outro canal
            // (atualizado a ~143 Hz – suficiente para proteção).
            float v, i;
            if (cvMode) {
                v = primary;             // V_bus é o canal de controle
                i = _ina.getSecondaryI(); // I_shunt vem do canal secundário
            } else {
                i = primary;             // I_shunt é o canal de controle
                v = _ina.getSecondaryV(); // V_bus vem do canal secundário
            }

            // Salva telemetria e potência para o Core 0.
            // Os três valores são escritos no mesmo bloco crítico para
            // garantir que V, I e P sejam sempre do mesmo instante.
            portENTER_CRITICAL(&_spinlock);
            _vOut = v;
            _iOut = i;
            _pOut = control::computePower(v, i);
            portEXIT_CRITICAL(&_spinlock);

            // ③ + ④ Calcula V_dac e escreve no DAC.
            //   Os setpoints passam pelo filtro EMA antes da equação,
            //   gerando uma rampa suave em vez de um degrau.
            //   Custo: ~1 µs (2 operações float na FPU do Xtensa LX6).
            const float vSetSmooth = _emaV.update(vSet);
            const float iSetSmooth = _emaI.update(iSet);

            // Crossover: avalia se deve trocar CV↔CC com base nas medições.
            // Usa os setpoints filtrados pelo EMA para não oscilar durante rampas.
            // O modo retornado pode diferir do 'mode' lido dos setpoints do usuário.
            const control::Mode activeMode = _crossover.evaluate(
                v, i, vSetSmooth, iSetSmooth, mode
            );

            const float v_dac = control::computeFeedbackVoltage(
                activeMode, v, i, vSetSmooth, iSetSmooth
            );
            _dac.writeVoltage(v_dac);

            // ⑤ Avança o canal secundário.
            //   tickSecondary() alterna internamente entre disparar a
            //   conversão (no meio do período) e ler o resultado (no fim),
            //   sem nunca conflitar com a conversão primária já concluída.
            _ina.tickSecondary();
        }
    }
};

// _instance é definido em psu.cpp (evita multiple definition em C++11/14)

} // namespace app
