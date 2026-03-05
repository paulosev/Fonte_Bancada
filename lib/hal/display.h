#pragma once
// ╔══════════════════════════════════════════════════════════════════════════╗
//  hal/display.h  –  Driver TFT ILI9488 + Touch XPT2046
// ╚══════════════════════════════════════════════════════════════════════════╝
//
// ── ISOLAMENTO DO CORE DE CONTROLE ───────────────────────────────────────────
//
//  Este driver roda EXCLUSIVAMENTE no Core 0 (Arduino loop).
//  O Core 1 (task de controle FreeRTOS) jamais chama qualquer método aqui.
//  A comunicação entre cores é feita por leitura atômica de variáveis
//  protegidas por spinlock em PSU — não há acesso direto ao TFT pelo Core 1.
//
//  Portanto o display pode levar o tempo que precisar para renderizar:
//  um frame completo de 480×320 leva ~60-150ms via SPI — o controle não
//  é afetado porque roda em paralelo no Core 1 com timer de hardware.
//
// ── HARDWARE (SPI2 compartilhado) ────────────────────────────────────────────
//
//  ILI9488  480×320 TFT  CS=5   DC=2  RST=4
//  XPT2046  Touch        CS=17  IRQ=16
//  MicroSD               CS=15
//  SPI2: MISO=12  MOSI=13  SCK=14
//
// ── CALIBRAÇÃO DO TOUCH ───────────────────────────────────────────────────────
//
//  Os valores de calibração abaixo foram obtidos empiricamente para o
//  módulo 3.5" ILI9488. Se o toque estiver descalibrado, ajuste as
//  constantes TOUCH_* em config.h e chame calibrateTouch() uma vez.
//
// ─────────────────────────────────────────────────────────────────────────────

#include <TFT_eSPI.h>
#include "config.h"

namespace hal {

// Ponto de toque calibrado em coordenadas de tela (0-479, 0-319)
struct TouchPoint {
    int16_t x{-1}, y{-1};
    bool    valid{false};
};

class Display {
public:
    TFT_eSPI tft;   // acesso público para as telas desenharem diretamente

    // ─────────────────────────────────────────────────────────────────────────
    // begin()  –  Inicializa TFT e touch
    // ─────────────────────────────────────────────────────────────────────────
    void begin() {
        tft.begin();
        tft.setRotation(TFT_ROTATION);  // paisagem
        tft.fillScreen(TFT_BLACK);
        tft.setTextDatum(TL_DATUM);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // getTouch()  –  Lê e calibra coordenadas do touch
    //
    // Retorna TouchPoint com valid=false se não há toque ou fora da tela.
    // Chamado pelo UI a cada tick do loop — não bloqueante.
    // ─────────────────────────────────────────────────────────────────────────
    TouchPoint getTouch() {
        TouchPoint pt;
        uint16_t tx, ty;

        // TFT_eSPI já gerencia o CS do touch internamente
        if (!tft.getTouch(&tx, &ty, TOUCH_SENSITIVITY)) return pt;

        // Mapeia raw → coordenadas de tela com calibração
        pt.x     = static_cast<int16_t>(tx);
        pt.y     = static_cast<int16_t>(ty);
        pt.valid = (pt.x >= 0 && pt.x < TFT_W && pt.y >= 0 && pt.y < TFT_H);
        return pt;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // getTouchReleased()  –  Detecta borda de soltura do toque
    //
    // Retorna o último ponto válido quando o dedo sai da tela.
    // Usado para eventos de "tap" (pressionar e soltar).
    // ─────────────────────────────────────────────────────────────────────────
    TouchPoint getTouchReleased() {
        TouchPoint pt = getTouch();
        if (pt.valid) {
            _lastTouch    = pt;
            _wasPressed   = true;
        } else if (_wasPressed) {
            _wasPressed   = false;
            return _lastTouch;  // evento de release
        }
        return TouchPoint{};    // sem evento
    }

    // ─────────────────────────────────────────────────────────────────────────
    // getLongPress()  –  Detecta toque longo (>LONG_PRESS_MS ms)
    //
    // Retorna true UMA VEZ quando a duração do toque atinge o limiar.
    // Não repete enquanto o dedo permanecer na tela.
    // ─────────────────────────────────────────────────────────────────────────
    bool getLongPress(int16_t x, int16_t y, uint16_t w, uint16_t h) {
        TouchPoint pt = getTouch();
        const bool inZone = pt.valid && pt.x >= x && pt.x < x+w && pt.y >= y && pt.y < y+h;
        const uint32_t now = millis();

        if (inZone) {
            if (!_lpActive) {
                _lpActive    = true;
                _lpStartTime = now;
                _lpFired     = false;
            }
            if (!_lpFired && now - _lpStartTime >= LONG_PRESS_MS) {
                _lpFired = true;
                return true;
            }
        } else {
            _lpActive = false;
            _lpFired  = false;
        }
        return false;
    }

    void clearScreen(uint32_t color = TFT_BLACK) {
        tft.fillScreen(color);
    }

private:
    TouchPoint _lastTouch;
    bool       _wasPressed   {false};
    bool       _lpActive     {false};
    bool       _lpFired      {false};
    uint32_t   _lpStartTime  {0};
};

} // namespace hal
