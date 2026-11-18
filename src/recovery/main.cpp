/*
 * MQ Firmware - Recovery / Updater application (factory partition)
 *
 * This is a deliberately small, self-contained application.  It must never
 * depend on the main firmware sources: the main app lives in ota_0 and can be
 * replaced or corrupted at any time, while this image is the immutable USB
 * recovery path.
 *
 * Responsibilities:
 *   - validate the ota_0 application and hand control back to it;
 *   - consume a pending OTA request written by the main firmware in the
 *     "mq-ota" NVS namespace;
 *   - download the application image over HTTPS (compact embedded roots),
 *     verify its SHA-256 digest, install it into ota_0 and reboot.
 *
 * The recovery image is built by [env:CYD-2432S028-recovery] and merged at the
 * factory offset from partitions.csv (0x10000).
 *
 * Only ESP-IDF networking is used on purpose.  The Arduino WiFi wrapper pulls
 * Bluetooth memory release, smart-config, network provisioning and coredump
 * code into the image; the IDF path keeps the factory app small enough to fit
 * in front of the main application.
 */

#include <Arduino.h>
#include <IPAddress.h>
#include <TFT_eSPI.h>
#include <esp_app_format.h>
#include <esp_event.h>
#include <esp_http_client.h>
#include <esp_netif.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <mbedtls/sha256.h>
#include <nvs.h>
#include <nvs_flash.h>

#include <cstring>

#include "ca_roots.h"

namespace {

constexpr const char *kLogTag = "[MQ]";
constexpr const char *kOtaNamespace = "mq-ota";
constexpr const char *kNetNamespace = "mq-net";
constexpr const char *kRecoveryVersion = "1.0";
constexpr size_t kDownloadChunk = 4096;
constexpr int kHttpTimeoutMs = 25000;
constexpr int kMaxRedirects = 5;

constexpr EventBits_t kWifiConnectedBit = BIT0;
constexpr EventBits_t kWifiFailedBit = BIT1;

TFT_eSPI tft;
EventGroupHandle_t wifiEvents = nullptr;
bool wifiStarted = false;

struct OtaRequest {
    String url;
    String sha256;
    uint32_t size = 0;

    bool valid() const { return url.length() > 0 && sha256.length() == 64; }
};

void backlightOn() {
#ifdef TFT_BL
    if (TFT_BL >= 0) {
        pinMode(TFT_BL, OUTPUT);
        digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
    }
#endif
}

void drawHeader() {
    tft.fillScreen(TFT_BLACK);
    tft.fillRect(0, 0, tft.width(), 28, TFT_ORANGE);
    tft.setTextColor(TFT_BLACK, TFT_ORANGE);
    tft.setTextSize(2);
    tft.setCursor(8, 7);
    tft.print("MQ RECOVERY");
    tft.setTextSize(1);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.setCursor(tft.width() - 24, 31);
    tft.print(kRecoveryVersion);
}

int g_logLine = 0;

void logLine(const String &text) {
    Serial.println(String(kLogTag) + " " + text);
    const int maxLines = (tft.height() - 60) / 12;
    if (g_logLine >= maxLines) {
        tft.fillRect(0, 36, tft.width(), tft.height() - 36, TFT_BLACK);
        g_logLine = 0;
    }
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setCursor(8, 40 + g_logLine * 12);
    tft.print(text);
    g_logLine++;
}

void drawProgress(uint32_t written, uint32_t total) {
    const int x = 8;
    const int y = tft.height() - 24;
    const int w = tft.width() - 16;
    const int h = 12;
    tft.drawRect(x, y, w, h, TFT_ORANGE);
    if (total == 0) return;
    uint32_t clamped = written > total ? total : written;
    int fill = (int)((uint64_t)(w - 4) * clamped / total);
    tft.fillRect(x + 2, y + 2, fill, h - 4, TFT_ORANGE);
    tft.fillRect(8, tft.height() - 44, 96, 16, TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(8, tft.height() - 44);
    tft.printf("%3lu%%", (unsigned long)((uint64_t)clamped * 100 / total));
    tft.setTextSize(1);
}

void showFatal(const String &message) {
    Serial.println(String(kLogTag) + " ERROR: " + message);
    tft.fillRect(0, tft.height() - 90, tft.width(), 90, TFT_BLACK);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(8, tft.height() - 82);
    tft.print("RECOVERY FAILED");
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(8, tft.height() - 68);
    tft.print(message);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.setCursor(8, tft.height() - 50);
    tft.print("Flash the full image over USB:");
    tft.setCursor(8, tft.height() - 38);
    tft.print("MQ-CYD-2432S028-full.bin");
}

String describePartition(const esp_partition_t *part) {
    if (part == nullptr) return "missing";
    return String(part->label) + " @0x" + String(part->address, HEX) + " " + String(part->size) + "B";
}

bool appImageValid(const esp_partition_t *part, esp_app_desc_t *descOut, String &err) {
    if (part == nullptr) {
        err = "ota_0 partition not found";
        return false;
    }
    esp_app_desc_t desc{};
    esp_err_t rc = esp_ota_get_partition_description(part, &desc);
    if (rc != ESP_OK) {
        err = String("invalid ota_0 image (") + esp_err_to_name(rc) + ")";
        return false;
    }
    if (descOut != nullptr) *descOut = desc;
    return true;
}

void wifiEventHandler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(wifiEvents, kWifiFailedBit);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupClearBits(wifiEvents, kWifiFailedBit);
        xEventGroupSetBits(wifiEvents, kWifiConnectedBit);
    }
}

bool readWifiCredentials(String &ssid, String &pass) {
    nvs_handle_t handle;
    if (nvs_open(kNetNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
    char ssidBuf[65] = {0};
    char passBuf[65] = {0};
    size_t ssidLen = sizeof(ssidBuf);
    size_t passLen = sizeof(passBuf);
    bool ok = nvs_get_str(handle, "ssid", ssidBuf, &ssidLen) == ESP_OK;
    nvs_get_str(handle, "pass", passBuf, &passLen);
    nvs_close(handle);
    if (!ok) return false;
    ssid = ssidBuf;
    pass = passBuf;
    return true;
}

bool connectWifi(String &err) {
    String ssid;
    String pass;
    if (!readWifiCredentials(ssid, pass)) {
        err = "No Wi-Fi credentials (mq-net)";
        return false;
    }
    logLine("Wi-Fi: " + ssid);

    wifiEvents = xEventGroupCreate();
    if (wifiEvents == nullptr) {
        err = "Event group allocation failed";
        return false;
    }

    esp_err_t rc = esp_netif_init();
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
        err = String("esp_netif_init: ") + esp_err_to_name(rc);
        return false;
    }
    rc = esp_event_loop_create_default();
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
        err = String("event_loop: ") + esp_err_to_name(rc);
        return false;
    }
    if (esp_netif_create_default_wifi_sta() == nullptr) {
        err = "Cannot create Wi-Fi station netif";
        return false;
    }

    wifi_init_config_t initConfig = WIFI_INIT_CONFIG_DEFAULT();
    rc = esp_wifi_init(&initConfig);
    if (rc != ESP_OK) {
        err = String("esp_wifi_init: ") + esp_err_to_name(rc);
        return false;
    }
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifiEventHandler, nullptr, nullptr);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifiEventHandler, nullptr, nullptr);

    wifi_config_t config{};
    strncpy((char *)config.sta.ssid, ssid.c_str(), sizeof(config.sta.ssid) - 1);
    strncpy((char *)config.sta.password, pass.c_str(), sizeof(config.sta.password) - 1);
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;

    rc = esp_wifi_set_mode(WIFI_MODE_STA);
    if (rc == ESP_OK) rc = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (rc == ESP_OK) rc = esp_wifi_start();
    if (rc != ESP_OK) {
        err = String("Wi-Fi start: ") + esp_err_to_name(rc);
        return false;
    }
    wifiStarted = true;

    EventBits_t bits = xEventGroupWaitBits(
        wifiEvents, kWifiConnectedBit | kWifiFailedBit, pdFALSE, pdFALSE, pdMS_TO_TICKS(25000)
    );
    if (!(bits & kWifiConnectedBit)) {
        err = "Wi-Fi connection failed";
        return false;
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip{};
    if (netif != nullptr && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        logLine("IP: " + IPAddress(ip.ip.addr).toString());
    } else {
        logLine("Wi-Fi connected");
    }
    return true;
}

bool readOtaRequest(OtaRequest &req) {
    nvs_handle_t handle;
    if (nvs_open(kOtaNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
    char url[512] = {0};
    char sha[65] = {0};
    size_t urlLen = sizeof(url);
    size_t shaLen = sizeof(sha);
    bool ok = nvs_get_str(handle, "url", url, &urlLen) == ESP_OK;
    ok = ok && nvs_get_str(handle, "sha256", sha, &shaLen) == ESP_OK;
    uint32_t size = 0;
    nvs_get_u32(handle, "size", &size);
    nvs_close(handle);
    if (!ok) return false;
    req.url = url;
    req.sha256 = sha;
    req.sha256.toLowerCase();
    req.size = size;
    return req.valid();
}

void clearOtaRequest() {
    nvs_handle_t handle;
    if (nvs_open(kOtaNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_erase_key(handle, "url");
    nvs_erase_key(handle, "sha256");
    nvs_erase_key(handle, "size");
    nvs_commit(handle);
    nvs_close(handle);
}

void rebootInto(const esp_partition_t *part, const char *reason) {
    esp_err_t rc = esp_ota_set_boot_partition(part);
    if (rc != ESP_OK) {
        showFatal(String("Cannot select boot partition: ") + esp_err_to_name(rc));
        return;
    }
    logLine(String("Boot -> ") + part->label + " (" + reason + ")");
    delay(700);
    ESP.restart();
}

String headerValue(esp_http_client_handle_t client, const char *name) {
    char *value = nullptr;
    if (esp_http_client_get_header(client, name, &value) != ESP_OK || value == nullptr) return String();
    return String(value);
}

bool downloadAndInstall(const OtaRequest &req) {
    String err;
    if (!connectWifi(err)) {
        showFatal(err);
        return false;
    }

    const esp_partition_t *update = esp_ota_get_next_update_partition(nullptr);
    if (update == nullptr) {
        showFatal("No OTA partition available");
        return false;
    }
    logLine("Target: " + describePartition(update));

    logLine("CA: embedded roots");

    esp_http_client_config_t config{};
    config.url = req.url.c_str();
    config.cert_pem = kRootCaPem;
    config.cert_len = sizeof(kRootCaPem);
    config.timeout_ms = kHttpTimeoutMs;
    config.buffer_size = 2048;
    config.buffer_size_tx = 1024;
    config.disable_auto_redirect = true;
    config.keep_alive_enable = false;
    config.user_agent = "MQ-Recovery/1.0";
    config.skip_cert_common_name_check = false;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        showFatal("HTTP client init failed");
        return false;
    }

    int status = 0;
    for (int redirect = 0; redirect <= kMaxRedirects; redirect++) {
        esp_err_t rc = esp_http_client_open(client, 0);
        if (rc != ESP_OK) {
            esp_http_client_cleanup(client);
            showFatal(String("HTTPS open: ") + esp_err_to_name(rc));
            return false;
        }
        esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        if (status == 301 || status == 302 || status == 307 || status == 308) {
            String location = headerValue(client, "Location");
            esp_http_client_close(client);
            if (location.length() == 0) {
                esp_http_client_cleanup(client);
                showFatal("Redirect without Location");
                return false;
            }
            logLine("Redirect: " + location.substring(0, 40));
            esp_http_client_set_url(client, location.c_str());
            continue;
        }
        break;
    }

    if (status != 200) {
        esp_http_client_cleanup(client);
        String msg = String("HTTP ") + status;
        if (status == 403 || status == 429) msg += " (GitHub rate limit)";
        showFatal(msg);
        return false;
    }

    int64_t contentLength = esp_http_client_get_content_length(client);
    uint32_t total = req.size;
    if (total == 0 && contentLength > 0) total = (uint32_t)contentLength;
    logLine("Download: " + String(total) + " bytes");

    esp_ota_handle_t handle = 0;
    esp_err_t rc = esp_ota_begin(update, OTA_SIZE_UNKNOWN, &handle);
    if (rc != ESP_OK) {
        esp_http_client_cleanup(client);
        showFatal(String("esp_ota_begin: ") + esp_err_to_name(rc));
        return false;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    static uint8_t buffer[kDownloadChunk];
    uint32_t written = 0;
    uint32_t lastUi = 0;
    bool ioError = false;

    while (true) {
        int got = esp_http_client_read(client, (char *)buffer, sizeof(buffer));
        if (got < 0) {
            ioError = true;
            err = "network read failed";
            break;
        }
        if (got == 0) break;

        mbedtls_sha256_update(&sha, buffer, (size_t)got);
        rc = esp_ota_write(handle, buffer, (size_t)got);
        if (rc != ESP_OK) {
            ioError = true;
            err = String("esp_ota_write: ") + esp_err_to_name(rc);
            break;
        }
        written += (uint32_t)got;
        if (millis() - lastUi >= 200) {
            lastUi = millis();
            drawProgress(written, total);
        }
        if (total > 0 && written > total) {
            ioError = true;
            err = "image larger than announced";
            break;
        }
    }

    if (!ioError && esp_http_client_is_complete_data_received(client) == false && contentLength > 0) {
        ioError = true;
        err = "incomplete download";
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);

    if (ioError) {
        esp_ota_abort(handle);
        showFatal(err);
        return false;
    }
    if (written == 0) {
        esp_ota_abort(handle);
        showFatal("Empty download");
        return false;
    }

    char hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    if (req.sha256 != String(hex)) {
        esp_ota_abort(handle);
        logLine("expected " + req.sha256.substring(0, 16) + "...");
        logLine("actual   " + String(hex).substring(0, 16) + "...");
        showFatal("SHA-256 mismatch");
        return false;
    }
    logLine("SHA-256 OK");

    rc = esp_ota_end(handle);
    if (rc != ESP_OK) {
        showFatal(String("esp_ota_end: ") + esp_err_to_name(rc));
        return false;
    }

    clearOtaRequest();
    drawProgress(written, written);
    logLine("Installed: " + String(written) + " bytes");
    rebootInto(update, "OTA installed");
    return true;
}

} // namespace

void setup() {
    Serial.begin(115200);
    delay(150);

    backlightOn();
    tft.init();
    tft.setRotation(1);
    drawHeader();

    logLine("MQ Recovery / Updater");
    logLine("Build: " + String(__DATE__) + " " + String(__TIME__));

    esp_err_t nvsRc = nvs_flash_init();
    if (nvsRc == ESP_ERR_NVS_NO_FREE_PAGES || nvsRc == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvsRc = nvs_flash_init();
    }
    if (nvsRc != ESP_OK && nvsRc != ESP_ERR_INVALID_STATE) {
        logLine(String("NVS: ") + esp_err_to_name(nvsRc));
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *factory =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
    const esp_partition_t *ota0 =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);

    logLine("Running: " + describePartition(running));
    logLine("Factory: " + describePartition(factory));
    logLine("OTA slot: " + describePartition(ota0));

    OtaRequest request;
    if (readOtaRequest(request)) {
        logLine("Pending OTA request");
        downloadAndInstall(request);
        return;
    }

    esp_app_desc_t desc{};
    String err;
    if (appImageValid(ota0, &desc, err)) {
        logLine("ota_0: " + String(desc.project_name) + " " + String(desc.version));
        logLine("ota_0: " + String(desc.date) + " " + String(desc.time));
        rebootInto(ota0, "ota_0 valid");
        return;
    }

    logLine(err);
    logLine("No pending update.");
    logLine("Connect USB and flash:");
    logLine("MQ-CYD-2432S028-full.bin");
    Serial.println("[MQ] Recovery idle: waiting for USB flash");
}

void loop() {
    // setup() either installs an update, hands control to ota_0, or parks here
    // until the board is reset with a fresh USB image.  Keep the loop idle.
    delay(1000);
}
