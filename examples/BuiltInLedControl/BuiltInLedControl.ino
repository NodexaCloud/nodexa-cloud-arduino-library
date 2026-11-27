#include <NodexaCloud.h>

// Set this version for each firmware release.
#define FIRMWARE_VERSION "1.0.0"

// ESP32-S3 Dev Module variant maps LED_BUILTIN to its RGB LED definition.
constexpr uint8_t BUILT_IN_LED_PIN = LED_BUILTIN;

#if __has_include("secrets.h")
#include "secrets.h"
#define NODEXA_CONFIGURED 1
#else
#define NODEXA_CONFIGURED 0
#endif

Nodexa::Client nodexa(FIRMWARE_VERSION);

void handleLedController(bool on) {
  pinMode(BUILT_IN_LED_PIN, OUTPUT);
  digitalWrite(BUILT_IN_LED_PIN, on ? HIGH : LOW);
  Serial.printf("esp32_led=%s; LED_BUILTIN command=%s.\n",
                on ? "true" : "false", on ? "ON" : "OFF");
}

void setup() {
  Serial.begin(115200);
  const uint32_t serialWaitStarted = millis();
  while (!Serial && millis() - serialWaitStarted < 5000) delay(10);
  nodexa.debug(true);  // This example prints the full Nodexa connection sequence.
  if (!nodexa.setFirmwareVersion(FIRMWARE_VERSION)) {
    Serial.println("Invalid firmware version.");
    return;
  }
  pinMode(BUILT_IN_LED_PIN, OUTPUT);
  digitalWrite(BUILT_IN_LED_PIN, LOW);
#if NODEXA_CONFIGURED
  if (!nodexa.subscribe("esp32_led", handleLedController)) {
    Serial.println("Could not subscribe to the esp32_led variable.");
    return;
  }
  if (!nodexa.begin({WIFI_SSID, WIFI_PASSWORD, SECRET_KEY, ACCOUNT_ID, PROJECT_ID})) {
    Serial.println("Could not start Nodexa. Check your connection settings.");
  }
#else
  Serial.println("Copy secrets.h.example to secrets.h and fill in your settings.");
#endif
}

void loop() {
  static int lastConnectionState = -1;
  const bool connected = nodexa.connected();
  const int connectionState = connected ? 1 : 0;
  if (connectionState != lastConnectionState) {
    Serial.printf("nodexa.connected() = %s\n",
                  connected ? "true" : "false");
    lastConnectionState = connectionState;
  }
  delay(100);
}
