#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

// Hardware pin mapping for RS-485 Transceiver on UART1
static constexpr int8_t   ABCD_UART_RX_PIN    = 17;
static constexpr int8_t   ABCD_UART_TX_PIN    = 18;
static constexpr int8_t   ABCD_UART_DERE_PIN  = 21;
static constexpr uint32_t ABCD_BAUD_RATE      = 38400;
static constexpr uint32_t ABCD_MIN_IDLE_GAP_MS = 15;

// Standard Carrier / ICP / Heil Ion ABCD Bus Addresses
static constexpr uint16_t ADDR_THERMOSTAT     = 0x2001;
static constexpr uint16_t ADDR_FURNACE        = 0x4001;
static constexpr uint16_t ADDR_FURNACE_HEIL   = 0x4401;
static constexpr uint16_t ADDR_ODU            = 0x5001;
static constexpr uint16_t ADDR_CONTROLLER     = 0x9201; // Peer Co-Controller / SAM address
static constexpr uint16_t ADDR_BROADCAST      = 0xF1F1;

// Function Opcodes
static constexpr uint8_t  FUNC_ACK_RESPONSE   = 0x06;
static constexpr uint8_t  FUNC_READ           = 0x0B;
static constexpr uint8_t  FUNC_WRITE          = 0x0C;
static constexpr uint8_t  FUNC_NAK            = 0x15;

// Fan Speed / Mode Enum on ABCD Bus
enum class AbcdFanMode : uint8_t {
    AUTO = 0,
    LOW_SPEED = 1,
    MED_SPEED = 2,
    HIGH_SPEED = 3
};

// Furnace Heating Stage on Heil G96CTN0601714A1
enum class FurnaceStage : uint8_t {
    OFF = 0,
    BLOWER_ONLY = 1,
    STAGE1_LOW_39K = 2,
    STAGE2_HIGH_60K = 3
};

static constexpr size_t ABCD_MAX_FRAME_BYTES  = 265;
static constexpr size_t ABCD_RING_BUFFER_CAP  = 180;

struct AbcdFrameRecord {
    uint32_t seq;
    uint32_t timestamp_ms;
    bool     is_tx;
    bool     is_simulated;
    bool     crc_ok;
    uint16_t dest_addr;
    uint16_t src_addr;
    uint8_t  length;
    uint8_t  func;
    uint8_t  table_id[3];
    uint16_t crc_rx;
    uint16_t crc_calc;
    uint8_t  raw_len;
    uint8_t  raw[ABCD_MAX_FRAME_BYTES];
    char     summary[96];
};

struct AbcdTelemetry {
    float        room_temp_f;
    bool         room_temp_valid;
    float        ion_heat_setpoint_f;
    bool         ion_heat_sp_valid;
    AbcdFanMode  ion_fan_mode;
    FurnaceStage furnace_stage;
    uint16_t     furnace_airflow_cfm;
    uint16_t     blower_rpm;
    float        blower_watts;
    uint8_t      static_press_hundredths;
    char         relay_status[24];
    float        supply_air_temp_f;
    uint32_t     last_update_ms;
};

struct AbcdBusStats {
    uint32_t total_rx_frames;
    uint32_t total_tx_frames;
    uint32_t crc_ok_count;
    uint32_t crc_err_count;
    uint32_t sim_frames_count;
    uint32_t physical_rx_bytes;
    uint32_t last_activity_ms;
    int8_t   active_rx_pin;
    int8_t   active_tx_pin;
    bool     psram_buffer_active;
    bool     tx_enabled;
    bool     sim_bus_enabled;
    bool     capture_paused;
};

class AbcdBusDriver {
public:
    void begin();
    void loop();

    // Control whether GPIO 21 (DE/RE) is locked LOW (Sniffer Only) or allowed to transmit
    void setTxEnabled(bool enabled);
    bool isTxEnabled() const { return m_stats.tx_enabled; }

    void setSimBusEnabled(bool enabled) { m_stats.sim_bus_enabled = enabled; }
    bool isSimBusEnabled() const { return m_stats.sim_bus_enabled; }

    void setUartPins(int8_t rxPin, int8_t txPin);

    void setCapturePaused(bool paused) { m_stats.capture_paused = paused; }
    bool isCapturePaused() const { return m_stats.capture_paused; }

    void clearCaptureBuffer();

    // Update simulated room temp and/or Ion thermostat setpoint for desk testing
    void setSimulatedRoomTempF(float tempF);
    void setSimulatedIonSetpointF(float setpointF);

    // Inject a realistic sequence of Ion Thermostat <-> Heil G96CTN0601714A1 frames
    void injectSimulatedCycle();

    // Transmit (or simulate on bench) a Co-Controller WRITE frame setting Fan Mode
    bool sendCoControllerCommand(AbcdFanMode fanMode, const char* reasonNote);

    // Send a 13-byte FUNC_READ query to destAddr for table t0:t1:t2
    bool sendReadRequest(uint16_t destAddr, uint16_t srcAddr, uint8_t t0, uint8_t t1, uint8_t t2);

    // Send a FUNC_WRITE command to destAddr for table t0:t1:t2 with an arbitrary payload
    bool sendWriteRequest(uint16_t destAddr, uint16_t srcAddr, uint8_t t0, uint8_t t1, uint8_t t2,
                          const uint8_t* payload, size_t payloadLen);

    const AbcdTelemetry& getTelemetry() const { return m_telemetry; }
    const AbcdBusStats&  getStats() const { return m_stats; }
    uint32_t getBusIdleMs() const;
    AbcdFanMode getLastBusCmdFanMode() const;
    bool isUrgentCounterCmdReady() const;

    // Access ring buffer records (newest first or chronological)
    size_t getStoredFrameCount() const { return m_ringCount; }
    bool   getFrameByOffsetFromNewest(size_t offsetFromNewest, AbcdFrameRecord& outRecord) const;

    // Helpers
    static uint16_t computeCrc16(const uint8_t* data, size_t len);
    static const char* addrToName(uint16_t addr);
    static const char* funcToName(uint8_t func);
    static const char* fanModeToName(AbcdFanMode mode);
    static const char* stageToName(FurnaceStage stage);

private:
    void processRxByte(uint8_t b);
    void finalizeRxFrame(bool isTx, bool isSimulated);
    void decodeAndRecordFrame(const uint8_t* buf, size_t len, bool isTx, bool isSimulated);
    void appendRecordToRing(const AbcdFrameRecord& rec);
    bool transmitRawFrame(const uint8_t* frame, size_t len, uint32_t minSilenceUs = 25000UL);
    bool inActiveOverrideHoldoff() const;
    void updateSimulatedFurnaceStage();

    AbcdFrameRecord* m_ringBuffer       = nullptr;
    size_t           m_ringHead         = 0;
    size_t           m_ringCount        = 0;
    uint32_t         m_seqCounter       = 0;

    uint8_t          m_rxBuf[ABCD_MAX_FRAME_BYTES];
    size_t           m_rxLen            = 0;
    uint32_t         m_lastByteRxUs     = 0;
    uint32_t         m_lastSimTickMs    = 0;
    uint32_t         m_lastPinCheckMs   = 0;
    uint8_t          m_simStep          = 0;
    uint8_t          m_ionHeatStageCmd  = 0;
    uint16_t         m_lastBusCmdCfm    = 0;
    bool             m_pendingCounterCmd      = false;
    bool             m_counterCmdAckSeen      = false;
    uint32_t         m_pendingCounterCmdMs    = 0;
    uint32_t         m_lastActiveOverrideTxMs = 0;
    uint32_t         m_lastSetpointPollMs     = 0;

    AbcdTelemetry    m_telemetry{};
    AbcdBusStats     m_stats{};
};

extern AbcdBusDriver g_abcdBus;
