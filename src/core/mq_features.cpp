#include "mq_features.h"
#include "display.h"
#include "mykeyboard.h"
#include "scrollableTextArea.h"
#include "utils.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <globals.h>

void mqPlaySound(MQSoundEvent ev) {
    if (bruceConfig.soundEnabled == 0) return;
    const int spkPin = (bruceConfigPins.buzzer >= 0) ? bruceConfigPins.buzzer : 26;
    switch (ev) {
        case MQ_SND_BOOT:
            tone(spkPin, 523, 80);
            delay(100);
            tone(spkPin, 659, 80);
            delay(100);
            tone(spkPin, 1047, 140);
            delay(160);
            break;
        case MQ_SND_NFC:
            tone(spkPin, 1047, 60);
            delay(75);
            tone(spkPin, 1568, 90);
            delay(100);
            break;
        case MQ_SND_IR:
            tone(spkPin, 2400, 40);
            delay(55);
            tone(spkPin, 1800, 50);
            delay(60);
            break;
        case MQ_SND_RF:
            tone(spkPin, 660, 60);
            delay(75);
            tone(spkPin, 990, 90);
            delay(100);
            break;
    }
    noTone(spkPin);
}

void mqShowGpioMonitor() {
    struct GpioEntry {
        uint8_t pin;
        const char *label;
    };
    static const GpioEntry gpios[] = {
        {5,  "CC1101 CS "},
        {18, "SPI SCK   "},
        {19, "SPI MISO  "},
        {23, "SPI MOSI  "},
        {22, "I2C SDA   "},
        {27, "I2C SCL   "},
        {26, "Speaker   "},
        {21, "TFT BL    "},
        {33, "Touch CS  "},
        {34, "LDR (In)  "},
        {35, "Bat ADC   "},
        {36, "Touch IRQ "},
        {4,  "RGB Red   "},
        {16, "RGB Green "},
        {17, "RGB Blue  "},
    };
    const int count = sizeof(gpios) / sizeof(gpios[0]);

    tft.fillScreen(bruceConfig.bgColor);
    drawMainBorderWithTitle("GPIO MONITOR", false);

    uint32_t lastDraw = 0;
    while (true) {
        if (millis() - lastDraw >= 250) {
            lastDraw = millis();
            tft.setTextSize(1);
            int y = STATUS_BAR_HEIGHT + 14;
            int colW = tftWidth / 2;
            for (int i = 0; i < count; i++) {
                int col = i / 8;
                int row = i % 8;
                int x = BORDER_PAD_X + col * colW;
                int ry = y + row * 18;
                int val = digitalRead(gpios[i].pin);
                tft.fillRect(x, ry, colW - 6, 14, bruceConfig.bgColor);
                tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
                tft.setCursor(x, ry);
                if (gpios[i].pin == 35) {
                    int mv = analogReadMilliVolts(35);
                    tft.printf("G%02d %s %dmV", gpios[i].pin, gpios[i].label, mv);
                } else {
                    tft.printf("G%02d %s ", gpios[i].pin, gpios[i].label);
                    tft.setTextColor(val ? TFT_GREEN : TFT_DARKGREY, bruceConfig.bgColor);
                    tft.print(val ? "HI" : "LO");
                }
            }
            tft.setTextColor(bruceConfig.secColor, bruceConfig.bgColor);
            tft.drawCentreString("Tap / Back to exit", tftWidth / 2, tftHeight - 14, 1);
        }

        if (check(EscPress) || check(SelPress) || check(NextPress) || check(PrevPress)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void mqShowIdleMascot() {
    Serial.println("[MQ] Mascot init: Qubi");
    tft.fillScreen(bruceConfig.bgColor);
    int cx = tftWidth / 2;
    int cy = tftHeight / 2 - 8;
    int frame = 0;
    uint32_t lastFrame = 0;

    while (true) {
        if (millis() - lastFrame >= 350) {
            lastFrame = millis();
            frame = (frame + 1) % 6;

            tft.fillRect(cx - 55, cy - 60, 110, 115, bruceConfig.bgColor);

            // Antenna
            int antOffset = (frame % 2 == 0) ? -2 : 2;
            tft.drawLine(cx, cy - 35, cx + antOffset, cy - 50, bruceConfig.priColor);
            tft.fillCircle(cx + antOffset, cy - 52, 4, (frame == 0) ? TFT_GREEN : bruceConfig.secColor);

            // Head (MQ-Bot)
            tft.drawRoundRect(cx - 34, cy - 35, 68, 48, 10, bruceConfig.priColor);
            tft.fillRoundRect(cx - 26, cy - 26, 52, 26, 6, 0x0841);

            // Eyes (blink on frame 4)
            if (frame == 4) {
                tft.drawLine(cx - 18, cy - 13, cx - 6, cy - 13, bruceConfig.priColor);
                tft.drawLine(cx + 6, cy - 13, cx + 18, cy - 13, bruceConfig.priColor);
            } else {
                int look = (frame == 2) ? 2 : ((frame == 3) ? -2 : 0);
                tft.fillCircle(cx - 12 + look, cy - 13, 5, bruceConfig.priColor);
                tft.fillCircle(cx + 12 + look, cy - 13, 5, bruceConfig.priColor);
            }

            // Body + MQ badge
            tft.drawRoundRect(cx - 24, cy + 16, 48, 30, 6, bruceConfig.secColor);
            tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
            tft.setTextSize(2);
            tft.drawCentreString("MQ", cx, cy + 23, 1);
            tft.setTextSize(1);
            tft.setTextColor(bruceConfig.secColor, bruceConfig.bgColor);
            tft.drawCentreString("Qubi - MQ Pet (tap to wake)", cx, tftHeight - 16, 1);
        }

        if (check(AnyKeyPress) || check(EscPress) || check(SelPress) || check(NextPress) || check(PrevPress)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    tft.fillScreen(bruceConfig.bgColor);
}

void mqCheckUpdates() {
    ScrollableTextArea area = ScrollableTextArea("MQ UPDATES");
    area.addLine("Current: MQ v" + String(BRUCE_VERSION));
    area.addLine("Built: " + String(__DATE__));
    area.addLine("");

    if (!WiFi.isConnected()) {
        area.addLine("WiFi: Not connected");
        area.addLine("Connect WiFi first to");
        area.addLine("check GitHub releases.");
        area.addLine("");
        area.addLine("Repo: github.com/MrQuster/");
        area.addLine("      mq-firmware");
        area.show();
        return;
    }

    displayInfo("Checking GitHub...", false);
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    String url = "https://api.github.com/repos/MrQuster/mq-firmware/releases/latest";
    if (http.begin(client, url)) {
        http.setUserAgent("MQ-Firmware");
        int code = http.GET();
        if (code == 200) {
            String payload = http.getString();
            int tagIdx = payload.indexOf("\"tag_name\"");
            String tag = "unknown";
            if (tagIdx >= 0) {
                int q1 = payload.indexOf('"', tagIdx + 10);
                int q2 = payload.indexOf('"', q1 + 1);
                if (q1 >= 0 && q2 > q1) tag = payload.substring(q1 + 1, q2);
            }
            area.addLine("Latest release: " + tag);
            area.addLine("Flash via WebUI or USB");
            area.addLine("with MQ-CYD-2432S028.bin");
        } else {
            area.addLine("GitHub HTTP " + String(code));
            area.addLine("No published release yet.");
            area.addLine("Manual update: flash");
            area.addLine("MQ-CYD-2432S028.bin");
        }
        http.end();
    } else {
        area.addLine("HTTPS connection failed.");
    }
    area.show();
}
