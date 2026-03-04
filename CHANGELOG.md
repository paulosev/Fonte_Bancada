# Changelog — Fonte de Bancada Digital ESP32

Todas as mudanças relevantes do projeto são documentadas aqui.
Formato baseado em [Keep a Changelog](https://keepachangelog.com/pt-BR/1.0.0/).

---

## [1.5.0] — EEPROM do DAC: bloqueio de hardware no power-on

### Adicionado
- `dac.h` — método `burnEEPROM()`: grava raw 4095 (~3,3 V) na EEPROM interna do MCP4725
- `psu.h` — método público `burnDACEEPROM()` expõe a gravação para o main
- Comando serial `burn` com confirmação obrigatória (`burnok`) para evitar gravação acidental

### Como funciona
Ao energizar o circuito, o MCP4725 carrega automaticamente o valor da EEPROM para sua
saída antes de qualquer comunicação I2C. Com 4095 gravado, FB = 3,3 V desde o instante
zero → XL4015 bloqueado → V_out = 0 V durante todo o boot do ESP32 (~300 ms).

```
t=0 ms    Alimentação sobe
t=~1 ms   MCP4725 carrega EEPROM (4095) → FB = 3,3 V automaticamente
t=~2 ms   XL4015 inicia com FB >> Vref → V_out = 0 V  ✔
t=~300 ms ESP32 termina boot → forceOff() confirma DAC = 4095
t=...     setOutput(true) → EMA rampa suave 0 → V_set
```

### Advertência
Gravar APENAS UMA VEZ. A EEPROM do MCP4725 suporta ~1.000.000 ciclos.
Chamar em todo boot esgotaria a memória em poucos anos.

---

## [1.4.0] — EMA: rampa suave de setpoint

### Adicionado
- `lib/control/ema.h` — filtro de Média Móvel Exponencial (EMA) para suavização de setpoints
- Constante `EMA_ALPHA = 0.005` em `config.h` (τ ≈ 140 ms, 99% do valor em ~700 ms)
- Dois filtros EMA em `psu.h`: `_emaV` (tensão) e `_emaI` (corrente)

### Alterado
- `setOutput(true)` reseta os filtros EMA para 0 → partida suave do zero até V_set
- Equação de feedback usa `vSetSmooth` e `iSetSmooth` (saídas do EMA) em vez dos setpoints brutos
- Elimina pico de tensão na partida da fonte e nas trocas de setpoint (ex: 5 V → 12 V)

### Equação aplicada
```
V_ema[n] = α × V_set + (1 − α) × V_ema[n−1]
```
Custo computacional: < 1 µs por ciclo (2 operações float na FPU do Xtensa LX6).

---

## [1.3.0] — On/Off por DAC + Buzzer de proteção

### Adicionado
- `lib/hal/buzzer.h` — driver não-bloqueante de buzzer (GPIO18)
  - Padrão: 200 ms ligado / 400 ms desligado, configurável em `config.h`
  - Acionado automaticamente ao disparar OVP ou OCP
  - Silenciado ao chamar `resetProtection()` ou `setOutput(false)`
- `dac.h` — método `forceOff()`: escreve raw 4095 (~3,3 V no FB)
  - Com FB >> Vref (1,25 V), XL4015 reduz duty cycle ao mínimo → V_out ≈ 0 V
  - Solução para ausência de pino enable no XL4015
- Constantes em `config.h`: `PIN_BUZZER`, `BUZZER_BEEP_MS`, `BUZZER_INTERVAL_MS`

### Alterado
- `setOutput(false)` usa `forceOff()` em vez de `shutdown(0)`
- Proteções OVP e OCP usam `forceOff()` para desligar a saída
- `loop()` chama `psu.tickBuzzer()` — gerenciamento não-bloqueante no Core 0

---

## [1.2.0] — Zero prático do setpoint

### Alterado
- `V_OUT_MIN` de `1.3f` para `0.0001f` (zero prático)
- O limite 1,3 V era confusão com a tensão mínima física do XL4015, não do setpoint
- Setpoint pode ser qualquer valor > 0; `0.0001 V` é tratado como zero (evita divisão por zero em `K = Vref / V_set`)

---

## [1.1.0] — Potência de saída

### Adicionado
- `control/feedback.h` — função `computePower(v, i)`: calcula `P = V × I` [W]
- Campo `volatile float _pOut` em `psu.h`, calculado no mesmo bloco crítico que `_vOut` e `_iOut`
- Getter `getPout()` com proteção de seção crítica
- `printStatus()` exibe `Pout` na linha de telemetria serial

---

## [1.0.0] — Versão inicial compilável

### Adicionado
- Controle digital do XL4015 via DAC MCP4725 no pino FB ("divisor de tensão virtual")
- **Modo CV** — tensão constante: `K = Vref / V_set`, `V_dac = V_out × K`
- **Modo CC** — corrente constante: `K_i = Vref / I_set`, `V_dac = I_out × K_i`
- **OVP** — proteção contra sobretensão com histérese (25 V / 0,5 V)
- **OCP** — proteção contra sobrecorrente com histérese (5,2 A / 0,1 A)
- **Dual-core FreeRTOS**: Core 1 dedicado ao loop de controle (prioridade máxima)
- **Timer de hardware** a 700 µs (≈ 1,43 kHz) notificando task via `vTaskNotifyGiveFromISR`
- **I2C Fast Mode Plus** (1 MHz): latência ~5× menor que 100 kHz padrão
- **INA219 single-shot com pipeline**: conversão de hardware em paralelo com a CPU
- **Canal prioritário por modo**: CV prioriza V_bus; CC prioriza I_shunt
- **Canal secundário** (~143 Hz) para proteção e telemetria do canal não-prioritário
- Interface serial (115200 baud): comandos `v`, `i`, `mcv`, `mcc`, `on`, `off`, `reset`, `s`
- Estrutura PlatformIO com camadas `hal/`, `control/`, `app/`

### Correções de compilação ao longo do desenvolvimento
- Removido `inline` em membro estático (C++17 não suportado no toolchain Xtensa)
- `_timerISR` movida para `psu.cpp` (evita `l32r dangerous relocation` no linker Xtensa)
- Removido `IRAM_ATTR` dos métodos HAL (apenas ISR precisa de IRAM)
- `portYIELD_FROM_ISR` removido (macro inconsistente no ESP32-Arduino framework)
- `1'000'000UL` → `1000000UL` (separador de dígitos C++14 rejeitado pelo GCC Xtensa)
- `requestFrom(..., true)` → `requestFrom(..., static_cast<uint8_t>(1))` (ambiguidade de overload)
- `lib/` movida para raiz do projeto (PlatformIO LDF não compila `.cpp` em `src/lib/`)
- `-I include` adicionado ao `build_flags` (libs locais não achavam `config.h`)

---

## Estrutura de arquivos

```
Fonte_Bancada/
├── CHANGELOG.md
├── README.md
├── platformio.ini
├── include/
│   └── config.h          ← todas as constantes configuráveis
├── src/
│   └── main.cpp          ← setup(), loop(), interface serial
└── lib/
    ├── hal/
    │   ├── ina219.h      ← driver INA219: single-shot, pipeline, CNVR
    │   ├── dac.h         ← driver MCP4725: Fast Write + forceOff()
    │   └── buzzer.h      ← buzzer não-bloqueante
    ├── control/
    │   ├── feedback.h    ← equações CV/CC e computePower()
    │   ├── protection.h  ← OVP e OCP com histérese
    │   └── ema.h         ← filtro EMA para rampa suave de setpoint
    └── app/
        ├── psu.h         ← orquestra tudo; FreeRTOS task + timer HW
        └── psu.cpp       ← _instance e _timerISR (fora do header)
```
