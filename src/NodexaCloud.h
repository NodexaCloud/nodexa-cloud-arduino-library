#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <atomic>
#include <cstddef>
#include <memory>
#include <type_traits>

namespace Nodexa {

enum class Storage : uint8_t { RAM };

static const uint8_t kMaxBatchVariables = 32;
static const size_t kMaxRequestBytes = 16 * 1024;

struct Response {
  int statusCode = 0;
  String body;

  bool ok() const { return statusCode >= 200 && statusCode < 300; }
};

struct OtaResult {
  const char* error = nullptr;
  int detail = 0;
  bool readyToRestart = false;

  bool ok() const { return error == nullptr; }
};

struct FirmwareUpdate {
  String id;
  String version;
  String url;
  String sha256;
  size_t sizeBytes = 0;

  bool available() const { return id.length() != 0; }
};

class Json {
 public:
  bool addPair(const char* key, const char* value);
  bool addPair(const char* key, const String& value);
  bool addPair(const char* key, const __FlashStringHelper* value);
  bool addPair(const char* key, bool value);
  bool addPair(const char* key, double value);
  bool addPair(const char* key, std::nullptr_t);

  template <typename T>
  typename std::enable_if<std::is_integral<T>::value, bool>::type
  addPair(const char* key, T value) {
    return std::is_signed<T>::value
        ? addSigned(key, static_cast<int64_t>(value))
        : addUnsigned(key, static_cast<uint64_t>(value));
  }

  void clear();
  bool valid() const { return _valid; }

 private:
  friend class Client;
  bool addSigned(const char* key, int64_t value);
  bool addUnsigned(const char* key, uint64_t value);
  bool appendPair(const char* key, const char* value, size_t length,
                  bool quoteValue);
  bool invalidate() {
    _valid = false;
    return false;
  }

  String _object;
  bool _valid = true;
};

class Batch {
 public:
  Batch();

  void clear();
  uint8_t size() const { return _count + _appendCount; }
  bool empty() const { return _count == 0 && _appendCount == 0; }
  bool valid() const { return _valid; }

  bool setBool(const char* variableId, bool value);
  bool setInt(const char* variableId, int64_t value);
  bool setFloat(const char* variableId, double value);
  bool setString(const char* variableId, const String& value);
  bool setJson(const char* variableId, const String& jsonValue);
  bool setListJson(const char* variableId, const String& jsonArray);
  bool appendListItem(const char* variableId, const String& jsonObject);

 private:
  friend class Client;

  bool appendValue(const char* variableId, const char* value, size_t length,
                   bool quote = false, bool listItem = false);
  String toPayload(const char* timestamp, uint32_t requestId = 0) const;
  static bool validVariableId(const char* variableId);
  static String quoteJson(const char* value);
  static String quoteJson(const String& value);

  String _values;
  String _appendValues;
  // Scalar offsets grow from the front and append offsets from the back.
  uint16_t _keyOffsets[kMaxBatchVariables];
  uint8_t _count;
  uint8_t _appendCount;
  bool _valid;
};

struct ConnectionConfig {
  const char* ssid;
  const char* password;
  const char* secretKey;  // Account secret from the Nodexa Developers tab.
  // Supply both IDs here or configure them separately before begin().
  const char* accountId = nullptr;
  const char* projectId = nullptr;
};

// Controls whether a subscription replays the last saved value when it starts.
enum class InitialSync : uint8_t {
  ChangesOnly,
  LatestValue,
  // Serializes as JSON null; the server default is ChangesOnly.
  Null
};

class Client {
 public:
  using IntCallback = void (*)(int32_t value);
  using BoolCallback = void (*)(bool value);
  using FloatCallback = void (*)(double value);
  using TextCallback = void (*)(const String& value);
  using PublishCallback = void (*)(bool committed, const char* errorCode);
  using ErrorCallback = void (*)(const char* errorCode, const char* variableId);
  using OtaProgressCallback = void (*)(uint8_t percent);

  template <size_t N>
  explicit Client(const char (&firmwareVersion)[N]) : Client() {
    static_assert(N > 1, "Nodexa firmware version must not be empty");
    setFirmwareVersion(firmwareVersion);
  }

  // Keep the Client alive for the sketch's lifetime, normally as a global.
  // Configure once before begin(). Restart the ESP32 to change credentials.
  bool setAccount(const String& accountId);
  bool setProject(const String& projectId);
  // The account-secret mode always uses the factory eFuse MAC as the device ID.
  bool setAccountSecret(const String& secretKey);
  // Set before begin. The version must be nonempty.
  bool setFirmwareVersion(const char* version);
  bool begin(const ConnectionConfig& config);
  bool begin(const ConnectionConfig& config, const String& accountId,
             const String& projectId);
  // Diagnostics are silent by default. Enable after Serial.begin().
  // Credentials and payload values are never written to the debug log.
  void debug(bool enabled) { _debug.store(enabled); }
  bool debug() const { return _debug.load(); }
  // Factory base MAC as 12 uppercase hexadecimal characters without separators.
  String macAddress() const { return _macAddress; }
  // Hardware reported by the ESP32. OTA storage is the next update slot.
  String chipModel() const;
  uint16_t chipRevision() const;
  size_t otaStorageBytes() const;

  // True when Wi-Fi is up and Nodexa has accepted a heartbeat.
  // Use streamReady() separately to check the real-time WebSocket.
  bool connected() const;
  // Kept as a compatibility alias for connected().
  bool loop() const;

  // The library sends heartbeat requests in its own low-priority RTOS task.
  // The server considers a device online for 90 seconds after its last request.
  bool setHeartbeatInterval(uint32_t intervalMs);
  uint32_t heartbeatInterval() const { return _heartbeatIntervalMs; }
  int lastHeartbeatStatus() const { return _lastHeartbeatStatus; }
  Response heartbeat();

  // Register up to 16 streamed variable IDs before begin().
  // Callbacks run in the library's FreeRTOS task.
  bool subscribe(const char* variableId, IntCallback callback,
                 InitialSync initialSync = InitialSync::ChangesOnly);
  bool subscribe(const char* variableId, BoolCallback callback,
                 InitialSync initialSync = InitialSync::ChangesOnly);
  bool subscribe(const char* variableId, FloatCallback callback,
                 InitialSync initialSync = InitialSync::ChangesOnly);
  bool subscribe(const char* variableId, TextCallback callback,
                 InitialSync initialSync = InitialSync::ChangesOnly);
  bool subscribeList(const char* variableId, TextCallback callback,
                     InitialSync initialSync = InitialSync::ChangesOnly);
  bool onInt(const char* variableId, IntCallback callback,
             InitialSync initialSync = InitialSync::ChangesOnly);

  // Optional for publish-only sketches. Call before begin().
  void enableRealtime() { if (!_ready) _useRealtime = true; }
  bool streamReady() const { return _streamReady.load(); }
  // Configure before begin(); called on the realtime task. Copy strings if retained.
  // Errors print to Serial only when debug(true) is enabled.
  void onError(ErrorCallback callback) { _errorCallback.store(callback); }
  int lastStreamTicketStatus() const { return _lastStreamTicketStatus.load(); }
  int lastStreamUpgradeStatus() const { return _lastStreamUpgradeStatus.load(); }
  // Queues a batch without waiting for the network. The optional callback
  // runs on the receive task after a server acknowledgement or failure.
  bool publish(const Batch& batch, PublishCallback callback = nullptr);

  // Queue a single value on the real-time connection. True means queued;
  // the optional callback reports the server's acknowledgement.
  bool setInt(const char* variableId, int64_t value, PublishCallback callback = nullptr);
  bool setFloat(const char* variableId, double value, PublishCallback callback = nullptr);
  bool setBool(const char* variableId, bool value, PublishCallback callback = nullptr);
  bool setString(const char* variableId, const String& value, PublishCallback callback = nullptr);

  void setTimeout(uint16_t timeoutMs) { _timeoutMs = timeoutMs; }
  // True when begin() accepted the configuration and started the manager task.
  // Use connected() to check live server connectivity.
  bool ready() const { return _ready; }

  // timestamp may be null. If supplied, use timezone-aware ISO 8601.
  Response write(const Batch& batch, const char* timestamp = nullptr);

  // Downloads and verifies firmware for the next restart. Call when idle.
  OtaResult stageFirmware(const char* httpsUrl, const char* sha256Hex);
  OtaResult stageFirmware(const char* httpsUrl, const char* sha256Hex,
                          OtaProgressCallback onProgress);
  // Check and stage a release in one call. Uses the Arduino board profile.
  OtaResult updateFirmware(OtaProgressCallback onProgress = nullptr);
  OtaResult updateFirmware(const char* board, OtaProgressCallback onProgress);
  // Use the same board profile when uploading the release in the console.
  OtaResult checkForFirmwareUpdate(FirmwareUpdate& update, const char* board);
  OtaResult installFirmwareUpdate(const FirmwareUpdate& update);
  OtaResult installFirmwareUpdate(const FirmwareUpdate& update,
                                  OtaProgressCallback onProgress);
  // Latest download percentage. Safe to read from another task.
  uint8_t otaDownloadPercent() const {
    return _otaDownloadPercent.load(std::memory_order_relaxed);
  }
  // Call after the new firmware passes the application's startup checks.
  OtaResult confirmFirmwareUpdate();

  // JSON List items use a RAM queue by default with a 16 KiB payload budget.
  // Direct mode blocks on HTTPS.
  bool enableOfflineQueue(uint8_t capacity = 16);
  bool enableOfflineQueue(Storage storage, uint8_t capacity = 16);
  void disableOfflineQueue() { _offlineQueueEnabled.store(false); }
  bool addItem(const char* variableId, const String& jsonObject);
  bool addItem(const char* variableId, const Json& record);
  size_t queuedItems() const;

  String batchEndpoint() const { return _endpoint; }

 private:
  Client();
  OtaResult stageFirmwareImage(const char* url, const char* sha256,
                               size_t expectedBytes, OtaProgressCallback onProgress,
                               const FirmwareUpdate* managedOffer, const char* board);
  OtaResult installFirmwareUpdate(const FirmwareUpdate& update,
                                  OtaProgressCallback onProgress, const char* board);
  std::atomic<uint8_t> _otaDownloadPercent{0};
  OtaResult postOta(const char* action, const String& payload, String* response = nullptr);
  OtaResult reportOta(const String& id, const char* state);
  String otaScope() const;
  QueueHandle_t getOrCreateOfflineQueue(uint8_t capacity);
  bool sendQueuedItem(QueueHandle_t queue, String& payload);
  Response postBatch(String& payload, bool readBody, TickType_t waitForTransport);
  std::atomic<QueueHandle_t> _offlineQueue{nullptr};
  std::atomic<bool> _offlineQueueEnabled{true};
  std::atomic<size_t> _offlineBytes{0};
  static void heartbeatTaskEntry(void* argument);
  void heartbeatTask();
  Response sendHeartbeat(bool readBody);
  bool syncClockWithNtpFallbacks();
  bool syncClockFromTimeApi();
  bool startHeartbeatTask();
  static void controlsTaskEntry(void* argument);
  void controlsTask();
  bool startControlsTask();
  bool openPublishWakeSocket();
  void wakeControlsTask();
  bool fetchControlsTicket(String& ticket);
  bool sendSubscriptions();
  bool connectControlsSocket();
  bool sendControlsFrame(uint8_t opcode, const uint8_t* payload,
                         size_t payloadLength);
  static const size_t kInlineFrameBytes = 256;
  bool readControlsFrame(uint8_t& opcode, char (&small)[kInlineFrameBytes],
                         std::unique_ptr<char[]>& large, size_t& payloadLength);
  struct PublishRequest {
    String payload;
    PublishCallback callback;
    uint32_t id;
  };
  bool handleControlsMessage(const char* payload, std::unique_ptr<char[]>& large,
                             bool& subscribed,
                             std::unique_ptr<PublishRequest>& pending);
  void releasePublishPayload(PublishRequest& request);
  enum class SubscriptionKind : uint8_t { Int, Bool, Float, Text, List };
  struct Subscription {
    char id[21];
    union {
      IntCallback onInt = nullptr;
      BoolCallback onBool;
      FloatCallback onFloat;
      TextCallback onText;
    };
    InitialSync initialSync = InitialSync::ChangesOnly;
    SubscriptionKind kind = SubscriptionKind::Int;
  };
  Subscription* addSubscription(
      const char* variableId,
      InitialSync initialSync = InitialSync::ChangesOnly);
  static String readFactoryMacAddress();
  static bool isTenDigitProjectId(const String& projectId);
  void reportStreamError(const char* code, const char* variableId);
  std::atomic<ErrorCallback> _errorCallback{nullptr};
  WiFiClientSecure _httpTls;
  std::unique_ptr<WiFiClientSecure> _controlsTls;
  SemaphoreHandle_t _httpMutex;
  TaskHandle_t _heartbeatTaskHandle;
  TaskHandle_t _controlsTaskHandle;
  QueueHandle_t _publishQueue;
  std::atomic<int> _publishWakeSocket;
  uint16_t _publishWakePort;
  std::atomic<uint32_t> _publishSequence{0};
  std::atomic<size_t> _publishBytes{0};
  bool _useRealtime;
  std::atomic<bool> _debug;
  std::atomic<bool> _streamReady;
  std::atomic<bool> _heartbeatReady;
  std::atomic<bool> _timeSyncStarted;
  std::atomic<bool> _timeApiSyncAttempted;
  std::atomic<int> _lastStreamTicketStatus;
  std::atomic<int> _lastStreamUpgradeStatus;
  String _accountId;
  String _projectId;
  String _macAddress;
  String _accountSecret;
  String _firmwareVersion;
  String _endpoint;
  String _heartbeatEndpoint;
  String _controlsTicketEndpoint;
  std::unique_ptr<Subscription[]> _subscriptions;
  uint8_t _subscriptionCapacity = 0;
  uint8_t _subscriptionCount;
  std::atomic<uint16_t> _timeoutMs;
  std::atomic<uint32_t> _heartbeatIntervalMs;
  uint64_t _cpuSampleTimestampUs;
  uint64_t _cpuIdleRuntimeCore0;
  uint64_t _cpuIdleRuntimeCore1;
  uint32_t _lastNtpFallbackAttemptMs;
  uint32_t _lastWifiRetryAttemptMs;
  bool _ntpFallbackAttempted;
  std::atomic<int> _lastHeartbeatStatus;
  std::atomic<bool> _ready;
  std::atomic_flag _starting = ATOMIC_FLAG_INIT;
};

class FirmwareUpdater {
 public:
  // Keep the updater alive for the sketch's lifetime, normally as a global.
  explicit FirmwareUpdater(Client& client) : _client(client) {}

  // Call from loop() after startup checks and when a restart is safe.
  // Restarts once the update is verified and the RAM JSON queue is empty.
  void run(Client::OtaProgressCallback onProgress = nullptr);

  // Call regularly from loop() after startup checks. Automatic downloads run
  // in a low-priority RTOS task, so the Arduino loop can keep running.
  // Returns true when an update is ready and leaves the restart to the sketch.
  bool poll(Client::OtaProgressCallback onProgress = nullptr);
  // Reports bytes downloaded as a percentage, independent of verification.
  uint8_t progress() const { return _client.otaDownloadPercent(); }
  OtaResult lastResult() const;

 private:
  enum class WorkerState : uint8_t { Idle, Running, Finished, ReadyToRestart };
  static void workerTaskEntry(void* argument);
  void runWorkerTask();
  void setLastResult(const OtaResult& result);

  Client& _client;
  mutable portMUX_TYPE _resultMux = portMUX_INITIALIZER_UNLOCKED;
  OtaResult _lastResult;
  std::atomic<WorkerState> _workerState{WorkerState::Idle};
  Client::OtaProgressCallback _progressCallback = nullptr;
  uint32_t _lastAttemptMs = 0;
  uint32_t _waitMs = 0;
  bool _bootConfirmed = false;
};

}  // namespace Nodexa
