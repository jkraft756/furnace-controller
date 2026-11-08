#pragma once

#include <Arduino.h>
#include "abcd_bus.h"

enum class OperatingMode : uint8_t {
    PASSIVE_SNIFFER_ONLY     = 0,
    AUTO_WOOD_CO_CONTROLLER  = 1,
    MANUAL_BLOWER_OVERRIDE   = 2
};

enum class ControllerState : uint8_t {
    MONITOR_ONLY         = 0,
    WOOD_ACTIVE_STANDBY  = 1,
    WOOD_BLOWER_HEATING  = 2,
    BOILER_COLD_COASTING = 3,
    GAS_FALLBACK_MODE    = 4,
    MANUAL_OVERRIDE      = 5
};

struct ControllerConfig {
    OperatingMode op_mode;
    float         boiler_on_f;
    float         boiler_off_f;
    float         comfort_setpoint_f;
    float         gas_fallback_setpoint_f;
    AbcdFanMode   target_fan_speed;
};

struct RgbVisual {
    uint8_t     r;
    uint8_t     g;
    uint8_t     b;
    const char* label;
    const char* hex;
};

class FurnaceController {
public:
    void begin();
    void loop();
    void forceEvaluate() { evaluateStateMachine(); }

    const ControllerConfig& getConfig() const { return m_config; }
    void updateConfig(const ControllerConfig& newCfg, bool saveToNvs = true);

    ControllerState getState() const { return m_state; }
    bool            isBoilerHotLatched() const { return m_boilerHotLatched; }
    const char*     getStateName() const;
    const char*     getOpModeName() const;
    const char*     getIonSetpointStatusText() const;
    RgbVisual       getStatusLedVisual() const;

    void applyStatusLed(bool pulseBright);

private:
    void loadFromNvs();
    void saveToNvs();
    void evaluateStateMachine();
    void transitionTo(ControllerState nextState, const char* reason);

    ControllerConfig m_config{};
    ControllerState  m_state               = ControllerState::MONITOR_ONLY;
    bool             m_boilerHotLatched    = false;
    bool             m_woodOverrideActive  = false;
    uint32_t         m_lastEvalMs          = 0;
    uint32_t         m_stateEnteredMs      = 0;
};

extern FurnaceController g_controller;
