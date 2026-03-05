// ╔══════════════════════════════════════════════════════════════════════════╗
//  FONTE DE BANCADA DIGITAL  –  ESP32 + XL4015 + INA219 + MCP4725 + TFT
//  Firmware v1.9.0
// ╚══════════════════════════════════════════════════════════════════════════╝
//
// ── SEPARAÇÃO DE CORES ────────────────────────────────────────────────────────
//
//  ┌─────────────────────────────────────────────────────────────┐
//  │  CORE 1  –  Task "ctrl"  (prioridade máxima, 700µs/ciclo)  │
//  │                                                             │
//  │  • Lê INA219 (V e I de saída)                              │
//  │  • Calcula V_dac via equação de feedback                    │
//  │  • Aplica filtro EMA nos setpoints                          │
//  │  • Detecta crossover CV↔CC automático                      │
//  │  • Verifica OVP / OCP                                       │
//  │  • Escreve DAC MCP4725 via I2C                              │
//  │                                                             │
//  │  O Core 1 NUNCA toca no display ou no touch.               │
//  │  Toda saída do Core 1 é via getters atômicos do PSU.        │
//  └─────────────────────────────────────────────────────────────┘
//
//  ┌─────────────────────────────────────────────────────────────┐
//  │  CORE 0  –  Arduino loop()  (prioridade normal)             │
//  │                                                             │
//  │  • UIManager::tick()  → TFT + touch                        │
//  │  • OTAManager::handle() → ArduinoOTA                       │
//  │  • drd.loop()           → double reset detector            │
//  │  • psu.tickBuzzer()     → buzzer não-bloqueante            │
//  │  • Serial (debug)       → comandos de texto opcionais       │
//  │                                                             │
//  │  O Core 0 lê o PSU via getters (atômicos) e chama         │
//  │  setVoltage/setCurrent/setOutput — protegidos por spinlock. │
//  └─────────────────────────────────────────────────────────────┘
//
// ── HARDWARE ─────────────────────────────────────────────────────────────────
//
//  XL4015   – CI step-down; malha fechada via pino FB
//  MCP4725  – DAC I2C 12-bit; gera V_dac para o pino FB (I2C)
//  INA219   – Sensor I2C de V e I (I2C)
//  ILI9488  – TFT 480×320, SPI2 (MISO=12 MOSI=13 SCK=14 CS=5 DC=2 RST=4)
//  XPT2046  – Touch SPI2 compartilhado (CS=17 IRQ=16)
//  Buzzer   – GPIO18
//
// ── OTA ───────────────────────────────────────────────────────────────────────
//
//  Ativar: 2× RST em <2s  OU  tocar ícone AP na tela principal (1s)
//          OU  menu Configurações → WiFi/OTA
//  Rede:   Fonte-OTA (aberta, sem senha)  ·  IP: 192.168.4.1
//  Upload: PlatformIO → descomente upload_port no platformio.ini
//
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>

// khoih-prog/ESP_DoubleResetDetector
// ESP_DRD_USE_EEPROM true: usa flash EEPROM em vez de RTC memory.
// Necessário porque o botão RST do ESP32-WROOM-32E gera POWERON_RESET
// que apaga a RTC memory — com EEPROM o flag persiste entre resets.
// Endereço EEPROM: 0 (reserva 512 bytes internamente pela lib)
#define ESP_DRD_USE_EEPROM      true
#define DOUBLERESETDETECTOR_DEBUG false
#include <ESP_DoubleResetDetector.h>

#include "config.h"
#include "psu.h"
#include "ota_manager.h"
#include "display.h"
#include "ui_manager.h"

// ─── Instâncias globais ───────────────────────────────────────────────────────

app::PSU        psu;
hal::Display    display;

// OTAManager é alocado dinamicamente: nullptr = WiFi desligado
app::OTAManager* otaMgr = nullptr;

DoubleResetDetector drd(DRD_TIMEOUT_S, 0);

// UIManager referencia otaMgr por ponteiro-para-ponteiro para poder
// criar/destruir o OTAManager internamente (ao tocar o botão no display)
app::UIManager  ui(display, psu, otaMgr);

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(100);

    Serial.println("\n========================================");
    Serial.println("  Fonte de Bancada 24V/5A  –  v1.9.0");
    Serial.println("  Core 1: controle CV/CC (700us/ciclo)");
    Serial.println("  Core 0: display TFT + OTA + serial");
    Serial.println("========================================");

    // I2C para PSU (INA219 + MCP4725)
    Wire.begin(PIN_SDA, PIN_SCL);
    Wire.setClock(I2C_FREQ_HZ);

    app::PSU::registerInstance(&psu);

    // ── Diagnóstico: motivo do reset ─────────────────────────────────────────
    {
        const int reason = static_cast<int>(esp_reset_reason());
        Serial.printf("[DRD] Motivo reset: %d  (1=POWERON, 3=SW, 4=WATCHDOG, 12=RST_PIN)\n", reason);
        Serial.println("[DRD] Usando EEPROM flash (persiste em POWERON_RESET)");
    }

    // ── Duplo reset: checado ANTES do psu.begin() ────────────────────────────
    // Usa EEPROM flash — sobrevive ao POWERON_RESET do botão EN/RST
    // do ESP32-WROOM-32E (que apagaria a RTC memory).
    Serial.println("[DRD] Verificando duplo reset...");
    if (drd.detectDoubleReset()) {
        Serial.println("[MAIN] Duplo reset detectado → modo OTA");
        // Inicializa display para mostrar tela OTA (se já estiver conectado)
        display.begin();
        // Inicia OTA sem depender do PSU
        otaMgr = new app::OTAManager(psu.getBuzzer());
        if (!otaMgr->begin()) {
            delete otaMgr;
            otaMgr = nullptr;
            Serial.println("[OTA] Falha ao iniciar AP.");
        }
        // Tenta iniciar PSU (pode falhar se hardware não estiver soldado)
        psu.begin();
        ui.begin();
        psu.beep(80);
        return;  // setup() termina aqui — loop() cuida do OTA
    }

    // ── Boot normal: hardware obrigatório ────────────────────────────────────
    if (!psu.begin()) {
        Serial.println("[MAIN] ERRO: Falha no I2C. Verifique INA219/MCP4725.");
        display.begin();
        display.tft.fillScreen(TFT_BLACK);
        display.tft.setTextColor(TFT_RED, TFT_BLACK);
        display.tft.setTextSize(2);
        display.tft.setTextDatum(MC_DATUM);
        display.tft.drawString("ERRO: I2C", 240, 140);
        display.tft.drawString("Verifique INA219/MCP4725", 240, 170);
        while (true) delay(1000);
    }

    // Display — Core 0 exclusivo
    display.begin();

    Serial.println("[MAIN] Boot normal.");

    // UI: splash + tela inicial
    ui.begin();

    // Beep de boot
    psu.beep(80);

    Serial.println("[MAIN] Pronto.");
}

// ─────────────────────────────────────────────────────────────────────────────
//  LOOP — Core 0
//
//  Ordem de chamada intencional:
//    1. OTA handle   — processa eventos de upload (prioridade alta no Core 0)
//    2. DRD loop     — limpa flag de duplo reset após janela de detecção
//    3. UI tick      — display + touch (pode levar ~5-80ms em redraw completo)
//    4. Buzzer tick  — buzzer não-bloqueante
//    5. Serial       — debug opcional
//
//  O vTaskDelay(1) ao final cede tempo ao idle task do FreeRTOS,
//  permitindo que o watchdog do Core 0 seja alimentado corretamente.
// ─────────────────────────────────────────────────────────────────────────────
void loop() {

    // ── 1. OTA ───────────────────────────────────────────────────────────────
    if (otaMgr && otaMgr->isActive()) {
        if (!otaMgr->handle()) {
            // Timeout ou erro: destrói e volta ao normal
            delete otaMgr;
            otaMgr = nullptr;
            psu.setOutput(true);    // religa saída após OTA
        }
    }

    // ── 2. Double Reset Detector ─────────────────────────────────────────────
    drd.loop();

    // ── 3. UI (display + touch) ───────────────────────────────────────────────
    //
    // tick() é não-bloqueante: lê toque, atualiza só o que mudou.
    // Redraw completo (troca de tela) pode levar até ~80ms — aceitável
    // porque o Core 1 continua executando o controle independentemente.
    ui.tick();

    // ── 4. Buzzer ────────────────────────────────────────────────────────────
    psu.tickBuzzer();

    // ── 5. Serial (debug — opcional, pode remover em produção) ───────────────
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim(); cmd.toLowerCase();
        if      (cmd.startsWith("v"))  { psu.setVoltage(cmd.substring(1).toFloat()); }
        else if (cmd.startsWith("i"))  { psu.setCurrent(cmd.substring(1).toFloat()); }
        else if (cmd == "on")          { psu.setOutput(true); }
        else if (cmd == "off")         { psu.setOutput(false); }
        else if (cmd == "mcv")         { psu.setMode(control::Mode::CV); }
        else if (cmd == "mcc")         { psu.setMode(control::Mode::CC); }
        else if (cmd == "xon")         { psu.setCrossoverEnabled(true); }
        else if (cmd == "xoff")        { psu.setCrossoverEnabled(false); }
        else if (cmd == "reset")       { psu.resetProtection(); }
        else if (cmd == "s")           { psu.printStatus(); }
    }

    // Cede slice ao FreeRTOS idle task (alimenta watchdog do Core 0)
    vTaskDelay(1);
}
