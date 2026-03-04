// ╔══════════════════════════════════════════════════════════════════════════╗
//  FONTE DE BANCADA DIGITAL  –  ESP32 + XL4015 + INA219 + MCP4725
//  Versão: 1.0
// ╚══════════════════════════════════════════════════════════════════════════╝
//
// ── VISÃO GERAL ──────────────────────────────────────────────────────────────
//
//  Fonte de bancada controlada digitalmente com tensão máxima de 24 V e
//  corrente máxima de 5 A.  O controle é feito por um "divisor de tensão
//  virtual ativo": em vez do divisor resistivo fixo que o XL4015 usa para
//  fechar sua malha analógica, o ESP32 calcula dinamicamente a tensão de
//  feedback (V_dac) e a entrega ao pino FB do CI via DAC MCP4725.
//
// ── HARDWARE ─────────────────────────────────────────────────────────────────
//
//  ┌─────────────┐    I2C 1 MHz     ┌─────────────┐
//  │   ESP32     │ ──────────────── │  MCP4725    │ ──→ FB pin do XL4015
//  │  (Core 1)   │                  │  DAC 12-bit │
//  │             │ ──────────────── │  INA219     │ ←── V_out + I_out
//  │  (Core 0)   │    I2C 1 MHz     └─────────────┘
//  └─────────────┘
//       │
//       └── XL4015 (step-down) → saída 1,3 V–24 V / 5 A
//
//  Componentes:
//    XL4015   – CI step-down; malha de controle analógica fechada via FB
//    MCP4725  – DAC I2C 12-bit; gera V_dac para o pino FB do XL4015
//    INA219   – Sensor I2C de tensão/corrente 12-bit no barramento de saída
//
// ── PRINCÍPIO DE CONTROLE ────────────────────────────────────────────────────
//
//  O XL4015 regula sua saída mantendo FB = 1,25 V (Vref interno).
//  O divisor resistivo original foi substituído por este cálculo digital:
//
//    Modo CV (tensão constante):
//      K     = Vref / V_set          →  fator de atenuação
//      V_dac = V_out × K
//      Quando V_out == V_set  →  V_dac == Vref  →  XL4015 em equilíbrio
//
//    Modo CC (corrente constante):
//      K_i   = Vref / I_set          →  ganho (V/A)
//      V_dac = I_out × K_i
//      Quando I_out == I_set  →  V_dac == Vref  →  XL4015 em equilíbrio
//
// ── FEATURES ─────────────────────────────────────────────────────────────────
//
//  ✔ Modo CV  – regulação de tensão constante (1,3 V a 24 V)
//  ✔ Modo CC  – regulação de corrente constante (0 A a 5 A)
//  ✔ OVP      – proteção contra sobretensão com histérese
//  ✔ OCP      – proteção contra sobrecorrente com histérese
//  ✔ P_out    – cálculo de potência de saída em tempo real (W)
//  ✔ EEPROM   – gravacao unica da EEPROM do DAC (comando 'burn'): DAC inicia em 3,3V no power-on
//  ✔ EMA      – rampa suave de setpoint: elimina pico na partida e troca de V_set
//  ✔ On/Off   – liga/desliga saída via DAC máximo (sem pino enable no XL4015)
//  ✔ Buzzer   – sinalização sonora de OVP/OCP (GPIO18, beep contínuo)
//  ✔ Interface serial para controle e monitoramento
//
// ── ARQUITETURA DE SOFTWARE ───────────────────────────────────────────────────
//
//  O ESP32 é dual-core; os dois cores são usados de forma dedicada:
//
//  Core 1  │  Task "ctrl"  (FreeRTOS, prioridade máxima)
//          │  Disparada por timer de hardware a cada 700 µs (≈ 1,4 kHz)
//          │
//          │  Ciclo de controle (tempo total ~590 µs):
//          │    ① triggerPrimary()    ~20 µs  → INA219 inicia conversão HW
//          │    ── INA219 converte 532 µs autonomamente (CPU livre) ──
//          │    ── proteções com dado anterior, leitura de setpoints ──
//          │    ② readPrimary()       ~20 µs  → polling CNVR, lê registrador
//          │    ③ computeFeedback()    ~1 µs  → cálculo float
//          │    ④ writeVoltage()      ~18 µs  → Fast Write I2C para MCP4725
//          │    ⑤ tickSecondary()     ~20 µs  → canal secundário (proteção)
//          │
//  Core 0  │  Arduino loop()  (prioridade normal)
//          │  Serial / UI / printStatus — nunca atrasa o controle
//
//  Dados compartilhados (setpoints ↔ telemetria) são protegidos por
//  portENTER_CRITICAL / portEXIT_CRITICAL para garantir atomicidade
//  de variáveis float em arquitetura dual-core.
//
// ── CANAL PRIORITÁRIO POR MODO ────────────────────────────────────────────────
//
//  O INA219 converte um canal de cada vez em modo single-shot.
//  O canal mais importante para a equação de controle converte a cada ciclo.
//  O canal secundário (proteção/monitoramento) atualiza a ~143 Hz.
//
//    Modo CV → primário: V_bus (controle)   secundário: I_shunt (OCP)
//    Modo CC → primário: I_shunt (controle) secundário: V_bus   (OVP)
//
// ── I2C FAST MODE PLUS (1 MHz) ───────────────────────────────────────────────
//
//  O clock I2C padrão (100 kHz) resulta em ~500 µs por transação.
//  A 1 MHz cada transação leva ~100 µs, reduzindo a latência total
//  do ciclo de ~1,5 ms para ~590 µs (≈ 2,5× mais rápido).
//  ESP32, INA219 e MCP4725 suportam 1 MHz.
//
// ── ESTRUTURA DE ARQUIVOS ─────────────────────────────────────────────────────
//
//  include/
//    config.h              – constantes, limites, pinos, parâmetros de tempo
//  src/
//    main.cpp              – este arquivo: setup(), loop(), interface serial
//    lib/
//      hal/
//        ina219.h          – driver INA219: single-shot, pipeline, CNVR polling
//        dac.h             – driver MCP4725: Fast Write 2-byte
//      control/
//        feedback.h        – equações CV/CC e cálculo de potência
//        protection.h      – OVP e OCP com histérese
//      app/
//        psu.h             – orquestra tudo; task FreeRTOS + timer de hardware
//
// ── INTERFACE SERIAL (115200 baud) ────────────────────────────────────────────
//
//  v<val>   – define tensão de saída   ex: v12.5  →  V_set = 12,5 V
//  i<val>   – define corrente limite   ex: i2.0   →  I_set = 2,0 A
//  mcv      – seleciona modo CV (tensão constante)
//  mcc      – seleciona modo CC (corrente constante)
//  on       – habilita saída
//  off      – desabilita saída (DAC → 0 V)
//  reset    – reseta proteção OVP/OCP após falha
//  s        – imprime status atual
//
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <Wire.h>
#include "config.h"
#include "psu.h"

// ─── Instância global da fonte ───────────────────────────────────────────────
// Única instância de PSU. Contém toda a lógica de aplicação, HAL e controle.
app::PSU psu;

// ─── Protótipo ────────────────────────────────────────────────────────────────
void handleSerial();

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(200);

    Serial.println("\n========================================");
    Serial.println("  Fonte de Bancada 24V/5A  –  ESP32");
    Serial.println("========================================");

    // I2C a 1 MHz (Fast Mode Plus).
    // Deve ser configurado antes de psu.begin() pois os drivers HAL
    // usam Wire diretamente sem re-configurar o clock.
    Wire.begin(PIN_SDA, PIN_SCL);
    Wire.setClock(I2C_FREQ_HZ);

    // Registra o ponteiro da instância para uso na ISR do timer de hardware.
    // A ISR é uma função estática e não tem acesso ao 'this'; este registro
    // é o mecanismo que conecta ISR → instância → FreeRTOS task.
    // DEVE ser chamado antes de psu.begin() que instala o timer.
    app::PSU::registerInstance(&psu);

    if (!psu.begin()) {
        Serial.println("[MAIN] Falha na inicialização. Verifique as conexões I2C.");
        // Trava aqui; sem hardware funcional não há nada a fazer.
        while (true) delay(1000);
    }

    Serial.println("[MAIN] Pronto. UI rodando no Core 0.");
    Serial.println("[MAIN] Comandos disponíveis (115200 baud):");
    Serial.println("  v<val>  – tensão   (ex: v12.5)");
    Serial.println("  i<val>  – corrente (ex: i2.0)");
    Serial.println("  mcv     – modo Tensão Constante (CV)");
    Serial.println("  mcc     – modo Corrente Constante (CC)");
    Serial.println("  on/off  – liga / desliga saída");
    Serial.println("  reset   – reset de proteção OVP/OCP");
    Serial.println("  s       – status atual");
  Serial.println("  burn    – grava EEPROM do DAC (usar UMA UNICA VEZ na instalacao)");
}

// ─────────────────────────────────────────────────────────────────────────────
// loop() roda no Core 0 com prioridade normal do FreeRTOS.
// Responsabilidade: UI serial e impressão periódica de status.
// O loop de controle real roda no Core 1 de forma completamente independente;
// qualquer lentidão aqui (ex: Serial lento) não afeta o controle.
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
    static uint32_t lastPrint = 0;
    const  uint32_t now       = millis();

    // Impressão periódica de telemetria na Serial
    if (now - lastPrint >= SERIAL_PRINT_MS) {
        lastPrint = now;
        psu.printStatus();
    }

    // Processa comandos recebidos pela Serial
    handleSerial();

    // Atualiza buzzer (beep não-bloqueante, gerenciado no Core 0)
    psu.tickBuzzer();

    // Yield explícito: cede ao idle task do Core 0 para não starvar
    // o watchdog e outras tasks de baixa prioridade (ex: WiFi stack).
    vTaskDelay(1);
}

// ─────────────────────────────────────────────────────────────────────────────
// handleSerial()
//
// Lê um comando da Serial (terminado em '\n') e despacha para a API de PSU.
// Todos os setpoints passam por constrain() interno em psu.setVoltage() /
// psu.setCurrent(), então não é necessário validar aqui.
// ─────────────────────────────────────────────────────────────────────────────
void handleSerial() {
    if (!Serial.available()) return;

    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    cmd.toLowerCase();
    if (cmd.length() == 0) return;

    // v<valor> – define setpoint de tensão (ex: "v5.0", "v12", "v24")
    if (cmd.startsWith("v")) {
        psu.setVoltage(cmd.substring(1).toFloat());
        Serial.printf("[CMD] V_set = %.2f V\n", psu.getVset());
    }
    // i<valor> – define setpoint de corrente (ex: "i1.5", "i0.5")
    else if (cmd.startsWith("i")) {
        psu.setCurrent(cmd.substring(1).toFloat());
        Serial.printf("[CMD] I_set = %.3f A\n", psu.getIset());
    }
    // mcv – modo CV: XL4015 regula V_out para V_set
    else if (cmd == "mcv") {
        psu.setMode(control::Mode::CV);
        Serial.println("[CMD] Modo: CV (Tensão Constante)");
    }
    // mcc – modo CC: XL4015 regula I_out para I_set
    else if (cmd == "mcc") {
        psu.setMode(control::Mode::CC);
        Serial.println("[CMD] Modo: CC (Corrente Constante)");
    }
    // on – habilita saída; DAC começa a receber V_dac calculado
    else if (cmd == "on") {
        psu.setOutput(true);
        Serial.println("[CMD] Saída LIGADA");
    }
    // off – desabilita saída; DAC vai a 0 V, XL4015 para de regular
    else if (cmd == "off") {
        psu.setOutput(false);
        Serial.println("[CMD] Saída DESLIGADA");
    }
    // reset – limpa flags OVP/OCP após o usuário corrigir o problema
    else if (cmd == "reset") {
        psu.resetProtection();
        Serial.println("[CMD] Proteção resetada");
    }
    // s – snapshot do estado atual (mesmo que a impressão periódica)
    else if (cmd == "s") {
        psu.printStatus();
    }
    // ── Gravação única da EEPROM do MCP4725 ──────────────────────────────────
    // Grava raw 4095 (~3,3 V) na EEPROM do DAC. Após isso, ao energizar o
    // circuito o MCP4725 já inicia com FB = 3,3 V, bloqueando o XL4015
    // antes mesmo do ESP32 terminar o boot. Elimina pico de tensão no power-on.
    // USAR APENAS UMA VEZ na primeira instalação — EEPROM tem ~1M ciclos.
    else if (cmd == "burn") {
        Serial.println("[BURN] ATENCAO: Este comando grava a EEPROM do MCP4725.");
        Serial.println("[BURN] Use apenas na primeira instalacao do firmware.");
        Serial.println("[BURN] Confirme digitando 'burnok' em 5 segundos...");
        // Aguarda confirmação explícita para evitar gravação acidental
        const uint32_t deadline = millis() + 5000;
        bool confirmed = false;
        while (millis() < deadline) {
            if (Serial.available()) {
                String conf = Serial.readStringUntil('\n');
                conf.trim(); conf.toLowerCase();
                if (conf == "burnok") { confirmed = true; break; }
                else { break; }
            }
            delay(50);
        }
        if (!confirmed) {
            Serial.println("[BURN] Cancelado.");
        } else {
            Serial.println("[BURN] Gravando EEPROM com valor 4095 (3.3V)...");
            if (psu.burnDACEEPROM()) {
                Serial.println("[BURN] OK! EEPROM gravada com sucesso.");
                Serial.println("[BURN] A partir de agora o DAC inicia em 3.3V ao energizar.");
                Serial.println("[BURN] Nao use este comando novamente.");
            } else {
                Serial.println("[BURN] ERRO: Falha na gravacao ou verificacao.");
                Serial.println("[BURN] Verifique a conexao I2C e tente novamente.");
            }
        }
    }
    else {
        Serial.println("[CMD] Comando não reconhecido. Comandos: v i mcv mcc on off reset s");
    }
}
