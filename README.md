# Fonte de Bancada Digital — ESP32 + XL4015

Fonte de bancada controlada digitalmente com saída de **1,3 V–24 V / 0–5 A**, construída com ESP32, CI step-down XL4015, sensor INA219 e DAC MCP4725.

---

## Sumário

- [Visão Geral](#visão-geral)
- [Hardware](#hardware)
- [Princípio de Controle](#princípio-de-controle)
- [Features](#features)
- [Arquitetura de Software](#arquitetura-de-software)
- [Estrutura de Arquivos](#estrutura-de-arquivos)
- [Configuração](#configuração)
- [Interface Serial](#interface-serial)
- [Como Compilar](#como-compilar)
- [Calibração](#calibração)

---

## Visão Geral

A fonte usa o CI step-down **XL4015** como estágio de potência. O XL4015 possui uma malha de controle analógica interna que regula sua saída mantendo o pino **FB (feedback)** em exatamente **1,25 V** (sua tensão de referência interna).

No projeto original, um divisor resistivo fixo fechava essa malha. Aqui, o divisor foi removido e substituído por um **divisor de tensão virtual ativo**: o ESP32 calcula continuamente a tensão que o pino FB deveria receber para cada medição de saída e a entrega via DAC MCP4725.

Isso permite controle digital total de tensão e corrente sem modificar o estágio de potência.

---

## Hardware

| Componente | Função |
|---|---|
| **ESP32** | Microcontrolador dual-core; executa controle e UI |
| **XL4015** | CI step-down 5 A; estágio de potência |
| **MCP4725** | DAC I2C 12-bit; gera tensão de feedback para o XL4015 |
| **INA219** | Sensor I2C 12-bit; mede tensão e corrente de saída |

### Diagrama de conexão

```
        ┌──────────────────────────────────────────────────────┐
        │                      ESP32                           │
        │                                                      │
        │  GPIO21 (SDA) ────────────────────────────┐         │
        │  GPIO22 (SCL) ─────────────────────┐      │         │
        └────────────────────────────────────┼──────┼─────────┘
                                             │      │
                         I2C 1 MHz           │      │
             ┌───────────────────────────────┘      │
             │           ┌──────────────────────────┘
             │           │
        ┌────┴───────────┴────┐          ┌──────────────────┐
        │      MCP4725        │          │     INA219        │
        │      DAC 12-bit     │          │  V + I sensor     │
        │                     │          │                   │
        │  OUT ───────────────┼──→ FB ──→│  V_bus ← V_out   │
        └─────────────────────┘  XL4015  │  I_shunt ← carga │
                                         └──────────────────┘
                                                │
                                         ┌──────┴──────┐
                                         │   Saída     │
                                         │ 1,3–24V/5A  │
                                         └─────────────┘
```

### Endereços I2C padrão

| Dispositivo | Endereço | Pinos de endereço |
|---|---|---|
| MCP4725 | `0x60` | A0=GND, A1=GND |
| INA219  | `0x40` | A0=GND, A1=GND |

### Resistor shunt (INA219)

| Shunt | I_máx | Calibração (config.h) |
|---|---|---|
| 0,1 Ω | 3,2 A | `CAL_VALUE = 4096`, `I_LSB_MA = 0,1` |
| 0,05 Ω | 5 A | Ajustar `CAL_VALUE` e `I_LSB_MA` em `ina219.h` |

---

## Princípio de Controle

### Modo CV — Tensão Constante

O XL4015 regula sua saída mantendo FB = Vref (1,25 V). Simulando o divisor resistivo original:

```
K     = Vref / V_set        (fator de atenuação)
V_dac = V_out × K
```

| Condição | V_dac | Ação do XL4015 |
|---|---|---|
| V_out = V_set | V_dac = Vref | Equilíbrio — nenhuma correção |
| V_out < V_set | V_dac < Vref | Aumenta duty cycle (eleva V_out) |
| V_out > V_set | V_dac > Vref | Reduz duty cycle (abaixa V_out) |

### Modo CC — Corrente Constante

Mesma estrutura, expressa em corrente (ganho transresistivo):

```
K_i   = Vref / I_set        (ganho em V/A)
V_dac = I_out × K_i
```

| Condição | V_dac | Ação do XL4015 |
|---|---|---|
| I_out = I_set | V_dac = Vref | Equilíbrio — nenhuma correção |
| I_out > I_set | V_dac > Vref | Reduz V_out (corrente cai) |
| I_out < I_set | V_dac < Vref | Eleva V_out (corrente sobe) |

---

## Features

| Feature | Detalhe |
|---|---|
| **Modo CV** | Regulação de tensão constante de 0 a 24 V (mínimo prático: 0,0001 V) |
| **Modo CC** | Regulação de corrente constante de 0 A a 5 A |
| **OVP** | Proteção contra sobretensão com histérese (dispara em 25 V) |
| **OCP** | Proteção contra sobrecorrente com histérese (dispara em 5,2 A) |
| **P_out** | Cálculo de potência de saída em tempo real: P = V × I |
| **Interface Serial** | Controle e monitoramento via monitor serial (115200 baud) |
| **Dual-Core** | Core 1 dedicado ao controle (1,43 kHz); Core 0 para UI |
| **I2C 1 MHz** | Fast Mode Plus: latência ~5× menor que 100 kHz padrão |
| **Pipeline ADC** | INA219 converte em paralelo com a CPU (sem busy-wait) |
| **Canal prioritário** | Modo CV: prioriza leitura de tensão. Modo CC: prioriza corrente |

---

## Arquitetura de Software

### Divisão de cores

```
┌─ Core 0 (Arduino loop, prioridade normal) ───────────────────────────┐
│  Interface serial, impressão de status, setters de setpoint          │
│  Prioridade: normal (pode ser preemptado pelo Core 1)                │
└──────────────────────────────────────────────────────────────────────┘
         ↕  portENTER_CRITICAL / portEXIT_CRITICAL (spinlock)
┌─ Core 1 (Task "ctrl", prioridade máxima FreeRTOS) ───────────────────┐
│  Timer de hardware → ISR → vTaskNotifyGiveFromISR → _controlLoop()   │
│  Período: 700 µs (≈ 1,43 kHz)                                        │
│                                                                       │
│  Ciclo (~590 µs):                                                     │
│   ① triggerPrimary()   ~20 µs  INA219 inicia conversão de hardware  │
│   ── hardware converte 532 µs (CPU livre para proteções/setpoints) ──│
│   ② readPrimary()      ~20 µs  polling CNVR; lê registrador         │
│   ③ computeFeedback()   ~1 µs  cálculo float (V_dac)                │
│   ④ writeVoltage()     ~18 µs  Fast Write I2C → MCP4725 → XL4015   │
│   ⑤ tickSecondary()    ~20 µs  canal secundário (proteção)          │
└──────────────────────────────────────────────────────────────────────┘
```

### Pipeline ADC

Em vez de aguardar o INA219 converter (busy-wait de 532 µs), o ESP32 dispara a conversão e usa o tempo para executar cálculos úteis. Quando `readPrimary()` é chamado, a conversão já terminou e o dado é lido imediatamente.

### Canal prioritário por modo

O INA219 converte apenas um canal por ciclo (single-shot 12-bit):

| Modo | Canal primário (1,43 kHz) | Canal secundário (~143 Hz) |
|---|---|---|
| CV | V_bus → equação de controle | I_shunt → OCP |
| CC | I_shunt → equação de controle | V_bus → OVP |

### Sincronização dual-core

Variáveis `float` compartilhadas (telemetria e setpoints) são protegidas com `portENTER_CRITICAL` / `portEXIT_CRITICAL`. A seção crítica envolve apenas a cópia da variável (nunca I2C), mantendo o tempo de bloqueio abaixo de 1 µs.

---

## Estrutura de Arquivos

```
bench-psu/
├── platformio.ini              ← dependências e configuração de build
├── include/
│   └── config.h                ← TODAS as constantes configuráveis
└── src/
    ├── main.cpp                ← setup(), loop(), interface serial
    └── lib/
        ├── hal/
        │   ├── ina219.h        ← driver INA219: single-shot, pipeline, CNVR
        │   └── dac.h           ← driver MCP4725: Fast Write 2 bytes
        ├── control/
        │   ├── feedback.h      ← equações CV/CC e computePower()
        │   └── protection.h    ← OVP e OCP com histérese
        └── app/
            └── psu.h           ← orquestra tudo; FreeRTOS task + timer HW
```

---

## Configuração

Todas as constantes estão em **`include/config.h`**. Principais parâmetros:

| Constante | Padrão | Descrição |
|---|---|---|
| `V_OUT_MAX` | 24,0 V | Tensão máxima de saída |
| `V_OUT_MIN` | 0,0001 V | Zero prático do setpoint (evita divisão por zero em K = Vref / V_set) |
| `I_OUT_MAX` | 5,0 A | Corrente máxima |
| `OVP_THRESHOLD` | 25,0 V | Limiar de disparo OVP |
| `OCP_THRESHOLD` | 5,2 A | Limiar de disparo OCP |
| `XL4015_VREF` | 1,25 V | Referência interna do XL4015 |
| `MCP4725_I2C_ADDR` | 0x60 | Endereço I2C do DAC |
| `INA219_I2C_ADDR` | 0x40 | Endereço I2C do sensor |
| `I2C_FREQ_HZ` | 1.000.000 | Clock I2C em Hz (1 MHz) |
| `CONTROL_PERIOD_US` | 700 | Período do ciclo de controle em µs |
| `DEFAULT_V_SET` | 5,0 V | Tensão padrão ao ligar |
| `DEFAULT_I_SET` | 1,0 A | Corrente padrão ao ligar |

---

## Interface Serial

Conecte ao ESP32 com **115200 baud**. Comandos disponíveis:

| Comando | Exemplo | Descrição |
|---|---|---|
| `v<valor>` | `v12.5` | Define setpoint de tensão (V_set) |
| `i<valor>` | `i2.0` | Define setpoint de corrente (I_set) |
| `mcv` | `mcv` | Seleciona modo CV (tensão constante) |
| `mcc` | `mcc` | Seleciona modo CC (corrente constante) |
| `on` | `on` | Habilita saída |
| `off` | `off` | Desabilita saída |
| `reset` | `reset` | Reseta proteção OVP/OCP |
| `s` | `s` | Exibe status atual |

### Exemplo de saída do monitor serial

```
========================================
  Fonte de Bancada 24V/5A  –  ESP32
========================================
[PSU] Controle iniciado (pipeline 12-bit): 700 µs/ciclo (~1429 Hz), Core 1, prio 24
[MAIN] Pronto. UI rodando no Core 0.
[PSU] Mode:CV | Vset: 5.00V Iset:1.000A | Vout: 0.00V Iout:0.000A Pout:  0.00W | Prot:OK
[CMD] Modo: CV (Tensão Constante)
[CMD] V_set = 12.00 V
[CMD] Saída LIGADA
[PSU] Mode:CV | Vset:12.00V Iset:1.000A | Vout:11.98V Iout:0.502A Pout:  6.01W | Prot:OK
[PSU] Mode:CV | Vset:12.00V Iset:1.000A | Vout:12.00V Iout:0.501A Pout:  6.01W | Prot:OK
```

---

## Como Compilar

### Pré-requisitos

- [PlatformIO](https://platformio.org/) (VSCode + extensão, ou CLI)
- ESP32 conectado via USB

### Build e upload

```bash
# Via CLI
pio run --target upload

# Via VSCode
# Clique em "PlatformIO: Upload" na barra inferior
```

### Dependências (instaladas automaticamente pelo PlatformIO)

```ini
lib_deps =
    adafruit/Adafruit INA219 @ ^1.2.3    # apenas para referência de calibração
    adafruit/Adafruit MCP4725 @ ^2.0.2   # não usado diretamente (Wire raw)
    adafruit/Adafruit BusIO @ ^1.16.1    # dependência transitiva
```

> **Nota:** os drivers HAL (`ina219.h`, `dac.h`) usam `Wire` diretamente para máxima performance. As bibliotecas Adafruit são listadas apenas por compatibilidade de dependências transitivas.

---

## Calibração

### INA219 — ajuste para shunt diferente

Se usar resistor shunt de **0,05 Ω** (para suportar 5 A com menor dissipação):

Em `src/lib/hal/ina219.h`, ajuste:

```cpp
namespace cal {
    // Para R_shunt = 0,05 Ω, I_LSB = 50 µA:
    // Cal = trunc(0,04096 / (0,00005 × 0,05)) = 16384
    constexpr uint16_t VALUE    = 16384u;
    constexpr float    I_LSB_MA = 0.05f;   // mA por LSB
    // ...
}
```

### Verificação da tensão de referência do XL4015

Meça a tensão real no pino FB do XL4015 com multímetro quando a fonte estiver em equilíbrio. Se diferir de 1,25 V, ajuste `XL4015_VREF` em `config.h` para o valor medido.

---

## Licença

Projeto de uso livre para fins educacionais e pessoais.
