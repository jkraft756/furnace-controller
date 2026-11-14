#include "controller.h"
#include "water_sensor.h"
#include <Preferences.h>
#include <math.h>

static constexpr uint8_t RGB_PIN_PRIMARY   = 48;
static constexpr uint8_t RGB_PIN_SECONDARY = 38;

FurnaceController g_controller;

static void writeOnboardRgb(uint8_t r, uint8_t g, uint8_t b) {
    rgbLedWrite(RGB_PIN_PRIMARY, r, g, b);
    rgbLedWrite(RGB_PIN_SECONDARY, r, g, b);
}

void FurnaceController::begin() {
    loadFromNvs();
    m_state              = ControllerState::MONITOR_ONLY;
    m_boilerHotLatched   = (g_waterSensor.getStatus().supply_temp_f >= m_config.boiler_on_f);
    m_woodOverrideActive = m_boilerHotLatched;
    m_stateEnteredMs     = millis();

    // Sync RS-485 transceiver TX enable and simulation mode with configured operating mode
    const bool isSim = (m_config.op_mode == OperatingMode::PROTOTYPING_SIMULATION);
    g_abcdBus.setTxEnabled(!isSim && m_config.op_mode != OperatingMode::PASSIVE_SNIFFER_ONLY);
    g_abcdBus.setSimBusEnabled(isSim);
    g_waterSensor.setForceSimulation(isSim);
    evaluateStateMachine();
}

void FurnaceController::loadFromNvs() {
    Preferences prefs;
    prefs.begin("furnace", true);

    const uint8_t modeVal  = prefs.getUChar("op_mode", 1); // Default: Auto Wood Co-Controller
    m_config.op_mode       = (modeVal <= 3) ? static_cast<OperatingMode>(modeVal)
                                            : OperatingMode::AUTO_WOOD_CO_CONTROLLER;
    m_config.boiler_on_f             = prefs.getFloat("boiler_on", 140.0f);
    m_config.boiler_off_f            = prefs.getFloat("boiler_off", 130.0f);
    m_config.comfort_setpoint_f      = prefs.getFloat("comfort_sp", 70.0f);

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
    prefs.putUChar("fan_spd", static_cast<uint8_t>(m_config.target_fan_speed));
    prefs.end();
}

void FurnaceController::updateConfig(const ControllerConfig& newCfg, bool save) {
    const bool leavingActiveTxMode =
        (m_state != ControllerState::MONITOR_ONLY) &&
        (m_config.op_mode != OperatingMode::PROTOTYPING_SIMULATION) &&
        (newCfg.op_mode == OperatingMode::PASSIVE_SNIFFER_ONLY ||
         newCfg.op_mode == OperatingMode::PROTOTYPING_SIMULATION);

    m_config = newCfg;
    if (m_config.boiler_off_f >= m_config.boiler_on_f) {
        m_config.boiler_off_f = m_config.boiler_on_f - 5.0f;
    }
    if (save) {
        saveToNvs();
    }
    if (leavingActiveTxMode) {
        g_abcdBus.sendCoControllerCommand(AbcdFanMode::AUTO,
                                          "Exiting Active Override -> Restore Fan AUTO");
    }
    const bool isSim = (m_config.op_mode == OperatingMode::PROTOTYPING_SIMULATION);
    g_abcdBus.setTxEnabled(!isSim && m_config.op_mode != OperatingMode::PASSIVE_SNIFFER_ONLY);
    g_abcdBus.setSimBusEnabled(isSim);
    g_waterSensor.setForceSimulation(isSim);
    evaluateStateMachine();
}

const char* FurnaceController::getStateName() const {
    switch (m_state) {
        case ControllerState::MONITOR_ONLY:         return "PASSIVE BUS MONITOR";
        case ControllerState::WOOD_ACTIVE_STANDBY:  return "WOOD HOT - STANDBY (SATISFIED)";
        case ControllerState::WOOD_BLOWER_HEATING:  return "WOOD HYDRONIC BLOWER HEATING";
        case ControllerState::BOILER_COLD_COASTING: return "BOILER COLD - COASTING (ROOM WARM)";
        case ControllerState::GAS_FALLBACK_MODE:    return "GAS FURNACE ACTIVE (THERMOSTAT RESTORED)";
        case ControllerState::MANUAL_OVERRIDE:      return "MANUAL ECM BLOWER OVERRIDE";
    }
    return "PASSIVE BUS MONITOR";
}

const char* FurnaceController::getOpModeName() const {
    switch (m_config.op_mode) {
        case OperatingMode::PASSIVE_SNIFFER_ONLY:    return "Passive Sniffer Only (DE/RE LOW)";
        case OperatingMode::AUTO_WOOD_CO_CONTROLLER: return "Auto Wood Boiler Co-Controller";
        case OperatingMode::MANUAL_BLOWER_OVERRIDE:  return "Manual Blower Override";
        case OperatingMode::PROTOTYPING_SIMULATION:  return "Prototyping Simulation";
    }
    return "Auto Wood Boiler Co-Controller";
}

const char* FurnaceController::getIonSetpointStatusText() const {
    if (g_abcdBus.isSimBusEnabled()) {
        return "SIMULATED (DESK MODE)";
    }
    if (g_abcdBus.getTelemetry().ion_heat_sp_valid) {
        return "LIVE FROM 0x2001 (00:49:03)";
    }
    return "PASSIVE / AWAITING READ";
}

RgbVisual FurnaceController::getStatusLedVisual() const {
    switch (m_state) {
        case ControllerState::MONITOR_ONLY:
            return {  0,  40,  72, "CYAN (Passive Monitor)",        "#00a8ff" };
        case ControllerState::WOOD_ACTIVE_STANDBY:
            return {  0,  72,  20, "GREEN (Wood Hot / Standby)",    "#00e676" };
        case ControllerState::WOOD_BLOWER_HEATING:
            return { 84,  38,   0, "AMBER (Wood Blower Heating)",   "#ffb300" };
        case ControllerState::BOILER_COLD_COASTING:
            return {  0,  56,  56, "TEAL (Boiler Cold / Coasting)", "#14b8a6" };
        case ControllerState::GAS_FALLBACK_MODE:
            return {  0,  16,  84, "BLUE (Gas Furnace Restored)",   "#3b82f6" };
        case ControllerState::MANUAL_OVERRIDE:
            return { 56,   0,  84, "PURPLE (Manual Fan Override)",  "#b388ff" };
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
                                              "Boiler Hot & Room Satisfied -> Fan AUTO");
            break;
        case ControllerState::WOOD_BLOWER_HEATING:
            g_abcdBus.sendCoControllerCommand(m_config.target_fan_speed,
                                              "Boiler Hot & Room Calling -> ECM Blower ON");
            break;
        case ControllerState::BOILER_COLD_COASTING:
            g_abcdBus.sendCoControllerCommand(AbcdFanMode::AUTO,
                                              "Boiler Cold but Room >= Comfort SP -> Fan AUTO, Coasting");
            break;
        case ControllerState::GAS_FALLBACK_MODE:
            g_abcdBus.sendCoControllerCommand(AbcdFanMode::AUTO,
                                              "Boiler Cold & Room < Comfort SP -> Fan AUTO (Gas Mode)");
            break;
        case ControllerState::MANUAL_OVERRIDE:
            g_abcdBus.sendCoControllerCommand(m_config.target_fan_speed,
                                              "Manual Blower Override Enabled");
            break;
    }

    // Refresh simulated furnace telemetry so stage/CFM reflect the new command immediately
    if (g_abcdBus.isSimBusEnabled()) {
        g_abcdBus.injectSimulatedCycle();
    }
}

void FurnaceController::evaluateStateMachine() {
    const bool isSimMode = (m_config.op_mode == OperatingMode::PROTOTYPING_SIMULATION);

    if (m_config.op_mode == OperatingMode::PASSIVE_SNIFFER_ONLY) {
        transitionTo(ControllerState::MONITOR_ONLY, "Passive Sniffer Mode Selected");
        return;
    }

    if (m_config.op_mode == OperatingMode::MANUAL_BLOWER_OVERRIDE) {
        if (m_state != ControllerState::MANUAL_OVERRIDE) {
            transitionTo(ControllerState::MANUAL_OVERRIDE, "Manual Override Selected");
        } else if (g_abcdBus.getTelemetry().ion_fan_mode != m_config.target_fan_speed ||
                   g_abcdBus.getLastBusCmdFanMode() != m_config.target_fan_speed) {
            g_abcdBus.sendCoControllerCommand(m_config.target_fan_speed,
                                              "Manual Fan Speed Enforced");
        }
        return;
    }

    // OperatingMode::AUTO_WOOD_CO_CONTROLLER / PROTOTYPING_SIMULATION
    const float supplyWaterF = g_waterSensor.getStatus().supply_temp_f;
    const float roomTempF    = g_abcdBus.getTelemetry().room_temp_f;

    // 1. Update Wood Boiler Hot Latch with hysteresis (boiler_on_f vs boiler_off_f)
    if (supplyWaterF >= m_config.boiler_on_f) {
        m_boilerHotLatched   = true;
        // ONLY when the Wood Boiler reaches the active threshold do we latch wood override active
        m_woodOverrideActive = true;
    } else if (supplyWaterF <= m_config.boiler_off_f) {
        m_boilerHotLatched = false;
    }

    // 2. CASE A: Wood Boiler Water is HOT (>= threshold)
    //    -> Cycle ECM Blower to maintain Comfort Setpoint
    if (m_boilerHotLatched) {
        if (isSimMode) {
            g_abcdBus.setSimulatedIonSetpointF(64.0f);
        }
        if (m_state == ControllerState::WOOD_BLOWER_HEATING) {
            if (roomTempF >= (m_config.comfort_setpoint_f + 0.5f)) {
                transitionTo(ControllerState::WOOD_ACTIVE_STANDBY, "Room Reached Comfort Setpoint + 0.5F");
            } else if (g_abcdBus.getTelemetry().ion_fan_mode != m_config.target_fan_speed ||
                       g_abcdBus.getLastBusCmdFanMode() != m_config.target_fan_speed) {
                g_abcdBus.sendCoControllerCommand(m_config.target_fan_speed,
                                                  "Enforce Wood Mode Blower Speed");
            }
        } else {
            if (roomTempF < m_config.comfort_setpoint_f) {
                transitionTo(ControllerState::WOOD_BLOWER_HEATING, "Boiler Hot & Room < Comfort Setpoint");
            } else {
                transitionTo(ControllerState::WOOD_ACTIVE_STANDBY, "Boiler Hot & Room >= Comfort Setpoint");
            }
        }
        return;
    }

    // 3. CASE B: Wood Boiler Water is COLD (<= boiler_off_f)
    if (m_woodOverrideActive && roomTempF >= m_config.comfort_setpoint_f) {
        if (isSimMode) {
            g_abcdBus.setSimulatedIonSetpointF(64.0f);
        }
        transitionTo(ControllerState::BOILER_COLD_COASTING,
                     "Boiler Just Cooled, Coasting on Residual Wood Heat Until Room < Comfort SP");
    } else {
        m_woodOverrideActive = false;
        if (isSimMode) {
            g_abcdBus.setSimulatedIonSetpointF(m_config.comfort_setpoint_f);
        }
        transitionTo(ControllerState::GAS_FALLBACK_MODE,
                     "Boiler Cold -> Fan AUTO (Gas Mode)");
        if (g_abcdBus.getTelemetry().ion_fan_mode != AbcdFanMode::AUTO) {
            g_abcdBus.sendCoControllerCommand(AbcdFanMode::AUTO,
                                              "Sync Restored Fan AUTO");
            if (g_abcdBus.isSimBusEnabled()) {
                g_abcdBus.injectSimulatedCycle();
            }
        }
    }
}

void FurnaceController::loop() {
    const uint32_t now = millis();
    const bool activeBlowerOverride =
        (m_state == ControllerState::WOOD_BLOWER_HEATING || m_state == ControllerState::MANUAL_OVERRIDE);
    const bool needPromptReassert =
        activeBlowerOverride &&
        (g_abcdBus.getLastBusCmdFanMode() != m_config.target_fan_speed) &&
        (g_abcdBus.isUrgentCounterCmdReady() || (g_abcdBus.getBusIdleMs() >= 45UL));

    if (needPromptReassert || (now - m_lastEvalMs >= 1000UL)) {
        m_lastEvalMs = now;
        evaluateStateMachine();
    }
}
