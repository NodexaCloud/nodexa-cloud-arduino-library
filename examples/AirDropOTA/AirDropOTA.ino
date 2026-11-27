#include <NodexaCloud.h>

#if __has_include("secrets.h")
#include "secrets.h"
#define NODEXA_CONFIGURED 1
#else
#define NODEXA_CONFIGURED 0
#endif

// Give each compiled application binary its own version.
constexpr char firmwareVersion[] = "1.0.0";
// Use this exact profile in the console for binaries built for this board.
constexpr char boardProfile[] = ARDUINO_BOARD;

Nodexa::Client nodexa(firmwareVersion);
bool rebootReady = false;
bool bootReported = false;
bool checkedBoot = false;
uint32_t lastBootAttempt = 0;

void printOtaError(const Nodexa::OtaResult& result) {
  Serial.printf("OTA: %s (%d)\n", result.error, result.detail);
}

void setup() {
  Serial.begin(115200);
#if NODEXA_CONFIGURED
  if (!nodexa.setFirmwareVersion(firmwareVersion) ||
      !nodexa.begin({WIFI_SSID, WIFI_PASSWORD, SECRET_KEY, ACCOUNT_ID, PROJECT_ID})) {
    Serial.println("Could not start Nodexa. Check your settings.");
    return;
  }
  Serial.printf("Version %s, board %s, flash %u bytes\n", firmwareVersion,
                boardProfile, static_cast<unsigned>(ESP.getFlashChipSize()));
  Serial.println("Type u to check and stage an update. Type r to boot a staged update.");
#else
  Serial.println("Copy secrets.h.example to secrets.h and enter your settings.");
#endif
}

void checkAndStage() {
  const auto result = nodexa.updateFirmware([](uint8_t percent) {
    Serial.printf("OTA download: %u%%\n", static_cast<unsigned>(percent));
  });
  if (!result.ok()) {
    printOtaError(result);
    return;
  }
  if (!result.readyToRestart) {
    Serial.println("No firmware update is available for this device.");
    return;
  }
  rebootReady = true;
  Serial.println("Firmware staged. Type r when ready to restart.");
}

void loop() {
  // In your project, also check the hardware and application before confirming.
  const uint32_t now = millis();
  if (!bootReported && nodexa.connected() &&
      (!checkedBoot || now - lastBootAttempt >= 10000)) {
    checkedBoot = true;
    lastBootAttempt = now;
    const auto result = nodexa.confirmFirmwareUpdate();
    bootReported = result.ok();
    if (!bootReported) printOtaError(result);
  }
  if (Serial.available()) {
    const int command = Serial.read();
    if (command == 'u' && bootReported && !rebootReady) checkAndStage();
    if (command == 'r' && rebootReady) {
      if (nodexa.queuedItems() == 0) ESP.restart();
      else Serial.println("Wait for queued records to finish before restarting.");
    }
  }
  delay(20);
}
