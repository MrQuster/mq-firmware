#pragma once

#include <Arduino.h>

struct MQModuleStatus {
    bool cc1101 = false;
    uint8_t cc1101Partnum = 0;
    uint8_t cc1101Version = 0;
    bool pn532 = false;
    uint32_t pn532Fw = 0;
};

extern MQModuleStatus mqModules;
extern volatile bool mqCreditsTriggered;

bool probeCC1101(bool logResult = false);
bool probePN532(bool logResult = false);
void probeAllModules(bool logAlways = false);
