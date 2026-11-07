#pragma once

#include <Arduino.h>
#include <stdint.h>

static constexpr uint8_t DS18B20_ONEWIRE_PIN = 4;

struct WaterSensorStatus {
    uint8_t  detected_probes;
    bool     using_simulation;
    float    supply_temp_f;
    float    return_temp_f;
    float    delta_t_f;
    uint32_t last_sample_ms;
};

class WaterSensorDriver {
public:
    void begin();
    void loop();

    // Rescan the 1-Wire bus on GPIO 4 for newly connected DS18B20 probes
    void rescanBus();

    // Set simulated Supply & Return water temperatures (used on bench or when forced)
    void setSimulatedTempsF(float supplyF, float returnF);
    void setForceSimulation(bool forceSim);

    const WaterSensorStatus& getStatus() const { return m_status; }

private:
    WaterSensorStatus m_status{};
    bool              m_forceSim        = false;
    float             m_simSupplyF      = 155.0f;
    float             m_simReturnF      = 138.0f;
    uint32_t          m_lastPollMs      = 0;
    bool              m_conversionInFlight = false;
    uint32_t          m_convStartMs     = 0;
};

extern WaterSensorDriver g_waterSensor;
