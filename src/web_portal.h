#pragma once

#include <Arduino.h>

class WebPortal {
public:
    void begin();
    void loop();
};

extern WebPortal g_webPortal;
