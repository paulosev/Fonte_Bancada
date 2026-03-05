#pragma once
// ╔══════════════════════════════════════════════════════════════════════════╗
//  app/ui_manager.h  –  Gerenciador de Interface Gráfica TFT
// ╚══════════════════════════════════════════════════════════════════════════╝
//
// ── ARQUITETURA ───────────────────────────────────────────────────────────────
//
//  UIManager é chamado por loop() no Core 0. Não existe nenhuma referência
//  ao UIManager no Core 1. O fluxo é:
//
//  Core 1 (700µs/ciclo):                Core 0 (loop):
//    ┌─────────────────────┐              ┌──────────────────────────┐
//    │  Controle CV/CC     │              │  UIManager::tick()       │
//    │  INA219 + DAC       │              │    ├─ lê PSU (atômico)   │
//    │  EMA + Crossover    │   getVout()  │    ├─ renderiza tela     │
//    │  OVP/OCP            │ ──────────►  │    ├─ detecta toque      │
//    │  (prioridade MAX)   │              │    └─ chama setVoltage()  │
//    └─────────────────────┘              └──────────────────────────┘
//
//  tick() é não-bloqueante: só redesenha quando necessário (dirty flag).
//  Renderização de tela completa: ~80ms (irrelevante para o Core 1).
//
// ── TELAS ─────────────────────────────────────────────────────────────────────
//
//  MAIN        Tela principal: V/I/P, ON/OFF, CV/CC, ícones WiFi e Config
//  NUMPAD      Teclado numérico: ajuste de V_set ou I_set
//  CONFIG      Configurações: Crossover, OVP, OCP, EMA, WiFi/OTA, EEPROM
//  PROTECTION  Modal de alerta OVP/OCP (sobrepõe qualquer tela)
//  OTA_CONFIRM Controle do AP WiFi: ligar / desligar
//  OTA_ACTIVE  AP ativo: SSID, IP, countdown, progresso, botão desligar
//
// ── REFRESH PARCIAL ───────────────────────────────────────────────────────────
//
//  Para não redesenhar a tela inteira a cada tick (caro em SPI):
//  - Valores numéricos são atualizados em regiões específicas (sprites)
//  - Fundo e elementos estáticos só são desenhados na transição de tela
//  - Taxa de refresh dos valores: ~10 Hz (a cada 100ms) — suficiente
//
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include "display.h"
#include "psu.h"
#include "ota_manager.h"
#include "config.h"

namespace app {

// Enumeração de telas
enum class Screen : uint8_t {
    MAIN, NUMPAD, CONFIG, PROTECTION, OTA_CONFIRM, OTA_ACTIVE
};

// Para o numpad: qual setpoint está sendo editado
enum class NumpadTarget : uint8_t { VOLTAGE, CURRENT, OVP, OCP };

// ─────────────────────────────────────────────────────────────────────────────
class UIManager {
public:
    UIManager(hal::Display& disp, app::PSU& psu, app::OTAManager*& otaMgr)
        : _disp(disp), _psu(psu), _otaMgr(otaMgr) {}

    // ─────────────────────────────────────────────────────────────────────────
    // begin()  –  Desenha splash e vai para tela principal
    // ─────────────────────────────────────────────────────────────────────────
    void begin() {
        _drawSplash();
        delay(1200);
        _goTo(Screen::MAIN);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // tick()  –  Chamado a cada iteração do loop() no Core 0
    //
    // Detecta toques, atualiza valores, verifica transições de tela.
    // Não bloqueante: retorna imediatamente se não há evento.
    // ─────────────────────────────────────────────────────────────────────────
    void tick() {
        const uint32_t now = millis();

        // ── Proteção tem prioridade absoluta ─────────────────────────────────
        if (_psu.isProtectionTripped() && _current != Screen::PROTECTION) {
            _goTo(Screen::PROTECTION);
            return;
        }
        // Sai da tela de proteção quando resetado
        if (_current == Screen::PROTECTION && !_psu.isProtectionTripped()) {
            _goTo(Screen::MAIN);
            return;
        }

        // ── OTA ativado por duplo RST (externo ao UI) ─────────────────────────
        if (_otaMgr && _otaMgr->isActive() && _current != Screen::OTA_ACTIVE) {
            _goTo(Screen::OTA_ACTIVE);
            return;
        }
        if (!_otaMgr && _current == Screen::OTA_ACTIVE) {
            _goTo(Screen::MAIN);
            return;
        }

        // ── Refresh periódico dos valores numéricos ───────────────────────────
        if (now - _lastRefresh >= UI_REFRESH_MS) {
            _lastRefresh = now;
            _refreshValues();
        }

        // ── Detecta toque ────────────────────────────────────────────────────
        hal::TouchPoint tap = _disp.getTouchReleased();
        if (tap.valid) _handleTap(tap.x, tap.y);

        // Toque longo no ícone WiFi da tela principal
        if (_current == Screen::MAIN) {
            if (_disp.getLongPress(TFT_W - 44, 4, 40, 36)) {
                _goTo(Screen::OTA_CONFIRM);
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // notifyOTAProgress()  –  Atualiza barra de progresso (chamado pelo OTA)
    // ─────────────────────────────────────────────────────────────────────────
    void notifyOTAProgress(uint8_t percent) {
        if (_current != Screen::OTA_ACTIVE) return;
        _otaProgress = percent;
        _drawOTAProgressBar();
    }

private:
    hal::Display&    _disp;
    app::PSU&        _psu;
    app::OTAManager*& _otaMgr;

    Screen      _current     {Screen::MAIN};
    uint32_t    _lastRefresh {0};
    uint8_t     _otaProgress {0};

    // Numpad
    NumpadTarget _numpadTarget {NumpadTarget::VOLTAGE};
    char         _numpadBuf[10] {0};
    uint8_t      _numpadLen {0};

    // Cache de valores para detectar mudança (evita redraw desnecessário)
    float   _cacheV{-1}, _cacheI{-1}, _cacheP{-1};
    bool    _cacheOut{false};
    control::Mode _cacheMode{control::Mode::CV};

    // ── Paleta de cores (macros TFT_eSPI em RGB565) ───────────────────────────
    static constexpr uint16_t COL_BG       = 0x0841;  // #0a0e14
    static constexpr uint16_t COL_PANEL    = 0x10C3;  // #111820
    static constexpr uint16_t COL_BORDER   = 0x1F1B;  // #1e2d3d
    static constexpr uint16_t COL_ACCENT   = 0x069F;  // #00d4ff
    static constexpr uint16_t COL_GREEN    = 0x07F1;  // #00ff88
    static constexpr uint16_t COL_ORANGE   = 0xFC40;  // #ff8c00
    static constexpr uint16_t COL_RED      = 0xF807;  // #ff3d3d
    static constexpr uint16_t COL_YELLOW   = 0xFFE0;  // #ffd700
    static constexpr uint16_t COL_OTA      = 0xABDF;  // #a78bfa
    static constexpr uint16_t COL_DIM      = 0x4A49;  // #4a5568
    static constexpr uint16_t COL_WHITE    = TFT_WHITE;

    // ── Utilitários de desenho ────────────────────────────────────────────────

    void _rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t fill, uint16_t border = 0, uint8_t r = 6) {
        _disp.tft.fillRoundRect(x, y, w, h, r, fill);
        if (border) _disp.tft.drawRoundRect(x, y, w, h, r, border);
    }

    void _label(int16_t x, int16_t y, const char* txt, uint8_t size, uint16_t color, uint8_t datum = TL_DATUM) {
        _disp.tft.setTextDatum(datum);
        _disp.tft.setTextColor(color, COL_BG);
        _disp.tft.setTextSize(size);
        _disp.tft.drawString(txt, x, y);
    }

    void _labelBg(int16_t x, int16_t y, const char* txt, uint8_t size, uint16_t color, uint16_t bg) {
        _disp.tft.setTextColor(color, bg);
        _disp.tft.setTextSize(size);
        _disp.tft.setTextDatum(TL_DATUM);
        _disp.tft.drawString(txt, x, y);
    }

    void _button(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t fill,
                 uint16_t tcol, const char* txt, uint8_t tsz = 2) {
        _rect(x, y, w, h, fill, 0, 6);
        _disp.tft.setTextDatum(MC_DATUM);
        _disp.tft.setTextColor(tcol, fill);
        _disp.tft.setTextSize(tsz);
        _disp.tft.drawString(txt, x + w/2, y + h/2);
    }

    bool _hit(int16_t tx, int16_t ty, int16_t x, int16_t y, int16_t w, int16_t h) {
        return tx >= x && tx < x+w && ty >= y && ty < y+h;
    }

    // ── Navegação ─────────────────────────────────────────────────────────────

    void _goTo(Screen s) {
        _current = s;
        _disp.clearScreen(COL_BG);
        _cacheV = _cacheI = _cacheP = -1;  // força redraw dos valores
        switch (s) {
            case Screen::MAIN:       _drawMainStatic();   break;
            case Screen::NUMPAD:     _drawNumpad();       break;
            case Screen::CONFIG:     _drawConfig();       break;
            case Screen::PROTECTION: _drawProtection();   break;
            case Screen::OTA_CONFIRM:_drawOTAConfirm();   break;
            case Screen::OTA_ACTIVE: _drawOTAActive();    break;
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // TELA PRINCIPAL — elementos estáticos
    // ─────────────────────────────────────────────────────────────────────────
    void _drawMainStatic() {
        auto& t = _disp.tft;

        // Top bar
        _rect(0, 0, TFT_W, 42, COL_PANEL, COL_BORDER, 0);

        // Painel V_out
        _rect(4, 48, 228, 120, COL_PANEL, COL_BORDER);
        _label(14, 54, "TENSAO", 1, COL_DIM);
        _label(200, 54, "V", 1, COL_DIM, TR_DATUM);

        // Painel I_out
        _rect(248, 48, 228, 120, COL_PANEL, COL_BORDER);
        _label(258, 54, "CORRENTE", 1, COL_DIM);
        _label(468, 54, "A", 1, COL_DIM, TR_DATUM);

        // Painel P_out
        _rect(4, 174, 472, 44, COL_PANEL, COL_BORDER);
        _label(14, 183, "POTENCIA", 1, COL_DIM);

        // Setpoints V e I
        _rect(4, 224, 148, 56, COL_PANEL, COL_BORDER);
        _label(12, 230, "SET V", 1, COL_DIM);

        _rect(158, 224, 148, 56, COL_PANEL, COL_BORDER);
        _label(166, 230, "SET I", 1, COL_DIM);

        // Botão ON/OFF (será colorido em _refreshValues)
        _rect(312, 224, 82, 56, COL_PANEL, COL_BORDER);

        // Botão Config ⚙
        _rect(400, 224, 76, 56, COL_PANEL, COL_BORDER);
        _label(438, 252, "CFG", 2, COL_DIM, MC_DATUM);

        // Ícone WiFi (toque longo)
        _rect(TFT_W - 44, 4, 40, 34, COL_PANEL, COL_BORDER);
        _label(TFT_W - 30, 14, "AP", 1, COL_OTA, MC_DATUM);

        // Linha separadora
        t.drawFastHLine(0, 286, TFT_W, COL_BORDER);

        // Barra de status (modo + proteção)
        _rect(0, 287, TFT_W, 33, COL_PANEL, 0, 0);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // TELA PRINCIPAL — refresh dos valores dinâmicos (~10Hz)
    // ─────────────────────────────────────────────────────────────────────────
    void _refreshValues() {
        if (_current != Screen::MAIN) return;

        const float v    = _psu.getVout();
        const float i    = _psu.getIout();
        const float p    = _psu.getPout();
        const bool  out  = _psu.isOutputEnabled();
        const auto  mode = _psu.getCrossoverMode();

        // V_out
        if (v != _cacheV) {
            _cacheV = v;
            char buf[10]; dtostrf(v, 6, 2, buf);
            _rect(8, 68, 218, 88, COL_PANEL, 0, 4);
            _disp.tft.setTextDatum(MR_DATUM);
            _disp.tft.setFreeFont(&FreeSans18pt7b);
            _disp.tft.setTextColor(COL_ACCENT, COL_PANEL);
            _disp.tft.drawString(buf, 220, 112);
            _disp.tft.setFreeFont(nullptr);
        }

        // I_out
        if (i != _cacheI) {
            _cacheI = i;
            char buf[10]; dtostrf(i, 6, 3, buf);
            _rect(252, 68, 218, 88, COL_PANEL, 0, 4);
            _disp.tft.setTextDatum(MR_DATUM);
            _disp.tft.setFreeFont(&FreeSans18pt7b);
            _disp.tft.setTextColor(COL_ORANGE, COL_PANEL);
            _disp.tft.drawString(buf, 464, 112);
            _disp.tft.setFreeFont(nullptr);
        }

        // P_out
        if (p != _cacheP) {
            _cacheP = p;
            char buf[12]; snprintf(buf, sizeof(buf), "%.2f W", p);
            _rect(140, 180, 200, 32, COL_PANEL, 0, 4);
            _disp.tft.setTextDatum(ML_DATUM);
            _disp.tft.setTextColor(COL_YELLOW, COL_PANEL);
            _disp.tft.setTextSize(2);
            _disp.tft.drawString(buf, 148, 196);
        }

        // Setpoints
        {
            char bv[10], bi[10];
            snprintf(bv, sizeof(bv), "%.2fV", _psu.getVset());
            snprintf(bi, sizeof(bi), "%.3fA", _psu.getIset());
            _rect(8, 244, 138, 28, COL_PANEL, 0, 4);
            _disp.tft.setTextColor(COL_ACCENT, COL_PANEL);
            _disp.tft.setTextSize(2); _disp.tft.setTextDatum(ML_DATUM);
            _disp.tft.drawString(bv, 12, 258);
            _rect(162, 244, 138, 28, COL_PANEL, 0, 4);
            _disp.tft.setTextColor(COL_ORANGE, COL_PANEL);
            _disp.tft.drawString(bi, 166, 258);
        }

        // Botão ON/OFF
        if (out != _cacheOut) {
            _cacheOut = out;
            _button(312, 224, 82, 56, out ? COL_GREEN : COL_RED,
                    out ? TFT_BLACK : TFT_WHITE, out ? "ON" : "OFF", 3);
        }

        // Status bar: modo + crossover
        if (mode != _cacheMode) {
            _cacheMode = mode;
            _rect(0, 287, TFT_W, 33, COL_PANEL, 0, 0);
            const bool isCV = (mode == control::Mode::CV);
            const char* mStr = isCV ? "CV" : "CC";
            const uint16_t mCol = isCV ? COL_ACCENT : COL_ORANGE;
            char buf[32];
            snprintf(buf, sizeof(buf), "Modo: %s %s",
                     mStr, _psu.isCrossoverEnabled() ? "[auto]" : "[manual]");
            _label(8, 296, buf, 2, mCol);
        }

        // Ícone WiFi: colorido se AP ativo
        {
            const bool apOn = (_otaMgr && _otaMgr->isActive());
            _rect(TFT_W - 44, 4, 40, 34, COL_PANEL, apOn ? COL_OTA : COL_BORDER);
            _disp.tft.setTextDatum(MC_DATUM);
            _disp.tft.setTextColor(apOn ? COL_OTA : COL_DIM, COL_PANEL);
            _disp.tft.setTextSize(1);
            _disp.tft.drawString(apOn ? "AP" : "AP", TFT_W - 24, 21);
        }

        // Modo na top bar
        {
            const bool isCV = (mode == control::Mode::CV);
            _rect(4, 4, 80, 34, COL_PANEL, 0, 4);
            _label(8, 14, isCV ? "CV AUTO" : "CC AUTO", 2,
                   isCV ? COL_ACCENT : COL_ORANGE);
        }

        // P_out na top bar
        {
            char buf[14]; snprintf(buf, sizeof(buf), "P %.1fW", p);
            _rect(90, 4, 120, 34, COL_PANEL, 0, 4);
            _label(94, 14, buf, 2, COL_YELLOW);
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // TELA NUMPAD
    // ─────────────────────────────────────────────────────────────────────────
    void _drawNumpad() {
        auto& t = _disp.tft;
        const bool isV = (_numpadTarget == NumpadTarget::VOLTAGE ||
                          _numpadTarget == NumpadTarget::OVP);
        const uint16_t acol = isV ? COL_ACCENT : COL_ORANGE;
        const char* title = (_numpadTarget == NumpadTarget::VOLTAGE) ? "AJUSTAR TENSAO" :
                            (_numpadTarget == NumpadTarget::CURRENT)  ? "AJUSTAR CORRENTE" :
                            (_numpadTarget == NumpadTarget::OVP)      ? "LIMITE OVP" : "LIMITE OCP";

        // Header
        _rect(0, 0, TFT_W, 44, COL_PANEL, COL_BORDER, 0);
        _label(12, 14, title, 2, acol);
        _button(TFT_W - 54, 6, 48, 32, COL_PANEL, COL_DIM, "< CLR", 1);

        // Display do valor
        _rect(4, 50, TFT_W - 8, 70, COL_PANEL, acol);
        _numpadBuf[_numpadLen] = '\0';
        const char* disp = _numpadLen ? _numpadBuf : "0";
        t.setTextDatum(MR_DATUM);
        t.setFreeFont(&FreeSans18pt7b);
        t.setTextColor(acol, COL_PANEL);
        t.drawString(disp, TFT_W - 20, 85);
        t.setFreeFont(nullptr);
        const char* unit = isV ? "V" : "A";
        _label(TFT_W - 12, 76, unit, 2, COL_DIM);

        // Teclado 4x4
        const char* keys[] = {
            "7","8","9","<<",
            "4","5","6","CLR",
            "1","2","3","",
            "0",".","","OK"
        };
        const int KX = 8, KY = 130, KW = 110, KH = 44, KG = 6;
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) {
                const char* k = keys[r*4+c];
                if (!k[0]) continue;
                int16_t x = KX + c*(KW+KG);
                int16_t y = KY + r*(KH+KG);
                uint16_t fill = (strcmp(k,"OK")==0)  ? COL_GREEN  :
                                (strcmp(k,"CLR")==0) ? COL_RED    : COL_PANEL;
                uint16_t tcol = (strcmp(k,"OK")==0)  ? TFT_BLACK  :
                                (strcmp(k,"CLR")==0) ? TFT_WHITE  : COL_WHITE;
                _button(x, y, KW, KH, fill, tcol, k, 2);
            }
        }
    }

    void _numpadInput(const char* k) {
        if (strcmp(k, "CLR") == 0) {
            _numpadLen = 0;
        } else if (strcmp(k, "<<") == 0) {
            if (_numpadLen > 0) _numpadLen--;
        } else if (strcmp(k, "OK") == 0) {
            _numpadBuf[_numpadLen] = '\0';
            float val = atof(_numpadBuf);
            switch (_numpadTarget) {
                case NumpadTarget::VOLTAGE: _psu.setVoltage(val); break;
                case NumpadTarget::CURRENT: _psu.setCurrent(val); break;
                case NumpadTarget::OVP:     /* TODO: setOVP(val) */ break;
                case NumpadTarget::OCP:     /* TODO: setOCP(val) */ break;
            }
            _goTo(Screen::MAIN);
            return;
        } else if (strcmp(k, ".") == 0) {
            // Só um ponto decimal
            for (uint8_t i = 0; i < _numpadLen; i++)
                if (_numpadBuf[i] == '.') return;
            if (_numpadLen < sizeof(_numpadBuf)-2) _numpadBuf[_numpadLen++] = '.';
        } else {
            if (_numpadLen < sizeof(_numpadBuf)-2) _numpadBuf[_numpadLen++] = k[0];
        }
        _drawNumpad();
    }

    // ─────────────────────────────────────────────────────────────────────────
    // TELA CONFIG
    // ─────────────────────────────────────────────────────────────────────────
    void _drawConfig() {
        _rect(0, 0, TFT_W, 44, COL_PANEL, COL_BORDER, 0);
        _label(12, 14, "CONFIGURACOES", 2, COL_WHITE);
        _button(TFT_W - 80, 6, 74, 32, COL_PANEL, COL_DIM, "< VOLTAR", 1);

        struct Item { const char* label; const char* val; uint16_t col; };
        const Item items[] = {
            { "Crossover Auto",   _psu.isCrossoverEnabled() ? "ON" : "OFF",
                                  _psu.isCrossoverEnabled() ? COL_GREEN : COL_DIM },
            { "OVP Limite",       "25.0 V",  COL_RED },
            { "OCP Limite",       "5.2 A",   COL_RED },
            { "EMA Speed",        "MED",     COL_YELLOW },
            { "WiFi / OTA",       "GERENCIAR", COL_OTA },
            { "EEPROM DAC",       "BURN (!)", COL_DIM },
        };

        for (int i = 0; i < 6; i++) {
            const int16_t y = 54 + i * 42;
            _rect(4, y, TFT_W - 8, 38, COL_PANEL, COL_BORDER);
            _label(14, y + 12, items[i].label, 2, COL_DIM);
            _disp.tft.setTextDatum(MR_DATUM);
            _disp.tft.setTextColor(items[i].col, COL_PANEL);
            _disp.tft.setTextSize(2);
            _disp.tft.drawString(items[i].val, TFT_W - 12, y + 19);
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // TELA PROTEÇÃO (modal)
    // ─────────────────────────────────────────────────────────────────────────
    void _drawProtection() {
        _disp.clearScreen(0x1800);  // vermelho escuro
        _disp.tft.drawRect(0, 0, TFT_W, TFT_H, COL_RED);
        _disp.tft.drawRect(2, 2, TFT_W-4, TFT_H-4, COL_RED);

        _disp.tft.setTextDatum(MC_DATUM);
        _disp.tft.setTextColor(COL_RED, 0x1800);

        _disp.tft.setTextSize(2);
        _disp.tft.drawString("! PROTECAO ATIVA !", TFT_W/2, 60);

        const bool ovp = _psu.isOVP();
        _disp.tft.setFreeFont(&FreeSans18pt7b);
        _disp.tft.drawString(ovp ? "OVP" : "OCP", TFT_W/2, 130);
        _disp.tft.setFreeFont(nullptr);

        _disp.tft.setTextSize(2);
        _disp.tft.setTextColor(COL_WHITE, 0x1800);
        _disp.tft.drawString(ovp ? "Sobretensao detectada" : "Sobrecorrente detectada",
                             TFT_W/2, 175);

        // Valores
        char buf[32];
        snprintf(buf, sizeof(buf), "Medido: %.2f %s",
                 ovp ? _psu.getVout() : _psu.getIout(), ovp ? "V" : "A");
        _disp.tft.setTextColor(COL_RED, 0x1800);
        _disp.tft.drawString(buf, TFT_W/2, 215);

        // Botão reset — grande
        _button(40, 250, TFT_W - 80, 54, COL_RED, TFT_WHITE, "TOQUE PARA RESETAR", 2);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // TELA OTA CONFIRM (controle do AP)
    // ─────────────────────────────────────────────────────────────────────────
    void _drawOTAConfirm() {
        const bool apOn = (_otaMgr && _otaMgr->isActive());

        _rect(0, 0, TFT_W, 44, COL_PANEL, COL_BORDER, 0);
        _label(12, 14, "WiFi / OTA", 2, COL_OTA);
        _button(TFT_W - 80, 6, 74, 32, COL_PANEL, COL_DIM, "< VOLTAR", 1);

        _rect(4, 52, TFT_W - 8, 100, COL_PANEL, COL_OTA);

        if (apOn) {
            _label(14, 62, "ACCESS POINT ATIVO", 2, COL_OTA);
            _label(14, 86, "Rede:  Fonte-OTA", 2, COL_WHITE);
            char ipbuf[20];
            snprintf(ipbuf, sizeof(ipbuf), "IP:    %s",
                     (_otaMgr ? WiFi.softAPIP().toString().c_str() : "192.168.4.1"));
            _label(14, 108, ipbuf, 2, COL_WHITE);
        } else {
            _label(14, 62, "WiFi DESLIGADO", 2, COL_DIM);
            _label(14, 86, "Instrucoes:", 1, COL_DIM);
            _label(14, 100, "1. Toque LIGAR abaixo", 1, COL_WHITE);
            _label(14, 114, "2. Conecte em \"Fonte-OTA\"", 1, COL_WHITE);
            _label(14, 128, "3. Upload via 192.168.4.1", 1, COL_WHITE);
        }

        // Botão principal
        _button(4, 162, TFT_W/2 - 8, 54,
                apOn ? COL_RED : COL_OTA,
                apOn ? TFT_WHITE : TFT_BLACK,
                apOn ? "DESLIGAR WiFi" : "LIGAR WiFi OTA", 2);

        // Botão voltar sem alterar
        _button(TFT_W/2 + 4, 162, TFT_W/2 - 8, 54, COL_PANEL, COL_DIM, "CANCELAR", 2);

        // Aviso: fonte desligada durante OTA
        if (apOn) {
            _rect(4, 224, TFT_W - 8, 36, 0x2000, COL_ORANGE);
            _label(14, 234, "Saida desligada durante OTA", 1, COL_ORANGE);
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // TELA OTA ACTIVE
    // ─────────────────────────────────────────────────────────────────────────
    void _drawOTAActive() {
        _rect(0, 0, TFT_W, 44, COL_PANEL, COL_OTA, 0);
        _label(12, 14, "MODO OTA ATIVO", 2, COL_OTA);

        _rect(4, 52, TFT_W - 8, 130, COL_PANEL, COL_OTA);
        _label(14, 62,  "Rede WiFi:", 1, COL_DIM);
        _label(14, 78,  "Fonte-OTA", 2, COL_OTA);
        _label(14, 104, "Endereco IP:", 1, COL_DIM);
        _label(14, 118, "192.168.4.1", 2, COL_WHITE);

        _label(14, 148, "Progresso do upload:", 1, COL_DIM);

        // Barra de progresso
        _rect(4, 192, TFT_W - 8, 24, COL_PANEL, COL_BORDER);
        _drawOTAProgressBar();

        // Timeout
        _rect(4, 224, TFT_W - 8, 36, COL_PANEL, COL_BORDER);
        _drawOTATimeout();

        // Botão desligar
        _button(4, 268, TFT_W - 8, 44, COL_RED, TFT_WHITE, "DESLIGAR WiFi", 2);
    }

    void _drawOTAProgressBar() {
        const int16_t px = 6, py = 194, pw = TFT_W - 12, ph = 20;
        _rect(px, py, pw, ph, COL_PANEL, 0, 3);
        if (_otaProgress > 0) {
            int16_t filled = (int16_t)(pw * _otaProgress / 100);
            _rect(px, py, filled, ph, COL_OTA, 0, 3);
        }
        char buf[8]; snprintf(buf, sizeof(buf), "%d%%", _otaProgress);
        _disp.tft.setTextDatum(MC_DATUM);
        _disp.tft.setTextColor(TFT_WHITE, COL_PANEL);
        _disp.tft.setTextSize(1);
        _disp.tft.drawString(buf, TFT_W/2, py + ph/2);
    }

    void _drawOTATimeout() {
        if (!_otaMgr) return;
        uint32_t elapsed = millis() - _otaMgr->getStartTime();
        uint32_t remaining = (OTA_TIMEOUT_MS > elapsed) ? (OTA_TIMEOUT_MS - elapsed) / 1000 : 0;
        char buf[24]; snprintf(buf, sizeof(buf), "Timeout: %02lu:%02lu", remaining/60, remaining%60);
        _rect(6, 226, TFT_W - 12, 30, COL_PANEL, 0, 3);
        _disp.tft.setTextColor(COL_YELLOW, COL_PANEL);
        _disp.tft.setTextSize(2);
        _disp.tft.setTextDatum(MC_DATUM);
        _disp.tft.drawString(buf, TFT_W/2, 241);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // SPLASH
    // ─────────────────────────────────────────────────────────────────────────
    void _drawSplash() {
        _disp.clearScreen(COL_BG);
        _disp.tft.setTextDatum(MC_DATUM);
        _disp.tft.setFreeFont(&FreeSans18pt7b);
        _disp.tft.setTextColor(COL_ACCENT, COL_BG);
        _disp.tft.drawString("Fonte Bancada", TFT_W/2, TFT_H/2 - 40);
        _disp.tft.setFreeFont(nullptr);
        _disp.tft.setTextColor(COL_DIM, COL_BG);
        _disp.tft.setTextSize(2);
        _disp.tft.drawString("24V / 5A", TFT_W/2, TFT_H/2 + 10);
        _disp.tft.setTextSize(1);
        _disp.tft.setTextColor(COL_DIM, COL_BG);
        _disp.tft.drawString("ESP32  ILI9488  v1.9.0", TFT_W/2, TFT_H/2 + 50);
        _disp.tft.drawString("Verificando hardware...", TFT_W/2, TFT_H/2 + 80);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // HANDLER DE TOQUE
    // ─────────────────────────────────────────────────────────────────────────
    void _handleTap(int16_t x, int16_t y) {
        switch (_current) {

        case Screen::MAIN:
            // ON/OFF
            if (_hit(x, y, 312, 224, 82, 56)) {
                _psu.setOutput(!_psu.isOutputEnabled());
                _cacheOut = !_cacheOut;  // força redraw
            }
            // Config
            else if (_hit(x, y, 400, 224, 76, 56)) {
                _goTo(Screen::CONFIG);
            }
            // Toque no V_set → numpad tensão
            else if (_hit(x, y, 4, 224, 148, 56)) {
                _numpadTarget = NumpadTarget::VOLTAGE;
                _numpadLen    = 0;
                _goTo(Screen::NUMPAD);
            }
            // Toque no I_set → numpad corrente
            else if (_hit(x, y, 158, 224, 148, 56)) {
                _numpadTarget = NumpadTarget::CURRENT;
                _numpadLen    = 0;
                _goTo(Screen::NUMPAD);
            }
            break;

        case Screen::NUMPAD: {
            // Mapeamento dos botões do teclado
            const char* keys[] = {
                "7","8","9","<<",
                "4","5","6","CLR",
                "1","2","3","",
                "0",".","","OK"
            };
            const int KX=8, KY=130, KW=110, KH=44, KG=6;
            for (int r = 0; r < 4; r++) {
                for (int c = 0; c < 4; c++) {
                    const char* k = keys[r*4+c];
                    if (!k[0]) continue;
                    if (_hit(x, y, KX+c*(KW+KG), KY+r*(KH+KG), KW, KH)) {
                        _numpadInput(k);
                        return;
                    }
                }
            }
            // Botão CLR no header
            if (_hit(x, y, TFT_W-54, 6, 48, 32)) {
                _numpadLen = 0; _drawNumpad();
            }
            break;
        }

        case Screen::CONFIG:
            // Voltar
            if (_hit(x, y, TFT_W-80, 6, 74, 32)) { _goTo(Screen::MAIN); break; }
            // Crossover toggle
            if (_hit(x, y, 4, 54, TFT_W-8, 38)) {
                _psu.setCrossoverEnabled(!_psu.isCrossoverEnabled());
                _drawConfig();
            }
            // OVP
            else if (_hit(x, y, 4, 96, TFT_W-8, 38)) {
                _numpadTarget = NumpadTarget::OVP; _numpadLen = 0;
                _goTo(Screen::NUMPAD);
            }
            // OCP
            else if (_hit(x, y, 4, 138, TFT_W-8, 38)) {
                _numpadTarget = NumpadTarget::OCP; _numpadLen = 0;
                _goTo(Screen::NUMPAD);
            }
            // WiFi/OTA
            else if (_hit(x, y, 4, 222, TFT_W-8, 38)) {
                _goTo(Screen::OTA_CONFIRM);
            }
            // EEPROM BURN
            else if (_hit(x, y, 4, 264, TFT_W-8, 38)) {
                // Confirmação simples: toque longo protege de acidente
                // aqui dispara diretamente (tela de config já é intencional)
                _psu.burnDACEEPROM();
                _drawConfig();
            }
            break;

        case Screen::PROTECTION:
            // Qualquer toque reseta
            _psu.resetProtection();
            _psu.setOutput(true);
            _goTo(Screen::MAIN);
            break;

        case Screen::OTA_CONFIRM: {
            const bool apOn = (_otaMgr && _otaMgr->isActive());
            // Botão principal: ligar/desligar
            if (_hit(x, y, 4, 162, TFT_W/2-8, 54)) {
                if (apOn) {
                    _otaMgr->stop();
                    delete _otaMgr;
                    _otaMgr = nullptr;
                } else {
                    _otaMgr = new app::OTAManager(_psu.getBuzzer());
                    _psu.setOutput(false);  // desliga fonte durante OTA
                    if (!_otaMgr->begin()) {
                        delete _otaMgr; _otaMgr = nullptr;
                    } else {
                        _goTo(Screen::OTA_ACTIVE); return;
                    }
                }
                _drawOTAConfirm();
            }
            // Cancelar
            else if (_hit(x, y, TFT_W/2+4, 162, TFT_W/2-8, 54) ||
                     _hit(x, y, TFT_W-80, 6, 74, 32)) {
                _goTo(Screen::MAIN);
            }
            break;
        }

        case Screen::OTA_ACTIVE:
            // Desligar WiFi
            if (_hit(x, y, 4, 268, TFT_W-8, 44)) {
                if (_otaMgr) { _otaMgr->stop(); delete _otaMgr; _otaMgr = nullptr; }
                _psu.setOutput(true);
                _goTo(Screen::MAIN);
            }
            break;
        }
    }
};

} // namespace app
