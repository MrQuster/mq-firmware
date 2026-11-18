#pragma once

#include <Arduino.h>

enum MQSoundEvent {
    MQ_SND_BOOT = 0,
    MQ_SND_NFC,
    MQ_SND_IR,
    MQ_SND_RF
};

void mqPlaySound(MQSoundEvent ev);
void mqShowGpioMonitor();
void mqCheckUpdates();
