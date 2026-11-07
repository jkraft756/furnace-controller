# ESP32-S3-DevKitC-1-N16R8 + RS-485 Furnace Controller: Framework & Architecture Guide

## 1. Hardware Platform & Target HVAC Equipment

- **Controller**: AITRIP ESP32-S3-DevKitC-1-N16R8 (ESP32-S3-WROOM-1-N16R8)
  - **SoC**: Dual-core Xtensa LX7 @ 240 MHz
  - **Memory**: 16 MB Quad SPI Flash (`qio`), 8 MB Octal SPI PSRAM (`opi`), 512 KB Internal SRAM
  - **USB Interfaces**:
    - Dedicated USB-to-UART Bridge (`CH343` / `CH9102`, `VID:PID=1A86:55D3`) on `/dev/cu.usbmodem5CE51967211` (`UART0`)
    - Native USB-OTG / USB Serial/JTAG port (`GPIO19` / `GPIO20`)
  - **Peripherals**:
    - `UART1` (`GPIO 18` RX, `GPIO 17` TX, `GPIO 21` RTS/DE_RE) -> 3.3V RS-485 Transceiver (`MAX3485` / `SP3485`)
    - `GPIO 4` (1-Wire Bus) -> Waterproof `DS18B20` Digital Temperature Probes (Wood Boiler Supply & Return Water Pipes)
    - Onboard WS2812 RGB LED (`GPIO 48` / `GPIO 38`)
- **Target Furnace**: **Heil `G96CTN0601714A1`** (QuietComfort 96 Series, International Comfort Products / Carrier Global)
  - **Type**: 96% AFUE Communicating Two-Stage Gas Furnace (`39,000 BTU/hr` Low Fire, `60,000 BTU/hr` High Fire)
  - **Blower**: Variable-Speed Constant-Torque / Constant-Airflow ECM Blower (`1/2 HP, 373W`, ~1,400 CFM nominal)
  - **Model Plug**: `HK70EZ089` (`Pin 1-4: 39 kΩ`, `Pin 2-3: 91 kΩ`)
- **Target Wall Control**: **Heil Ion System Control** (4-wire ABCD communicating bus master `0x2001`)

---

## 2. Development Framework Comparison

For building a deterministic RS-485 ABCD bus sniffer, packet analyzer, and Wood Boiler / Furnace Blower co-controller on the ESP32-S3, three primary firmware ecosystems were evaluated:

| Feature / Criterion | **PlatformIO (C++ / Arduino or ESP-IDF)** | **MicroPython / CircuitPython** | **ESPHome (YAML + C++ Custom Components)** |
| :--- | :--- | :--- | :--- |
| **UART / RS-485 Timing Determinism** | **Excellent (< 10 µs)**. Direct hardware UART FIFO, RS-485 half-duplex direction control, and collision-avoidance idle gap timers. | **Poor (1–15 ms jitter)**. Garbage collection pauses and bytecode interpretation can drop or corrupt back-to-back RS-485 frames. | **Moderate**. Polling loop runs every ~16 ms (`loop()`); requires custom C++ & FreeRTOS tasks to avoid frame bottlenecks. |
| **8 MB Octal PSRAM Ring Buffers** | **Full Native Control**. Direct `ps_malloc()` allocation for storing hundreds of raw & decoded RS-485 frames without touching internal SRAM. | **Limited**. Managed heap makes deterministic binary packet ring buffers slower. | **Manual**. Requires custom C++ component allocation. |
| **ABCD Bus Protocol Analysis & Co-Control** | **First-Class**. Native support for passive listening (`DE/RE = LOW`) and collision-free peer transmission (`>= 15 ms` idle gap + CRC-16/ARC). | **Fragile**. High risk of missed inter-frame gaps during active bus writes. | **Complex**. Requires writing the entire protocol engine in C++ and wrapping it for YAML configuration. |
| **Web UI & CSV Packet Export** | **Rich**. Embedded responsive multi-tab dashboard, live JSON telemetry, CSV packet export, and interactive bench simulation. | **Manual**. Requires custom asyncio network stacks and watchdog handling. | **Log-centric**. Designed for high-level sensor state reporting rather than raw protocol reverse engineering. |

### Selected Framework: **PlatformIO with C++ (Arduino / ESP-IDF Hybrid)**

---

## 3. Heil Ion / ICP ABCD Bus Protocol & Electrical Overview

The Heil Ion System Controller and Heil `G96CTN0601714A1` furnace communicate over a 4-wire bus labeled **A, B, C, D**:

| Terminal | Signal | Description |
| :--- | :--- | :--- |
| **A** | `DATA +` (`RS-485 A` / Non-inverting) | Differential RS-485 data line |
| **B** | `DATA -` (`RS-485 B` / Inverting) | Differential RS-485 data line |
| **C** | `24 VAC Hot` | 24 VAC power supply from furnace transformer (fused via `1A` inline fuse to 24VAC->5VDC buck converter) |
| **D** | `24 VAC Common` | Reference ground / 24 VAC return |

### Protocol Characteristics
- **Physical Layer**: Half-duplex differential RS-485 (`38400 bps`, `8N1`)
- **Frame Structure (10+ bytes)**:
  1. **Destination Address** (2 bytes: `0x2001` Thermostat, `0x4001` Furnace IFC, `0x9201` Co-Controller/SAM, `0xFFFF` Broadcast)
  2. **Source Address** (2 bytes)
  3. **Payload Length** (1 byte: length of payload + 3-byte table/reg header)
  4. **PID / Node Sequence** (2 bytes, typically `0x00, 0x00`)
  5. **Function / Opcode** (1 byte: `0x0B` READ, `0x0C` WRITE, `0x06` ACK/RESPONSE, `0x15` NAK)
  6. **Table & Register ID** (3 bytes: e.g., `00:3B:01` Zone 1 Config/Setpoint/Fan, `00:3B:02` Zone 1 Room Temp, `00:04:02` Furnace Blower & Stage Status)
  7. **Data Payload** ($N$ bytes)
  8. **CRC-16** (2 bytes, little-endian `CRC-16/ARC` polynomial `0x8005` / reflected `0xA001`, init `0x0000`)

---

## 4. Parallel ABCD Bus Co-Controller Architecture

Instead of severing the factory thermostat wires for an inline Man-in-the-Middle bridge, the ESP32-S3 connects **in parallel as a peer device on the 4-wire ABCD bus** (identical to an ICP/Carrier System Access Module `0x9201`):

1. **Zero Wire Cutting / Inherently Fail-Safe**: The factory wiring between the Ion Thermostat (`0x2001`) and the Heil `G96CTN0601714A1` (`0x4001`) remains 100% intact.
2. **Stage 1 — Passive Bus Sniffer (`DE/RE` Locked `LOW`)**:
   - Captures, validates CRC-16, decodes registers (Room Temp, Heat Setpoint, Fan Mode, Furnace Stage, Airflow CFM), and stores the last 250 frames in the 8 MB Octal PSRAM ring buffer for live inspection and `.csv` export.
3. **Stage 2 — Automated Wood Boiler Co-Controller**:
   - Monitors the hydronic Wood Boiler water temperature entering the plenum heat exchanger coil via `DS18B20` probes on `GPIO 4`.
   - When `Boiler Supply >= 140°F`, shifts the Ion gas backup setpoint down to the fallback threshold (`64°F`) so gas stays off, and cycles the Heil `G96CTN` ECM blower (`Low`/`Med`/`High`) whenever the sensed room temperature drops below the target comfort setpoint (`70.0°F`).
   - When `Boiler Supply < 130°F` (fire out), automatically restores the Ion thermostat's normal gas heating setpoint (`70.0°F`) and sets Fan Mode back to `AUTO` for seamless 2-stage gas fallback.
