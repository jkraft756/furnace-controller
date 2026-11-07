#include "controller.h"
#include "water_sensor.h"
#include <Preferences.h>

static constexpr uint8_t RGB_PIN_PRIMARY   = 48;
static constexpr uint8_t RGB_PIN_SECONDARY = 38;

FurnaceController g_controller;

static void writeOnboardRgb(uint8_t r, uint8_t g, uint8_t b) {
    rgbLedWrite(RGB_PIN_PRIMARY, r, g, b);
    rgbLedWrite(RGB_PIN_SECONDARY, r, g, b);
}

void FurnaceController::begin() {
    loadFromNvs();
    m_state          = ControllerState::MONITOR_ONLY;
    m_stateEnteredMs = millis();

    // Sync RS-485 transceiver TX enable with configured operating mode
    g_abcdBus.setTxEnabled(m_config.op_mode != OperatingMode::PASSIVE_SNIFFER_ONLY);
    evaluateStateMachine();
}

void FurnaceController::loadFromNvs() {
    Preferences prefs;
    prefs.begin("furnace", true);

    const uint8_t modeVal  = prefs.getUChar("op_mode", 1); // Default: Auto Wood Co-Controller
    m_config.op_mode       = (modeVal <= 2) ? static_cast<OperatingMode>(modeVal)
                                            : OperatingMode::AUTO_WOOD_CO_CONTROLLER;
    m_config.boiler_on_f             = prefs.getFloat("boiler_on", 140.0f);
    m_config.boiler_off_f            = prefs.getFloat("boiler_off", 130.0f);
    m_config.comfort_setpoint_f      = prefs.getFloat("comfort_sp", 70.0f);
    m_config.gas_fallback_setpoint_f = prefs.getFloat("fallback_sp", 64.0f);

    const uint8_t fanVal   = prefs.getUChar("fan_spd", static_cast<uint8_t>(AbcdFanMode::MED_SPEED));
    m_config.target_fan_speed = (fanVal >= 1 && fanVal <= 3)
                                    ? static_cast<AbcdFanMode>(fanVal)
                                    : AbcdFanMode::MED_SPEED;
    prefs.end();
}

void FurnaceController::saveToNvs() {
    Preferences prefs;
    prefs.begin("furnace", false);
    prefs.putUChar("op_mode", static_cast<uint8_t>(m_config.op_mode));
    prefs.putFloat("boiler_on", m_config.boiler_on_f);
    prefs.putFloat("boiler_off", m_config.boiler_off_f);
    prefs.putFloat("comfort_sp", m_config.comfort_setpoint_f);
    prefs.putFloat("fallback_sp", m_config.gas_fallback_setpoint_f);
    prefs.putUChar("fan_spd", static_cast<uint8_t>(m_config.target_fan_speed));
    prefs.end();
}

void FurnaceController::updateConfig(const ControllerConfig& newCfg, bool save) {
    m_config = newCfg;
    if (m_config.boiler_off_f >= m_config.boiler_on_f) {
        m_config.boiler_off_f = m_config.boiler_on_f - 5.0f;
    }
    if (save) {
        saveToNvs();
    }
    g_abcdBus.setTxEnabled(m_config.op_mode != OperatingMode::PASSIVE_SNIFFER_ONLY);
    evaluateStateMachine();
}

const char* FurnaceController::getStateName() const {
    switch (m_state) {
        case ControllerState::MONITOR_ONLY:        return "PASSIVE BUS MONITOR";
        case ControllerState::WOOD_ACTIVE_STANDBY: return "WOOD HOT - STANDBY (SATISFIED)";
        case ControllerState::WOOD_BLOWER_HEATING: return "WOOD HYDRONIC BLOWER HEATING";
        case ControllerState::GAS_FALLBACK_MODE:   return "GAS FURNACE FALLBACK (BOILER COLD)";
        case ControllerState::MANUAL_OVERRIDE:     return "MANUAL ECM BLOWER OVERRIDE";
    }
    return "PASSIVE BUS MONITOR";
}

const char* FurnaceController::getOpModeName() const {
    switch (m_config.op_mode) {
        case OperatingMode::PASSIVE_SNIFFER_ONLY:    return "Passive Sniffer Only (DE/RE LOW)";
        case OperatingMode::AUTO_WOOD_CO_CONTROLLER: return "Auto Wood Boiler Co-Controller";
        case OperatingMode::MANUAL_BLOWER_OVERRIDE:  return "Manual Blower Override";
    }
    return "Auto Wood Boiler Co-Controller";
}

RgbVisual FurnaceController::getStatusLedVisual() const {
    switch (m_state) {
        case ControllerState::MONITOR_ONLY:
            return {  0,  40,  72, "CYAN (Passive Monitor)",       "#00a8ff" };
        case ControllerState::WOOD_ACTIVE_STANDBY:
            return {  0,  72,  20, "GREEN (Wood Hot / Standby)",   "#00e676" };
        case ControllerState::WOOD_BLOWER_HEATING:
            return { 84,  38,   0, "AMBER (Wood Blower Heating)",  "#ffb300" };
        case ControllerState::GAS_FALLBACK_MODE:
            return {  0,  16,  84, "BLUE (Gas Fallback Active)",   "#3b82f6" };
        case ControllerState::MANUAL_OVERRIDE:
            return { 56,   0,  84, "PURPLE (Manual Fan Override)", "#b388ff" };
    }
    return { 0, 40, 72, "CYAN", "#00a8ff" };
}

void FurnaceController::applyStatusLed(bool pulseBright) {
    const RgbVisual v = getStatusLedVisual();
    if (pulseBright) {
        writeOnboardRgb(v.r, v.g, v.b);
    } else {
        writeOnboardRgb(v.r / 6, v.g / 6, v.b / 6);
    }
}

void FurnaceController::transitionTo(ControllerState nextState, const char* reason) {
    if (m_state == nextState) {
        return;
    }

    const ControllerState prev = m_state;
    m_state          = nextState;
    m_stateEnteredMs = millis();

    Serial.printf("[CTRL STATE] %u -> %s (%s)\n",
                  (unsigned)prev, getStateName(), reason ? reason : "");

    // Emit corresponding ABCD Bus Co-Controller command when changing states
    switch (m_state) {
        case ControllerState::MONITOR_ONLY:
            break;
        case ControllerState::WOOD_ACTIVE_STANDBY:
            g_abcdBus.sendCoControllerCommand(AbcdFanMode::AUTO,
                                              m_config.gas_fallback_setpoint_f,
                                              "Boiler Hot & Room Satisfied -> Fan AUTO, Gas Fallback SP");
            break;
        case ControllerState::WOOD_BLOWER_HEATING:
            g_abcdBus.sendCoControllerCommand(m_config.target_fan_speed,
                                              m_config.gas_fallback_setpoint_f,
                                              "Boiler Hot & Room Calling -> Hydronic ECM Blower ON");
            break;
        case ControllerState::GAS_FALLBACK_MODE:
            g_abcdBus.sendCoControllerCommand(AbcdFanMode::AUTO,
                                              m_config.comfort_setpoint_f,
                                              "Boiler Cold -> Restore Ion Gas Setpoint & Fan AUTO");
            break;
        case ControllerState::MANUAL_OVERRIDE:
            g_abcdBus.sendCoControllerCommand(m_config.target_fan_speed,
                                              m_config.gas_fallback_setpoint_f,
                                              "Manual Blower Override Enabled");
            break;
    }
}

void FurnaceController::evaluateStateMachine() {
    if (m_config.op_mode == OperatingMode::PASSIVE_SNIFFER_ONLY) {
        transitionTo(ControllerState::MONITOR_ONLY, "Passive Sniffer Mode Selected");
        return;
    }

    if (m_config.op_mode == OperatingMode::MANUAL_BLOWER_OVERRIDE) {
        if (m_state != ControllerState::MANUAL_OVERRIDE) {
            transitionTo(ControllerState::MANUAL_OVERRIDE, "Manual Override Selected");
        } else if (g_abcdBus.getTelemetry().ion_fan_mode != m_config.target_fan_speed) {
            g_abcdBus.sendCoControllerCommand(m_config.target_fan_speed,
                                              m_config.gas_fallback_setpoint_f,
                                              "Manual Fan Speed Updated");
        }
        return;
    }

    // OperatingMode::AUTO_WOOD_CO_CONTROLLER
    const float supplyWaterF = g_waterSensor.getStatus().supply_temp_f;
    const float roomTempF    = g_abcdBus.getTelemetry().room_temp_f;

    // Determine whether the Wood Boiler water is hot (with hysteresis between boiler_on_f and boiler_off_f)
    const bool wasWoodActive = (m_state == ControllerState::WOOD_ACTIVE_STANDBY ||
                                m_state == ControllerState::WOOD_BLOWER_HEATING);
    const bool boilerHot = wasWoodActive ? (supplyWaterF >= m_config.boiler_off_f)
                                         : (supplyWaterF >= m_config.boiler_on_f);

    if (!boilerHot) {
        transitionTo(ControllerState::GAS_FALLBACK_MODE, "Boiler Water Below Threshold");
        return;
    }

    // Boiler is hot! Check room temperature against comfort_setpoint_f (with +-0.4F deadband)
    if (m_state == ControllerState::WOOD_BLOWER_HEATING) {
        if (roomTempF >= (m_config.comfort_setpoint_f + 0.4f)) {
            transitionTo(ControllerState::WOOD_ACTIVE_STANDBY, "Room Reached Comfort Setpoint");
        } else if (g_abcdBus.getTelemetry().ion_fan_mode != m_config.target_fan_speed) {
            g_abcdBus.sendCoControllerCommand(m_config.target_fan_speed,
                                              m_config.gas_fallback_setpoint_f,
                                              "Sync Target Blower Speed");
        }
    } else {
        if (roomTempF <= (m_config.comfort_setpoint_f - 0.3f)) {
            transitionTo(ControllerState::WOOD_BLOWER_HEATING, "Room Below Comfort Setpoint");
        } else {
            transitionTo(ControllerState::WOOD_ACTIVE_STANDBY, "Boiler Hot & Room Satisfied");
        }
    }
}

void FurnaceController::loop() {
    const uint32_t now = millis();
    if (now - m_lastEvalMs >= 1000UL) {
        m_lastEvalMs = now;
        evaluateStateMachine();
    }
}
