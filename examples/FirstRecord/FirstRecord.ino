#include <NodexaCloud.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif

Nodexa::Client nodexa("1.0.0");

void setup() {
  Serial.begin(115200);
#if __has_include("secrets.h")
  if (!nodexa.begin({WIFI_SSID, WIFI_PASSWORD, SECRET_KEY, ACCOUNT_ID, PROJECT_ID})) {
    Serial.println("Could not start Nodexa. Check the settings in secrets.h.");
    return;
  }

  Nodexa::Json record;
  record.addPair("message", "Hello from my ESP32!");
  record.addPair("count", 1);

  if (nodexa.addItem("hello_records", record)) {
    Serial.println("Record queued. Open Devices > View records after it connects.");
  } else {
    Serial.println("Could not queue the record.");
  }
#else
  Serial.println("Copy secrets.h.example to secrets.h and enter your settings.");
#endif
}

void loop() {
  delay(100);
}
