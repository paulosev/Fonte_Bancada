# Changelog — Fonte de Bancada Digital ESP32

Todas as mudanças relevantes do projeto são documentadas aqui.
Formato baseado em [Keep a Changelog](https://keepachangelog.com/pt-BR/1.0.0/).

---

## [1.9.0] — Interface Gráfica TFT 480×320 (ILI9488 + XPT2046)

### Adicionado
- `lib/hal/display.h` — driver HAL para ILI9488 + XPT2046: tap, release, long press
- `lib/app/ui_manager.h` — gerenciador de UI com 6 telas completas
- `include/config.h` — constantes TFT (dimensões, rotação, sensibilidade, refresh)
- Biblioteca: `bodmer/TFT_eSPI @ ^2.5.43` com pinagem via `build_flags`

### Telas implementadas
| Tela | Acesso |
|---|---|
| Splash | Boot |
| Principal | Sempre — V/I/P, ON/OFF, CV/CC, ícone AP, ícone Config |
| Ajuste Setpoint | Toque em V_set ou I_set → teclado numérico |
| Configurações | Ícone ⚙ → crossover, OVP, OCP, EMA, WiFi/OTA, EEPROM |
| Alerta Proteção | Automático (OVP/OCP) — modal com buzzer |
| WiFi/OTA Confirmar | 3 caminhos: toque longo AP, Config→WiFi, 2×RST |
| OTA Ativo | AP ligado: SSID, IP, countdown, progresso, desligar |

### Arquitetura dual-core (display NÃO interfere no controle)
- **Core 1** (prioridade MAX, 700µs/ciclo): INA219 + DAC + EMA + crossover + OVP/OCP
- **Core 0** (prioridade normal): `ui.tick()` + `otaMgr.handle()` + buzzer + serial
- `ui.tick()` é não-bloqueante: só redesenha regiões que mudaram (dirty flag)
- Redraw completo (~80ms) não afeta Core 1 — cores são independentes
- Comunicação Core0↔Core1: getters atômicos + spinlock (já existente no PSU)

### WiFi/OTA pelo display
- Toque longo (1s) no ícone AP da tela principal
- Item "WiFi/OTA" em Configurações
- 2× RST (hardware — funciona sem display)
- Botão "DESLIGAR WiFi" disponível na tela OTA Ativo

---

## [1.8.0] — OTA via Access Point (sem roteador, sem credenciais)

### Adicionado
- `lib/app/ota_manager.h` — gerenciador OTA em modo Access Point
- Constantes em `config.h`: `OTA_AP_SSID`, `OTA_HOSTNAME`, `DRD_TIMEOUT_S`, `OTA_TIMEOUT_MS`
- Biblioteca: `cjmcv/ESP_DoubleResetDetector`
- Linhas comentadas em `platformio.ini` para upload OTA (`upload_port = 192.168.4.1`)
- `.gitignore` — exclui pasta `.pio/` e `.vscode/`

### Como usar
1. Dê **2× RST** em menos de 2 s
2. Buzzer emite **3 beeps curtos** → AP ativo
3. Conecte o notebook na rede WiFi **"Fonte-OTA"** (aberta, sem senha)
4. No `platformio.ini`, descomente `upload_protocol` e `upload_port = 192.168.4.1`
5. Faça o upload normalmente pelo PlatformIO
6. ESP reinicia automaticamente com o novo firmware

### Vantagens do modo AP
- Não precisa de roteador nem rede WiFi externa
- Sem arquivo de credenciais para gerenciar
- Funciona em qualquer ambiente (bancada, campo, oficina)
- WiFi fica ativo apenas durante a janela OTA (5 min padrão)

### Indicações do buzzer
| Padrão | Significado |
|---|---|
| 1 beep curto (setup) | Boot concluído normalmente |
| 3 beeps curtos repetidos | Modo OTA ativo, aguardando upload |
| 1 beep longo | Timeout OTA — AP desligado |

---

## [1.7.0] — OTA WiFi + Duplo Reset (versão com STA — substituída por AP)

### Adicionado
- `lib/app/ota_manager.h` — gerenciador de OTA com ArduinoOTA
- `include/credentials.h` — credenciais WiFi e senha OTA (não commitado)
- `include/credentials.h.example` — modelo para o GitHub
- `.gitignore` — exclui credentials.h e pasta .pio/
- `psu.h` — métodos `getBuzzer()` e `beep()` para uso externo
- Constantes em `config.h`: `DRD_TIMEOUT_S`, `OTA_TIMEOUT_MS`, `WIFI_CONNECT_TIMEOUT_MS`
- Biblioteca: `cjmcv/ESP_DoubleResetDetector`
- Linhas comentadas em `platformio.ini` para upload OTA direto

### Como usar
1. Copie `include/credentials.h.example` para `include/credentials.h`
2. Preencha SSID, senha WiFi e senha OTA
3. Faça o primeiro upload via serial normalmente
4. Para atualizar firmware depois: dê 2× RST em menos de 2 s
5. Buzzer emite 3 beeps → WiFi ativo, ESP visível como `fonte-bancada.local`
6. No PlatformIO: selecione a porta OTA e faça upload

### Indicações do buzzer
| Padrão | Significado |
|---|---|
| 1 beep curto | Boot concluído |
| 1 beep longo | Conectando WiFi... |
| 3 beeps curtos | Modo OTA ativo (repetido a cada 3 s) |
| 1 beep longo final | OTA encerrado (timeout ou erro) |
| Beep contínuo | OVP / OCP ativo |

### Segurança
- WiFi só fica ativo durante janela OTA (padrão: 5 min)
- Upload protegido por senha (`OTA_PASSWORD` em credentials.h)
- Credenciais nunca vão para o repositório (.gitignore)

---

## [1.6.0] — Crossover automático CV↔CC

### Adicionado
- `lib/control/crossover.h` — detector de crossover com histerese de tempo
- Constantes em `config.h`: `CROSSOVER_DEADBAND` (2%) e `CROSSOVER_MIN_CYCLES` (5 ciclos)
- `psu.h` — método `setCrossoverEnabled(bool)` e `isCrossoverEnabled()`
- Comandos seriais `xon` (habilita) e `xoff` (desabilita crossover)
- `printStatus()` exibe modo ativo com indicador `(auto)` ou `(man)`

### Como funciona
O usuário define V_set e I_set simultaneamente. A fonte opera no modo
que for necessário para respeitar os dois limites ao mesmo tempo:

- **CV ativo**: carga leve, I_out < I_set — tensão regulada em V_set
- **Crossover**: I_out atinge I_set — modo CC assume automaticamente
- **CC ativo**: carga pesada, corrente limitada — tensão cede proporcionalmente
- **Retorno a CV**: carga diminui, I_out cai abaixo do limiar — CV volta a regular

### Parâmetros de transição
- Banda morta de 2%: transição CV→CC em `I_out ≥ I_set × 0,98`
- Histerese temporal: 5 ciclos consecutivos (~3,5 ms) para confirmar troca
- Evita oscilação quando a carga opera exatamente no ponto de crossover

### Exemplo
```
V_set = 12 V, I_set = 2 A
  Carga 100Ω → I = 0,12 A → CV: mantém 12 V
  Carga   6Ω → I = 2,0 A  → crossover → CC: mantém 2 A, V = 12 V
  Carga   2Ω → CC: mantém 2 A, V cai para 4 V
```

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
