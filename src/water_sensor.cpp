#include "water_sensor.h"
#include <OneWire.h>
#include <DallasTemperature.h>

static OneWire           s_oneWire(DS18B20_ONEWIRE_PIN);
static DallasTemperature s_sensors(&s_oneWire);

WaterSensorDriver g_waterSensor;

void WaterSensorDriver::begin() {
    s_sensors.begin();
    s_sensors.setWaitForConversion(false); // Non-blocking 1-Wire temperature conversions
    rescanBus();
}

void WaterSensorDriver::rescanBus() {
    s_sensors.begin();
    const uint8_t count = s_sensors.getDeviceCount();
    m_status.detected_probes  = count;
    m_status.using_simulation = (count == 0) || m_forceSim;

    if (m_status.using_simulation) {
        m_status.supply_temp_f  = m_simSupplyF;
        m_status.return_temp_f  = m_simReturnF;
        m_status.delta_t_f      = m_simSupplyF - m_simReturnF;
        m_status.last_sample_ms = millis();
    }
}

void WaterSensorDriver::setSimulatedTempsF(float supplyF, float returnF) {
    m_simSupplyF = supplyF;
    m_simReturnF = returnF;
    if (m_status.using_simulation) {
        m_status.supply_temp_f  = m_simSupplyF;
        m_status.return_temp_f  = m_simReturnF;
        m_status.delta_t_f      = m_simSupplyF - m_simReturnF;
        m_status.last_sample_ms = millis();
    }
}

void WaterSensorDriver::setForceSimulation(bool forceSim) {
    m_forceSim = forceSim;
    m_status.using_simulation = (m_status.detected_probes == 0) || m_forceSim;
    if (m_status.using_simulation) {
        m_status.supply_temp_f = m_simSupplyF;
        m_status.return_temp_f = m_simReturnF;
        m_status.delta_t_f     = m_simSupplyF - m_simReturnF;
    }
}

void WaterSensorDriver::loop() {
    const uint32_t now = millis();

    if (m_status.using_simulation) {
        if (now - m_lastPollMs >= 2000UL) {
            m_lastPollMs            = now;
            m_status.supply_temp_f  = m_simSupplyF;
            m_status.return_temp_f  = m_simReturnF;
            m_status.delta_t_f      = m_simSupplyF - m_simReturnF;
            m_status.last_sample_ms = now;
        }
        return;
    }

    // Non-blocking DS18B20 conversion cycle every 2 seconds
    if (!m_conversionInFlight && (now - m_lastPollMs >= 2000UL)) {
        m_lastPollMs         = now;
        m_conversionInFlight = true;
        m_convStartMs        = now;
        s_sensors.requestTemperatures();
    } else if (m_conversionInFlight && (now - m_convStartMs >= 750UL)) {
        m_conversionInFlight = false;

        const float t0 = s_sensors.getTempFByIndex(0);
        if (t0 > -50.0f && t0 < 260.0f) {
            m_status.supply_temp_f = t0;
        }

        if (m_status.detected_probes >= 2) {
            const float t1 = s_sensors.getTempFByIndex(1);
            if (t1 > -50.0f && t1 < 260.0f) {
                m_status.return_temp_f = t1;
            }
        } else {
            m_status.return_temp_f = m_status.supply_temp_f;
        }

        m_status.delta_t_f      = m_status.supply_temp_f - m_status.return_temp_f;
        m_status.last_sample_ms = now;
    }
}
