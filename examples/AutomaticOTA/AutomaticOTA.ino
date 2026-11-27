#include <NodexaCloud.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif

Nodexa::Client nodexa("1.0.0");
Nodexa::FirmwareUpdater ota(nodexa);

void setup() {
  Serial.begin(115200);
#if __has_include("secrets.h")
  nodexa.debug(true);
  if (!nodexa.begin({WIFI_SSID, WIFI_PASSWORD, SECRET_KEY, ACCOUNT_ID, PROJECT_ID})) {
    Serial.println("Could not start Nodexa. Check your settings.");
    return;
  }
  Serial.printf("OTA board: %s, flash: %u bytes\n",
                ARDUINO_BOARD, static_cast<unsigned>(ESP.getFlashChipSize()));
#else
  Serial.println("Copy secrets.h.example to secrets.h and enter your settings.");
#endif
}

void loop() {
  // On a real machine, call run only after startup checks and while idle.
  ota.run();
  delay(20);
}
