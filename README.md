# NodexaCloud Arduino library

This lightweight ESP32 library sends typed variable batches to Nodexa over verified HTTPS and can receive device-specific variable updates over verified WSS. The ESP32's factory MAC address is always its device ID in account-secret mode. A cloud variable marked **Stream** is published by the backend when its value changes; the ESP32 does not need a separate MQTT client.

## Install

Copy `arduino/NodexaCloud` into your Arduino sketchbook `libraries` folder, then restart Arduino IDE. The library uses the ESP32 Arduino core's `WiFiClientSecure` and `HTTPClient`; it has no ArduinoJson dependency.

## Send your first record

1. Create a **List** variable called `hello_records` in your Nodexa project.
2. In Arduino IDE, open **File → Examples → NodexaCloud → FirstRecord**. Save a local copy and fill in `secrets.h` using the example template.
3. Upload the sketch. After the ESP32 connects, open **Devices → View records** to see the message.

The example queues a record once at startup and sends it in the background
when the connection is ready. Edit the `addPair()` lines to send your own
message or number.

## Device setup

Declare the client globally so it stays alive while its background tasks run.
Start the library once per boot from setup.

1. In the Nodexa console, open **Developers** and generate or copy the **Account Secret Key**.
2. Add your Wi-Fi credentials, account ID, project ID and account secret to your local `secrets.h` file. No root CA value is required.
3. Call `begin({WIFI_SSID, WIFI_PASSWORD, SECRET_KEY, ACCOUNT_ID, PROJECT_ID})` from `setup()`. The library identifies the device by its factory MAC and uses the account secret for writes and heartbeats.

The account secret is created for new accounts and is shown when first issued. The server stores only its hash. If you did not save it, use **Generate a new secret key** in the Developers tab to rotate it and display the replacement once. Rotation invalidates the previous key.

Account-secret mode sends `X-Nodexa-Secret`. This account-level key can write to the account's projects, so keep it private in firmware and rotate it if it is exposed.

## Built-in LED sample

Open the `BuiltInLedControl` example with the **ESP32S3 Dev Module** board selected. Create a Boolean variable with ID `esp32_led` in the project and enable **Stream**. The sample follows changes to that value on the board's RGB LED: `true` turns it on and `false` turns it off. It uses the selected board variant's `LED_BUILTIN` definition and adds no extra Arduino library dependency.

## Send values

In `setup()`, use the account secret and Wi-Fi settings from `secrets.h`:

```cpp
if (!nodexa.begin({WIFI_SSID, WIFI_PASSWORD, SECRET_KEY, ACCOUNT_ID, PROJECT_ID})) {
  Serial.println("Invalid Nodexa configuration.");
}
```

The factory MAC burned into the ESP32 is available through `nodexa.macAddress()` as 12 uppercase hexadecimal characters without separators. The API device ID uses that same MAC in lowercase without separators.

Use `nodexa.connected()` to check whether Wi-Fi is connected and Nodexa has accepted a heartbeat:

```cpp
if (nodexa.connected()) {
  Serial.println("Connected to Nodexa.");
} else {
  Serial.println("Connecting or recovering...");
}
```

`nodexa.loop()` remains as a compatibility alias. Use `nodexa.streamReady()` separately to check whether the real-time WebSocket is subscribed and ready. `nodexa.ready()` only means the configuration was accepted and the background manager task started.

Library diagnostics are off by default. After `Serial.begin()`, call `nodexa.debug(true)` before `begin()` to log Wi-Fi connection attempts, factory MAC, NTP, heartbeat responses, and real-time reconnects. Call `nodexa.debug(false)` to silence those diagnostics. The BuiltInLedControl example enables debug to show this startup sequence. Secrets and value payloads are never logged.

The library reconnects Wi-Fi every five seconds while disconnected. Failed heartbeats retry after three seconds; once Wi-Fi returns, Nodexa must accept a heartbeat before API writes or the WebSocket resume. The WebSocket task retries with backoff and resubscribes after reconnecting. Heartbeats run in a low-priority FreeRTOS task, so Arduino's `loop()` remains available for application code. `nodexa.write(batch)` waits for its HTTPS response, so call it from a dedicated FreeRTOS task if it must not block the sketch.

## Subscribe to a real-time variable

Enable **Stream** on any project variable, then register its callback before `begin()`. The library uses one device-scoped WebSocket in a background FreeRTOS task for up to 16 variable subscriptions. It keeps the connection open and resubscribes after disconnecting. By default, each subscription uses `InitialSync::ChangesOnly`: the server does not replay the stored value when the subscription starts, and sends changes made after the subscription is active. Use `InitialSync::LatestValue` to request the last saved value on startup. `InitialSync::Null` sends a null policy; the server treats null or a missing policy as `ChangesOnly`.

```cpp
void handleTemperature(double value) {
  Serial.printf("Temperature received in real time = %.2f\n", value);
}

void setup() {
  // Register the callback before calling nodexa.begin(...).
  nodexa.subscribe("temperature", handleTemperature); // ChangesOnly by default
}
```

To restore the saved value when a subscription starts, pass the policy explicitly:

```cpp
nodexa.subscribe("temperature", handleTemperature,
                 Nodexa::InitialSync::LatestValue);
```

To send a null policy explicitly, use `Nodexa::InitialSync::Null`; the server resolves it to `ChangesOnly`.

`ChangesOnly` also applies after a WebSocket reconnect. Values changed while the ESP32 was disconnected are not replayed automatically in this mode.

The callback type must match the variable type. Enable **Stream** on each subscribed variable.

| Cloud type | Arduino callback |
| --- | --- |
| Boolean | `subscribe(id, [](bool value) { ... })` |
| Int | `subscribe(id, [](int32_t value) { ... })` |
| Float | `subscribe(id, [](double value) { ... })` |
| String | `subscribe(id, [](const String& value) { ... })` |
| List | `subscribeList(id, [](const String& jsonArray) { ... })` |

## Publish on the real-time connection

For one value, use `nodexa.setInt(id, value)`, `setFloat`, `setBool`, or
`setString`. Call `enableRealtime()` before `begin()` for a publish-only
sketch, or register a subscription before `begin()`. Once `streamReady()` is
true, each call queues one value without waiting for the server:

```cpp
bool queued = nodexa.setBool("esp32_led", true,
    [](bool committed, const char* errorCode) {
      if (committed) Serial.println("Saved");
      else Serial.println(errorCode);
    });
if (!queued) Serial.println("Could not queue the value");
```

The optional callback reports the server result on the real-time task. A
`false` return means the request was not queued, for example because the
stream is disconnected or the queue is full. These calls do not buffer values
while offline. A disconnect before acknowledgement leaves the result unknown.

When `streamReady()` is true, queue a `Batch` for the same connection:

```cpp
Nodexa::Batch batch;
batch.setFloat("temperature", 24.75);
bool queued = nodexa.publish(batch, [](bool committed, const char* errorCode) {
  if (committed) Serial.println("Nodexa saved the batch.");
  else Serial.println(errorCode);
});
```

`publish()` returns whether the batch entered the local queue. The callback reports the server commit or a failure and runs on the same task as subscription callbacks. A disconnect or missing acknowledgement leaves the commit outcome unknown. The library does not replay an in-flight batch automatically. A changed Stream value can also arrive through its subscription, including when the same device changed it. Call `enableRealtime()` before `begin()` for a publish-only sketch. `write()` remains available for HTTPS batches.

## Variable definitions and deletion

Cloud variables are project-level definitions. Each ESP32 stores its own values for a definition; the console's Cloud variables list does not subscribe to device values. Renaming a definition changes its display name while keeping the firmware's variable ID.

Deleting a definition permanently removes its stored values for all devices in that project. Requests using a deleted ID return `variable_not_found`; deletion does not create the variable again. Active device subscriptions report `subscription_error` and close, and a rejected real-time publish reports `publish_error` with the same code. An already-transmitted network frame can still arrive after deletion.

The library prints stream errors to Serial Monitor. To handle them in your sketch, register a callback before `begin()`:

```cpp
nodexa.onError([](const char* code, const char* variableId) {
  // Runs on the background real-time task. Copy either string before retaining it.
  if (strcmp(code, "variable_not_found") == 0) {
    // Stop using this reference or update the sketch with an existing variable ID.
  }
});
```

HTTPS `write()` returns the HTTP status and JSON error in its `Response`; check `response.ok()` and `response.body`. Existing flashed firmware needs to be rebuilt and uploaded to use the new stream-error reporting callback.

## Device online status

### Queue JSON records while offline

Use `Nodexa::Json` to build a record from keys and values. The library handles
JSON quoting and escaping, and keeps numbers, Booleans, and nulls as their
original types.

```cpp
Nodexa::Json record;
record.addPair("transaction_id", "JDM4303");
record.addPair("amount", 10.00);
record.addPair("paid", true);
record.addPair("note", nullptr);

if (!nodexa.addItem("transactio_list", record)) {
  Serial.println("Could not queue the record.");
}
```

`addPair()` accepts text, integers, finite decimal numbers, Booleans, and
`nullptr`. Use each key once per record. A failed `addPair()` marks the record
invalid, so `addItem()` rejects it instead of sending a partial record.
Call `record.clear()` to start a fresh record. You can also pass a raw JSON
object string to `addItem()`.

The RAM queue starts automatically on the first `addItem` call after `begin()`.
To choose a different capacity, call `nodexa.enableOfflineQueue(Nodexa::Storage::RAM, 32)`
after `begin()` and before the first item. Use
`nodexa.addItem("transactio_list", json)` to queue one JSON object and
`nodexa.queuedItems()` to see the pending count. `addItem` returns false if
the queue is full, cannot be allocated, or the item is invalid. The connection task sends
items in order after the heartbeat succeeds and retries failures using a stable
operation ID, so a lost response does not duplicate a saved record.

The queue defaults to 16 records and can be configured for up to 64. Encoded
records share a 16 KiB payload budget, so large records may fill it sooner.
A reset or loss of power clears the RAM queue. A rejected item remains at the
front and blocks later items until the server accepts it. Enable `debug(true)`
to see its HTTP status. Size the queue for the device's available memory.

Call `nodexa.disableOfflineQueue()` to send subsequent `addItem` calls directly
over HTTPS. In direct mode `addItem` blocks until the request completes and
returns whether the server accepted it; it returns false while disconnected.
Items already queued continue to retry in the background. Call
`enableOfflineQueue(Nodexa::Storage::RAM)` to queue new items again.

Provide the firmware version when creating the client:

```cpp
Nodexa::Client nodexa("1.0.0");
```

The version is required and reported in the device heartbeat. An empty string
literal fails compilation. Invalid values such as blank strings are rejected
before `begin()`. Versions must contain 1�64 printable ASCII characters.
For managed OTA, use a numeric version such as `1.0.0` and increase it for
each release. `setFirmwareVersion()` can change it before `begin()`.

The library sends an authenticated heartbeat every 30 seconds by default. It uses `X-Nodexa-Secret` and does not create a variable value or add to transferred-byte totals. The Nodexa server marks the device **Online** while its last heartbeat or successful value write was received within 90 seconds; otherwise it shows **Offline**. The console refreshes device presence while the Devices view is open.

Each heartbeat also reports total and used allocatable heap bytes, combining
internal RAM and initialized PSRAM. Static program data is not included. On
ESP32 targets whose FreeRTOS runtime statistics use the ESP timer, it reports
CPU usage as an average across cores since the previous successful heartbeat.
The first heartbeat establishes the CPU sample baseline, so CPU usage appears
after the next successful heartbeat.

The heartbeat request is:

```text
POST {account_endpoint}/projects/{project_id}/devices/{device_id}/heartbeat
X-Nodexa-Secret: {account_secret_key}
Content-Type: application/json

{"firmware_version":"1.0.0","heap_total_bytes":327680,"heap_used_bytes":152000,"cpu_usage_percent":18}
```

`setHeartbeatInterval(30000)` can be used to change the interval to a value from 5000 to 60000 milliseconds. `lastHeartbeatStatus()` returns the last HTTP status from the background task (`0` means no attempt has completed yet). `heartbeat()` is available for an explicit request when needed.

```cpp
Nodexa::Batch batch;
batch.setBool("pump_running", true);
batch.setInt("wash_count", 8);
batch.setFloat("water_temperature", 24.75);
batch.setString("station_name", "Carwash 1");
batch.appendListItem("transactio_list",
                    "{\"transaction_id\":\"sample-001\","
                    "\"transaction_amount\":5.00,"
                    "\"timestamp_ms\":1795500000000}");

Nodexa::Response response = nodexa.write(batch);
if (!response.ok()) {
  Serial.printf("Nodexa write failed: HTTP %d %s\n",
                response.statusCode, response.body.c_str());
}
```

The batch supports up to 32 values and appended List items combined; the server accepts request bodies up to 16 KiB. Variable IDs must be lowercase letters, digits, or underscores, up to 20 characters. Each ID should appear once in a batch. `setJson` accepts a raw JSON value; `setListJson` accepts a JSON array and replaces that List value. `appendListItem` accepts one JSON object and appends it without reading the existing List. Use a timezone-aware ISO 8601 timestamp only when the device clock is synchronized.

In account-secret mode, the firmware sends a write to:

```text
POST {account_endpoint}/projects/{project_id}/devices/{device_id}/variables/batch
X-Nodexa-Secret: {account_secret_key}
Content-Type: application/json

{"values":{"sensor_1":0},"append":{"transactio_list":{"transaction_id":"sample-001","transaction_amount":5.00,"timestamp_ms":1795500000000}}}
```

The backend stores each device's values separately. A third-party website can read that device's values from:

```text
GET {account_endpoint}/projects/{project_id}/devices/{device_id}/variables
GET {account_endpoint}/projects/{project_id}/devices/{device_id}/variables/{variable_id}
```

To read append-only items from a List variable, use the paginated developer route with the signed-in owner's access token:

```text
GET {account_endpoint}/projects/{project_id}/devices/{device_id}/variables/{variable_id}/items?limit=50
Authorization: Bearer {developer_access_token}
```

The route returns newest items first and supports `limit` up to 100 plus the `next_cursor` from the previous response. Each item is returned separately; it does not build or transfer the whole history to the ESP32.

These GET routes use the account endpoint as their read credential. Anyone who learns that endpoint can read the account's device values. Keep it private; for browser apps, the website origin also needs CORS authorization. A backend relay is preferable when end users must not be able to inspect the endpoint.

## TLS and credential handling

The `ConnectionConfig` overload uses the ESP32 Arduino core's built-in root CA bundle to verify HTTPS certificates, so sketches do not need a `ROOT_CA` value. Certificate verification remains enabled. The library does not print credentials. Rotating the account secret invalidates it for every device using that account.

## Stage a firmware update over Wi-Fi

### Automatic checks

Open **AutomaticOTA** for the shortest managed OTA setup. Set the version of
each build when creating `Nodexa::Client nodexa("1.0.0")` and
create one updater next to your client:

```cpp
Nodexa::Client nodexa("1.0.0");
Nodexa::FirmwareUpdater ota(nodexa);

void loop() {
  ota.run();
  delay(20);
}
```

Call `ota.run()` regularly from `loop()` after your startup checks pass and
while the device can safely restart. It checks for the latest compatible
release every five minutes and retries errors after 30 seconds. The download
runs in a low-priority FreeRTOS task, so `loop()` keeps running. If Wi-Fi drops,
the download resumes from the last written byte while the ESP32 stays powered.
`ota.run()` restarts once the image is verified and the RAM JSON queue is empty.
After reboot, it confirms the installed release when you call it again after
your startup checks.

Use `ota.run(onProgress)` to receive percentage changes on the OTA task, or
read `ota.progress()` from your display code. For manual restart control, call
`ota.poll()` instead. It returns `true` when the verified firmware is ready to
boot. Use `ota.lastResult()` for the last OTA result or enable
`nodexa.debug(true)` to see OTA status in Serial Monitor.

### Manual checks

Open the **AirDropOTA** example to use Nodexa's managed API. Flash the first
version over USB with an OTA partition scheme. In **AirDrop OTA** in the console,
create a **Global OTA** or **Specific device OTA** draft. Upload a newer
application binary built for the same board and flash configuration, using the
board profile and flash size printed by the example. Activate the draft.
In Serial Monitor, type **u** to check for and stage the firmware and **r**
when you are ready to reboot. Keep the update code in each firmware version.

Set the running sketch's version when creating the client before
`begin()`. Once connected, check and stage an update with one call:

```cpp
auto result = nodexa.updateFirmware();
if (!result.ok()) {
  Serial.printf("OTA error: %s (%d)\n", result.error, result.detail);
} else if (result.readyToRestart) {
  Serial.println("Firmware ready. Restart when it is safe.");
} else {
  Serial.println("No update available.");
}
```

`updateFirmware()` uses the selected Arduino board's profile. For a custom
profile, call `updateFirmware("my-board-profile", nullptr)` and use the same
profile when uploading the binary. The server offers an eligible device release
first, then an eligible Global release in the project.

Call `ESP.restart()` when your device is idle. In the new sketch, call
`confirmFirmwareUpdate()` after startup checks and connection. The library
saves the release across reboot and retries its status reporting when you call
confirmation again after a network failure.

For sketches that need to inspect or approve an offer before downloading, use
`checkForFirmwareUpdate()` and `installFirmwareUpdate()` separately.

### Display download progress

```cpp
auto result = nodexa.updateFirmware([](uint8_t percent) {
  Serial.printf("Downloading firmware: %u%%\n", static_cast<unsigned>(percent));
});
```

The callback starts at 0 and reports changes in the whole percentage as bytes
are received and written. Keep it short because it runs on the task downloading
the firmware. The direct `stageFirmware(url, sha256, callback)` call supports
the same callback.

Use `nodexa.otaDownloadPercent()` to read the latest value from another task.
It retains the last percentage after a download ends. **100% means every byte
was received.** Check `result.ok()` before restarting to confirm the firmware
passed verification and was selected for the next boot.

### Stage from a trusted URL

Start with an ESP32 running a sketch that calls `stageFirmware()` when you
approve an update. Choose an Arduino partition scheme with two OTA app slots
and an OTA data partition. Export the **application** `.bin` for your exact
board and flash settings. Host it at a direct HTTPS URL that serves the file
with `Content-Length`, and get its 64-character SHA-256 from a trusted source.

Call this from your sketch when the device is connected and idle:

```cpp
void applyFirmware(const char* url, const char* sha256) {
  auto result = nodexa.stageFirmware(url, sha256);
  if (!result.ok()) {
    Serial.printf("Update failed: %s (%d)\n", result.error, result.detail);
    return;
  }

  Serial.println("Firmware verified. Restart when the device is ready.");
  // Call ESP.restart() at your application's safe restart point.
}
```

The call waits for the download and writes in 1 KiB chunks to the inactive
slot. It selects the new slot only after the SHA-256 and ESP32 image checks
pass. Your sketch controls when to restart. Let queued JSON records finish
sending before restarting, since pending records live in RAM.

Set the version in the client constructor in the new sketch so the next heartbeat reports the version actually running.
Post-boot health checks and rollback depend on your bootloader and Arduino
core configuration.

## BatchWrite example

Open **File → Examples → NodexaCloud → BatchWrite** in Arduino IDE. Save a local copy of the sketch, then copy `secrets.h.example` from the example folder to `secrets.h` beside the sketch. Fill in the Wi-Fi settings, Nodexa identifiers and account secret. Keep `secrets.h` out of source control.

Create a List variable with the ID `transactio_list` in your Nodexa project. Once the clock is synchronized, the sketch uses `millis()` to queue one sample every five seconds from `loop()`. Each sample contains a random transaction ID, an amount from 5.00 to 50.00, and an epoch timestamp in milliseconds. Nodexa sends the records from its background queue task.

BatchWrite prints the transaction ID and pending count when a record enters the queue. With `nodexa.debug(true)`, the library also logs when the server saves it or when a request needs to retry. In the console, open **Devices → View records** to see saved records.

`setListJson` replaces the current stored list for that device. `appendListItem` appends one JSON object as a separate record without sending or rewriting records already stored. If Stream is enabled, the developer project stream receives an append event containing that item.
