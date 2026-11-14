#include "abcd_bus.h"
#include <Preferences.h>
#include <esp32-hal-psram.h>
#include <driver/uart.h>
#include <driver/gpio.h>
#include <math.h>
#include <string.h>

static float s_lastSavedRoomTempF = -999.0f;

static void saveRoomTempToNvs(float roomF) {
    if (fabsf(roomF - s_lastSavedRoomTempF) < 0.1f) {
        return;
    }
    Preferences prefs;
    if (prefs.begin("furnace", false)) {
        prefs.putFloat("last_room_f", roomF);
        prefs.end();
        s_lastSavedRoomTempF = roomF;
    }
}

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
        case ADDR_THERMOSTAT:   return "Ion Tstat";
        case ADDR_FURNACE:
        case ADDR_FURNACE_HEIL: return "Heil G96CTN";
        case ADDR_ODU:          return "Outdoor Unit";
        case ADDR_CONTROLLER:   return "ESP32 CoCtrl";
        case ADDR_BROADCAST:    return "Broadcast";
        default:                return "Bus Node";
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
        case AbcdFanMode::LOW_SPEED:  return "LOW (413 CFM)";
        case AbcdFanMode::MED_SPEED:  return "MED (667 CFM)";
        case AbcdFanMode::HIGH_SPEED: return "HIGH (1050 CFM)";
    }
    return "AUTO";
}

static AbcdFanMode cfmToFanMode(uint16_t cfm) {
    if (cfm == 0) return AbcdFanMode::AUTO;
    if (cfm <= 500) return AbcdFanMode::LOW_SPEED;
    if (cfm <= 850) return AbcdFanMode::MED_SPEED;
    return AbcdFanMode::HIGH_SPEED;
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

    // Load last known live room temperature from NVS so reboots/OTA updates immediately show real room temp
    float savedRoomF = 75.3f;
    {
        Preferences prefs;
        if (prefs.begin("furnace", true)) {
            savedRoomF = prefs.getFloat("last_room_f", 75.3f);
            prefs.end();
        }
    }
    if (savedRoomF < 35.0f || savedRoomF > 105.0f) {
        savedRoomF = 75.3f;
    }
    s_lastSavedRoomTempF = savedRoomF;

    m_telemetry.room_temp_f             = savedRoomF;
    m_telemetry.room_temp_valid         = false;
    m_telemetry.ion_heat_setpoint_f     = 70.0f;
    m_telemetry.ion_heat_sp_valid       = false;
    m_telemetry.ion_fan_mode            = AbcdFanMode::AUTO;
    m_lastSetpointPollMs                = 0;
    m_telemetry.furnace_stage           = FurnaceStage::OFF;
    m_telemetry.furnace_airflow_cfm     = 0;
    m_telemetry.blower_rpm              = 0;
    m_telemetry.blower_watts            = 0.0f;
    m_telemetry.static_press_hundredths = 0;
    snprintf(m_telemetry.relay_status, sizeof(m_telemetry.relay_status), "IDLE (0x06)");
    m_telemetry.supply_air_temp_f       = 72.0f;
    m_telemetry.last_update_ms          = millis();

    m_stats.tx_enabled              = false;
    m_stats.sim_bus_enabled         = false;
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

    // Configure UART1 for 38400 8N1 on RX=GPIO 17, TX=GPIO 18
    // Note: pinMode MUST be called BEFORE Serial1.begin() because in Arduino-ESP32 3.x (periman),
    // calling pinMode() after Serial1.begin() detaches the pin from the UART peripheral matrix!
    pinMode(ABCD_UART_RX_PIN, INPUT_PULLUP);
    Serial1.setRxBufferSize(1024);
    Serial1.begin(ABCD_BAUD_RATE, SERIAL_8N1, ABCD_UART_RX_PIN, ABCD_UART_TX_PIN);
    gpio_pullup_en(static_cast<gpio_num_t>(ABCD_UART_RX_PIN));

    m_stats.active_rx_pin  = ABCD_UART_RX_PIN;
    m_stats.active_tx_pin  = ABCD_UART_TX_PIN;
    m_lastPinCheckMs       = millis();

    // Start in safe listen-only mode (GPIO 21 DE/RE locked LOW)
    pinMode(ABCD_UART_DERE_PIN, OUTPUT);
    digitalWrite(ABCD_UART_DERE_PIN, LOW);

    if (m_stats.sim_bus_enabled) {
        injectSimulatedCycle();
    }
}

void AbcdBusDriver::setUartPins(int8_t rxPin, int8_t txPin) {
    Serial1.end();
    pinMode(rxPin, INPUT_PULLUP);
    Serial1.setRxBufferSize(1024);
    Serial1.begin(ABCD_BAUD_RATE, SERIAL_8N1, rxPin, txPin);
    gpio_pullup_en(static_cast<gpio_num_t>(rxPin));
    while (Serial1.available() > 0) {
        Serial1.read();
    }
    m_rxLen = 0;
    m_stats.active_rx_pin = rxPin;
    m_stats.active_tx_pin = txPin;
    m_stats.physical_rx_bytes = 0;
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

void AbcdBusDriver::updateSimulatedFurnaceStage() {
    if (!m_stats.sim_bus_enabled) {
        return;
    }
    if (m_telemetry.ion_fan_mode == AbcdFanMode::LOW_SPEED) {
        m_telemetry.furnace_stage       = FurnaceStage::BLOWER_ONLY;
        m_telemetry.furnace_airflow_cfm = 650;
        m_telemetry.blower_rpm          = 620;
        m_telemetry.supply_air_temp_f   = 104.0f;
    } else if (m_telemetry.ion_fan_mode == AbcdFanMode::MED_SPEED) {
        m_telemetry.furnace_stage       = FurnaceStage::BLOWER_ONLY;
        m_telemetry.furnace_airflow_cfm = 1050;
        m_telemetry.blower_rpm          = 890;
        m_telemetry.supply_air_temp_f   = 108.0f;
    } else if (m_telemetry.ion_fan_mode == AbcdFanMode::HIGH_SPEED) {
        m_telemetry.furnace_stage       = FurnaceStage::BLOWER_ONLY;
        m_telemetry.furnace_airflow_cfm = 1400;
        m_telemetry.blower_rpm          = 1140;
        m_telemetry.supply_air_temp_f   = 112.0f;
    } else {
        // Fan is AUTO: simulate normal Ion Thermostat gas heat call with +0.5F hysteresis
        const bool wasGasFiring = (m_telemetry.furnace_stage == FurnaceStage::STAGE1_LOW_39K ||
                                   m_telemetry.furnace_stage == FurnaceStage::STAGE2_HIGH_60K);
        const bool shouldFireGas = wasGasFiring
            ? (m_telemetry.room_temp_f < (m_telemetry.ion_heat_setpoint_f + 0.5f))
            : (m_telemetry.room_temp_f < m_telemetry.ion_heat_setpoint_f);

        if (shouldFireGas) {
            m_telemetry.furnace_stage       = FurnaceStage::STAGE1_LOW_39K;
            m_telemetry.furnace_airflow_cfm = 950;
            m_telemetry.blower_rpm          = 820;
            m_telemetry.supply_air_temp_f   = 122.0f;
        } else {
            m_telemetry.furnace_stage       = FurnaceStage::OFF;
            m_telemetry.furnace_airflow_cfm = 0;
            m_telemetry.blower_rpm          = 0;
            m_telemetry.supply_air_temp_f   = 72.0f;
        }
    }
}

void AbcdBusDriver::setSimulatedRoomTempF(float tempF) {
    m_telemetry.room_temp_f = tempF;
    updateSimulatedFurnaceStage();
}

void AbcdBusDriver::setSimulatedIonSetpointF(float setpointF) {
    m_telemetry.ion_heat_setpoint_f = setpointF;
    updateSimulatedFurnaceStage();
}

uint32_t AbcdBusDriver::getBusIdleMs() const {
    const uint32_t now = millis();
    if (now >= m_stats.last_activity_ms) {
        return now - m_stats.last_activity_ms;
    }
    return 0;
}

AbcdFanMode AbcdBusDriver::getLastBusCmdFanMode() const {
    return cfmToFanMode(m_lastBusCmdCfm);
}

bool AbcdBusDriver::isUrgentCounterCmdReady() const {
    if (!m_pendingCounterCmd) {
        return false;
    }
    const bool ackOrTimeout = m_counterCmdAckSeen || ((millis() - m_pendingCounterCmdMs) >= 18UL);
    const bool busQuiet     = (micros() - m_lastByteRxUs) >= 6000UL;
    return ackOrTimeout && busQuiet;
}

bool AbcdBusDriver::inActiveOverrideHoldoff() const {
    return (m_lastActiveOverrideTxMs != 0) && ((millis() - m_lastActiveOverrideTxMs) < 1500UL);
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

    // 4. Periodically poll actual Ion Thermostat Zone 1 Heat Setpoint (00:49:03)
    if (!m_stats.sim_bus_enabled && m_stats.tx_enabled && !m_pendingCounterCmd) {
        const uint32_t now = millis();
        const bool dueInitial  = !m_telemetry.ion_heat_sp_valid && (m_lastSetpointPollMs == 0) && (now >= 3000UL);
        const bool duePeriodic = (m_lastSetpointPollMs != 0) && ((now - m_lastSetpointPollMs) >= 15000UL);
        if ((dueInitial || duePeriodic) && ((micros() - m_lastByteRxUs) >= 40000UL)) {
            m_lastSetpointPollMs = now;
            sendReadRequest(ADDR_THERMOSTAT, ADDR_CONTROLLER, 0x00, 0x49, 0x03);
        }
    }
}

void AbcdBusDriver::processRxByte(uint8_t b) {
    m_lastByteRxUs = micros();
    m_stats.last_activity_ms = millis();
    if (m_stats.physical_rx_bytes == 0 && m_stats.sim_bus_enabled) {
        m_stats.sim_bus_enabled = false;
        clearCaptureBuffer();
    }
    m_stats.physical_rx_bytes++;
    if (m_rxLen < ABCD_MAX_FRAME_BYTES) {
        m_rxBuf[m_rxLen++] = b;
    } else {
        finalizeRxFrame(false, false);
        m_rxBuf[0] = b;
        m_rxLen = 1;
    }

    if (m_rxLen >= 10) {
        const size_t expectedLen = static_cast<size_t>(m_rxBuf[4]) + 10;
        if (expectedLen >= 10 && expectedLen <= ABCD_MAX_FRAME_BYTES && m_rxLen == expectedLen) {
            finalizeRxFrame(false, false);
            return;
        }
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

static void extractAsciiField(const uint8_t* src, size_t maxLen, char* dst, size_t dstSize) {
    if (dstSize == 0) return;
    size_t outIdx = 0;
    for (size_t i = 0; i < maxLen && (outIdx + 1) < dstSize; ++i) {
        const uint8_t c = src[i];
        if (c == 0) break;
        if (c >= 0x20 && c <= 0x7E) {
            dst[outIdx++] = static_cast<char>(c);
        }
    }
    while (outIdx > 0 && dst[outIdx - 1] == ' ') {
        outIdx--;
    }
    dst[outIdx] = '\0';
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

    if (len >= 13) {
        rec.table_id[0] = buf[8];
        rec.table_id[1] = buf[9];
        rec.table_id[2] = buf[10];
    } else {
        rec.table_id[0] = 0;
        rec.table_id[1] = 0;
        rec.table_id[2] = 0;
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

    if (len < 13) {
        const uint8_t statusByte = (len >= 11) ? buf[8] : 0;
        if (rec.func == FUNC_ACK_RESPONSE) {
            if (rec.crc_ok && !isTx && m_pendingCounterCmd &&
                rec.src_addr == ADDR_FURNACE_HEIL && rec.dest_addr == ADDR_THERMOSTAT) {
                m_counterCmdAckSeen = true;
            }
            snprintf(rec.summary, sizeof(rec.summary), "ACK (Status 0x%02X)", (unsigned)statusByte);
        } else if (rec.func == FUNC_NAK) {
            snprintf(rec.summary, sizeof(rec.summary), "NAK (Error 0x%02X)", (unsigned)statusByte);
        } else {
            snprintf(rec.summary, sizeof(rec.summary), "%s (Status 0x%02X)", funcToName(rec.func), (unsigned)statusByte);
        }
        appendRecordToRing(rec);
        return;
    }

    // Default summary for table frames before specific table decoding
    snprintf(rec.summary, sizeof(rec.summary), "%s %02X:%02X:%02X (%uB payload)",
             funcToName(rec.func), rec.table_id[0], rec.table_id[1], rec.table_id[2],
             (unsigned)(len - 13));

    if (rec.crc_ok && len >= 13) {
        const uint8_t* payload = &buf[11];
        const size_t payloadLen = len - 13;

        // Table 00:01:04 -> Device Identity (Model, Control Board P/N, Serial Number)
        if (rec.table_id[0] == 0x00 && rec.table_id[1] == 0x01 && rec.table_id[2] == 0x04) {
            if (rec.func == FUNC_READ) {
                snprintf(rec.summary, sizeof(rec.summary), "Poll Device Identity (00:01:04)");
            } else if (rec.func == FUNC_ACK_RESPONSE && payloadLen >= 95) {
                char board[20]  = {0};
                char model[20]  = {0};
                char serial[16] = {0};
                extractAsciiField(payload + 48, 14, board, sizeof(board));
                extractAsciiField(payload + 62, 16, model, sizeof(model));
                extractAsciiField(payload + 82, 12, serial, sizeof(serial));
                snprintf(rec.summary, sizeof(rec.summary),
                         "Device ID: Heil %s (Bd %s, S/N %s)",
                         model[0] ? model : "G96CTN0601714A1",
                         board[0] ? board : "CESR131672-04",
                         serial[0] ? serial : "4721M103295");
            }
        }
        // Table 00:02:02 -> Broadcast Time (0x2001 -> 0xF1F1)
        else if (rec.table_id[0] == 0x00 && rec.table_id[1] == 0x02 && rec.table_id[2] == 0x02) {
            if (payloadLen >= 3) {
                snprintf(rec.summary, sizeof(rec.summary),
                         "Broadcast Time: %02u:%02u (DoW %u)",
                         (unsigned)payload[0], (unsigned)payload[1], (unsigned)payload[2]);
            }
        }
        // Table 00:02:03 -> Broadcast Date (0x2001 -> 0xF1F1)
        else if (rec.table_id[0] == 0x00 && rec.table_id[1] == 0x02 && rec.table_id[2] == 0x03) {
            if (payloadLen >= 3) {
                snprintf(rec.summary, sizeof(rec.summary),
                         "Broadcast Date: %02u/%02u/20%02u",
                         (unsigned)payload[0], (unsigned)payload[1], (unsigned)payload[2]);
            }
        }
        // Table 00:03:02 -> Furnace Configuration / Profile
        else if (rec.table_id[0] == 0x00 && rec.table_id[1] == 0x03 && rec.table_id[2] == 0x02) {
            if (rec.func == FUNC_READ) {
                snprintf(rec.summary, sizeof(rec.summary), "Poll Furnace Config (00:03:02)");
            } else if (rec.func == FUNC_ACK_RESPONSE && payloadLen >= 4) {
                snprintf(rec.summary, sizeof(rec.summary),
                         "Furnace Config (00:03:02): Mode=0x%02X Cap=0x%02X Flags=0x%02X%02X",
                         (unsigned)payload[0], (unsigned)payload[1],
                         (unsigned)payload[2], (unsigned)payload[3]);
            }
        }
        // Table 00:03:04 -> Furnace Timers / Status
        else if (rec.table_id[0] == 0x00 && rec.table_id[1] == 0x03 && rec.table_id[2] == 0x04) {
            if (rec.func == FUNC_READ) {
                snprintf(rec.summary, sizeof(rec.summary), "Poll Furnace Timers (00:03:04)");
            } else if (rec.func == FUNC_ACK_RESPONSE && payloadLen >= 8) {
                snprintf(rec.summary, sizeof(rec.summary),
                         "Furnace Timers (00:03:04): %02X%02X %02X%02X %02X%02X %02X%02X",
                         payload[0], payload[1], payload[2], payload[3],
                         payload[4], payload[5], payload[6], payload[7]);
            }
        }
        // Table 00:03:05 -> Master Thermostat-to-Furnace Heat Stage & Blower CFM Command
        else if (rec.table_id[0] == 0x00 && rec.table_id[1] == 0x03 && rec.table_id[2] == 0x05) {
            if (rec.func == FUNC_WRITE && payloadLen >= 12) {
                const uint8_t  heatStageCmd = payload[0];
                const uint16_t cmdCfm       = (static_cast<uint16_t>(payload[4]) << 8) | payload[5];
                m_lastBusCmdCfm = cmdCfm;
                if (!isTx) {
                    m_ionHeatStageCmd     = heatStageCmd;
                    m_pendingCounterCmd   = true;
                    m_counterCmdAckSeen   = false;
                    m_pendingCounterCmdMs = rec.timestamp_ms;
                } else {
                    m_pendingCounterCmd   = false;
                    m_counterCmdAckSeen   = false;
                }
                const AbcdFanMode cmdFanMode = cfmToFanMode(cmdCfm);
                if (m_stats.sim_bus_enabled) {
                    m_telemetry.ion_fan_mode        = cmdFanMode;
                    m_telemetry.furnace_airflow_cfm = cmdCfm;
                    if (heatStageCmd == 0x02) {
                        m_telemetry.furnace_stage = FurnaceStage::STAGE1_LOW_39K;
                    } else if (heatStageCmd > 0x02) {
                        m_telemetry.furnace_stage = FurnaceStage::STAGE2_HIGH_60K;
                    } else if (cmdCfm > 0) {
                        m_telemetry.furnace_stage = FurnaceStage::BLOWER_ONLY;
                    } else if (m_telemetry.furnace_airflow_cfm == 0) {
                        m_telemetry.furnace_stage = FurnaceStage::OFF;
                    }
                    m_telemetry.last_update_ms = rec.timestamp_ms;
                }
                const char* heatStr = (heatStageCmd == 0) ? "Heat=OFF" :
                                      (heatStageCmd == 0x02 ? "Heat=STG1(39k)" : "Heat=STG2(60k)");
                snprintf(rec.summary, sizeof(rec.summary),
                         "%s (00:03:05): %s, Fan=%u CFM (%s)",
                         isTx ? "ESP32 Co-Ctrl Cmd" : "Ion Master Cmd",
                         heatStr, (unsigned)cmdCfm, fanModeToName(cmdFanMode));
            }
        }
        // Table 00:03:06 -> Heil Furnace Airflow, Torque/Flow, CFM Limits & Static Pressure
        else if (rec.table_id[0] == 0x00 && rec.table_id[1] == 0x03 && rec.table_id[2] == 0x06) {
            if (rec.func == FUNC_READ) {
                snprintf(rec.summary, sizeof(rec.summary), "Poll Furnace Airflow & Limits (00:03:06)");
            } else if (rec.func == FUNC_ACK_RESPONSE && payloadLen >= 10) {
                const uint16_t outFlow      = (static_cast<uint16_t>(payload[1]) << 8) | payload[2];
                const uint16_t targetCfm    = (static_cast<uint16_t>(payload[3]) << 8) | payload[4];
                const uint8_t  spHundredths = payload[9];
                m_telemetry.static_press_hundredths = spHundredths;
                if (!(targetCfm == 0 && inActiveOverrideHoldoff())) {
                    m_telemetry.furnace_airflow_cfm = targetCfm;
                    m_telemetry.ion_fan_mode        = cfmToFanMode(targetCfm);
                    if (targetCfm == 0 && outFlow == 0) {
                        m_telemetry.blower_rpm = 0;
                        if (m_telemetry.furnace_stage != FurnaceStage::STAGE1_LOW_39K &&
                            m_telemetry.furnace_stage != FurnaceStage::STAGE2_HIGH_60K) {
                            m_telemetry.furnace_stage = FurnaceStage::OFF;
                        }
                    } else if (targetCfm > 0 &&
                               m_telemetry.furnace_stage != FurnaceStage::STAGE1_LOW_39K &&
                               m_telemetry.furnace_stage != FurnaceStage::STAGE2_HIGH_60K) {
                        m_telemetry.furnace_stage = FurnaceStage::BLOWER_ONLY;
                    }
                }
                m_telemetry.last_update_ms = rec.timestamp_ms;
                snprintf(rec.summary, sizeof(rec.summary),
                         "Heil Airflow (00:03:06): Cmd=%u CFM, Torque=%u, SP=0.%02u in.wc",
                         (unsigned)targetCfm, (unsigned)outFlow, (unsigned)spHundredths);
            }
        }
        // Table 00:03:07 -> Ion Heat Demand / Stage Command
        else if (rec.table_id[0] == 0x00 && rec.table_id[1] == 0x03 && rec.table_id[2] == 0x07) {
            if (rec.func == FUNC_WRITE && payloadLen >= 4) {
                const uint8_t heatDemand = payload[1];
                snprintf(rec.summary, sizeof(rec.summary),
                         "Ion Heat Demand Cmd (00:03:07): Demand=%u (%02X %02X %02X %02X)",
                         (unsigned)heatDemand, payload[0], payload[1], payload[2], payload[3]);
            }
        }
        // Table 00:03:0A -> Furnace Operating Limits / High Limit
        else if (rec.table_id[0] == 0x00 && rec.table_id[1] == 0x03 && rec.table_id[2] == 0x0A) {
            if (rec.func == FUNC_READ) {
                snprintf(rec.summary, sizeof(rec.summary), "Poll Furnace Limits (00:03:0A)");
            } else if (rec.func == FUNC_ACK_RESPONSE && payloadLen >= 8) {
                snprintf(rec.summary, sizeof(rec.summary),
                         "Heil Limits (00:03:0A): MaxTemp=%uF, Rise=%u..%uF",
                         (unsigned)payload[1], (unsigned)payload[5], (unsigned)payload[7]);
            }
        }
        // Table 00:03:0D -> Active Fault / Status Codes
        else if (rec.table_id[0] == 0x00 && rec.table_id[1] == 0x03 && rec.table_id[2] == 0x0D) {
            if (rec.func == FUNC_READ) {
                snprintf(rec.summary, sizeof(rec.summary), "Poll Active Faults (00:03:0D)");
            } else if (rec.func == FUNC_ACK_RESPONSE && payloadLen >= 4) {
                snprintf(rec.summary, sizeof(rec.summary),
                         "Heil Fault Status (00:03:0D): Code=%02X (%02X %02X %02X)",
                         payload[0], payload[1], payload[2], payload[3]);
            }
        }
        // Table 00:03:16 -> Heil ECM Blower Motor Telemetry (Heat Stage, Target CFM & Live RPM)
        else if (rec.table_id[0] == 0x00 && rec.table_id[1] == 0x03 && rec.table_id[2] == 0x16) {
            if (rec.func == FUNC_READ) {
                snprintf(rec.summary, sizeof(rec.summary), "Poll ECM Blower RPM/CFM (00:03:16)");
            } else if (rec.func == FUNC_ACK_RESPONSE && payloadLen >= 8) {
                const uint8_t  activeStg = payload[0];
                const uint16_t targetCfm = (static_cast<uint16_t>(payload[4]) << 8) | payload[5];
                uint16_t       blowerRpm = (static_cast<uint16_t>(payload[6]) << 8) | payload[7];
                if (targetCfm == 0 && (m_telemetry.blower_watts < 1.0f || (payload[6] == 0x02 && payload[7] == 0x00))) {
                    blowerRpm = 0;
                }
                if (!(targetCfm == 0 && inActiveOverrideHoldoff())) {
                    m_telemetry.furnace_airflow_cfm = targetCfm;
                    m_telemetry.blower_rpm          = blowerRpm;
                    m_telemetry.ion_fan_mode        = cfmToFanMode(targetCfm);
                    if (activeStg == 0x02) {
                        m_telemetry.furnace_stage = FurnaceStage::STAGE1_LOW_39K;
                    } else if (activeStg > 0x02) {
                        m_telemetry.furnace_stage = FurnaceStage::STAGE2_HIGH_60K;
                    } else {
                        m_telemetry.furnace_stage = (targetCfm > 0 || blowerRpm > 0) ? FurnaceStage::BLOWER_ONLY : FurnaceStage::OFF;
                    }
                }
                m_telemetry.last_update_ms = rec.timestamp_ms;
                snprintf(rec.summary, sizeof(rec.summary),
                         "Heil ECM Motor (00:03:16): Stg=%u | %u CFM @ %u RPM",
                         (unsigned)activeStg, (unsigned)targetCfm, (unsigned)blowerRpm);
            }
        }
        // Table 00:3B:01 -> Zone 1 Fan Mode & Heat Setpoint Configuration
        else if (rec.table_id[0] == 0x00 && rec.table_id[1] == 0x3B && rec.table_id[2] == 0x01) {
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
                updateSimulatedFurnaceStage();

                snprintf(rec.summary, sizeof(rec.summary),
                         "Zone1 Config %s: Fan=%s, IonHeatSP=%uF",
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
        // Table 00:04:02 -> Heil G96CTN0601714A1 ECM Profile (33B) or Simulated Status (6B)
        else if (rec.table_id[0] == 0x00 && rec.table_id[1] == 0x04 && rec.table_id[2] == 0x02) {
            if (rec.func == FUNC_READ) {
                snprintf(rec.summary, sizeof(rec.summary), "Poll Heil G96CTN Blower/Stage Status (00:04:02)");
            } else if (rec.func == FUNC_ACK_RESPONSE && payloadLen == 6) {
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
            } else if (rec.func == FUNC_ACK_RESPONSE && payloadLen >= 20) {
                const uint16_t pMin = (payloadLen >= 27) ? ((static_cast<uint16_t>(payload[25]) << 8) | payload[26]) : 0;
                const uint16_t pMax = (payloadLen >= 29) ? ((static_cast<uint16_t>(payload[27]) << 8) | payload[28]) : 0;
                snprintf(rec.summary, sizeof(rec.summary),
                         "Heil ECM Profile (00:04:02): %uB curve (Min=%u, Max=%u)",
                         (unsigned)payloadLen, (unsigned)pMin, (unsigned)pMax);
            }
        }
        // Table 00:04:03 -> Ion Fan / Blower Command
        else if (rec.table_id[0] == 0x00 && rec.table_id[1] == 0x04 && rec.table_id[2] == 0x03) {
            if (rec.func == FUNC_WRITE && payloadLen >= 4) {
                snprintf(rec.summary, sizeof(rec.summary),
                         "Ion Blower Cmd (00:04:03): Mode=%02X Fan=%02X Speed=%02X State=%02X",
                         payload[0], payload[1], payload[2], payload[3]);
            }
        }
        // Table 00:04:04 -> Heil Furnace Relay/Ignition State & Airflow Status
        else if (rec.table_id[0] == 0x00 && rec.table_id[1] == 0x04 && rec.table_id[2] == 0x04) {
            if (rec.func == FUNC_READ) {
                snprintf(rec.summary, sizeof(rec.summary), "Poll Heil Telemetry (00:04:04)");
            } else if (rec.func == FUNC_ACK_RESPONSE && payloadLen >= 10) {
                const uint8_t  relayMask = payload[3];
                const uint16_t targetCfm = (static_cast<uint16_t>(payload[6]) << 8) | payload[7];
                const uint16_t outFlow   = (static_cast<uint16_t>(payload[8]) << 8) | payload[9];
                const char*    relayStr  = "IDLE";
                if (relayMask == 0x1F) {
                    relayStr = "FLAME ON";
                } else if (relayMask == 0x1E) {
                    relayStr = "INDUCER+HSI";
                } else if (relayMask == 0x0E) {
                    relayStr = "POST-PURGE";
                } else if (relayMask != 0x06) {
                    relayStr = "ACTIVE";
                }
                snprintf(m_telemetry.relay_status, sizeof(m_telemetry.relay_status),
                         "%s (0x%02X)", relayStr, (unsigned)relayMask);
                if (!(targetCfm == 0 && inActiveOverrideHoldoff())) {
                    m_telemetry.furnace_airflow_cfm = targetCfm;
                    m_telemetry.ion_fan_mode        = cfmToFanMode(targetCfm);
                    if (targetCfm == 0 && outFlow == 0) {
                        m_telemetry.blower_rpm = 0;
                        if (m_telemetry.furnace_stage != FurnaceStage::STAGE1_LOW_39K &&
                            m_telemetry.furnace_stage != FurnaceStage::STAGE2_HIGH_60K) {
                            m_telemetry.furnace_stage = FurnaceStage::OFF;
                        }
                    } else if (targetCfm > 0 &&
                               m_telemetry.furnace_stage != FurnaceStage::STAGE1_LOW_39K &&
                               m_telemetry.furnace_stage != FurnaceStage::STAGE2_HIGH_60K) {
                        m_telemetry.furnace_stage = FurnaceStage::BLOWER_ONLY;
                    }
                }
                m_telemetry.last_update_ms = rec.timestamp_ms;
                snprintf(rec.summary, sizeof(rec.summary),
                         "Heil Telemetry (00:04:04): %s (0x%02X) | Cmd=%u CFM, Out=%u",
                         relayStr, (unsigned)relayMask, (unsigned)targetCfm, (unsigned)outFlow);
            }
        }
        // Table 00:04:09 -> Ion System Control Write/Read
        else if (rec.table_id[0] == 0x00 && rec.table_id[1] == 0x04 && rec.table_id[2] == 0x09) {
            if (rec.func == FUNC_READ) {
                snprintf(rec.summary, sizeof(rec.summary), "Poll Ion Control (00:04:09)");
            } else if (payloadLen >= 4) {
                snprintf(rec.summary, sizeof(rec.summary),
                         "Ion Control Cmd (00:04:09): %02X %02X %02X %02X",
                         payload[0], payload[1], payload[2], payload[3]);
            }
        }
        // Table 00:04:0D -> Heil Gas Burner / Ignition Status
        else if (rec.table_id[0] == 0x00 && rec.table_id[1] == 0x04 && rec.table_id[2] == 0x0D) {
            if (rec.func == FUNC_READ) {
                snprintf(rec.summary, sizeof(rec.summary), "Poll Heil Burner Status (00:04:0D)");
            } else if (rec.func == FUNC_ACK_RESPONSE && payloadLen >= 6) {
                const uint8_t stg = payload[0];
                if (stg == 1) {
                    m_telemetry.furnace_stage  = FurnaceStage::STAGE1_LOW_39K;
                    m_telemetry.last_update_ms = rec.timestamp_ms;
                } else if (stg >= 2) {
                    m_telemetry.furnace_stage  = FurnaceStage::STAGE2_HIGH_60K;
                    m_telemetry.last_update_ms = rec.timestamp_ms;
                } else if (m_telemetry.furnace_stage == FurnaceStage::STAGE1_LOW_39K ||
                           m_telemetry.furnace_stage == FurnaceStage::STAGE2_HIGH_60K) {
                    m_telemetry.furnace_stage = (m_telemetry.furnace_airflow_cfm > 0 || m_telemetry.blower_rpm > 0)
                                                ? FurnaceStage::BLOWER_ONLY : FurnaceStage::OFF;
                    m_telemetry.last_update_ms = rec.timestamp_ms;
                }
                snprintf(rec.summary, sizeof(rec.summary),
                         "Heil Burner Status (00:04:0D): Stage=%u (%02X %02X %02X %02X %02X %02X)",
                         (unsigned)stg, payload[0], payload[1], payload[2], payload[3], payload[4], payload[5]);
            }
        }
        // Table 00:04:13 -> Heil ECM Motor Power (IEEE-754 Watts) & Runtime
        else if (rec.table_id[0] == 0x00 && rec.table_id[1] == 0x04 && rec.table_id[2] == 0x13) {
            if (rec.func == FUNC_READ) {
                snprintf(rec.summary, sizeof(rec.summary), "Poll Heil ECM Power (00:04:13)");
            } else if (rec.func == FUNC_ACK_RESPONSE && payloadLen >= 12) {
                const uint32_t rawFloat = (static_cast<uint32_t>(payload[8])  << 24) |
                                          (static_cast<uint32_t>(payload[9])  << 16) |
                                          (static_cast<uint32_t>(payload[10]) << 8)  |
                                           static_cast<uint32_t>(payload[11]);
                float watts = 0.0f;
                memcpy(&watts, &rawFloat, sizeof(watts));
                if (watts >= 0.0f && watts <= 2000.0f) {
                    m_telemetry.blower_watts   = watts;
                    m_telemetry.last_update_ms = rec.timestamp_ms;
                }
                snprintf(rec.summary, sizeof(rec.summary),
                         "Heil ECM Power (00:04:13): %.1f Watts",
                         watts);
            }
        }
        // Table 00:04:14 -> Ion Sensed Room Temperature Broadcast (1/16ths °F)
        else if (rec.table_id[0] == 0x00 && rec.table_id[1] == 0x04 && rec.table_id[2] == 0x14) {
            if (rec.func == FUNC_WRITE && payloadLen >= 6) {
                const uint16_t rawRoom16 = (static_cast<uint16_t>(payload[1]) << 8) | payload[2];
                const float    roomF     = rawRoom16 / 16.0f;
                if (roomF >= 35.0f && roomF <= 105.0f) {
                    m_telemetry.room_temp_f     = roomF;
                    m_telemetry.room_temp_valid = true;
                    m_telemetry.last_update_ms  = rec.timestamp_ms;
                    saveRoomTempToNvs(roomF);
                }
                snprintf(rec.summary, sizeof(rec.summary),
                         "Ion Room Temp Broadcast (00:04:14): %.2fF (0x%04X)",
                         roomF, (unsigned)rawRoom16);
            }
        }
        // Table 00:04:1B -> Heil Target Airflow / Param
        else if (rec.table_id[0] == 0x00 && rec.table_id[1] == 0x04 && rec.table_id[2] == 0x1B) {
            if (rec.func == FUNC_READ) {
                snprintf(rec.summary, sizeof(rec.summary), "Poll Heil Target Param (00:04:1B)");
            } else if (rec.func == FUNC_ACK_RESPONSE && payloadLen >= 2) {
                const uint16_t val = (static_cast<uint16_t>(payload[0]) << 8) | payload[1];
                snprintf(rec.summary, sizeof(rec.summary),
                         "Heil Target Param (00:04:1B): %u (0x%04X)",
                         (unsigned)val, (unsigned)val);
            }
        }
        // Table 00:49:03 -> Ion Thermostat SYSCTRL Setpoints (Zone 1 Heat SP = float32 BE bytes 0..3)
        else if (rec.table_id[0] == 0x00 && rec.table_id[1] == 0x49 && rec.table_id[2] == 0x03) {
            if (rec.func == FUNC_READ) {
                snprintf(rec.summary, sizeof(rec.summary), "Poll Ion SYSCTRL Setpoints (00:49:03)");
            } else if (rec.func == FUNC_ACK_RESPONSE && rec.src_addr == ADDR_THERMOSTAT && payloadLen >= 4) {
                const uint32_t rawFloat = (static_cast<uint32_t>(payload[0]) << 24) |
                                          (static_cast<uint32_t>(payload[1]) << 16) |
                                          (static_cast<uint32_t>(payload[2]) << 8)  |
                                           static_cast<uint32_t>(payload[3]);
                float sp = 0.0f;
                memcpy(&sp, &rawFloat, sizeof(sp));
                if (sp >= 40.0f && sp <= 95.0f) {
                    m_telemetry.ion_heat_setpoint_f = sp;
                    m_telemetry.ion_heat_sp_valid   = true;
                    m_telemetry.last_update_ms      = rec.timestamp_ms;
                }
                snprintf(rec.summary, sizeof(rec.summary),
                         "Ion SYSCTRL Setpoints (00:49:03): Heat=%.1fF", sp);
            }
        }
    }

    appendRecordToRing(rec);
}

bool AbcdBusDriver::transmitRawFrame(const uint8_t* frame, size_t len, uint32_t minSilenceUs) {
    if (!m_stats.tx_enabled) {
        return false;
    }

    // Wait for >= minSilenceUs of true physical bus silence since the last received byte
    // while continuing to pump incoming bytes so we never transmit mid-burst
    const uint32_t waitStart = millis();
    while ((micros() - m_lastByteRxUs) < minSilenceUs) {
        while (Serial1.available() > 0) {
            processRxByte(static_cast<uint8_t>(Serial1.read()));
        }
        if (millis() - waitStart > 150UL) {
            break;
        }
        delayMicroseconds(250);
    }

    digitalWrite(ABCD_UART_DERE_PIN, HIGH);
    delayMicroseconds(150);
    Serial1.write(frame, len);
    Serial1.flush();
    while (Serial1.available() > 0) {
        Serial1.read();
    }
    m_rxLen = 0;
    digitalWrite(ABCD_UART_DERE_PIN, LOW);

    return true;
}

bool AbcdBusDriver::sendCoControllerCommand(AbcdFanMode fanMode, const char* reasonNote) {
    m_telemetry.last_update_ms = millis();

    // Safety Interlock: Never override an active Stage 1 or Stage 2 Gas Heat call from the Ion Thermostat!
    if (m_ionHeatStageCmd != 0) {
        Serial.printf("[ABCD CO-CTRL] Skipping blower override: Ion Thermostat is calling for Gas Heat (StageCmd=0x%02X)\n",
                      (unsigned)m_ionHeatStageCmd);
        return false;
    }

    // Build an authentic 25-byte ABCD WRITE 00:03:05 frame to ADDR_FURNACE_HEIL (0x4401)
    // Payload (12 bytes): 00 00 00 00 [CFM_HI] [CFM_LO] 00 00 00 00 00 78
    uint16_t cmdCfm = 0;
    switch (fanMode) {
        case AbcdFanMode::AUTO:       cmdCfm = 0;    break;
        case AbcdFanMode::LOW_SPEED:  cmdCfm = 413;  break; // 0x019D
        case AbcdFanMode::MED_SPEED:  cmdCfm = 667;  break; // 0x029B
        case AbcdFanMode::HIGH_SPEED: cmdCfm = 1050; break; // 0x041A
    }

    uint8_t frame[25];

    frame[0]  = static_cast<uint8_t>((ADDR_FURNACE_HEIL >> 8) & 0xFF);
    frame[1]  = static_cast<uint8_t>(ADDR_FURNACE_HEIL & 0xFF);
    frame[2]  = static_cast<uint8_t>((ADDR_THERMOSTAT >> 8) & 0xFF);
    frame[3]  = static_cast<uint8_t>(ADDR_THERMOSTAT & 0xFF);
    frame[4]  = 0x0F; // 3-byte table ID (00:03:05) + 12-byte payload = 15 bytes
    frame[5]  = 0x00;
    frame[6]  = 0x00;
    frame[7]  = FUNC_WRITE;
    frame[8]  = 0x00;
    frame[9]  = 0x03;
    frame[10] = 0x05;
    // 12-byte payload: 00 00 00 00 [CFM_HI] [CFM_LO] 00 00 00 00 00 78
    frame[11] = 0x00; // Heat Stage Command = 0 (Heat OFF, Blower Only)
    frame[12] = 0x00;
    frame[13] = 0x00;
    frame[14] = 0x00;
    frame[15] = static_cast<uint8_t>((cmdCfm >> 8) & 0xFF);
    frame[16] = static_cast<uint8_t>(cmdCfm & 0xFF);
    frame[17] = 0x00;
    frame[18] = 0x00;
    frame[19] = 0x00;
    frame[20] = 0x00;
    frame[21] = 0x00;
    frame[22] = 0x78;

    const uint16_t crc = computeCrc16(frame, 23);
    frame[23] = static_cast<uint8_t>(crc & 0xFF);
    frame[24] = static_cast<uint8_t>((crc >> 8) & 0xFF);

    m_pendingCounterCmd = false;
    m_counterCmdAckSeen = false;
    if (fanMode != AbcdFanMode::AUTO) {
        m_lastActiveOverrideTxMs = millis();
    } else {
        m_lastActiveOverrideTxMs = 0;
    }

    const bool physicalSent = transmitRawFrame(frame, 25, 6000UL);
    decodeAndRecordFrame(frame, 25, true, !physicalSent);

    Serial.printf("[ABCD CO-CTRL TX] %s -> 00:03:05 CmdCFM=%u (%s) (%s)\n",
                   physicalSent ? "RS485-TX" : "SIM-TX",
                   (unsigned)cmdCfm,
                   fanModeToName(fanMode),
                   reasonNote ? reasonNote : "");
    return true;
}

bool AbcdBusDriver::sendReadRequest(uint16_t destAddr, uint16_t srcAddr, uint8_t t0, uint8_t t1, uint8_t t2) {
    uint8_t frame[13];
    frame[0]  = static_cast<uint8_t>((destAddr >> 8) & 0xFF);
    frame[1]  = static_cast<uint8_t>(destAddr & 0xFF);
    frame[2]  = static_cast<uint8_t>((srcAddr >> 8) & 0xFF);
    frame[3]  = static_cast<uint8_t>(srcAddr & 0xFF);
    frame[4]  = 0x03; // 3-byte table ID
    frame[5]  = 0x00;
    frame[6]  = 0x00;
    frame[7]  = FUNC_READ;
    frame[8]  = t0;
    frame[9]  = t1;
    frame[10] = t2;

    const uint16_t crc = computeCrc16(frame, 11);
    frame[11] = static_cast<uint8_t>(crc & 0xFF);
    frame[12] = static_cast<uint8_t>((crc >> 8) & 0xFF);

    const bool prevTx = m_stats.tx_enabled;
    m_stats.tx_enabled = true;
    const bool physicalSent = transmitRawFrame(frame, 13);
    m_stats.tx_enabled = prevTx;

    decodeAndRecordFrame(frame, 13, true, !physicalSent);
    return physicalSent;
}

bool AbcdBusDriver::sendWriteRequest(uint16_t destAddr, uint16_t srcAddr, uint8_t t0, uint8_t t1, uint8_t t2,
                                     const uint8_t* payload, size_t payloadLen) {
    if (payloadLen > 0 && payload == nullptr) {
        Serial.println("[ABCD PROBE WRITE] Rejected: null payload with non-zero length");
        return false;
    }

    const size_t totalLen = 13 + payloadLen;
    if (totalLen > ABCD_MAX_FRAME_BYTES || (3 + payloadLen) > 0xFF) {
        Serial.printf("[ABCD PROBE WRITE] Rejected: payloadLen=%u exceeds max frame size (%u)\n",
                      (unsigned)payloadLen, (unsigned)ABCD_MAX_FRAME_BYTES);
        return false;
    }

    uint8_t frame[ABCD_MAX_FRAME_BYTES];
    frame[0]  = static_cast<uint8_t>((destAddr >> 8) & 0xFF);
    frame[1]  = static_cast<uint8_t>(destAddr & 0xFF);
    frame[2]  = static_cast<uint8_t>((srcAddr >> 8) & 0xFF);
    frame[3]  = static_cast<uint8_t>(srcAddr & 0xFF);
    frame[4]  = static_cast<uint8_t>(3 + payloadLen);
    frame[5]  = 0x00;
    frame[6]  = 0x00;
    frame[7]  = FUNC_WRITE;
    frame[8]  = t0;
    frame[9]  = t1;
    frame[10] = t2;

    if (payloadLen > 0) {
        memcpy(&frame[11], payload, payloadLen);
    }

    const uint16_t crc = computeCrc16(frame, 11 + payloadLen);
    frame[11 + payloadLen] = static_cast<uint8_t>(crc & 0xFF);
    frame[12 + payloadLen] = static_cast<uint8_t>((crc >> 8) & 0xFF);

    const bool physicalSent = transmitRawFrame(frame, totalLen);
    decodeAndRecordFrame(frame, totalLen, true, !physicalSent);

    if (!m_stats.tx_enabled) {
        Serial.printf("[ABCD PROBE WRITE] SIM-ONLY (Passive Sniffer mode, TX disabled): 0x%04X->0x%04X %02X:%02X:%02X (%u bytes)\n",
                      (unsigned)srcAddr, (unsigned)destAddr,
                      (unsigned)t0, (unsigned)t1, (unsigned)t2,
                      (unsigned)payloadLen);
    } else {
        Serial.printf("[ABCD PROBE WRITE] %s: 0x%04X->0x%04X %02X:%02X:%02X (%u bytes)\n",
                      physicalSent ? "RS485-TX" : "SIM-TX",
                      (unsigned)srcAddr, (unsigned)destAddr,
                      (unsigned)t0, (unsigned)t1, (unsigned)t2,
                      (unsigned)payloadLen);
    }

    return physicalSent;
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
        updateSimulatedFurnaceStage();
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
        f[16] = static_cast<uint8_t>(m_telemetry.supply_air_temp_f + 0.5f);
        const uint16_t crc = computeCrc16(f, 17);
        f[17] = static_cast<uint8_t>(crc & 0xFF);
        f[18] = static_cast<uint8_t>((crc >> 8) & 0xFF);
        decodeAndRecordFrame(f, 19, false, true);
    }
}
