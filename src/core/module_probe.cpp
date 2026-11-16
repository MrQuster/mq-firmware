#include "module_probe.h"
#include "bus_HAL.h"
#include "configPins.h"
#include <Adafruit_PN532.h>
#include <SPI.h>
#include <globals.h>

MQModuleStatus mqModules;
volatile bool mqCreditsTriggered = false;

bool probeCC1101(bool logResult) {
    const auto &bus = bruceConfigPins.CC1101_bus;
    if (bus.sck < 0 || bus.miso < 0 || bus.mosi < 0 || bus.cs < 0) {
        mqModules.cc1101 = false;
        mqModules.cc1101Partnum = 0;
        mqModules.cc1101Version = 0;
        if (logResult) Serial.println("[MQ] Probe CC1101: NOT CONFIGURED");
        return false;
    }

    pinMode(bus.cs, OUTPUT);
    digitalWrite(bus.cs, HIGH);

    SPIClass *spi = acquireSPIBus(bus.sck, bus.miso, bus.mosi);
    bool ownSpi = false;
    if (!spi) {
        spi = &SPI;
        spi->begin(bus.sck, bus.miso, bus.mosi, bus.cs);
        ownSpi = true;
    }

    spi->beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    digitalWrite(bus.cs, LOW);
    delayMicroseconds(5);
    spi->transfer(0xF0); // CC1101_PARTNUM | READ_BURST (0x30 | 0xC0)
    uint8_t partnum = spi->transfer(0x00);
    digitalWrite(bus.cs, HIGH);
    delayMicroseconds(5);

    digitalWrite(bus.cs, LOW);
    delayMicroseconds(5);
    spi->transfer(0xF1); // CC1101_VERSION | READ_BURST (0x31 | 0xC0)
    uint8_t version = spi->transfer(0x00);
    digitalWrite(bus.cs, HIGH);
    spi->endTransaction();

    if (ownSpi) {
        spi->end();
    }

    bool found = (partnum == 0x00) && (version == 0x14 || version == 0x04 || version == 0x17);
    mqModules.cc1101 = found;
    mqModules.cc1101Partnum = partnum;
    mqModules.cc1101Version = version;

    if (logResult) {
        Serial.printf(
            "[MQ] Probe CC1101 (SPI SCK=%d MISO=%d MOSI=%d CS=%d): %s (PARTNUM=0x%02X VERSION=0x%02X)\n",
            bus.sck,
            bus.miso,
            bus.mosi,
            bus.cs,
            found ? "FOUND" : "NOT FOUND",
            partnum,
            version
        );
    }
    return found;
}

bool probePN532(bool logResult) {
    I2CDeviceQuery query;
    query.addr = PN532_I2C_ADDRESS;
    query.prefer = I2CPrefer::UserFirst;
    query.retries = 2;
    query.fallbackToUser = false;

    I2CLease lease = i2cLeaseDevice(query);
    bool found = false;
    uint32_t versiondata = 0;

    if (lease && lease.wire()) {
        lease.setTimeOut(50);
        Adafruit_PN532 nfc(-1, -1);
        nfc.setInterface(lease.wire());
        nfc.begin();
        versiondata = nfc.getFirmwareVersion();
        if (versiondata != 0 && ((versiondata >> 24) & 0xFF) == 0x32) {
            found = true;
        }
    }

    mqModules.pn532 = found;
    mqModules.pn532Fw = versiondata;

    if (logResult) {
        Serial.printf(
            "[MQ] Probe PN532 (I2C SDA=%d SCL=%d addr=0x%02X): %s (FW=0x%08lX)\n",
            bruceConfigPins.i2c_bus.sda,
            bruceConfigPins.i2c_bus.scl,
            PN532_I2C_ADDRESS,
            found ? "FOUND" : "NOT FOUND",
            (unsigned long)versiondata
        );
    }
    return found;
}

void probeAllModules(bool logAlways) {
    bool prevCC = mqModules.cc1101;
    bool prevPN = mqModules.pn532;

    bool curCC = probeCC1101(false);
    if (logAlways || curCC != prevCC) {
        Serial.printf(
            "[MQ] Probe CC1101 (SPI SCK=%d MISO=%d MOSI=%d CS=%d): %s (PARTNUM=0x%02X VERSION=0x%02X)\n",
            bruceConfigPins.CC1101_bus.sck,
            bruceConfigPins.CC1101_bus.miso,
            bruceConfigPins.CC1101_bus.mosi,
            bruceConfigPins.CC1101_bus.cs,
            curCC ? "FOUND" : "NOT FOUND",
            mqModules.cc1101Partnum,
            mqModules.cc1101Version
        );
    }

    bool curPN = probePN532(false);
    if (logAlways || curPN != prevPN) {
        Serial.printf(
            "[MQ] Probe PN532 (I2C SDA=%d SCL=%d addr=0x%02X): %s (FW=0x%08lX)\n",
            bruceConfigPins.i2c_bus.sda,
            bruceConfigPins.i2c_bus.scl,
            PN532_I2C_ADDRESS,
            curPN ? "FOUND" : "NOT FOUND",
            (unsigned long)mqModules.pn532Fw
        );
    }
}
