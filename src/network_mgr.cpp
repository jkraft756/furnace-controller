#include "network_mgr.h"
#include "controller.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_system.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define MDNS_HOSTNAME "furnace-controller"
#endif

static constexpr uint8_t  BOOT_BUTTON_PIN       = 0;
static constexpr uint16_t CAPTIVE_DNS_PORT      = 53;
static constexpr uint32_t STA_BOOT_TIMEOUT_MS   = 15000UL;
static constexpr uint32_t STA_LOST_FALLBACK_MS  = 15000UL;
static constexpr uint32_t AP_STA_RETRY_INTERVAL = 60000UL;
static constexpr uint32_t AP_SHUTDOWN_STABLE_MS = 30000UL;

static DNSServer s_dnsServer;
FurnaceNetworkManager   g_netMgr;

static void safeCopyStr(char* dst, size_t dstSize, const char* src) {
    if (!dst || dstSize == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dstSize - 1);
    dst[dstSize - 1] = '\0';
}

const char* FurnaceNetworkManager::getResetReasonString() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "Power-On Reset";
        case ESP_RST_SW:        return "Software Reboot (Web/OTA)";
        case ESP_RST_PANIC:     return "Exception / Panic Reset";
        case ESP_RST_INT_WDT:   return "Interrupt Watchdog";
        case ESP_RST_TASK_WDT:  return "Task Watchdog";
        case ESP_RST_WDT:       return "Other Watchdog";
        case ESP_RST_BROWNOUT:  return "24VAC/5V Brownout Reset";
        case ESP_RST_USB:       return "USB UART Reset";
        default:                return "Hardware Reset";
    }
}

const char* FurnaceNetworkManager::getRunningPartitionLabel() {
    const esp_partition_t* part = esp_ota_get_running_partition();
    return (part && part->label[0]) ? part->label : "app0";
}

void FurnaceNetworkManager::loadFromNvs() {
    Preferences prefs;
    prefs.begin("netcfg", true);

    String sSsid = prefs.getString("sta_ssid", WIFI_SSID);
    String sPass = prefs.getString("sta_pass", WIFI_PASSWORD);
    String mHost = prefs.getString("mdns_host", MDNS_HOSTNAME);

    // If NVS had an old placeholder or empty SSID while secrets.h has a real SSID, seed from secrets.h
    if ((sSsid.length() == 0 || sSsid == "YOUR_WIFI_SSID") && strcmp(WIFI_SSID, "YOUR_WIFI_SSID") != 0) {
        sSsid = WIFI_SSID;
        sPass = WIFI_PASSWORD;
    }

    safeCopyStr(m_cfg.sta_ssid, sizeof(m_cfg.sta_ssid), sSsid.c_str());
    safeCopyStr(m_cfg.sta_pass, sizeof(m_cfg.sta_pass), sPass.c_str());
    safeCopyStr(m_cfg.mdns_host, sizeof(m_cfg.mdns_host), mHost.length() > 0 ? mHost.c_str() : "furnace-controller");

    m_cfg.use_static_ip = prefs.getBool("static_en", false);
    safeCopyStr(m_cfg.static_ip,   sizeof(m_cfg.static_ip),   prefs.getString("static_ip",   "192.168.111.240").c_str());
    safeCopyStr(m_cfg.static_gw,   sizeof(m_cfg.static_gw),   prefs.getString("static_gw",   "192.168.111.1").c_str());
    safeCopyStr(m_cfg.static_mask, sizeof(m_cfg.static_mask), prefs.getString("static_mask", "255.255.255.0").c_str());
    safeCopyStr(m_cfg.static_dns,  sizeof(m_cfg.static_dns),  prefs.getString("static_dns",  "1.1.1.1").c_str());

    safeCopyStr(m_cfg.ap_ssid, sizeof(m_cfg.ap_ssid), prefs.getString("ap_ssid", "Furnace-Controller-Setup").c_str());
    safeCopyStr(m_cfg.ap_pass, sizeof(m_cfg.ap_pass), prefs.getString("ap_pass", "furnace1234").c_str());
    m_cfg.ap_always_on = prefs.getBool("ap_always", false);

    prefs.end();
}

void FurnaceNetworkManager::saveToNvs() {
    Preferences prefs;
    prefs.begin("netcfg", false);
    prefs.putString("sta_ssid",    m_cfg.sta_ssid);
    prefs.putString("sta_pass",    m_cfg.sta_pass);
    prefs.putString("mdns_host",   m_cfg.mdns_host);
    prefs.putBool("static_en",     m_cfg.use_static_ip);
    prefs.putString("static_ip",   m_cfg.static_ip);
    prefs.putString("static_gw",   m_cfg.static_gw);
    prefs.putString("static_mask", m_cfg.static_mask);
    prefs.putString("static_dns",  m_cfg.static_dns);
    prefs.putString("ap_ssid",     m_cfg.ap_ssid);
    prefs.putString("ap_pass",     m_cfg.ap_pass);
    prefs.putBool("ap_always",     m_cfg.ap_always_on);
    prefs.end();
}

void FurnaceNetworkManager::resetToDefaults() {
    Preferences prefs;
    prefs.begin("netcfg", false);
    prefs.clear();
    prefs.end();
    loadFromNvs();
    saveToNvs();
}

void FurnaceNetworkManager::applyStaConfigAndConnect() {
    if (m_cfg.ap_always_on || m_apActive || m_manualApActive) {
        WiFi.mode(WIFI_AP_STA);
    } else {
        WiFi.mode(WIFI_STA);
    }

    WiFi.setHostname(m_cfg.mdns_host);

    if (m_cfg.use_static_ip) {
        IPAddress ip, gw, mask, dns;
        if (ip.fromString(m_cfg.static_ip) &&
            gw.fromString(m_cfg.static_gw) &&
            mask.fromString(m_cfg.static_mask)) {
            if (!dns.fromString(m_cfg.static_dns)) {
                dns = gw;
            }
            WiFi.config(ip, gw, mask, dns);
            Serial.printf("  Static IPv4 Cfg  : %s (GW: %s, Mask: %s)\n",
                          m_cfg.static_ip, m_cfg.static_gw, m_cfg.static_mask);
        }
    } else {
        WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
    }

    if (strlen(m_cfg.sta_ssid) > 0 && strcmp(m_cfg.sta_ssid, "YOUR_WIFI_SSID") != 0) {
        WiFi.begin(m_cfg.sta_ssid, m_cfg.sta_pass);
    }
}

void FurnaceNetworkManager::startFallbackAp(const char* reason) {
    if (m_apActive) {
        return;
    }

    WiFi.mode(WIFI_AP_STA);
    const IPAddress apIp(192, 168, 4, 1);
    const IPAddress apGw(192, 168, 4, 1);
    const IPAddress apMask(255, 255, 255, 0);
    WiFi.softAPConfig(apIp, apGw, apMask);

    const char* pass = (strlen(m_cfg.ap_pass) >= 8) ? m_cfg.ap_pass : nullptr;
    WiFi.softAP(m_cfg.ap_ssid, pass);
    m_apActive = true;

    s_dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    s_dnsServer.start(CAPTIVE_DNS_PORT, "*", apIp);
    m_dnsStarted = true;

    Serial.printf("[NET AP] Fallback Access Point ACTIVE (%s) -> SSID: \"%s\" | IP: http://%s/\n",
                  reason ? reason : "Fallback",
                  m_cfg.ap_ssid,
                  WiFi.softAPIP().toString().c_str());
}

void FurnaceNetworkManager::stopFallbackAp() {
    if (!m_apActive) {
        return;
    }
    if (m_dnsStarted) {
        s_dnsServer.stop();
        m_dnsStarted = false;
    }
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    m_apActive       = false;
    m_manualApActive = false;
    Serial.println("[NET AP] Home Wi-Fi stable -> Fallback Access Point shut down.");
}

void FurnaceNetworkManager::ensureServicesStarted() {
    if (!m_mdnsStarted && (WiFi.isConnected() || m_apActive)) {
        if (MDNS.begin(m_cfg.mdns_host)) {
            MDNS.addService("http", "tcp", 80);
            m_mdnsStarted = true;
            Serial.printf("  mDNS Hostname    : http://%s.local/\n", m_cfg.mdns_host);
        }
    }

    if (!m_otaStarted && (WiFi.isConnected() || m_apActive)) {
        ArduinoOTA.setHostname(m_cfg.mdns_host);
        ArduinoOTA.onStart([]() {
            Serial.println("\n[ArduinoOTA] Starting wireless firmware update...");
        });
        ArduinoOTA.onEnd([]() {
            Serial.println("\n[ArduinoOTA] Update complete! Rebooting...");
        });
        ArduinoOTA.onError([](ota_error_t error) {
            Serial.printf("\n[ArduinoOTA] Error[%u]\n", (unsigned)error);
        });
        ArduinoOTA.begin();
        m_otaStarted = true;
        Serial.println("  ArduinoOTA Svc   : Listening on port 3232");
    }
}

void FurnaceNetworkManager::begin() {
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
    loadFromNvs();
    saveToNvs(); // Ensure initial seed is persisted in NVS

    Serial.println("------------------------------------------------------------------------------");
    applyStaConfigAndConnect();
    Serial.printf("  Wi-Fi Station    : Connecting to SSID \"%s\" (MAC: %s)\n",
                  m_cfg.sta_ssid, WiFi.macAddress().c_str());

    if (m_cfg.ap_always_on) {
        startFallbackAp("Always-On AP Mode Configured");
    }

    const uint32_t startMs = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - startMs) < STA_BOOT_TIMEOUT_MS) {
        g_controller.applyStatusLed(true);
        delay(200);
        g_controller.applyStatusLed(false);
        delay(200);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.isConnected()) {
        m_staConnectedSinceMs = millis();
        m_staDisconnectMs     = 0;
        Serial.printf("  Wi-Fi Connected  : YES (RSSI: %d dBm, Channel: %d)\n",
                      (int)WiFi.RSSI(), (int)WiFi.channel());
        Serial.printf("  Station Web UI   : http://%s/\n", WiFi.localIP().toString().c_str());
    } else {
        m_staDisconnectMs = millis();
        Serial.println("  Wi-Fi Connected  : NO (Starting Fallback Setup AP + 60s Background Retry)");
        startFallbackAp("Boot STA Timeout");
    }

    ensureServicesStarted();
    Serial.printf("  Active Partition : %s (Reset Reason: %s)\n",
                  getRunningPartitionLabel(), getResetReasonString());
    Serial.println("  Physical AP Btn  : Hold BOOT (GPIO 0) for 3s anytime to toggle Setup AP");
    Serial.println("  Serial CLI Ready : Type 'help' for interactive bench commands");
    Serial.println("------------------------------------------------------------------------------");
    Serial.println();
}

void FurnaceNetworkManager::updateConfig(const NetworkConfig& newCfg, bool save, bool reconnectNow) {
    m_cfg = newCfg;
    if (strlen(m_cfg.mdns_host) == 0) {
        safeCopyStr(m_cfg.mdns_host, sizeof(m_cfg.mdns_host), "furnace-controller");
    }
    if (strlen(m_cfg.ap_ssid) == 0) {
        safeCopyStr(m_cfg.ap_ssid, sizeof(m_cfg.ap_ssid), "Furnace-Controller-Setup");
    }
    if (save) {
        saveToNvs();
    }
    if (m_cfg.ap_always_on && !m_apActive) {
        startFallbackAp("Always-On AP Enabled");
    }
    if (reconnectNow) {
        scheduleReconnect(400);
    }
}

void FurnaceNetworkManager::setManualApOverride(bool enable, uint32_t durationMs) {
    if (enable) {
        m_manualApActive   = true;
        m_manualApExpireMs = millis() + durationMs;
        startFallbackAp("Manual AP Override (10m)");
    } else {
        m_manualApActive = false;
        if (!m_cfg.ap_always_on && WiFi.isConnected()) {
            stopFallbackAp();
        }
    }
}

void FurnaceNetworkManager::scheduleReconnect(uint32_t delayMs) {
    m_pendingReconnect = true;
    m_reconnectAtMs    = millis() + delayMs;
}

void FurnaceNetworkManager::scheduleReboot(uint32_t delayMs) {
    m_pendingReboot = true;
    m_rebootAtMs    = millis() + delayMs;
}

String FurnaceNetworkManager::scanNetworksJson() {
    // Perform synchronous 2.4 GHz scan (takes ~1.8s) and format as JSON
    const int n = WiFi.scanNetworks(false, false);
    String json;
    json.reserve(1024);
    json += "{\"count\":";
    json += String(n > 0 ? n : 0);
    json += ",\"networks\":[";

    for (int i = 0; i < n; ++i) {
        String ssid = WiFi.SSID(i);
        ssid.replace("\"", "\\\"");
        const int32_t rssi = WiFi.RSSI(i);
        const int32_t ch   = WiFi.channel(i);
        const bool    open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);

        char item[192];
        snprintf(item, sizeof(item),
                 "%s{\"ssid\":\"%s\",\"rssi\":%ld,\"channel\":%ld,\"secure\":%s}",
                 (i > 0 ? "," : ""),
                 ssid.c_str(),
                 (long)rssi,
                 (long)ch,
                 open ? "false" : "true");
        json += item;
    }

    json += "]}";
    WiFi.scanDelete();
    return json;
}

NetworkRuntimeStatus FurnaceNetworkManager::getStatus() const {
    NetworkRuntimeStatus st{};
    st.sta_connected      = WiFi.isConnected();
    st.ap_active          = m_apActive;
    st.manual_ap_active   = m_manualApActive;
    st.captive_dns_active = m_dnsStarted;
    st.ota_ready          = m_otaStarted;
    st.sta_rssi_dbm       = st.sta_connected ? (int32_t)WiFi.RSSI() : 0;
    st.sta_channel        = st.sta_connected ? (uint8_t)WiFi.channel() : 0;
    st.ap_clients         = m_apActive ? WiFi.softAPgetStationNum() : 0;

    safeCopyStr(st.sta_ip,   sizeof(st.sta_ip),   st.sta_connected ? WiFi.localIP().toString().c_str() : "0.0.0.0");
    safeCopyStr(st.ap_ip,    sizeof(st.ap_ip),    m_apActive ? WiFi.softAPIP().toString().c_str() : "0.0.0.0");
    safeCopyStr(st.mac_addr, sizeof(st.mac_addr), WiFi.macAddress().c_str());
    return st;
}

void FurnaceNetworkManager::pollBootButton() {
    const bool down = (digitalRead(BOOT_BUTTON_PIN) == LOW);
    const uint32_t now = millis();

    if (down) {
        if (!m_bootBtnPressed) {
            m_bootBtnPressed   = true;
            m_bootBtnTriggered = false;
            m_bootBtnDownMs    = now;
        } else if (!m_bootBtnTriggered && (now - m_bootBtnDownMs >= 3000UL)) {
            m_bootBtnTriggered = true;
            setManualApOverride(!m_manualApActive, 600000UL);
        }
    } else {
        m_bootBtnPressed   = false;
        m_bootBtnTriggered = false;
    }
}

void FurnaceNetworkManager::loop() {
    const uint32_t now = millis();

    pollBootButton();

    if (m_dnsStarted) {
        s_dnsServer.processNextRequest();
    }
    if (m_otaStarted) {
        ArduinoOTA.handle();
    }

    if (m_pendingReboot && (int32_t)(now - m_rebootAtMs) >= 0) {
        m_pendingReboot = false;
        Serial.println("[SYS] Executing scheduled reboot...");
        delay(100);
        ESP.restart();
    }

    if (m_pendingReconnect && (int32_t)(now - m_reconnectAtMs) >= 0) {
        m_pendingReconnect = false;
        Serial.printf("[NET] Reconnecting Station to SSID \"%s\"...\n", m_cfg.sta_ssid);
        WiFi.disconnect(false);
        delay(50);
        applyStaConfigAndConnect();
        m_staDisconnectMs = now;
    }

    //Expire manual AP override after 10 minutes
    if (m_manualApActive && (int32_t)(now - m_manualApExpireMs) >= 0) {
        m_manualApActive = false;
    }

    // Track STA link health & manage automatic Fallback AP
    if (WiFi.isConnected()) {
        if (m_staConnectedSinceMs == 0) {
            m_staConnectedSinceMs = now;
            m_staDisconnectMs     = 0;
            Serial.printf("[NET STA] Connected to \"%s\" -> IP: http://%s/ (RSSI: %d dBm)\n",
                          WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
            ensureServicesStarted();
        }

        // If Fallback AP was started temporarily and STA has been stable for 30s (with 0 AP clients), shut down AP
        if (m_apActive && !m_cfg.ap_always_on && !m_manualApActive) {
            if ((now - m_staConnectedSinceMs >= AP_SHUTDOWN_STABLE_MS) && (WiFi.softAPgetStationNum() == 0)) {
                stopFallbackAp();
            }
        }
    } else {
        m_staConnectedSinceMs = 0;
        if (m_staDisconnectMs == 0) {
            m_staDisconnectMs = now;
        }

        // Start Fallback AP if STA has been disconnected for >= 15 seconds
        if (!m_apActive && (now - m_staDisconnectMs >= STA_LOST_FALLBACK_MS)) {
            startFallbackAp("STA Connection Lost >15s");
        }

        // While in Fallback AP mode, retry connecting to home router every 60s (if no client is actively on AP)
        if (m_apActive && (now - m_lastApStaRetryMs >= AP_STA_RETRY_INTERVAL)) {
            m_lastApStaRetryMs = now;
            if (WiFi.softAPgetStationNum() == 0 && strlen(m_cfg.sta_ssid) > 0) {
                Serial.printf("[NET RETRY] Background retry for home Wi-Fi \"%s\"...\n", m_cfg.sta_ssid);
                WiFi.begin(m_cfg.sta_ssid, m_cfg.sta_pass);
            }
        }
    }
}
