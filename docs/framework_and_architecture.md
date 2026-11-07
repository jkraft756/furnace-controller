# ESP32-S3-DevKitC-1-N16R8 + RS-485 Furnace Controller: Framework & Architecture Guide

## 1. Hardware Platform Overview

- **Controller**: AITRIP ESP32-S3-DevKitC-1-N16R8 (ESP32-S3-WROOM-1-N16R8)
  - **SoC**: Dual-core Xtensa LX7 @ 240 MHz
  - **Memory**: 16 MB Quad SPI Flash (`qio`), 8 MB Octal SPI PSRAM (`opi`), 512 KB Internal SRAM
  - **USB Interfaces**:
    - Dedicated USB-to-UART Bridge (`CH343` / `CH9102`, `VID:PID=1A86:55D3`) on `/dev/cu.usbmodem5CE51967211` (`UART0`)
    - Native USB-OTG / USB Serial/JTAG port (`GPIO19` / `GPIO20`)
  - **Peripherals**: 3x Hardware UARTs (`UART0`, `UART1`, `UART2`), TWAI, SPI, I2C, RMT (onboard WS2812 RGB LED on GPIO 48 / GPIO 38)
- **Transceiver Base**: M5Stack Atomic RS485 Base (`MAX3485` / `SP485EEN` isolated/DC-DC RS-485 transceiver)
- **Target System**: Heil Ion Communicating System (International Comfort Products / Carrier ABCD 4-wire RS-485 communicating HVAC bus)

---

## 2. Development Framework Comparison

For building a reliable RS-485 Man-in-the-Middle (MitM) bridge, packet analyzer, and HVAC controller on the ESP32-S3, three primary firmware ecosystems were evaluated:

| Feature / Criterion | **PlatformIO (C++ / Arduino or ESP-IDF)** | **MicroPython / CircuitPython** | **ESPHome (YAML + C++ Custom Components)** |
| :--- | :--- | :--- | :--- |
| **UART / RS-485 Timing Determinism** | **Excellent (< 10 µs)**. Direct hardware UART FIFO, RS-485 half-duplex direction control, and FreeRTOS ISR queues. | **Poor (1–15 ms jitter)**. Garbage collection pauses and bytecode interpretation can drop or corrupt back-to-back RS-485 frames. | **Moderate**. Polling loop runs every ~16 ms (`loop()`); requires custom C++ & FreeRTOS tasks to avoid frame bottlenecks. |
| **Dual-Core FreeRTOS Utilization** | **Full Native Control**. Pin RS-485 bus transceiver tasks to Core 1 and Wi-Fi/Web/MQTT/Logging to Core 0 (`xTaskCreatePinnedToCore`). | **Limited**. GIL / single-core execution model prevents true deterministic parallel bus forwarding. | **Implicit**. Single-threaded main loop unless dropping down to raw C++ FreeRTOS tasks anyway. |
| **Man-in-the-Middle (Dual UART) Routing** | **First-Class**. Native support for bridging `UART1` (Thermostat side) and `UART2` (Furnace/ODU side) with zero-copy inspection and CRC recalculation. | **Fragile**. High risk of buffer overruns or missed inter-frame gaps during active MitM rewriting. | **Complex**. Requires writing the entire MitM engine in C++ and wrapping it for YAML configuration. |
| **Debugging & Packet Analysis** | **Rich**. High-speed native USB CDC logging (`Serial`), binary/pcap frame streaming, unit testing, and static analysis. | **Interactive REPL**. Great for quick pin toggles, poor for sustained line-rate binary protocol decoding. | **Log-centric**. Designed for high-level sensor state reporting rather than raw protocol reverse engineering. |
| **OTA & Smart Home Integration** | **Flexible**. Supports AsyncWebServer, MQTT (Home Assistant Auto-Discovery), WebSockets, and ArduinoOTA / ESP-IDF OTA. | **Manual**. Requires custom asyncio network stacks and watchdog handling. | **Turnkey**. Native Home Assistant API and OTA, best suited *after* a protocol is 100% static and low-bandwidth. |

### Recommendation: **PlatformIO with C++ (Arduino / ESP-IDF Hybrid)**
PlatformIO with C++ gives deterministic microsecond-level UART framing, hardware interrupt efficiency, native FreeRTOS dual-core task isolation, and full control over both hardware UARTs required for a fail-safe RS-485 Man-in-the-Middle architecture.

---

## 3. Heil Ion / ICP ABCD Bus Protocol & Electrical Overview

The Heil Ion System Controller (and compatible ICP / Carrier Infinity / Bryant Evolution communicating equipment) communicates over a 4-wire bus labeled **A, B, C, D**:

| Terminal | Signal | Description |
| :--- | :--- | :--- |
| **A** | `DATA +` (`RS-485 A` / Non-inverting) | Differential RS-485 data line |
| **B** | `DATA -` (`RS-485 B` / Inverting) | Differential RS-485 data line |
| **C** | `24 VAC Common` | Reference ground / AC return |
| **D** | `24 VAC Hot` | 24 VAC power supply from furnace transformer |

> **Electrical Safety Note**: Only connect **A** and **B** (and an isolated signal ground reference if applicable) to the RS-485 transceiver data terminals. Never apply raw 24 VAC (`C`/`D`) to the DC inputs of an RS-485 transceiver unless using a properly rated 24 VAC-to-DC buck rectifier.

### Protocol Characteristics
- **Physical Layer**: Half-duplex differential RS-485
- **Typical Baud Rate**: `38400 bps`, `8N1` (8 data bits, no parity, 1 stop bit)
- **Frame Structure**:
  1. **Destination Address** (2 bytes, e.g., Thermostat `0x2001`, Furnace/IFC `0x4001`, Outdoor Unit `0x5001`, Broadcast `0xF1F1`)
  2. **Source Address** (2 bytes)
  3. **Payload Length** (1 byte)
  4. **PID / Register / Table Identifiers** (3 bytes)
  5. **Command / Opcode** (1 byte: `0x0B` Read, `0x0C` Write, `0x06` ACK/Response, `0x15` NAK)
  6. **Data Payload** ($N$ bytes)
  7. **CRC-16** (2 bytes, CRC-16/MODBUS or Carrier-specific polynomial depending on generation)

---

## 4. Dual-Core Man-in-the-Middle (MitM) Architecture

To intercept, inspect, and selectively modify commands between the Heil Ion Wall Control (Thermostat) and the Furnace Integrated Furnace Control (IFC) without triggering communication fault codes, the firmware uses a **Dual-Core FreeRTOS Architecture**:

```
+-----------------------------------------------------------------------------------+
|                            M5Stack StampS3 (ESP32-S3)                             |
|                                                                                   |
|   CORE 1 (Real-Time RS-485 Engine)          CORE 0 (Network, Telemetry & UI)      |
|  +--------------------------------+        +-----------------------------------+  |
|  | UART1 Task (Thermostat Side)   |        | Wi-Fi / MQTT / Home Assistant     |  |
|  |  - Frame Sync & Timeout Timer  |        | WebSocket Live Packet Sniffer     |  |
|  |  - CRC-16 Verification         |        | REST API & Override Rules Engine  |  |
|  +---------------+----------------+        | USB CDC High-Speed Frame Logger   |  |
|                  |                         | WS2812 Status LED State Machine   |  |
|                  v                         +-----------------+-----------------+  |
|  +--------------------------------+                          ^                    |
|  | Deterministic MitM Router      | === Lock-Free RingBuf ==>|                    |
|  |  - Pass-through by default     | <== Atomic Rule Table ===+                    |
|  |  - Payload Rewrite + Re-CRC    |                                               |
|  +---------------+----------------+                                               |
|                  |                                                                |
|                  v                                                                |
|  +--------------------------------+                                               |
|  | UART2 Task (Furnace/Bus Side)  |                                               |
|  |  - Half-Duplex TX/RX Framing   |                                               |
|  +--------------------------------+                                               |
+-----------------------------------------------------------------------------------+
```

### Phased Rollout Strategy
1. **Phase 1 — Passive Bus Sniffer (Single RS-485 Interface)**:
   - Connect in high-impedance parallel (`A`/`B`) to the live ABCD bus.
   - Capture, validate CRC-16, decode registers (airflow CFM, blower RPM, stage/modulation %, duct static pressure, outdoor temp, setpoints), and stream frames over USB Serial / WebSockets.
2. **Phase 2 — Active Responder / Spoofer Validation**:
   - Bench-test frame transmission, inter-frame gap timing, and CRC generation.
3. **Phase 3 — Inline Dual-Transceiver Man-in-the-Middle (MitM)**:
   - Physically split the `A`/`B` data pair between the Thermostat (`UART1`) and the Furnace (`UART2`) with a hardware NC relay bypass for fail-safe operation on power loss.
   - Forward all frames transparently while selectively overriding target registers on the fly.
