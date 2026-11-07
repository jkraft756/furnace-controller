#include <Arduino.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_psram.h>
#include <esp_system.h>

// AITRIP ESP32-S3-DevKitC-1-N16R8 revisions commonly wire the onboard WS2812 RGB LED
// to either GPIO 48 (v1.0) or GPIO 38 (v1.1). Driving both ensures visual confirmation.
static constexpr uint8_t RGB_PIN_PRIMARY   = 48;
static constexpr uint8_t RGB_PIN_SECONDARY = 38;

struct RgbColor {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    const char* name;
};

static const RgbColor HEARTBEAT_PALETTE[] = {
    {  0,  32,  64, "CYAN   (System Ready)"     },
    {  0,  64,  16, "GREEN  (Bus Idle / OK)"    },
    { 64,  32,   0, "AMBER  (Telemetry Active)" },
    { 32,   0,  64, "PURPLE (Core-1 Sync)"      },
};
static constexpr size_t PALETTE_LEN = sizeof(HEARTBEAT_PALETTE) / sizeof(HEARTBEAT_PALETTE[0]);

static uint32_t g_tick = 0;

static void setOnboardRgb(uint8_t r, uint8_t g, uint8_t b) {
    rgbLedWrite(RGB_PIN_PRIMARY, r, g, b);
    rgbLedWrite(RGB_PIN_SECONDARY, r, g, b);
}

static void printHardwareBanner() {
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    uint32_t flash_size_bytes = 0;
    esp_flash_get_size(nullptr, &flash_size_bytes);

    Serial.println();
    Serial.println("======================================================================");
    Serial.println("  FURNACE CONTROLLER — HARDWARE VERIFICATION & HELLO WORLD");
    Serial.println("  Board: AITRIP ESP32-S3-DevKitC-1-N16R8 (UART Bridge: CH343/CH9102)");
    Serial.println("======================================================================");
    Serial.printf("  Chip Model       : %s (Rev v%d.%d)\n",
                  ESP.getChipModel(), chip_info.revision / 100, chip_info.revision % 100);
    Serial.printf("  CPU Cores        : %d Cores @ %lu MHz\n",
                  ESP.getChipCores(), (unsigned long)ESP.getCpuFreqMHz());
    Serial.printf("  SDK Version      : %s\n", ESP.getSdkVersion());
    Serial.printf("  Flash Size       : %lu MB (%lu bytes, Mode: QIO)\n",
                  (unsigned long)(flash_size_bytes / (1024 * 1024)), (unsigned long)flash_size_bytes);
    Serial.printf("  PSRAM Detected   : %s\n", psramFound() ? "YES (Octal PSRAM Initialized)" : "NO");
    Serial.printf("  Total PSRAM      : %.2f MB (%lu bytes)\n",
                  ESP.getPsramSize() / (1024.0 * 1024.0), (unsigned long)ESP.getPsramSize());
    Serial.printf("  Free PSRAM       : %.2f MB (%lu bytes)\n",
                  ESP.getFreePsram() / (1024.0 * 1024.0), (unsigned long)ESP.getFreePsram());
    Serial.printf("  Internal SRAM    : %lu bytes total / %lu bytes free\n",
                  (unsigned long)ESP.getHeapSize(), (unsigned long)ESP.getFreeHeap());
    Serial.printf("  Execution Core   : setup() running on Core %d\n", xPortGetCoreID());
    Serial.println("======================================================================");
    Serial.println();
}

void setup() {
    Serial.begin(115200);
    delay(400);

    // Quick boot-up RGB sweep
    setOnboardRgb(64, 0, 0);
    delay(150);
    setOnboardRgb(0, 64, 0);
    delay(150);
    setOnboardRgb(0, 0, 64);
    delay(150);
    setOnboardRgb(0, 0, 0);

    printHardwareBanner();
}

void loop() {
    const RgbColor& color = HEARTBEAT_PALETTE[g_tick % PALETTE_LEN];

    // Crisp status pulse on onboard WS2812 RGB LED (GPIO 48 & GPIO 38)
    setOnboardRgb(color.r, color.g, color.b);

    Serial.printf("[HEARTBEAT #%04lu] Uptime: %6lu s | Core: %d | LED: %-26s | Free Heap: %6lu B | Free PSRAM: %7lu B\n",
                  (unsigned long)g_tick,
                  millis() / 1000UL,
                  xPortGetCoreID(),
                  color.name,
                  (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)ESP.getFreePsram());

    delay(250);
    // Dim between pulses so the heartbeat flash is visually crisp
    setOnboardRgb(color.r / 8, color.g / 8, color.b / 8);
    delay(1750);

    g_tick++;
}
