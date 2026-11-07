#include "abcd_bus.h"
#include <esp32-hal-psram.h>
#include <driver/uart.h>
#include <string.h>

AbcdBusDriver g_abcdBus;

uint16_t AbcdBusDriver::computeCrc16(const uint8_t* data, size_t len) {
    // Standard Carrier / ICP ABCD CRC-16/ARC (poly 0x8005 reflected = 0xA001, init 0x0000)
    uint16_t crc = 0x0000;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(data[i]);
        for (uint8_t bit = 0; bit < 8; ++bit) {
            if (crc & 0x0001U) {
                crc = (crc >> 1) ^ 0xA001U;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

const char* AbcdBusDriver::addrToName(uint16_t addr) {
    switch (addr) {
        case ADDR_THERMOSTAT: return "Ion Tstat";
        case ADDR_FURNACE:    return "Heil G96CTN";
        case ADDR_ODU:        return "Outdoor Unit";
        case ADDR_CONTROLLER: return "ESP32 CoCtrl";
        case ADDR_BROADCAST:  return "Broadcast";
        default:              return "Bus Node";
    }
}

const char* AbcdBusDriver::funcToName(uint8_t func) {
    switch (func) {
        case FUNC_ACK_RESPONSE: return "ACK/RESP";
        case FUNC_READ:         return "READ";
        case FUNC_WRITE:        return "WRITE";
        case FUNC_NAK:          return "NAK";
        default:                return "DATA";
    }
}

const char* AbcdBusDriver::fanModeToName(AbcdFanMode mode) {
    switch (mode) {
        case AbcdFanMode::AUTO:       return "AUTO";
        case AbcdFanMode::LOW_SPEED:  return "LOW (650 CFM)";
        case AbcdFanMode::MED_SPEED:  return "MED (1050 CFM)";
        case AbcdFanMode::HIGH_SPEED: return "HIGH (1400 CFM)";
    }
    return "AUTO";
}

const char* AbcdBusDriver::stageToName(FurnaceStage stage) {
    switch (stage) {
        case FurnaceStage::OFF:             return "IDLE / OFF";
        case FurnaceStage::BLOWER_ONLY:     return "ECM BLOWER ONLY (Hydronic/Fan)";
        case FurnaceStage::STAGE1_LOW_39K:  return "GAS STAGE 1 LO (39k BTU)";
        case FurnaceStage::STAGE2_HIGH_60K: return "GAS STAGE 2 HI (60k BTU)";
    }
    return "IDLE / OFF";
}

void AbcdBusDriver::begin() {
    memset(&m_telemetry, 0, sizeof(m_telemetry));
    memset(&m_stats, 0, sizeof(m_stats));

    // Default initial telemetry values for bench visibility before physical bus frames arrive
    m_telemetry.room_temp_f         = 68.5f;
    m_telemetry.ion_heat_setpoint_f = 70.0f;
    m_telemetry.ion_fan_mode        = AbcdFanMode::AUTO;
    m_telemetry.furnace_stage       = FurnaceStage::OFF;
    m_telemetry.furnace_airflow_cfm = 0;
    m_telemetry.blower_rpm          = 0;
    m_telemetry.supply_air_temp_f   = 72.0f;
    m_telemetry.last_update_ms      = millis();

    m_stats.tx_enabled              = false;
    m_stats.sim_bus_enabled         = true; // Default enabled on bench until physical RX bytes arrive
    m_stats.capture_paused          = false;
    m_stats.last_activity_ms        = millis();

    // Allocate 250-packet ring buffer in 8 MB Octal PSRAM
    const size_t bytesNeeded = sizeof(AbcdFrameRecord) * ABCD_RING_BUFFER_CAP;
    if (psramFound()) {
        m_ringBuffer = static_cast<AbcdFrameRecord*>(ps_malloc(bytesNeeded));
        if (m_ringBuffer != nullptr) {
            m_stats.psram_buffer_active = true;
            memset(m_ringBuffer, 0, bytesNeeded);
        }
    }
    if (m_ringBuffer == nullptr) {
        m_ringBuffer = static_cast<AbcdFrameRecord*>(malloc(bytesNeeded));
        if (m_ringBuffer != nullptr) {
            memset(m_ringBuffer, 0, bytesNeeded);
        }
    }

    // Configure UART1 for 38400 8N1 on RX=GPIO 18, TX=GPIO 17
    Serial1.setRxBufferSize(1024);
    Serial1.begin(ABCD_BAUD_RATE, SERIAL_8N1, ABCD_UART_RX_PIN, ABCD_UART_TX_PIN);
    // Enable internal pull-up on RX (GPIO 18) so an unconnected pin on the desk stays at UART idle HIGH
    pinMode(ABCD_UART_RX_PIN, INPUT_PULLUP);

    // Start in safe listen-only mode (GPIO 21 DE/RE locked LOW)
    pinMode(ABCD_UART_DERE_PIN, OUTPUT);
    digitalWrite(ABCD_UART_DERE_PIN, LOW);

    // Seed an initial simulated cycle so the packet analyzer has immediate example frames on boot
    injectSimulatedCycle();
}

void AbcdBusDriver::setTxEnabled(bool enabled) {
    m_stats.tx_enabled = enabled;
    if (!enabled) {
        // Lock DE/RE pin LOW for 100% passive listening
        pinMode(ABCD_UART_DERE_PIN, OUTPUT);
        digitalWrite(ABCD_UART_DERE_PIN, LOW);
    }
}

void AbcdBusDriver::clearCaptureBuffer() {
    m_ringHead  = 0;
    m_ringCount = 0;
}

uint32_t AbcdBusDriver::getBusIdleMs() const {
    const uint32_t now = millis();
    if (now >= m_stats.last_activity_ms) {
        return now - m_stats.last_activity_ms;
    }
    return 0;
}

void AbcdBusDriver::loop() {
    // 1. Read any incoming bytes from physical RS-485 UART1
    while (Serial1.available() > 0) {
        const uint8_t b = static_cast<uint8_t>(Serial1.read());
        processRxByte(b);
    }

    // 2. Check inter-frame gap (> 4.5 ms silence at 38400 baud marks end of frame)
    if (m_rxLen > 0 && (micros() - m_lastByteRxUs) > 4500UL) {
        finalizeRxFrame(false, false);
    }

    // 3. Periodic synthetic bus polling in Desk Simulation Mode (every 4 seconds)
    if (m_stats.sim_bus_enabled && (millis() - m_lastSimTickMs >= 4000UL)) {
        m_lastSimTickMs = millis();
        injectSimulatedCycle();
    }
}

void AbcdBusDriver::processRxByte(uint8_t b) {
    m_lastByteRxUs = micros();
    m_stats.last_activity_ms = millis();
    if (m_rxLen < ABCD_MAX_FRAME_BYTES) {
        m_rxBuf[m_rxLen++] = b;
    } else {
        finalizeRxFrame(false, false);
        m_rxBuf[0] = b;
        m_rxLen = 1;
    }
}

void AbcdBusDriver::finalizeRxFrame(bool isTx, bool isSimulated) {
    if (m_rxLen >= 10) {
        decodeAndRecordFrame(m_rxBuf, m_rxLen, isTx, isSimulated);
    }
    m_rxLen = 0;
}

void AbcdBusDriver::appendRecordToRing(const AbcdFrameRecord& rec) {
    if (m_ringBuffer == nullptr || m_stats.capture_paused) {
        return;
    }
    m_ringBuffer[m_ringHead] = rec;
    m_ringHead = (m_ringHead + 1) % ABCD_RING_BUFFER_CAP;
    if (m_ringCount < ABCD_RING_BUFFER_CAP) {
        m_ringCount++;
    }
}

bool AbcdBusDriver::getFrameByOffsetFromNewest(size_t offsetFromNewest, AbcdFrameRecord& outRecord) const {
    if (m_ringBuffer == nullptr || offsetFromNewest >= m_ringCount) {
        return false;
    }
    const size_t idx = (m_ringHead + ABCD_RING_BUFFER_CAP - 1 - offsetFromNewest) % ABCD_RING_BUFFER_CAP;
    outRecord = m_ringBuffer[idx];
    return true;
}

void AbcdBusDriver::decodeAndRecordFrame(const uint8_t* buf, size_t len, bool isTx, bool isSimulated) {
    if (len < 10 || len > ABCD_MAX_FRAME_BYTES) {
        return;
    }

    AbcdFrameRecord rec{};
    rec.seq          = ++m_seqCounter;
    rec.timestamp_ms = millis();
    rec.is_tx        = isTx;
    rec.is_simulated = isSimulated;
    rec.dest_addr    = (static_cast<uint16_t>(buf[0]) << 8) | buf[1];
    rec.src_addr     = (static_cast<uint16_t>(buf[2]) << 8) | buf[3];
    rec.length       = buf[4];
    rec.func         = buf[7];

    if (len >= 11) {
        rec.table_id[0] = buf[8];
        rec.table_id[1] = buf[9];
        rec.table_id[2] = buf[10];
    }

    rec.crc_rx   = static_cast<uint16_t>(buf[len - 2]) | (static_cast<uint16_t>(buf[len - 1]) << 8);
    rec.crc_calc = computeCrc16(buf, len - 2);
    rec.crc_ok   = (rec.crc_rx == rec.crc_calc);

    rec.raw_len  = static_cast<uint8_t>(len);
    memcpy(rec.raw, buf, len);

    if (isTx) {
        m_stats.total_tx_frames++;
    } else {
        m_stats.total_rx_frames++;
    }
    if (isSimulated) {
        m_stats.sim_frames_count++;
    }
    if (rec.crc_ok) {
        m_stats.crc_ok_count++;
        if (!isSimulated && !isTx && m_stats.sim_bus_enabled) {
            m_stats.sim_bus_enabled = false;
        }
    } else {
        m_stats.crc_err_count++;
    }
    m_stats.last_activity_ms = rec.timestamp_ms;

    // Decode known Heil Ion / ICP tables when CRC is valid
    snprintf(rec.summary, sizeof(rec.summary), "%s %02X:%02X:%02X (%uB payload)",
             funcToName(rec.func), rec.table_id[0], rec.table_id[1], rec.table_id[2],
             (unsigned)(len > 13 ? len - 13 : 0));

    if (rec.crc_ok && len >= 13) {
        const uint8_t* payload = &buf[11];
        const size_t payloadLen = len - 13;

        // Table 00:3B:01 -> Zone 1 Fan Mode & Heat Setpoint Configuration
        if (rec.table_id[0] == 0x00 && rec.table_id[1] == 0x3B && rec.table_id[2] == 0x01) {
            if ((rec.func == FUNC_WRITE || rec.func == FUNC_ACK_RESPONSE) && payloadLen >= 2) {
                const uint8_t fanByte = payload[0];
                const uint8_t spByte  = payload[1]; // Setpoint in °F
                if (fanByte <= 3) {
                    m_telemetry.ion_fan_mode = static_cast<AbcdFanMode>(fanByte);
                }
                if (spByte >= 50 && spByte <= 90) {
                    m_telemetry.ion_heat_setpoint_f = static_cast<float>(spByte);
                }
                m_telemetry.last_update_ms = rec.timestamp_ms;

                // Update furnace blower CFM telemetry to match active fan speed
                if (m_telemetry.ion_fan_mode == AbcdFanMode::LOW_SPEED) {
                    m_telemetry.furnace_stage       = FurnaceStage::BLOWER_ONLY;
                    m_telemetry.furnace_airflow_cfm = 650;
                    m_telemetry.blower_rpm          = 620;
                } else if (m_telemetry.ion_fan_mode == AbcdFanMode::MED_SPEED) {
                    m_telemetry.furnace_stage       = FurnaceStage::BLOWER_ONLY;
                    m_telemetry.furnace_airflow_cfm = 1050;
                    m_telemetry.blower_rpm          = 890;
                } else if (m_telemetry.ion_fan_mode == AbcdFanMode::HIGH_SPEED) {
                    m_telemetry.furnace_stage       = FurnaceStage::BLOWER_ONLY;
                    m_telemetry.furnace_airflow_cfm = 1400;
                    m_telemetry.blower_rpm          = 1140;
                } else if (m_telemetry.furnace_stage == FurnaceStage::BLOWER_ONLY) {
                    m_telemetry.furnace_stage       = FurnaceStage::OFF;
                    m_telemetry.furnace_airflow_cfm = 0;
                    m_telemetry.blower_rpm          = 0;
                }

                snprintf(rec.summary, sizeof(rec.summary),
                         "Zone1 Config %s: Fan=%s, HeatSP=%uF",
                         funcToName(rec.func),
                         fanModeToName(m_telemetry.ion_fan_mode),
                         (unsigned)spByte);
            } else if (rec.func == FUNC_READ) {
                snprintf(rec.summary, sizeof(rec.summary), "Poll Zone1 Config (00:3B:01)");
            }
        }
        // Table 00:3B:02 -> Zone 1 Sensed Room Temperature (tenths of °F, big-endian uint16)
        else if (rec.table_id[0] == 0x00 && rec.table_id[1] == 0x3B && rec.table_id[2] == 0x02) {
            if ((rec.func == FUNC_WRITE || rec.func == FUNC_ACK_RESPONSE) && payloadLen >= 2) {
                const uint16_t rawTenths = (static_cast<uint16_t>(payload[0]) << 8) | payload[1];
                if (rawTenths >= 350 && rawTenths <= 1100) {
                    m_telemetry.room_temp_f = rawTenths / 10.0f;
                    m_telemetry.last_update_ms = rec.timestamp_ms;
                }
                snprintf(rec.summary, sizeof(rec.summary),
                         "Ion Zone1 Room Temp: %.1fF", m_telemetry.room_temp_f);
            } else if (rec.func == FUNC_READ) {
                snprintf(rec.summary, sizeof(rec.summary), "Poll Zone1 Room Temp (00:3B:02)");
            }
        }
        // Table 00:04:02 -> Heil G96CTN0601714A1 Furnace Status (Stage, CFM, RPM, Supply Air °F)
        else if (rec.table_id[0] == 0x00 && rec.table_id[1] == 0x04 && rec.table_id[2] == 0x02) {
            if (rec.func == FUNC_ACK_RESPONSE && payloadLen >= 6) {
                const uint8_t  stg = payload[0];
                const uint16_t cfm = (static_cast<uint16_t>(payload[1]) << 8) | payload[2];
                const uint16_t rpm = (static_cast<uint16_t>(payload[3]) << 8) | payload[4];
                const uint8_t  sat = payload[5];
                if (stg <= 3) {
                    m_telemetry.furnace_stage = static_cast<FurnaceStage>(stg);
                }
                m_telemetry.furnace_airflow_cfm = cfm;
                m_telemetry.blower_rpm          = rpm;
                m_telemetry.supply_air_temp_f   = static_cast<float>(sat);
                m_telemetry.last_update_ms      = rec.timestamp_ms;

                snprintf(rec.summary, sizeof(rec.summary),
                         "Heil G96CTN Status: %s | %u CFM (%u RPM) | Supply %uF",
                         stageToName(m_telemetry.furnace_stage),
                         (unsigned)cfm, (unsigned)rpm, (unsigned)sat);
            } else if (rec.func == FUNC_READ) {
                snprintf(rec.summary, sizeof(rec.summary), "Poll Heil G96CTN Blower/Stage Status (00:04:02)");
            }
        }
    }

    appendRecordToRing(rec);
}

bool AbcdBusDriver::transmitRawFrame(const uint8_t* frame, size_t len) {
    if (!m_stats.tx_enabled) {
        return false;
    }

    // Wait for >= 15 ms of bus silence before asserting DE/RE HIGH
    const uint32_t waitStart = millis();
    while (getBusIdleMs() < ABCD_MIN_IDLE_GAP_MS) {
        if (millis() - waitStart > 100UL) {
            break;
        }
        delay(1);
    }

    digitalWrite(ABCD_UART_DERE_PIN, HIGH);
    delayMicroseconds(150);
    Serial1.write(frame, len);
    Serial1.flush();
    delayMicroseconds(300);
    digitalWrite(ABCD_UART_DERE_PIN, LOW);

    return true;
}

bool AbcdBusDriver::sendCoControllerCommand(AbcdFanMode fanMode, float targetHeatSetpointF, const char* reasonNote) {
    // Build an authentic ABCD WRITE frame from 0x9201 (ESP32 Co-Controller) -> 0x4001 (Heil G96CTN) / 0x2001 (Ion Tstat)
    uint8_t frame[16];
    const uint8_t spRounded = static_cast<uint8_t>(targetHeatSetpointF + 0.5f);

    frame[0]  = static_cast<uint8_t>((ADDR_FURNACE >> 8) & 0xFF);
    frame[1]  = static_cast<uint8_t>(ADDR_FURNACE & 0xFF);
    frame[2]  = static_cast<uint8_t>((ADDR_CONTROLLER >> 8) & 0xFF);
    frame[3]  = static_cast<uint8_t>(ADDR_CONTROLLER & 0xFF);
    frame[4]  = 0x05; // 3-byte table ID + 2-byte payload
    frame[5]  = 0x00;
    frame[6]  = 0x00;
    frame[7]  = FUNC_WRITE;
    frame[8]  = 0x00;
    frame[9]  = 0x3B;
    frame[10] = 0x01;
    frame[11] = static_cast<uint8_t>(fanMode);
    frame[12] = spRounded;

    const uint16_t crc = computeCrc16(frame, 13);
    frame[13] = static_cast<uint8_t>(crc & 0xFF);
    frame[14] = static_cast<uint8_t>((crc >> 8) & 0xFF);

    const bool physicalSent = transmitRawFrame(frame, 15);
    decodeAndRecordFrame(frame, 15, true, !physicalSent);

    Serial.printf("[ABCD CO-CTRL TX] %s -> Fan=%s, IonBackupSP=%uF (%s)\n",
                  physicalSent ? "RS485-TX" : "SIM-TX",
                  fanModeToName(fanMode),
                  (unsigned)spRounded,
                  reasonNote ? reasonNote : "");
    return true;
}

void AbcdBusDriver::injectSimulatedCycle() {
    // Frame 1: Ion Thermostat (0x2001) broadcasts current room temperature (Table 00:3B:02)
    {
        uint8_t f[15];
        const uint16_t roomTenths = static_cast<uint16_t>(m_telemetry.room_temp_f * 10.0f + 0.5f);
        f[0]  = 0x40; f[1]  = 0x01; // Dest: Heil G96CTN (0x4001)
        f[2]  = 0x20; f[3]  = 0x01; // Src : Ion Thermostat (0x2001)
        f[4]  = 0x05;
        f[5]  = 0x00; f[6]  = 0x00;
        f[7]  = FUNC_WRITE;
        f[8]  = 0x00; f[9]  = 0x3B; f[10] = 0x02;
        f[11] = static_cast<uint8_t>((roomTenths >> 8) & 0xFF);
        f[12] = static_cast<uint8_t>(roomTenths & 0xFF);
        const uint16_t crc = computeCrc16(f, 13);
        f[13] = static_cast<uint8_t>(crc & 0xFF);
        f[14] = static_cast<uint8_t>((crc >> 8) & 0xFF);
        decodeAndRecordFrame(f, 15, false, true);
    }

    // Frame 2: Ion Thermostat (0x2001) polls Heil G96CTN (0x4001) status register (Table 00:04:02)
    {
        uint8_t f[13];
        f[0]  = 0x40; f[1]  = 0x01;
        f[2]  = 0x20; f[3]  = 0x01;
        f[4]  = 0x03;
        f[5]  = 0x00; f[6]  = (m_simStep++);
        f[7]  = FUNC_READ;
        f[8]  = 0x00; f[9]  = 0x04; f[10] = 0x02;
        const uint16_t crc = computeCrc16(f, 11);
        f[11] = static_cast<uint8_t>(crc & 0xFF);
        f[12] = static_cast<uint8_t>((crc >> 8) & 0xFF);
        decodeAndRecordFrame(f, 13, false, true);
    }

    // Frame 3: Heil G96CTN0601714A1 (0x4001) replies to Ion Thermostat (0x2001) with live stage, CFM, RPM
    {
        uint8_t f[19];
        f[0]  = 0x20; f[1]  = 0x01; // Dest: Ion Thermostat
        f[2]  = 0x40; f[3]  = 0x01; // Src : Heil G96CTN
        f[4]  = 0x09;               // 3-byte table + 6-byte status payload
        f[5]  = 0x00; f[6]  = m_simStep;
        f[7]  = FUNC_ACK_RESPONSE;
        f[8]  = 0x00; f[9]  = 0x04; f[10] = 0x02;
        f[11] = static_cast<uint8_t>(m_telemetry.furnace_stage);
        f[12] = static_cast<uint8_t>((m_telemetry.furnace_airflow_cfm >> 8) & 0xFF);
        f[13] = static_cast<uint8_t>(m_telemetry.furnace_airflow_cfm & 0xFF);
        f[14] = static_cast<uint8_t>((m_telemetry.blower_rpm >> 8) & 0xFF);
        f[15] = static_cast<uint8_t>(m_telemetry.blower_rpm & 0xFF);
        f[16] = static_cast<uint8_t>(m_telemetry.furnace_airflow_cfm > 0 ? 108 : 72);
        const uint16_t crc = computeCrc16(f, 17);
        f[17] = static_cast<uint8_t>(crc & 0xFF);
        f[18] = static_cast<uint8_t>((crc >> 8) & 0xFF);
        decodeAndRecordFrame(f, 19, false, true);
    }
}
