#include <Arduino.h>
#include <WiFi.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_psram.h>
#include <esp_system.h>

#include "abcd_bus.h"
#include "water_sensor.h"
#include "controller.h"
#include "network_mgr.h"
#include "web_portal.h"

static uint32_t g_tick            = 0;
static uint32_t g_lastHeartbeatMs = 0;
static bool     g_pulseHigh       = false;
static String   g_cliLine;

static void printHardwareBanner() {
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    uint32_t flash_size_bytes = 0;
    esp_flash_get_size(nullptr, &flash_size_bytes);

    Serial.println();
    Serial.println("==============================================================================");
    Serial.println("  HEIL G96CTN0601714A1 & WOOD BOILER CO-CONTROLLER + ABCD BUS ANALYZER");
    Serial.println("  Board: AITRIP ESP32-S3-DevKitC-1-N16R8 (16MB Quad Flash / 8MB Octal PSRAM)");
    Serial.println("==============================================================================");
    Serial.printf("  Chip Model       : %s (Rev v%d.%d), %d Cores @ %lu MHz\n",
                  ESP.getChipModel(), chip_info.revision / 100, chip_info.revision % 100,
                  ESP.getChipCores(), (unsigned long)ESP.getCpuFreqMHz());
    Serial.printf("  Flash / PSRAM    : %lu MB Flash | %.2f MB Octal PSRAM (%lu B free)\n",
                  (unsigned long)(flash_size_bytes / (1024 * 1024)),
                  ESP.getPsramSize() / (1024.0 * 1024.0),
                  (unsigned long)ESP.getFreePsram());
    Serial.printf("  RS-485 ABCD UART1: 38400 8N1 (RX=GPIO %d, TX=GPIO %d, DE/RE=GPIO %d)\n",
                  ABCD_UART_RX_PIN, ABCD_UART_TX_PIN, ABCD_UART_DERE_PIN);
    Serial.printf("  1-Wire Water Bus : GPIO %u (%u DS18B20 probes detected -> %s)\n",
                  (unsigned)DS18B20_ONEWIRE_PIN,
                  (unsigned)g_waterSensor.getStatus().detected_probes,
                  g_waterSensor.getStatus().using_simulation ? "Web Simulation Mode" : "Physical Probes Active");
    Serial.println("==============================================================================");
}

static bool parseHexByteToken(const String& tok, uint8_t& outByte) {
    if (tok.length() == 0) {
        return false;
    }
    char* endPtr = nullptr;
    const unsigned long val = strtoul(tok.c_str(), &endPtr, 16);
    if (endPtr == tok.c_str() || *endPtr != '\0' || val > 0xFFUL) {
        return false;
    }
    outByte = static_cast<uint8_t>(val);
    return true;
}

static size_t splitWhitespaceTokens(const String& s, String* outTokens, size_t maxTokens) {
    size_t count = 0;
    int idx = 0;
    const int len = s.length();
    while (idx < len && count < maxTokens) {
        while (idx < len && isspace(static_cast<unsigned char>(s[idx]))) {
            ++idx;
        }
        if (idx >= len) {
            break;
        }
        int end = idx;
        while (end < len && !isspace(static_cast<unsigned char>(s[end]))) {
            ++end;
        }
        outTokens[count++] = s.substring(idx, end);
        idx = end;
    }
    return count;
}

static void handleSerialCliLine(String cmd) {
    cmd.trim();
    if (cmd.length() == 0) {
        return;
    }

    if (cmd.equalsIgnoreCase("help")) {
        Serial.println("\n--- Furnace Controller Serial CLI ---");
        Serial.println("  status              : Print full boiler, room, furnace & bus summary");
        Serial.println("  wifi                : Print current Wi-Fi STA, Fallback AP & OTA network status");
        Serial.println("  scan                : Scan nearby 2.4 GHz Wi-Fi networks and print JSON results");
        Serial.println("  ap on / ap off      : Manually enable or disable the Setup Access Point (192.168.4.1)");
        Serial.println("  sim boiler <tempF>  : Set simulated Wood Boiler supply water temp (e.g. 'sim boiler 160')");
        Serial.println("  sim room <tempF>    : Set simulated Ion Room temp (e.g. 'sim room 68.0')");
        Serial.println("  mode <0|1|2|3>      : Set mode (0=Passive Sniffer, 1=Auto Wood Co-Ctrl, 2=Manual Fan, 3=Prototyping Sim)");
        Serial.println("  packets             : Print the 10 most recent ABCD RS-485 bus frames");
        Serial.println("  probe read <t0> <t1> <t2>            : Send FUNC_READ to Ion Thermostat (0x2001) as 0x9201");
        Serial.println("  probe write <t0> <t1> <t2> <b0> [..] : Send FUNC_WRITE to Ion Thermostat (0x2001) as 0x9201");
        Serial.println("  probe scan                           : Probe key Ion tables (00:3B:01..03, 00:04:03, 00:04:09)");
        Serial.println("  reboot              : Schedule a clean ESP32-S3 soft reboot");
        Serial.println("-------------------------------------\n");
    } else if (cmd.equalsIgnoreCase("status")) {
        const WaterSensorStatus& ws = g_waterSensor.getStatus();
        const AbcdTelemetry&     bt = g_abcdBus.getTelemetry();
        const AbcdBusStats&      bs = g_abcdBus.getStats();
        const ControllerConfig&  cc = g_controller.getConfig();
        Serial.println("\n--- Live Controller Status ---");
        Serial.printf("  State / Mode     : %s | %s\n", g_controller.getStateName(), g_controller.getOpModeName());
        Serial.printf("  Boiler Water     : Supply %.1f F | Return %.1f F (dT %.1f F) [%s]\n",
                      ws.supply_temp_f, ws.return_temp_f, ws.delta_t_f,
                      ws.using_simulation ? "SIM" : "DS18B20");
        Serial.printf("  Room / Setpoints : Room %.1f F | Target Comfort %.1f F | Ion Setpoint %.1f F\n",
                      bt.room_temp_f, cc.comfort_setpoint_f, bt.ion_heat_setpoint_f);
        Serial.printf("  Heil G96CTN      : Stage=%s | Fan=%s | %u CFM (%u RPM)\n",
                      AbcdBusDriver::stageToName(bt.furnace_stage),
                      AbcdBusDriver::fanModeToName(bt.ion_fan_mode),
                      (unsigned)bt.furnace_airflow_cfm, (unsigned)bt.blower_rpm);
        Serial.printf("  ABCD Bus Stats   : RX=%lu TX=%lu CRC_OK=%lu CRC_ERR=%lu Stored=%u/%u\n\n",
                      (unsigned long)bs.total_rx_frames, (unsigned long)bs.total_tx_frames,
                      (unsigned long)bs.crc_ok_count, (unsigned long)bs.crc_err_count,
                      (unsigned)g_abcdBus.getStoredFrameCount(), (unsigned)ABCD_RING_BUFFER_CAP);
    } else if (cmd.equalsIgnoreCase("wifi")) {
        const NetworkConfig&        nc = g_netMgr.getConfig();
        const NetworkRuntimeStatus  ns = g_netMgr.getStatus();
        Serial.println("\n--- Wi-Fi, Fallback AP & OTA Status ---");
        Serial.printf("  STA Configured   : SSID=\"%s\" | StaticIP=%s\n",
                      nc.sta_ssid, nc.use_static_ip ? nc.static_ip : "DHCP");
        Serial.printf("  STA Connected    : %s | IP=%s | RSSI=%ld dBm | Ch=%u | MAC=%s\n",
                      ns.sta_connected ? "YES" : "NO",
                      ns.sta_ip,
                      (long)ns.sta_rssi_dbm, (unsigned)ns.sta_channel,
                      ns.mac_addr);
        Serial.printf("  mDNS / ArduinoOTA: http://%s.local/ | Port 3232 (%s)\n",
                      nc.mdns_host, ns.ota_ready ? "READY" : "WAITING");
        Serial.printf("  Rescue AP Mode   : %s | SSID=\"%s\" | IP=%s | Clients=%u\n\n",
                      ns.ap_active ? "ACTIVE" : "OFF (Auto-Standby)",
                      nc.ap_ssid,
                      ns.ap_ip,
                      (unsigned)ns.ap_clients);
    } else if (cmd.equalsIgnoreCase("scan")) {
        Serial.println("[CLI] Scanning 2.4 GHz Wi-Fi networks...");
        Serial.println(g_netMgr.scanNetworksJson());
    } else if (cmd.equalsIgnoreCase("ap on")) {
        g_netMgr.setManualApOverride(true);
    } else if (cmd.equalsIgnoreCase("ap off")) {
        g_netMgr.setManualApOverride(false);
    } else if (cmd.equalsIgnoreCase("reboot")) {
        Serial.println("[CLI] Rebooting ESP32-S3...");
        g_netMgr.scheduleReboot(200);
    } else if (cmd.startsWith("sim boiler ")) {
        const float t = cmd.substring(11).toFloat();
        g_waterSensor.setSimulatedTempsF(t, t - 15.0f);
        g_controller.forceEvaluate();
        Serial.printf("[CLI] Simulated Boiler Supply set to %.1f F -> State: %s\n", t, g_controller.getStateName());
    } else if (cmd.startsWith("sim room ")) {
        const float t = cmd.substring(9).toFloat();
        g_abcdBus.setSimulatedRoomTempF(t);
        g_abcdBus.injectSimulatedCycle();
        g_controller.forceEvaluate();
        Serial.printf("[CLI] Simulated Room Temp set to %.1f F -> State: %s\n", t, g_controller.getStateName());
    } else if (cmd.startsWith("mode ")) {
        const int m = cmd.substring(5).toInt();
        if (m >= 0 && m <= 3) {
            ControllerConfig cfg = g_controller.getConfig();
            cfg.op_mode = static_cast<OperatingMode>(m);
            g_controller.updateConfig(cfg, true);
            Serial.printf("[CLI] Operating Mode updated to %d (%s)\n", m, g_controller.getOpModeName());
        }
    } else if (cmd.equalsIgnoreCase("packets")) {
        const size_t count = min(g_abcdBus.getStoredFrameCount(), (size_t)10);
        Serial.printf("\n--- Last %u ABCD RS-485 Frames ---\n", (unsigned)count);
        AbcdFrameRecord rec{};
        for (size_t i = 0; i < count; ++i) {
            if (g_abcdBus.getFrameByOffsetFromNewest(i, rec)) {
                Serial.printf("  #%03lu [%6lu ms] %s 0x%04X->0x%04X %-8s %02X:%02X:%02X | %s (CRC %s)\n",
                              (unsigned long)rec.seq,
                              (unsigned long)rec.timestamp_ms,
                              rec.is_tx ? "TX" : "RX",
                              (unsigned)rec.src_addr,
                              (unsigned)rec.dest_addr,
                              AbcdBusDriver::funcToName(rec.func),
                              rec.table_id[0], rec.table_id[1], rec.table_id[2],
                              rec.summary,
                              rec.crc_ok ? "OK" : "ERR");
            }
        }
        Serial.println();
    } else if (cmd.equalsIgnoreCase("probe scan")) {
        static constexpr uint8_t kScanTables[5][3] = {
            {0x00, 0x3B, 0x01},
            {0x00, 0x3B, 0x02},
            {0x00, 0x3B, 0x03},
            {0x00, 0x04, 0x03},
            {0x00, 0x04, 0x09},
        };
        Serial.printf("[CLI PROBE] Scanning 5 Ion Thermostat (0x%04X) tables from 0x%04X...\n",
                      (unsigned)ADDR_THERMOSTAT, (unsigned)ADDR_CONTROLLER);
        for (size_t i = 0; i < 5; ++i) {
            const uint8_t t0 = kScanTables[i][0];
            const uint8_t t1 = kScanTables[i][1];
            const uint8_t t2 = kScanTables[i][2];
            const bool sent = g_abcdBus.sendReadRequest(ADDR_THERMOSTAT, ADDR_CONTROLLER, t0, t1, t2);
            Serial.printf("  [%u/5] READ %02X:%02X:%02X -> %s\n",
                          (unsigned)(i + 1), (unsigned)t0, (unsigned)t1, (unsigned)t2,
                          sent ? "RS485-TX" : "SIM-ONLY");
            const uint32_t waitStart = millis();
            while (millis() - waitStart < 60UL) {
                g_abcdBus.loop();
                delay(1);
            }
        }
        Serial.println("[CLI PROBE] Scan complete. Run 'packets' to inspect responses.");
    } else if (cmd.startsWith("probe read ")) {
        String tokens[8];
        const size_t n = splitWhitespaceTokens(cmd.substring(11), tokens, 8);
        uint8_t t0 = 0, t1 = 0, t2 = 0;
        if (n != 3 || !parseHexByteToken(tokens[0], t0) ||
            !parseHexByteToken(tokens[1], t1) || !parseHexByteToken(tokens[2], t2)) {
            Serial.println("[CLI PROBE] Usage: probe read <t0> <t1> <t2> (hex bytes, e.g. 'probe read 00 3B 01')");
        } else {
            const bool sent = g_abcdBus.sendReadRequest(ADDR_THERMOSTAT, ADDR_CONTROLLER, t0, t1, t2);
            const uint32_t waitStart = millis();
            while (millis() - waitStart < 60UL) {
                g_abcdBus.loop();
                delay(1);
            }
            Serial.printf("[CLI PROBE] READ 0x%04X->0x%04X %02X:%02X:%02X (%s)\n",
                          (unsigned)ADDR_CONTROLLER, (unsigned)ADDR_THERMOSTAT,
                          (unsigned)t0, (unsigned)t1, (unsigned)t2,
                          sent ? "RS485-TX" : "SIM-ONLY");
        }
    } else if (cmd.startsWith("probe write ")) {
        constexpr size_t kMaxTokens = 3 + (ABCD_MAX_FRAME_BYTES - 13);
        String tokens[kMaxTokens];
        const size_t n = splitWhitespaceTokens(cmd.substring(12), tokens, kMaxTokens);
        uint8_t t0 = 0, t1 = 0, t2 = 0;
        if (n < 4 || !parseHexByteToken(tokens[0], t0) ||
            !parseHexByteToken(tokens[1], t1) || !parseHexByteToken(tokens[2], t2)) {
            Serial.println("[CLI PROBE] Usage: probe write <t0> <t1> <t2> <b0> [b1 ...] (hex bytes)");
        } else {
            uint8_t payload[ABCD_MAX_FRAME_BYTES - 13];
            size_t payloadLen = 0;
            bool parseOk = true;
            for (size_t i = 3; i < n; ++i) {
                if (!parseHexByteToken(tokens[i], payload[payloadLen++])) {
                    parseOk = false;
                    break;
                }
            }
            if (!parseOk) {
                Serial.println("[CLI PROBE] Invalid hex byte in payload.");
            } else {
                const bool sent = g_abcdBus.sendWriteRequest(ADDR_THERMOSTAT, ADDR_CONTROLLER,
                                                             t0, t1, t2, payload, payloadLen);
                const uint32_t waitStart = millis();
                while (millis() - waitStart < 60UL) {
                    g_abcdBus.loop();
                    delay(1);
                }
                if (!g_abcdBus.isTxEnabled()) {
                    Serial.println("[CLI PROBE] Note: TX is disabled (Passive Sniffer mode); frame was simulated-only.");
                } else {
                    Serial.printf("[CLI PROBE] WRITE 0x%04X->0x%04X %02X:%02X:%02X (%u bytes) -> %s\n",
                                  (unsigned)ADDR_CONTROLLER, (unsigned)ADDR_THERMOSTAT,
                                  (unsigned)t0, (unsigned)t1, (unsigned)t2,
                                  (unsigned)payloadLen, sent ? "RS485-TX" : "SIM-ONLY");
                }
            }
        }
    } else {
        Serial.printf("[CLI] Unknown command '%s'. Type 'help'.\n", cmd.c_str());
    }
}

static void pollSerialCli() {
    while (Serial.available() > 0) {
        const char c = static_cast<char>(Serial.read());
        if (c == '\n' || c == '\r') {
            if (g_cliLine.length() > 0) {
                handleSerialCliLine(g_cliLine);
                g_cliLine = "";
            }
        } else if (g_cliLine.length() < 120) {
            g_cliLine += c;
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(400);

    g_waterSensor.begin();
    g_abcdBus.begin();
    g_controller.begin();

    printHardwareBanner();
    g_netMgr.begin();
    g_webPortal.begin();

    Serial.println("  Serial CLI Ready : Type 'help' for interactive bench commands");
    Serial.println("------------------------------------------------------------------------------");
    Serial.println();

    g_lastHeartbeatMs = millis();
}

void loop() {
    g_abcdBus.loop();
    g_waterSensor.loop();
    g_controller.loop();
    g_netMgr.loop();
    g_webPortal.loop();
    pollSerialCli();

    const uint32_t now = millis();

    // Non-blocking 250ms bright pulse on onboard WS2812 status LED
    if (g_pulseHigh && (now - g_lastHeartbeatMs >= 250UL)) {
        g_pulseHigh = false;
        g_controller.applyStatusLed(false);
    }

    // Periodic 2-second telemetry log line
    if (now - g_lastHeartbeatMs >= 2000UL) {
        g_lastHeartbeatMs = now;
        g_pulseHigh       = true;
        g_controller.applyStatusLed(true);

        const WaterSensorStatus& ws = g_waterSensor.getStatus();
        const AbcdTelemetry&     bt = g_abcdBus.getTelemetry();
        const AbcdBusStats&      bs = g_abcdBus.getStats();
        const NetworkRuntimeStatus ns = g_netMgr.getStatus();

        Serial.printf("[#%04lu | %5lus] %-28s | Boiler: %5.1fF | Room: %4.1fF (IonSP:%2.0fF) | Fan: %-15s | Bus RX:%lu TX:%lu | IP: %s%s\n",
                      (unsigned long)g_tick,
                      now / 1000UL,
                      g_controller.getStateName(),
                      ws.supply_temp_f,
                      bt.room_temp_f,
                      bt.ion_heat_setpoint_f,
                      AbcdBusDriver::fanModeToName(bt.ion_fan_mode),
                      (unsigned long)bs.total_rx_frames,
                      (unsigned long)bs.total_tx_frames,
                      ns.sta_connected ? ns.sta_ip : "DISCONNECTED",
                      ns.ap_active ? " [AP:192.168.4.1]" : "");

        g_tick++;
    }

    delay(2);
}
