#pragma once

#include <Arduino.h>

struct NetworkConfig {
    char sta_ssid[64];
    char sta_pass[64];
    char mdns_host[48];
    bool use_static_ip;
    char static_ip[20];
    char static_gw[20];
    char static_mask[20];
    char static_dns[20];
    char ap_ssid[48];
    char ap_pass[48];
    bool ap_always_on;
};

struct NetworkRuntimeStatus {
    bool     sta_connected;
    bool     ap_active;
    bool     manual_ap_active;
    bool     captive_dns_active;
    bool     ota_ready;
    int32_t  sta_rssi_dbm;
    uint8_t  sta_channel;
    uint8_t  ap_clients;
    char     sta_ip[20];
    char     ap_ip[20];
    char     mac_addr[20];
};

class FurnaceNetworkManager {
public:
    void begin();
    void loop();

    const NetworkConfig& getConfig() const { return m_cfg; }
    NetworkRuntimeStatus getStatus() const;

    // Save new network config to NVS and optionally trigger STA reconnect / AP update
    void updateConfig(const NetworkConfig& newCfg, bool saveToNvs = true, bool reconnectNow = true);

    // Reset NVS network config back to compile-time secrets.h defaults
    void resetToDefaults();

    // Manually toggle Fallback AP on for 10 minutes (or off)
    void setManualApOverride(bool enable, uint32_t durationMs = 600000UL);

    // Schedule a non-blocking STA reconnect or system reboot after HTTP response completes
    void scheduleReconnect(uint32_t delayMs = 400);
    void scheduleReboot(uint32_t delayMs = 800);

    // Scan nearby 2.4 GHz Wi-Fi networks and return JSON array string
    String scanNetworksJson();

    // Helper for system diagnostics
    static const char* getResetReasonString();
    static const char* getRunningPartitionLabel();

private:
    void loadFromNvs();
    void saveToNvs();
    void applyStaConfigAndConnect();
    void startFallbackAp(const char* reason);
    void stopFallbackAp();
    void ensureServicesStarted();
    void pollBootButton();

    NetworkConfig m_cfg{};
    bool          m_apActive            = false;
    bool          m_manualApActive      = false;
    uint32_t      m_manualApExpireMs    = 0;
    bool          m_dnsStarted          = false;
    bool          m_mdnsStarted         = false;
    bool          m_otaStarted          = false;

    uint32_t      m_staDisconnectMs     = 0;
    uint32_t      m_staConnectedSinceMs = 0;
    uint32_t      m_lastApStaRetryMs    = 0;

    bool          m_pendingReconnect    = false;
    uint32_t      m_reconnectAtMs       = 0;
    bool          m_pendingReboot       = false;
    uint32_t      m_rebootAtMs          = 0;

    bool          m_bootBtnPressed      = false;
    bool          m_bootBtnTriggered    = false;
    uint32_t      m_bootBtnDownMs       = 0;
};

extern FurnaceNetworkManager g_netMgr;
