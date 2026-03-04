// ╔══════════════════════════════════════════════════════════════════════════╗
//  FONTE DE BANCADA DIGITAL  –  ESP32 + XL4015 + INA219 + MCP4725
//  Versão: 1.7.0
// ╚══════════════════════════════════════════════════════════════════════════╝
//
// ── VISÃO GERAL ──────────────────────────────────────────────────────────────
//
//  Fonte de bancada controlada digitalmente com tensão máxima de 24 V e
//  corrente máxima de 5 A. O controle é feito por um "divisor de tensão
//  virtual ativo": o ESP32 calcula dinamicamente a tensão de feedback
//  (V_dac) e a entrega ao pino FB do XL4015 via DAC MCP4725.
//
// ── HARDWARE ─────────────────────────────────────────────────────────────────
//
//  XL4015   – CI step-down; malha analógica fechada via pino FB
//  MCP4725  – DAC I2C 12-bit; gera V_dac para o pino FB
//  INA219   – Sensor I2C 12-bit de tensão e corrente de saída
//  Buzzer   – Sinalização sonora (GPIO18): proteções e modo OTA
//
// ── FEATURES ─────────────────────────────────────────────────────────────────
//
//  ✔ Modo CV       – tensão constante (0 a 24 V)
//  ✔ Modo CC       – corrente constante (0 a 5 A)
//  ✔ Crossover     – troca automática CV↔CC por detecção de limite
//  ✔ OVP / OCP     – proteções com histérese + buzzer de alarme
//  ✔ P_out         – potência de saída em tempo real (W)
//  ✔ EMA           – rampa suave de setpoint (sem pico na partida/troca)
//  ✔ On/Off        – liga/desliga via DAC máximo (XL4015 sem pino enable)
//  ✔ EEPROM DAC    – MCP4725 inicia em 3,3 V sozinho (zero pico no boot)
//  ✔ OTA WiFi      – atualização de firmware via AP próprio (sem roteador)
//  ✔ Duplo Reset   – ativa modo OTA sem display/serial (2× RST em <2s)
//
// ── ARQUITETURA DE SOFTWARE ───────────────────────────────────────────────────
//
//  Core 1  │  Task "ctrl" (FreeRTOS, prio máxima, 700 µs/ciclo)
//          │  Controle CV/CC, crossover, EMA, proteções, DAC, INA219
//          │
//  Core 0  │  Arduino loop() (prioridade normal)
//          │  Serial/UI, buzzer, OTA handle, double reset check
//
// ── OTA — ATUALIZAÇÃO SEM FIO ─────────────────────────────────────────────────
//
//  1. Dê dois cliques rápidos no botão RST (dentro de 2 s)
//  2. Buzzer emite 3 beeps curtos → modo OTA ativo
//  3. Conecte o notebook na rede WiFi "Fonte-OTA" (sem senha)
//  4. No PlatformIO: descomente upload_port = 192.168.4.1 no platformio.ini
//  5. Faça upload — ESP reinicia automaticamente com o novo firmware
//  6. Sem upload em 5 min → AP desliga, volta ao normal
//
//  Não requer roteador, credenciais ou configuração prévia.
//
// ── INTERFACE SERIAL (115200 baud) ────────────────────────────────────────────
//
//  v<val>   – tensão   (ex: v12.5)
//  i<val>   – corrente (ex: i2.0)
//  mcv/mcc  – modo CV / CC (manual)
//  xon/xoff – crossover automático ligado / desligado
//  on/off   – liga / desliga saída
//  reset    – reset proteção OVP/OCP
//  s        – status
//  burn     – grava EEPROM do DAC (UMA VEZ na instalação)
//
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <Wire.h>
// khoih-prog/ESP_DoubleResetDetector: define obrigatório antes do include
#define ESP_DRD_USE_EEPROM  false   // usa RTC memory, não EEPROM flash
#define DOUBLERESETDETECTOR_DEBUG false
#include <ESP_DoubleResetDetector.h>
#include "config.h"
#include "psu.h"
#include "ota_manager.h"

// ─── Instâncias globais ───────────────────────────────────────────────────────
app::PSU psu;

// OTAManager referencia o buzzer interno do PSU via getter
// Declarado após psu para garantir ordem de construção
app::OTAManager* otaManager = nullptr;

DoubleResetDetector drd(DRD_TIMEOUT_S, 0 /* endereço EEPROM RTC */);

// ─── Protótipos ───────────────────────────────────────────────────────────────
void handleSerial();

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(200);

    Serial.println("\n========================================");
    Serial.println("  Fonte de Bancada 24V/5A  –  ESP32");
    Serial.println("  Firmware v1.8.0");
    Serial.println("========================================");

    Wire.begin(PIN_SDA, PIN_SCL);
    Wire.setClock(I2C_FREQ_HZ);

    app::PSU::registerInstance(&psu);

    if (!psu.begin()) {
        Serial.println("[MAIN] Falha na inicialização. Verifique I2C.");
        while (true) delay(1000);
    }

    // ── Verifica duplo reset ──────────────────────────────────────────────────
    // doubleResetDetected() DEVE ser chamado no setup(), antes de qualquer
    // delay longo. O DRD usa RTC memory para persistir entre resets.
    if (drd.detectDoubleReset()) {
        Serial.println("[MAIN] Duplo reset detectado → iniciando modo OTA...");
        // OTAManager precisa do buzzer; acessa via psu.getBuzzer()
        otaManager = new app::OTAManager(psu.getBuzzer());
        if (!otaManager->begin()) {
            // Falha ao conectar WiFi — descarta e opera normalmente
            delete otaManager;
            otaManager = nullptr;
        }
    } else {
        Serial.println("[MAIN] Boot normal. WiFi desligado.");
    }

    // Sinaliza fim do setup: 1 beep curto
    psu.beep(100);

    Serial.println("[MAIN] Pronto. Comandos: v i mcv mcc xon xoff on off reset s burn");
    if (otaManager && otaManager->isActive()) {
        Serial.printf("[OTA]  Aguardando upload em: %s.local\n", OTA_HOSTNAME);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void loop() {
    // ── Modo OTA ─────────────────────────────────────────────────────────────
    // Quando OTA ativo: processa eventos OTA, silencia controle serial normal.
    if (otaManager && otaManager->isActive()) {
        if (!otaManager->handle()) {
            // handle() retornou false → timeout ou erro → volta ao normal
            delete otaManager;
            otaManager = nullptr;
        }
        // Durante OTA: não processa serial nem imprime status
        // (evita interferência com o upload)
        drd.loop();
        vTaskDelay(1);
        return;
    }

    // ── Operação normal ───────────────────────────────────────────────────────
    static uint32_t lastPrint = 0;
    if (millis() - lastPrint >= SERIAL_PRINT_MS) {
        lastPrint = millis();
        psu.printStatus();
    }

    handleSerial();
    psu.tickBuzzer();

    // DRD deve ser "resetado" no loop após o tempo de detecção passar.
    // Isso limpa o flag da RTC memory para o próximo boot normal.
    drd.loop();

    vTaskDelay(1);
}

// ─────────────────────────────────────────────────────────────────────────────
void handleSerial() {
    if (!Serial.available()) return;
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    cmd.toLowerCase();
    if (cmd.length() == 0) return;

    if (cmd.startsWith("v")) {
        psu.setVoltage(cmd.substring(1).toFloat());
        Serial.printf("[CMD] V_set = %.2f V\n", psu.getVset());
    }
    else if (cmd.startsWith("i")) {
        psu.setCurrent(cmd.substring(1).toFloat());
        Serial.printf("[CMD] I_set = %.3f A\n", psu.getIset());
    }
    else if (cmd == "mcv") {
        psu.setMode(control::Mode::CV);
        Serial.println("[CMD] Modo manual: CV");
    }
    else if (cmd == "mcc") {
        psu.setMode(control::Mode::CC);
        Serial.println("[CMD] Modo manual: CC");
    }
    else if (cmd == "xon") {
        psu.setCrossoverEnabled(true);
    }
    else if (cmd == "xoff") {
        psu.setCrossoverEnabled(false);
    }
    else if (cmd == "on") {
        psu.setOutput(true);
        Serial.println("[CMD] Saída LIGADA");
    }
    else if (cmd == "off") {
        psu.setOutput(false);
        Serial.println("[CMD] Saída DESLIGADA");
    }
    else if (cmd == "reset") {
        psu.resetProtection();
        Serial.println("[CMD] Proteção resetada");
    }
    else if (cmd == "s") {
        psu.printStatus();
    }
    else if (cmd == "burn") {
        Serial.println("[BURN] ATENCAO: Grava EEPROM do MCP4725.");
        Serial.println("[BURN] Use apenas na primeira instalacao.");
        Serial.println("[BURN] Confirme digitando 'burnok' em 5 segundos...");
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
            Serial.println("[BURN] Gravando EEPROM com 4095 (3.3V)...");
            if (psu.burnDACEEPROM()) {
                Serial.println("[BURN] OK! Nao use este comando novamente.");
            } else {
                Serial.println("[BURN] ERRO: Verifique o I2C.");
            }
        }
    }
    else {
        Serial.println("[CMD] Comandos: v i mcv mcc xon xoff on off reset s burn");
    }
}
