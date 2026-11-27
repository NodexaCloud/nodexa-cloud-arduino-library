#include <NodexaCloud.h>
// Increase this version for each firmware release.
#define FIRMWARE_VERSION "1.0.3"

#if __has_include("secrets.h")
#include "secrets.h"
#define NODEXA_CONFIGURED 1
#else
#define NODEXA_CONFIGURED 0
#endif

Nodexa::Client nodexa(FIRMWARE_VERSION);
Nodexa::FirmwareUpdater ota(nodexa);

void printOtaProgress(uint8_t percent) {
  Serial.printf("\rFirmware download: %u%%", static_cast<unsigned>(percent));
  if (percent >= 100) Serial.println();
}

void setup() {
  Serial.begin(115200);
  const uint32_t serialWaitStarted = millis();
  while (!Serial && millis() - serialWaitStarted < 5000) delay(10);
  Serial.println();
  Serial.println("Nodexa ESP32 starting. Serial Monitor: 115200 baud.");
  nodexa.debug(true);  // Set false to silence Nodexa diagnostics.
#if NODEXA_CONFIGURED
  if (!nodexa.begin({WIFI_SSID, WIFI_PASSWORD, SECRET_KEY, ACCOUNT_ID, PROJECT_ID})) {
    Serial.println("Could not start Nodexa. Check your connection settings.");
    return;
  }
  Serial.printf("OTA enabled for board %s (%u bytes flash).\n",
                ARDUINO_BOARD, static_cast<unsigned>(ESP.getFlashChipSize()));
#else
  Serial.println("Copy secrets.h.example to secrets.h and fill in your settings.");
#endif
}

void loop() {
#if NODEXA_CONFIGURED
  // Progress prints from the OTA task while loop() stays responsive.
  ota.run(printOtaProgress);
#endif
  delay(20);
}
