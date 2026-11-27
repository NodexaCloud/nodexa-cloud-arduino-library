#include "NodexaCloud.h"
#include "NodexaHttpResponse.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <cJSON.h>
#include <lwip/sockets.h>
#include <esp_system.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <sdkconfig.h>
#if defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) && \
    CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && \
    (!defined(CONFIG_FREERTOS_SMP) || !CONFIG_FREERTOS_SMP) && \
    defined(CONFIG_FREERTOS_NUMBER_OF_CORES) && \
    defined(CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER) && \
    CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
#include <freertos/idf_additions.h>
#define NODEXA_HAS_CPU_RUNTIME_STATS 1
#else
#define NODEXA_HAS_CPU_RUNTIME_STATS 0
#endif
#include <sys/time.h>
#include <time.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <new>
#include <algorithm>
#include <unistd.h>
#include <utility>

namespace Nodexa {

namespace {

static const char kApiHost[] = "api.nodexa.it.com";
static const char kCurrentTimeEndpoint[] =
    "https://current-time.genservice.xyz";
static const char* const kNtpFallbackServers[] = {
    "pool.ntp.org", "time.google.com", "time.cloudflare.com",
    "time.windows.com", "time.nist.gov"};
static const char kControlsPath[] = "/api/v1/device-controls/stream";
static const char kWebSocketGuid[] =
    "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
static const size_t kMaxControlsFrameBytes = 32 * 1024;
static const uint32_t kPublishAckTimeoutMs = 15000;
static const uint32_t kNtpResponseTimeoutMs = 1200;
static const uint32_t kNtpRetryIntervalMs = 5000;
static const uint32_t kNtpUnixEpochOffset = 2208988800UL;
static const uint32_t kHeartbeatRetryIntervalMs = 3000;
static const uint32_t kWifiRetryIntervalMs = 5000;
static const uint32_t kQueueRetryIntervalMs = 5000;
static const uint8_t kMaxSubscriptions = 16;
static const size_t kOfflineQueueBytes = kMaxRequestBytes;
static const size_t kPublishQueueBytes = kMaxRequestBytes + 48;

void trimJson(const char*& value, size_t& length) {
  while (length && isspace(static_cast<unsigned char>(*value))) {
    ++value;
    --length;
  }
  while (length && isspace(static_cast<unsigned char>(value[length - 1]))) {
    --length;
  }
}

bool reserveQueueBytes(std::atomic<size_t>& used, size_t bytes, size_t limit) {
  size_t current = used.load(std::memory_order_relaxed);
  while (current <= limit && bytes <= limit - current) {
    if (used.compare_exchange_weak(current, current + bytes,
                                   std::memory_order_relaxed)) return true;
  }
  return false;
}

class MutexLock {
 public:
  MutexLock(SemaphoreHandle_t mutex, TickType_t wait)
      : _mutex(mutex && xSemaphoreTake(mutex, wait) == pdTRUE ? mutex : nullptr) {}
  ~MutexLock() { if (_mutex) xSemaphoreGive(_mutex); }
  explicit operator bool() const { return _mutex != nullptr; }
  MutexLock(const MutexLock&) = delete;
  MutexLock& operator=(const MutexLock&) = delete;

 private:
  SemaphoreHandle_t _mutex;
};

struct BeginGuard {
  std::atomic_flag& flag;
  ~BeginGuard() { flag.clear(); }
};

const char* jsonEscape(unsigned char c) {
  switch (c) {
    case '"': return "\\\"";
    case '\\': return "\\\\";
    case '\b': return "\\b";
    case '\f': return "\\f";
    case '\n': return "\\n";
    case '\r': return "\\r";
    case '\t': return "\\t";
    default: return nullptr;
  }
}

size_t quotedJsonSize(const char* value, size_t length) {
  size_t size = 2;
  for (size_t i = 0; i < length; ++i) {
    const auto c = static_cast<unsigned char>(value[i]);
    size += jsonEscape(c) ? 2 : (c < 0x20 ? 6 : 1);
  }
  return size;
}

bool appendQuotedJson(String& output, const char* value, size_t length) {
  static const char hex[] = "0123456789abcdef";
  if (!output.concat('"')) return false;
  for (size_t i = 0; i < length; ++i) {
    const auto c = static_cast<unsigned char>(value[i]);
    if (const auto escape = jsonEscape(c)) {
      if (!output.concat(escape, 2)) return false;
    } else if (c < 0x20) {
      const char escaped[] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 0x0f]};
      if (!output.concat(escaped, sizeof(escaped))) return false;
    } else if (!output.concat(static_cast<char>(c))) {
      return false;
    }
  }
  return output.concat('"');
}

sockaddr_in loopbackAddress(uint16_t port) {
  sockaddr_in address{};
#if LWIP_SOCKET_HAVE_SA_LEN
  address.sin_len = sizeof(address);
#endif
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  return address;
}

bool waitForControlData(WiFiClientSecure& client, uint32_t timeoutMs,
                        int wakeSocket = -1, bool* woke = nullptr) {
  const uint32_t started = millis();
  while (millis() - started < timeoutMs) {
    if (client.available() > 0) return true;
    const int socket = client.fd();
    if (socket < 0 || socket >= FD_SETSIZE || !client.connected()) return false;
    const uint32_t elapsed = millis() - started;
    if (elapsed >= timeoutMs) return false;
    const uint32_t remaining = timeoutMs - elapsed;
    timeval timeout{};
    timeout.tv_sec = remaining / 1000;
    timeout.tv_usec = (remaining % 1000) * 1000;
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(socket, &readable);
    if (wakeSocket >= 0) FD_SET(wakeSocket, &readable);
    const int lastSocket = socket > wakeSocket ? socket : wakeSocket;
    if (select(lastSocket + 1, &readable, nullptr, nullptr, &timeout) <= 0) {
      return false;
    }
    if (wakeSocket >= 0 && FD_ISSET(wakeSocket, &readable)) {
      char notification;
      recv(wakeSocket, &notification, sizeof(notification), MSG_DONTWAIT);
      if (woke) *woke = true;
      return true;
    }
  }
  return false;
}

bool readControlByte(WiFiClientSecure& client, uint8_t& value,
                     uint32_t timeoutMs) {
  if (!waitForControlData(client, timeoutMs)) return false;
  const int next = client.read();
  if (next < 0) return false;
  value = static_cast<uint8_t>(next);
  return true;
}

bool readControlLine(WiFiClientSecure& client, String& line,
                     uint32_t started, uint32_t timeoutMs) {
  line.remove(0);
  for (;;) {
    const uint32_t elapsed = millis() - started;
    if (elapsed >= timeoutMs) return false;
    uint8_t byte;
    if (!readControlByte(client, byte, timeoutMs - elapsed)) return false;
    if (byte == '\n') return true;
    if (byte == 0 || line.length() >= 1024 ||
        !line.concat(static_cast<char>(byte))) {
      return false;
    }
  }
}

bool encodeBase64(const uint8_t* input, size_t inputLength, char* output,
                  size_t outputCapacity) {
  size_t outputLength = 0;
  if (mbedtls_base64_encode(
          reinterpret_cast<unsigned char*>(output), outputCapacity - 1,
          &outputLength, input, inputLength) != 0 ||
      outputLength >= outputCapacity) {
    return false;
  }
  output[outputLength] = '\0';
  return true;
}

}  // namespace

Batch::Batch() : _count(0), _appendCount(0), _valid(true) {}

void Batch::clear() {
  _values = "";
  _appendValues = "";
  _count = 0;
  _appendCount = 0;
  _valid = true;
}

bool Batch::setBool(const char* variableId, bool value) {
  return value ? appendValue(variableId, "true", 4)
               : appendValue(variableId, "false", 5);
}

bool Batch::setInt(const char* variableId, int64_t value) {
  char number[24];
  const int length = snprintf(number, sizeof(number), "%lld",
                              static_cast<long long>(value));
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(number)) {
    _valid = false;
    return false;
  }
  return appendValue(variableId, number, length);
}

bool Batch::setFloat(const char* variableId, double value) {
  char number[32];
  const int length = isfinite(value)
      ? snprintf(number, sizeof(number), "%.15g", value) : 0;
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(number)) {
    _valid = false;
    return false;
  }
  return appendValue(variableId, number, length);
}

bool Batch::setString(const char* variableId, const String& value) {
  return appendValue(variableId, value.c_str(), value.length(), true);
}

bool Batch::setJson(const char* variableId, const String& jsonValue) {
  const char* value = jsonValue.c_str();
  size_t length = jsonValue.length();
  trimJson(value, length);
  return appendValue(variableId, value, length);
}

bool Batch::setListJson(const char* variableId, const String& jsonArray) {
  const char* value = jsonArray.c_str();
  size_t length = jsonArray.length();
  trimJson(value, length);
  if (length < 2 || value[0] != '[' || value[length - 1] != ']') {
    _valid = false;
    return false;
  }
  return appendValue(variableId, value, length);
}

bool Batch::appendListItem(const char* variableId, const String& jsonObject) {
  const char* value = jsonObject.c_str();
  size_t length = jsonObject.length();
  trimJson(value, length);
  if (length < 2 || value[0] != '{' || value[length - 1] != '}') {
    _valid = false;
    return false;
  }
  return appendValue(variableId, value, length, false, true);
}

bool Batch::appendValue(const char* variableId, const char* value, size_t length,
                        bool quote, bool listItem) {
  if (!_valid || !validVariableId(variableId) || (!quote && length == 0) ||
      length > kMaxRequestBytes || size() >= kMaxBatchVariables) {
    _valid = false;
    return false;
  }

  char key[24];
  const size_t keyLength = snprintf(key, sizeof(key), "\"%s\":", variableId);
  for (uint8_t i = 0; i < _count; ++i) {
    if (strncmp(_values.c_str() + _keyOffsets[i], key, keyLength) == 0) {
      _valid = false;
      return false;
    }
  }
  for (uint8_t i = 0; i < _appendCount; ++i) {
    const size_t offset = _keyOffsets[kMaxBatchVariables - 1 - i];
    if (strncmp(_appendValues.c_str() + offset, key, keyLength) == 0) {
      _valid = false;
      return false;
    }
  }

  String& fields = listItem ? _appendValues : _values;
  uint8_t& count = listItem ? _appendCount : _count;
  const size_t valueBytes = quote ? quotedJsonSize(value, length) : length;
  const size_t addedBytes = keyLength + valueBytes + (count != 0);
  if (_values.length() + _appendValues.length() + addedBytes + 64 >
          kMaxRequestBytes ||
      !fields.reserve(fields.length() + addedBytes) ||
      (count != 0 && !fields.concat(','))) {
    _valid = false;
    return false;
  }
  const size_t index = listItem ? kMaxBatchVariables - 1 - count : count;
  _keyOffsets[index] = fields.length();
  if (!fields.concat(key, keyLength) ||
      !(quote ? appendQuotedJson(fields, value, length)
              : fields.concat(value, length))) {
    _valid = false;
    return false;
  }
  ++count;
  return true;
}

String Batch::toPayload(const char* timestamp, uint32_t requestId) const {
  if (!_valid || (_count == 0 && _appendCount == 0)) return "";
  constexpr char values[] = "\"values\":{";
  constexpr char append[] = ",\"append\":{";
  constexpr char timeField[] = ",\"timestamp\":";
  char header[48] = "{";
  if (requestId != 0) {
    snprintf(header, sizeof(header), "{\"type\":\"publish\",\"request_id\":%lu,",
             static_cast<unsigned long>(requestId));
  }
  const size_t timeLength = timestamp ? strlen(timestamp) : 0;
  if (timeLength > kMaxRequestBytes) return "";
  const size_t timeBytes = timeLength ? quotedJsonSize(timestamp, timeLength) : 0;
  const size_t headerLength = strlen(header);
  const size_t payloadBytes = headerLength + sizeof(values) - 1 + _values.length() + 2 +
      (_appendCount ? sizeof(append) - 1 + _appendValues.length() + 1 : 0) +
      (timeLength ? sizeof(timeField) - 1 + timeBytes : 0);
  if (payloadBytes > kMaxRequestBytes + headerLength - 1) return "";
  String payload;
  if (!payload.reserve(payloadBytes) ||
      !payload.concat(header, headerLength) || !payload.concat(values) ||
      (_values.length() && !payload.concat(_values)) ||
      !payload.concat('}')) return "";
  if (_appendCount > 0) {
    if (!payload.concat(append) || !payload.concat(_appendValues) ||
        !payload.concat('}')) return "";
  }
  if (timeLength && (!payload.concat(timeField) ||
                     !appendQuotedJson(payload, timestamp, timeLength))) {
    return "";
  }
  if (!payload.concat('}')) return "";
  return payload;
}

bool Batch::validVariableId(const char* variableId) {
  if (variableId == nullptr) return false;
  const size_t length = strlen(variableId);
  if (length == 0 || length > 20) return false;
  for (size_t i = 0; i < length; ++i) {
    const char c = variableId[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
      return false;
    }
  }
  return true;
}

String Batch::quoteJson(const char* value) {
  if (!value) value = "";
  const size_t length = strlen(value);
  String quoted;
  if (!quoted.reserve(quotedJsonSize(value, length)) ||
      !appendQuotedJson(quoted, value, length)) return "";
  return quoted;
}

String Batch::quoteJson(const String& value) {
  String quoted;
  if (!quoted.reserve(quotedJsonSize(value.c_str(), value.length())) ||
      !appendQuotedJson(quoted, value.c_str(), value.length())) return "";
  return quoted;
}

bool Json::addPair(const char* key, const char* value) {
  return value ? appendPair(key, value, strlen(value), true)
               : addPair(key, nullptr);
}

bool Json::addPair(const char* key, const String& value) {
  return appendPair(key, value.c_str(), value.length(), true);
}

bool Json::addPair(const char* key, const __FlashStringHelper* value) {
  // ESP32 program memory is directly addressable.
  return addPair(key, reinterpret_cast<const char*>(value));
}

bool Json::addPair(const char* key, bool value) {
  return value ? appendPair(key, "true", 4, false)
               : appendPair(key, "false", 5, false);
}

bool Json::addPair(const char* key, std::nullptr_t) {
  return appendPair(key, "null", 4, false);
}

bool Json::addSigned(const char* key, int64_t value) {
  char number[24];
  const int length = snprintf(number, sizeof(number), "%lld",
                              static_cast<long long>(value));
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(number)) return invalidate();
  return appendPair(key, number, length, false);
}

bool Json::addUnsigned(const char* key, uint64_t value) {
  char number[24];
  const int length = snprintf(number, sizeof(number), "%llu",
                              static_cast<unsigned long long>(value));
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(number)) return invalidate();
  return appendPair(key, number, length, false);
}

bool Json::addPair(const char* key, double value) {
  if (!isfinite(value)) return invalidate();
  char number[32];
  const int length = snprintf(number, sizeof(number), "%.15g", value);
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(number)) return invalidate();
  return appendPair(key, number, length, false);
}

bool Json::appendPair(const char* key, const char* value, size_t length,
                      bool quoteValue) {
  if (!_valid || !key || length > kMaxRequestBytes) return invalidate();
  const size_t keyLength = strlen(key);
  if (keyLength > kMaxRequestBytes) return invalidate();

  const bool first = _object.length() == 0;
  const size_t encodedValue = quoteValue ? quotedJsonSize(value, length) : length;
  const size_t bytes = (first ? 1 : _object.length()) +
                       quotedJsonSize(key, keyLength) + encodedValue + 2;
  if (bytes > kMaxRequestBytes || !_object.reserve(bytes)) return invalidate();

  if (!first) _object.remove(_object.length() - 1);
  if (!_object.concat(first ? '{' : ',') ||
      !appendQuotedJson(_object, key, keyLength) || !_object.concat(':')) {
    return invalidate();
  }
  const bool added = quoteValue ? appendQuotedJson(_object, value, length)
                                : _object.concat(value, length);
  if (!added || !_object.concat('}')) return invalidate();
  return true;
}

void Json::clear() {
  _object.remove(0);
  _valid = true;
}

Client::Client()
    : _httpMutex(nullptr),
      _heartbeatTaskHandle(nullptr),
      _controlsTaskHandle(nullptr),
      _publishQueue(nullptr),
      _publishWakeSocket(-1),
      _publishWakePort(0),
      _useRealtime(false),
      _debug(false),
      _streamReady(false),
      _heartbeatReady(false),
      _timeSyncStarted(false),
      _timeApiSyncAttempted(false),
      _lastStreamTicketStatus(0),
      _lastStreamUpgradeStatus(0),
      _subscriptionCount(0),
      _timeoutMs(10000),
      _heartbeatIntervalMs(30000),
      _cpuSampleTimestampUs(0),
      _cpuIdleRuntimeCore0(0),
      _cpuIdleRuntimeCore1(0),
      _lastNtpFallbackAttemptMs(0),
      _lastWifiRetryAttemptMs(0),
      _ntpFallbackAttempted(false),
      _lastHeartbeatStatus(0),
      _ready(false) {}

bool Client::setAccount(const String& accountId) {
  if (_ready) return false;
  _accountId = "";
  if (accountId.length() == 0 || accountId.length() > 15) return false;
  for (size_t i = 0; i < accountId.length(); ++i) {
    const char c = accountId[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9'))) return false;
  }
  _accountId = accountId;
  return _accountId == accountId;
}

bool Client::setProject(const String& projectId) {
  if (_ready) return false;
  _projectId = "";
  if (!isTenDigitProjectId(projectId)) return false;
  _projectId = projectId;
  return _projectId == projectId;
}

bool Client::setAccountSecret(const String& secretKey) {
  if (_ready) return false;
  _accountSecret = "";
  if (secretKey.length() < 32 || secretKey.length() > 256) return false;
  _accountSecret = secretKey;
  return _accountSecret == secretKey;
}

bool Client::onInt(const char* variableId, IntCallback callback,
                   InitialSync initialSync) {
  return subscribe(variableId, callback, initialSync);
}

bool Client::subscribe(const char* variableId, IntCallback callback,
                      InitialSync initialSync) {
  auto* subscription = callback ? addSubscription(variableId, initialSync) : nullptr;
  if (!subscription) return false;
  subscription->onInt = callback;
  subscription->kind = SubscriptionKind::Int;
  return true;
}

bool Client::subscribe(const char* variableId, BoolCallback callback,
                      InitialSync initialSync) {
  auto* subscription = callback ? addSubscription(variableId, initialSync) : nullptr;
  if (!subscription) return false;
  subscription->onBool = callback;
  subscription->kind = SubscriptionKind::Bool;
  return true;
}

bool Client::subscribe(const char* variableId, FloatCallback callback,
                      InitialSync initialSync) {
  auto* subscription = callback ? addSubscription(variableId, initialSync) : nullptr;
  if (!subscription) return false;
  subscription->onFloat = callback;
  subscription->kind = SubscriptionKind::Float;
  return true;
}

bool Client::subscribe(const char* variableId, TextCallback callback,
                      InitialSync initialSync) {
  auto* subscription = callback ? addSubscription(variableId, initialSync) : nullptr;
  if (!subscription) return false;
  subscription->onText = callback;
  subscription->kind = SubscriptionKind::Text;
  return true;
}

bool Client::subscribeList(const char* variableId, TextCallback callback,
                           InitialSync initialSync) {
  auto* subscription = callback ? addSubscription(variableId, initialSync) : nullptr;
  if (!subscription) return false;
  subscription->onText = callback;
  subscription->kind = SubscriptionKind::List;
  return true;
}

Client::Subscription* Client::addSubscription(const char* variableId,
                                              InitialSync initialSync) {
  if (_ready || !Batch::validVariableId(variableId)) {
    return nullptr;
  }
  for (uint8_t i = 0; i < _subscriptionCount; ++i) {
    if (strcmp(_subscriptions[i].id, variableId) == 0) {
      _subscriptions[i] = {};
      strcpy(_subscriptions[i].id, variableId);
      _subscriptions[i].initialSync = initialSync;
      return &_subscriptions[i];
    }
  }
  if (_subscriptionCount == _subscriptionCapacity) {
    if (_subscriptionCapacity == kMaxSubscriptions) return nullptr;
    const uint8_t capacity = _subscriptionCapacity ? _subscriptionCapacity * 2 : 1;
    std::unique_ptr<Subscription[]> entries(new (std::nothrow) Subscription[capacity]{});
    if (!entries) return nullptr;
    for (uint8_t i = 0; i < _subscriptionCount; ++i) {
      entries[i] = _subscriptions[i];
    }
    _subscriptions = std::move(entries);
    _subscriptionCapacity = capacity;
  }
  Subscription& subscription = _subscriptions[_subscriptionCount++];
  strcpy(subscription.id, variableId);
  subscription.initialSync = initialSync;
  return &subscription;
}

bool Client::enableOfflineQueue(Storage storage, uint8_t capacity) {
  if (storage != Storage::RAM) return false;
  return enableOfflineQueue(capacity);
}

bool Client::enableOfflineQueue(uint8_t capacity) {
  if (!getOrCreateOfflineQueue(capacity)) return false;
  _offlineQueueEnabled.store(true);
  return true;
}

QueueHandle_t Client::getOrCreateOfflineQueue(uint8_t capacity) {
  if (!_ready || capacity == 0 || capacity > 64) {
    return nullptr;
  }
  if (auto queue = _offlineQueue.load()) return queue;
  const auto created = xQueueCreate(capacity, sizeof(String*));
  if (!created) return _offlineQueue.load();
  QueueHandle_t existing = nullptr;
  if (!_offlineQueue.compare_exchange_strong(existing, created)) {
    vQueueDelete(created);
    return existing;
  }
  return created;
}

size_t Client::queuedItems() const {
  const auto queue = _offlineQueue.load();
  return queue ? uxQueueMessagesWaiting(queue) : 0;
}

bool Client::addItem(const char* variableId, const Json& record) {
  if (!record.valid()) return false;
  if (record._object.length() == 0) return addItem(variableId, "{}");
  return addItem(variableId, record._object);
}

bool Client::addItem(const char* variableId, const String& jsonObject) {
  if (!_ready || !Batch::validVariableId(variableId) ||
      jsonObject.length() > kMaxRequestBytes) return false;
  const bool queueEnabled = _offlineQueueEnabled.load();
  auto queue = _offlineQueue.load();
  if (queueEnabled && queue && uxQueueSpacesAvailable(queue) == 0) return false;
  if (!queueEnabled && !connected()) return false;

  const char* json = jsonObject.c_str();
  size_t length = jsonObject.length();
  trimJson(json, length);
  if (length < 2 || json[0] != '{' || json[length - 1] != '}') return false;

  constexpr char prefix[] = "{\"values\":{},\"append\":{\"";
  constexpr char operationField[] = ",\"operation_id\":\"";
  constexpr size_t operationBytes = sizeof(operationField) - 1 + 36 + 1;
  const size_t bytes = sizeof(prefix) - 1 + strlen(variableId) + 2 + length +
                       2 + (queueEnabled ? operationBytes : 0);
  String payload;
  if (bytes > kMaxRequestBytes || !payload.reserve(bytes)) return false;
  payload += prefix;
  payload += variableId;
  payload += "\":";
  payload.concat(json, length);
  payload += '}';
  if (!queueEnabled) {
    payload += '}';
    return payload.length() == bytes &&
           postBatch(payload, false, portMAX_DELAY).ok();
  }

  uint8_t id[16];
  for (size_t i = 0; i < sizeof(id); i += 4) {
    const uint32_t randomValue = esp_random();
    memcpy(id + i, &randomValue, 4);
  }
  id[6] = (id[6] & 0x0f) | 0x40;
  id[8] = (id[8] & 0x3f) | 0x80;
  char uuid[37];
  snprintf(uuid, sizeof(uuid), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7],
           id[8], id[9], id[10], id[11], id[12], id[13], id[14], id[15]);
  payload += operationField;
  payload += uuid;
  payload += "\"}";
  if (payload.length() != bytes) return false;
  if (!queue) queue = getOrCreateOfflineQueue(16);
  if (!queue) return false;
  if (!reserveQueueBytes(_offlineBytes, bytes, kOfflineQueueBytes)) return false;
  auto* item = new (std::nothrow) String(std::move(payload));
  if (!item || xQueueSend(queue, &item, 0) != pdPASS) {
    delete item;
    _offlineBytes.fetch_sub(bytes, std::memory_order_relaxed);
    return false;
  }
  if (_heartbeatTaskHandle) xTaskNotifyGive(_heartbeatTaskHandle);
  return true;
}

bool Client::sendQueuedItem(QueueHandle_t queue, String& payload) {
  // A simultaneous explicit write must not hold up the heartbeat task.
  const auto response = postBatch(payload, false, 0);
  if (!response.ok()) {
    if (_debug.load()) {
      Serial.printf("Nodexa: queued JSON pending (HTTP %d). Retrying.\n",
                    response.statusCode);
    }
    return false;
  }
  String* completed = nullptr;
  if (xQueueReceive(queue, &completed, 0) == pdPASS && completed) {
    const size_t bytes = completed->length();
    delete completed;
    _offlineBytes.fetch_sub(bytes, std::memory_order_relaxed);
  }
  if (_debug.load()) Serial.println("Nodexa: queued JSON saved.");
  return true;
}

bool Client::setFirmwareVersion(const char* version) {
  if (_ready) return false;
  if (version == nullptr) return false;
  {
    const size_t length = strlen(version);
    if (length == 0 || length > 64) return false;
    bool hasNonSpace = false;
    for (size_t i = 0; i < length; ++i) {
      const unsigned char c = static_cast<unsigned char>(version[i]);
      if (c < 32 || c > 126) return false;
      if (c != ' ') hasNonSpace = true;
    }
    if (!hasNonSpace) return false;
  }
  _firmwareVersion = version;
  return _firmwareVersion == version;
}

bool Client::setInt(const char* variableId, int64_t value, PublishCallback callback) {
  Batch batch;
  return batch.setInt(variableId, value) && publish(batch, callback);
}

bool Client::setFloat(const char* variableId, double value, PublishCallback callback) {
  Batch batch;
  return batch.setFloat(variableId, value) && publish(batch, callback);
}

bool Client::setBool(const char* variableId, bool value, PublishCallback callback) {
  Batch batch;
  return batch.setBool(variableId, value) && publish(batch, callback);
}

bool Client::setString(const char* variableId, const String& value, PublishCallback callback) {
  Batch batch;
  return batch.setString(variableId, value) && publish(batch, callback);
}

bool Client::publish(const Batch& batch, PublishCallback callback) {
  if (!_heartbeatReady.load() || !_streamReady.load() || !_publishQueue ||
      uxQueueSpacesAvailable(_publishQueue) == 0 || !batch.valid() ||
      batch.empty()) return false;
  uint32_t id = _publishSequence.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!id) id = _publishSequence.fetch_add(1, std::memory_order_relaxed) + 1;
  String payload = batch.toPayload(nullptr, id);
  const size_t payloadBytes = payload.length();
  if (!payloadBytes ||
      !reserveQueueBytes(_publishBytes, payloadBytes, kPublishQueueBytes)) {
    return false;
  }
  auto* request = new (std::nothrow) PublishRequest{std::move(payload), callback, id};
  if (!request || xQueueSend(_publishQueue, &request, 0) != pdPASS) {
    delete request;
    _publishBytes.fetch_sub(payloadBytes, std::memory_order_relaxed);
    return false;
  }
  if (_debug.load()) {
    Serial.printf("Nodexa: queued real-time publish (%u bytes).\n",
                  static_cast<unsigned>(payloadBytes));
  }
  wakeControlsTask();
  return true;
}

String Client::readFactoryMacAddress() {
  uint8_t mac[6];
  if (esp_efuse_mac_get_default(mac) != ESP_OK) return String();
  char macAddress[13];
  for (unsigned i = 0; i < 6; ++i) {
    snprintf(macAddress + 2 * i, 3, "%02X", mac[i]);
  }
  return String(macAddress);
}

bool Client::begin(const ConnectionConfig& config, const String& accountId,
                   const String& projectId) {
  return begin({config.ssid, config.password, config.secretKey,
                accountId.c_str(), projectId.c_str()});
}

bool Client::begin(const ConnectionConfig& config) {
  if (_starting.test_and_set()) return false;
  const BeginGuard guard{_starting};
  if (_ready) return false;
  if (_firmwareVersion.length() == 0) {
    if (_debug.load()) Serial.println("Nodexa: firmware version is required.");
    return false;
  }
  if (config.accountId || config.projectId) {
    if (!config.accountId || !config.projectId ||
        !setAccount(config.accountId) || !setProject(config.projectId)) {
      if (_debug.load()) Serial.println("Nodexa: invalid account or project ID.");
      return false;
    }
  }
  if (!config.ssid || !*config.ssid || !config.secretKey ||
      !*config.secretKey || _accountId.length() == 0 ||
      !isTenDigitProjectId(_projectId)) {
    if (_debug.load()) {
      Serial.println("Nodexa: begin failed; check SSID, account, project, and secret configuration.");
    }
    return false;
  }

  _macAddress = readFactoryMacAddress();
  if (_macAddress.length() != 12 || !setAccountSecret(config.secretKey)) {
    if (_debug.load()) {
      Serial.println("Nodexa: begin failed; could not read the factory MAC or accept the secret key.");
    }
    return false;
  }
  _httpTls.useBuiltinCACertBundle();
  String deviceId = _macAddress;
  deviceId.toLowerCase();
  constexpr char root[] = "https://nodexa.it.com/api/v1/";
  const size_t endpointBytes = sizeof(root) - 1 + _accountId.length() +
      sizeof("/projects/") - 1 + _projectId.length() +
      sizeof("/devices/") - 1 + deviceId.length();
  String deviceEndpoint;
  if (deviceId.length() != 12 || !deviceEndpoint.reserve(endpointBytes) ||
      !deviceEndpoint.concat(root) || !deviceEndpoint.concat(_accountId) ||
      !deviceEndpoint.concat("/projects/") || !deviceEndpoint.concat(_projectId) ||
      !deviceEndpoint.concat("/devices/") || !deviceEndpoint.concat(deviceId)) {
    return false;
  }
  const auto setEndpoint = [&](String& target, const char* path) {
    target.remove(0);
    return target.reserve(endpointBytes + strlen(path)) &&
           target.concat(deviceEndpoint) && target.concat(path);
  };
  const bool realtime = _useRealtime || _subscriptionCount != 0;
  if (!setEndpoint(_endpoint, "/variables/batch") ||
      !setEndpoint(_heartbeatEndpoint, "/heartbeat") ||
      (realtime && !setEndpoint(_controlsTicketEndpoint, "/controls/ticket"))) {
    return false;
  }
  _streamReady.store(false);
  _heartbeatReady.store(false);
  _timeSyncStarted.store(false);
  _timeApiSyncAttempted.store(false);
  _lastNtpFallbackAttemptMs = 0;
  _lastWifiRetryAttemptMs = millis();
  _ntpFallbackAttempted = false;
  if (_debug.load()) {
    Serial.printf("ESP32 MAC Address: %s\n", _macAddress.c_str());
    Serial.printf("Connecting to WiFi: %s...\n", config.ssid);
  }
  if (!startHeartbeatTask()) {
    if (_debug.load()) {
      Serial.println("Nodexa: failed to start the heartbeat manager task.");
    }
    return false;
  }
  WiFi.setAutoReconnect(true);
  WiFi.begin(config.ssid, config.password);
  _ready.store(true);
  return true;
}

bool Client::connected() const {
  return _ready && WiFi.status() == WL_CONNECTED && _heartbeatReady.load();
}

bool Client::loop() const { return connected(); }

bool Client::setHeartbeatInterval(uint32_t intervalMs) {
  if (intervalMs < 5000 || intervalMs > 60000) return false;
  _heartbeatIntervalMs = intervalMs;
  return true;
}

bool Client::startHeartbeatTask() {
  if (_heartbeatTaskHandle != nullptr) return true;
  _httpMutex = xSemaphoreCreateMutex();
  if (!_httpMutex) return false;
  if (xTaskCreate(heartbeatTaskEntry, "NodexaHeartbeat", 8192, this,
                  tskIDLE_PRIORITY + 1, &_heartbeatTaskHandle) == pdPASS) {
    return true;
  }
  vSemaphoreDelete(_httpMutex);
  _httpMutex = nullptr;
  _heartbeatTaskHandle = nullptr;
  return false;
}

bool Client::startControlsTask() {
  if (_controlsTaskHandle != nullptr) return true;
  _controlsTls.reset(new (std::nothrow) WiFiClientSecure());
  if (!_controlsTls) return false;
  _controlsTls->useBuiltinCACertBundle();
  _controlsTls->setTimeout(_timeoutMs);
  _publishQueue = xQueueCreate(4, sizeof(PublishRequest*));
  if (_publishQueue == nullptr) {
    _controlsTls.reset();
    if (_debug.load()) {
      Serial.println("Nodexa: could not allocate the real-time publish queue.");
    }
    return false;
  }
  if (xTaskCreate(controlsTaskEntry, "NodexaControls", 8192, this,
                  tskIDLE_PRIORITY + 1, &_controlsTaskHandle) == pdPASS) {
    if (_debug.load()) {
      Serial.printf("Nodexa: real-time task started for %u variable(s).\n",
                    static_cast<unsigned>(_subscriptionCount));
    }
    return true;
  }
  vQueueDelete(_publishQueue);
  _publishQueue = nullptr;
  _controlsTaskHandle = nullptr;
  _controlsTls.reset();
  if (_debug.load()) {
    Serial.println("Nodexa: could not start the real-time task.");
  }
  return false;
}

bool Client::openPublishWakeSocket() {
  if (_publishWakeSocket >= 0) return true;
  const int wakeSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (wakeSocket < 0) return false;

  sockaddr_in address = loopbackAddress(0);
  socklen_t addressLength = sizeof(address);
  if (wakeSocket >= FD_SETSIZE ||
      bind(wakeSocket, reinterpret_cast<sockaddr*>(&address),
           sizeof(address)) != 0 ||
      getsockname(wakeSocket, reinterpret_cast<sockaddr*>(&address),
                  &addressLength) != 0) {
    close(wakeSocket);
    return false;
  }
  _publishWakePort = ntohs(address.sin_port);
  _publishWakeSocket = wakeSocket;
  return true;
}

void Client::wakeControlsTask() {
  const int socket = _publishWakeSocket.load();
  if (socket < 0 || _publishWakePort == 0) return;
  // Wake select() when the heartbeat state changes or the sketch queues work.
  const sockaddr_in address = loopbackAddress(_publishWakePort);
  const char notification = 1;
  sendto(socket, &notification, sizeof(notification), MSG_DONTWAIT,
         reinterpret_cast<const sockaddr*>(&address), sizeof(address));
}

void Client::heartbeatTaskEntry(void* argument) {
  static_cast<Client*>(argument)->heartbeatTask();
}

void Client::controlsTaskEntry(void* argument) {
  static_cast<Client*>(argument)->controlsTask();
}

void Client::heartbeatTask() {
  bool wifiConnectedLogged = false;
  bool queueRetryPending = false;
  uint32_t lastQueueFailureMs = 0;
  for (;;) {
    if (!_ready) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    if (WiFi.status() != WL_CONNECTED) {
      if (wifiConnectedLogged) {
        // Retry time synchronization immediately after network recovery.
        _ntpFallbackAttempted = false;
        _lastNtpFallbackAttemptMs = 0;
      }
      wifiConnectedLogged = false;
      const bool wasHeartbeatReady = _heartbeatReady.exchange(false);
      _streamReady.store(false);
      _lastHeartbeatStatus = 0;
      if (wasHeartbeatReady) wakeControlsTask();

      const uint32_t now = millis();
      if (now - _lastWifiRetryAttemptMs >= kWifiRetryIntervalMs) {
        _lastWifiRetryAttemptMs = now;
        if (_debug.load()) {
          Serial.println("WiFi failed to connect. Retrying...");
        }
        if (WiFi.status() != WL_CONNECTED && !WiFi.reconnect()) {
          if (_debug.load()) {
            Serial.println("WiFi retry could not be started.");
          }
        }
      }
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    if (!wifiConnectedLogged) {
      wifiConnectedLogged = true;
      if (_debug.load()) {
        Serial.println("WiFi Connected");
      }
    }

    if (!_timeSyncStarted.exchange(true)) {
      configTime(0, 0, "pool.ntp.org", "time.google.com",
                 "time.cloudflare.com");
    }
    if (time(nullptr) < 1700000000) {
      const uint32_t now = millis();
      if (!_ntpFallbackAttempted ||
          now - _lastNtpFallbackAttemptMs >= kNtpRetryIntervalMs) {
        _ntpFallbackAttempted = true;
        _lastNtpFallbackAttemptMs = now;
        if (_debug.load()) {
          Serial.println(
              "Clock failed to synchronize with primary NTP; trying failover.");
        }
        if (!syncClockWithNtpFallbacks()) {
          if (_debug.load()) {
            Serial.printf("NTP failover did not respond; retrying in %u seconds.\n",
                          kNtpRetryIntervalMs / 1000);
          }
        }
      }
      if (time(nullptr) < 1700000000) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }
    }

    const Response response = sendHeartbeat(false);
    _lastHeartbeatStatus = response.statusCode;
    if (!response.ok()) {
      const bool wasHeartbeatReady = _heartbeatReady.exchange(false);
      _streamReady.store(false);
      if (wasHeartbeatReady) wakeControlsTask();
      vTaskDelay(pdMS_TO_TICKS(kHeartbeatRetryIntervalMs));
      continue;
    }

    _heartbeatReady.store(true);
    const uint32_t lastHeartbeatMs = millis();

    // Refine NTP time only after Nodexa has accepted the initial heartbeat.
    if (!_timeApiSyncAttempted.exchange(true)) {
      syncClockFromTimeApi();
    }

    if ((_useRealtime || _subscriptionCount != 0) &&
        !startControlsTask()) {
      if (_debug.load()) {
        Serial.println("Nodexa: could not start the real-time task; will retry.");
      }
    }
    // Give the heartbeat priority between uploads and wake early for new items.
    const uint32_t interval = _heartbeatIntervalMs;
    while (connected()) {
      const uint32_t now = millis();
      const uint32_t elapsed = now - lastHeartbeatMs;
      if (elapsed >= interval) break;

      uint32_t waitMs = std::min<uint32_t>(1000, interval - elapsed);
      const auto queue = _offlineQueue.load();
      String* item = nullptr;
      if (queue && xQueuePeek(queue, &item, 0) == pdPASS) {
        const uint32_t retryElapsed = now - lastQueueFailureMs;
        if (!queueRetryPending || retryElapsed >= kQueueRetryIntervalMs) {
          queueRetryPending = !sendQueuedItem(queue, *item);
          if (queueRetryPending) lastQueueFailureMs = millis();
          continue;
        }
        waitMs = std::min(waitMs, kQueueRetryIntervalMs - retryElapsed);
      }
      const TickType_t waitTicks = pdMS_TO_TICKS(waitMs);
      ulTaskNotifyTake(pdTRUE, waitTicks ? waitTicks : 1);
    }
  }
}

bool Client::syncClockWithNtpFallbacks() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiUDP udp;
  if (!udp.begin(0)) {
    if (_debug.load()) Serial.println("Nodexa: could not open the NTP UDP socket.");
    return false;
  }

  for (const char* server : kNtpFallbackServers) {
    if (_debug.load()) Serial.printf("Nodexa: querying NTP server %s.\n", server);
    uint8_t request[48]{};
    request[0] = 0x23;  // NTP version 4, client mode.
    const uint32_t nonceHigh = esp_random();
    const uint32_t nonceLow = esp_random();
    request[40] = static_cast<uint8_t>(nonceHigh >> 24);
    request[41] = static_cast<uint8_t>(nonceHigh >> 16);
    request[42] = static_cast<uint8_t>(nonceHigh >> 8);
    request[43] = static_cast<uint8_t>(nonceHigh);
    request[44] = static_cast<uint8_t>(nonceLow >> 24);
    request[45] = static_cast<uint8_t>(nonceLow >> 16);
    request[46] = static_cast<uint8_t>(nonceLow >> 8);
    request[47] = static_cast<uint8_t>(nonceLow);

    if (!udp.beginPacket(server, 123)) {
      if (_debug.load()) {
        Serial.printf("Nodexa: could not resolve or open NTP server %s.\n",
                      server);
      }
      continue;
    }
    const size_t written = udp.write(request, sizeof(request));
    const int sendStatus = udp.endPacket();
    if (written != sizeof(request) || sendStatus != 1) {
      if (_debug.load()) {
        Serial.printf("Nodexa: could not send an NTP request to %s.\n", server);
      }
      continue;
    }

    const uint32_t startedAt = millis();
    while (millis() - startedAt < kNtpResponseTimeoutMs) {
      const int packetSize = udp.parsePacket();
      if (packetSize >= static_cast<int>(sizeof(request))) {
        uint8_t response[48]{};
        const int received = udp.read(response, sizeof(response));
        const uint8_t leapIndicator = response[0] >> 6;
        const uint8_t version = (response[0] >> 3) & 0x07;
        const uint8_t mode = response[0] & 0x07;
        const uint8_t stratum = response[1];
        const bool validResponse =
            received == static_cast<int>(sizeof(response)) &&
            leapIndicator != 3 && version >= 3 && version <= 4 && mode == 4 &&
            stratum >= 1 && stratum <= 15 &&
            memcmp(response + 24, request + 40, 8) == 0;
        if (validResponse) {
          const uint32_t ntpSeconds =
              (static_cast<uint32_t>(response[40]) << 24) |
              (static_cast<uint32_t>(response[41]) << 16) |
              (static_cast<uint32_t>(response[42]) << 8) |
              static_cast<uint32_t>(response[43]);
          const uint32_t ntpFraction =
              (static_cast<uint32_t>(response[44]) << 24) |
              (static_cast<uint32_t>(response[45]) << 16) |
              (static_cast<uint32_t>(response[46]) << 8) |
              static_cast<uint32_t>(response[47]);
          uint64_t unixSeconds = ntpSeconds;
          if (unixSeconds >= kNtpUnixEpochOffset) {
            unixSeconds -= kNtpUnixEpochOffset;
          } else {
            unixSeconds += (1ULL << 32) - kNtpUnixEpochOffset;
          }

          if (unixSeconds >= 1700000000ULL) {
            timeval currentTime{};
            currentTime.tv_sec = static_cast<time_t>(unixSeconds);
            currentTime.tv_usec = static_cast<suseconds_t>(
                (static_cast<uint64_t>(ntpFraction) * 1000000ULL) >> 32);
            if (settimeofday(&currentTime, nullptr) == 0) {
              if (_debug.load()) {
                Serial.printf("Clock synchronized using NTP failover via %s.\n",
                              server);
              }
              udp.stop();
              return true;
            }
          }
        }
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (_debug.load()) {
      Serial.printf("Nodexa: no valid NTP response from %s.\n", server);
    }
  }

  udp.stop();
  return false;
}

bool Client::syncClockFromTimeApi() {
  const MutexLock lock(_httpMutex, portMAX_DELAY);
  if (!lock) return false;

  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(_timeoutMs);
  http.setTimeout(_timeoutMs);
  if (!http.begin(_httpTls, kCurrentTimeEndpoint)) {
    if (_debug.load()) {
      Serial.println(
          "Clock failed to synchronize (Failover is used instead).");
    }
    return false;
  }

  const int status = http.GET();
  bool synchronized = false;
  if (status == 200) {
    String response;
    const bool complete = detail::readHttpBody(http, response, 64);
    response.trim();
    bool isEpochMilliseconds = complete && response.length() == 13;
    for (size_t i = 0; isEpochMilliseconds && i < response.length(); ++i) {
      isEpochMilliseconds = response[i] >= '0' && response[i] <= '9';
    }

    if (isEpochMilliseconds) {
      const uint64_t epochMilliseconds =
          strtoull(response.c_str(), nullptr, 10);
      if (epochMilliseconds >= 1700000000000ULL) {
        timeval currentTime{};
        currentTime.tv_sec =
            static_cast<time_t>(epochMilliseconds / 1000ULL);
        currentTime.tv_usec =
            static_cast<suseconds_t>((epochMilliseconds % 1000ULL) * 1000ULL);
        synchronized = settimeofday(&currentTime, nullptr) == 0;
      }
    }
  }
  http.end();

  if (synchronized) {
    if (_debug.load()) {
      Serial.println("Clock synchronized");
    }
  } else {
    if (_debug.load()) {
      Serial.printf("Clock failed to synchronize (Failover is used instead). "
                    "Time API HTTP status: %d.\n",
                    status);
    }
  }

  return synchronized;
}

bool Client::fetchControlsTicket(String& ticket) {
  if (_controlsTicketEndpoint.length() == 0 ||
      WiFi.status() != WL_CONNECTED || !_heartbeatReady.load()) {
    return false;
  }

  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(_timeoutMs);
  http.setTimeout(_timeoutMs);
  if (!http.begin(*_controlsTls, _controlsTicketEndpoint)) {
    if (_debug.load()) {
      Serial.println("Nodexa: could not start the real-time ticket request.");
    }
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Nodexa-Secret", _accountSecret);
  const int status = http.POST("{\"protocol\":2}");
  _lastStreamTicketStatus.store(status);
  if (_debug.load()) {
    Serial.printf("Nodexa: real-time ticket HTTP status %d.\n", status);
  }
  String responseBody;
  const bool complete = status >= 200 && status < 300 &&
      detail::readHttpBody(http, responseBody, kMaxControlsFrameBytes + 256);
  http.end();
  if (!complete || !detail::jsonWithinDepth(responseBody.c_str())) return false;

  cJSON* response = cJSON_ParseWithOpts(responseBody.c_str(), nullptr, true);
  const cJSON* token = cJSON_GetObjectItemCaseSensitive(response, "ticket");
  if (cJSON_IsString(token)) ticket = token->valuestring;
  cJSON_Delete(response);
  const bool ticketValid =
      ticket.length() > 0 && ticket.length() <= kMaxControlsFrameBytes;
  if (!ticketValid && _debug.load()) {
    Serial.println("Nodexa: real-time ticket response was missing or invalid.");
  }
  return ticketValid;
}

bool Client::sendSubscriptions() {
  String message;
  if (!message.reserve(40 + _subscriptionCount * 72) ||
      !message.concat("{\"type\":\"subscribe\",\"variables\":[")) {
    return false;
  }
  for (uint8_t i = 0; i < _subscriptionCount; ++i) {
    if (i != 0) message += ',';
    const Subscription& subscription = _subscriptions[i];
    message += "{\"variable_id\":\"";
    message += subscription.id;
    message += "\",\"initial_sync\":";
    switch (subscription.initialSync) {
      case InitialSync::LatestValue:
        message += "\"latest_value\"";
        break;
      case InitialSync::Null:
        message += "null";
        break;
      case InitialSync::ChangesOnly:
      default:
        message += "\"changes_only\"";
        break;
    }
    message += '}';
  }
  message += "]}";
  return sendControlsFrame(
      0x1, reinterpret_cast<const uint8_t*>(message.c_str()), message.length());
}

bool Client::connectControlsSocket() {
  _lastStreamUpgradeStatus.store(0);
  if (WiFi.status() != WL_CONNECTED || !_heartbeatReady.load()) return false;
  if (_controlsTls->connected()) _controlsTls->stop();
  _controlsTls->setTimeout(2500);
  if (_debug.load()) {
    Serial.printf("Nodexa: opening verified WebSocket connection to %s:443.\n",
                  kApiHost);
  }
  if (!_controlsTls->connect(kApiHost, 443, _timeoutMs)) {
    if (_debug.load()) {
      Serial.println("Nodexa: WebSocket TLS connection failed.");
    }
    return false;
  }

  uint8_t nonce[16];
  for (size_t i = 0; i < sizeof(nonce); ++i) {
    nonce[i] = static_cast<uint8_t>(esp_random());
  }
  char websocketKey[32];
  if (!encodeBase64(nonce, sizeof(nonce), websocketKey,
                    sizeof(websocketKey))) {
    if (_debug.load()) {
      Serial.println("Nodexa: could not create the WebSocket handshake key.");
    }
    _controlsTls->stop();
    return false;
  }

  const String request =
      String("GET ") + kControlsPath + " HTTP/1.1\r\nHost: " + kApiHost +
      "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " +
      websocketKey +
      "\r\nSec-WebSocket-Version: 13\r\n\r\n";
  if (_controlsTls->write(
          reinterpret_cast<const uint8_t*>(request.c_str()),
          request.length()) != request.length()) {
    if (_debug.load()) {
      Serial.println("Nodexa: could not send the WebSocket upgrade request.");
    }
    _controlsTls->stop();
    return false;
  }

  String line;
  const uint32_t headerStarted = millis();
  if (!readControlLine(*_controlsTls, line, headerStarted, 5000)) {
    _controlsTls->stop();
    return false;
  }
  int status = 0;
  if (line.length() >= 12 && line.startsWith("HTTP/1.") && line[8] == ' ' &&
      isdigit(static_cast<unsigned char>(line[9])) &&
      isdigit(static_cast<unsigned char>(line[10])) &&
      isdigit(static_cast<unsigned char>(line[11]))) {
    status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');
  }
  _lastStreamUpgradeStatus.store(status);
  if (status != 101) {
    if (_debug.load()) {
      Serial.printf("Nodexa: WebSocket upgrade was rejected (HTTP %d).\n",
                    status);
    }
    _controlsTls->stop();
    return false;
  }

  String receivedAccept;
  bool headersComplete = false;
  constexpr char acceptHeader[] = "sec-websocket-accept:";
  for (uint8_t lineNumber = 0; lineNumber < 32; ++lineNumber) {
    if (!readControlLine(*_controlsTls, line, headerStarted, 5000)) break;
    line.trim();
    if (line.length() == 0) {
      headersComplete = true;
      break;
    }
    if (strncasecmp(line.c_str(), acceptHeader, sizeof(acceptHeader) - 1) == 0) {
      receivedAccept = line.c_str() + sizeof(acceptHeader) - 1;
      receivedAccept.trim();
    }
  }

  String acceptSource = String(websocketKey) + kWebSocketGuid;
  uint8_t digest[20];
  const mbedtls_md_info_t* sha1 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
  char expectedAccept[32];
  if (!headersComplete || sha1 == nullptr ||
      mbedtls_md(sha1,
                 reinterpret_cast<const unsigned char*>(acceptSource.c_str()),
                 acceptSource.length(), digest) != 0 ||
      !encodeBase64(digest, sizeof(digest), expectedAccept,
                    sizeof(expectedAccept)) ||
      receivedAccept != expectedAccept) {
    if (_debug.load()) {
      Serial.println("Nodexa: WebSocket accept header validation failed.");
    }
    _controlsTls->stop();
    return false;
  }
  _controlsTls->setTimeout(1500);
  if (_debug.load()) {
    Serial.println("Nodexa: WebSocket handshake accepted (HTTP 101).");
  }
  return true;
}

bool Client::sendControlsFrame(uint8_t opcode, const uint8_t* payload,
                               size_t payloadLength) {
  if (!_controlsTls->connected() || payloadLength > kMaxControlsFrameBytes) {
    return false;
  }
  if ((opcode & 0x08) != 0 && payloadLength > 125) return false;
  uint8_t header[8];
  size_t headerLength = 0;
  header[headerLength++] = static_cast<uint8_t>(0x80 | (opcode & 0x0f));
  if (payloadLength <= 125) {
    header[headerLength++] = static_cast<uint8_t>(0x80 | payloadLength);
  } else {
    header[headerLength++] = 0x80 | 126;
    header[headerLength++] = static_cast<uint8_t>((payloadLength >> 8) & 0xff);
    header[headerLength++] = static_cast<uint8_t>(payloadLength & 0xff);
  }
  uint8_t mask[4];
  for (size_t i = 0; i < sizeof(mask); ++i) {
    mask[i] = static_cast<uint8_t>(esp_random());
    header[headerLength++] = mask[i];
  }
  if (_controlsTls->write(header, headerLength) != headerLength) return false;

  uint8_t masked[1024];
  for (size_t offset = 0; offset < payloadLength;) {
    size_t length = payloadLength - offset;
    if (length > sizeof(masked)) length = sizeof(masked);
    for (size_t i = 0; i < length; ++i) {
      masked[i] = payload[offset + i] ^ mask[(offset + i) & 3];
    }
    if (_controlsTls->write(masked, length) != length) return false;
    offset += length;
  }
  return true;
}

bool Client::readControlsFrame(uint8_t& opcode, char (&small)[kInlineFrameBytes],
                               std::unique_ptr<char[]>& large, size_t& payloadLength) {
  uint8_t first = 0;
  uint8_t second = 0;
  if (!readControlByte(*_controlsTls, first, 2500) ||
      !readControlByte(*_controlsTls, second, 1500)) {
    return false;
  }
  if ((first & 0xf0) != 0x80 || (second & 0x80) != 0) return false;
  opcode = first & 0x0f;
  uint64_t frameLength = second & 0x7f;
  if (frameLength == 126) {
    uint8_t upper = 0;
    uint8_t lower = 0;
    if (!readControlByte(*_controlsTls, upper, 1500) ||
        !readControlByte(*_controlsTls, lower, 1500)) {
      return false;
    }
    frameLength = (static_cast<uint16_t>(upper) << 8) | lower;
  } else if (frameLength == 127) {
    uint64_t extendedLength = 0;
    for (uint8_t i = 0; i < 8; ++i) {
      uint8_t part = 0;
      if (!readControlByte(*_controlsTls, part, 1500)) return false;
      extendedLength = (extendedLength << 8) | part;
    }
    frameLength = extendedLength;
  }
  if (frameLength > kMaxControlsFrameBytes ||
      ((opcode & 0x08) != 0 && frameLength > 125)) {
    return false;
  }

  payloadLength = static_cast<size_t>(frameLength);
  char* payload = small;
  if (payloadLength >= sizeof(small)) {
    large.reset(new (std::nothrow) char[payloadLength + 1]);
    if (!large) return false;
    payload = large.get();
  }
  for (size_t offset = 0; offset < payloadLength;) {
    if (!waitForControlData(*_controlsTls, 3000)) return false;
    const int available = _controlsTls->available();
    if (available <= 0) return false;
    const size_t remaining = payloadLength - offset;
    const size_t length = remaining < static_cast<size_t>(available)
        ? remaining : static_cast<size_t>(available);
    const int bytesRead = _controlsTls->read(
        reinterpret_cast<uint8_t*>(payload + offset), length);
    if (bytesRead <= 0) return false;
    offset += bytesRead;
  }
  if (opcode == 0x1 && memchr(payload, '\0', payloadLength)) return false;
  payload[payloadLength] = '\0';
  return true;
}

void Client::reportStreamError(const char* code, const char* variableId) {
  if (_debug.load()) {
    Serial.printf("Nodexa: %s%s%s\n", code, variableId ? " - " : "",
                  variableId ? variableId : "");
  }
  if (const auto callback = _errorCallback.load()) callback(code, variableId);
}

bool Client::handleControlsMessage(
    const char* payload, std::unique_ptr<char[]>& large, bool& subscribed,
    std::unique_ptr<PublishRequest>& pending) {
  if (!detail::jsonWithinDepth(payload)) {
    reportStreamError("invalid_stream_message", nullptr);
    return true;
  }
  std::unique_ptr<cJSON, decltype(&cJSON_Delete)> message(
      cJSON_ParseWithOpts(payload, nullptr, true), cJSON_Delete);
  large.reset();
  if (!message) {
    reportStreamError("invalid_stream_message", nullptr);
    return true;
  }
  const cJSON* type = cJSON_GetObjectItemCaseSensitive(message.get(), "type");
  if (!cJSON_IsString(type)) return true;
  if (strcmp(type->valuestring, "subscription_error") == 0 ||
      strcmp(type->valuestring, "error") == 0) {
    const cJSON* code = cJSON_GetObjectItemCaseSensitive(message.get(), "code");
    const cJSON* variable = cJSON_GetObjectItemCaseSensitive(message.get(), "variable_id");
    const cJSON* error = cJSON_GetObjectItemCaseSensitive(message.get(), "error");
    if (!cJSON_IsString(code)) code = cJSON_GetObjectItemCaseSensitive(error, "code");
    reportStreamError(cJSON_IsString(code) ? code->valuestring : "subscription_rejected",
                      cJSON_IsString(variable) ? variable->valuestring : nullptr);
    _streamReady.store(false);
    return false;
  }
  if (strcmp(type->valuestring, "ready") == 0 && !subscribed) {
    if (_debug.load()) {
      Serial.printf("Nodexa: server ready; subscribing to %u variable(s).\n",
                    static_cast<unsigned>(_subscriptionCount));
    }
    return sendSubscriptions();
  }
  if (strcmp(type->valuestring, "subscribed") == 0) {
    subscribed = true;
    _streamReady.store(true);
    if (_debug.load()) {
      Serial.println("Nodexa: real-time subscription is ready.");
    }
    return true;
  }
  if (strcmp(type->valuestring, "published") == 0 ||
      strcmp(type->valuestring, "publish_error") == 0) {
    const cJSON* id = cJSON_GetObjectItemCaseSensitive(message.get(), "request_id");
    if (!pending || !cJSON_IsNumber(id) || id->valuedouble != pending->id) {
      return true;
    }
    const bool committed = strcmp(type->valuestring, "published") == 0;
    if (_debug.load()) {
      Serial.printf("Nodexa: real-time publish request %lu %s.\n",
                    static_cast<unsigned long>(pending->id),
                    committed ? "committed" : "rejected");
    }
    const cJSON* code = cJSON_GetObjectItemCaseSensitive(message.get(), "code");
    if (!committed) {
      const cJSON* variable = cJSON_GetObjectItemCaseSensitive(message.get(), "variable_id");
      reportStreamError(cJSON_IsString(code) ? code->valuestring : "rejected",
                        cJSON_IsString(variable) ? variable->valuestring : nullptr);
    }
    if (pending->callback) {
      pending->callback(committed, committed ? nullptr :
                        cJSON_IsString(code) ? code->valuestring : "rejected");
    }
    pending.reset();
    return true;
  }

  const cJSON* variable = cJSON_GetObjectItemCaseSensitive(message.get(), "variable_id");
  const cJSON* value = cJSON_GetObjectItemCaseSensitive(message.get(), "value");
  if (!subscribed || !cJSON_IsString(variable)) return true;
  for (uint8_t i = 0; i < _subscriptionCount; ++i) {
    const Subscription& subscription = _subscriptions[i];
    if (strcmp(subscription.id, variable->valuestring) != 0) continue;
    if (_debug.load()) {
      Serial.printf("Nodexa: received streamed update for '%s' (type %s).\n",
                    variable->valuestring, type->valuestring);
    }
    if (subscription.kind == SubscriptionKind::Int &&
        strcmp(type->valuestring, "int") == 0 &&
        cJSON_IsNumber(value) && value->valuedouble >= INT32_MIN &&
        value->valuedouble <= INT32_MAX &&
        trunc(value->valuedouble) == value->valuedouble) {
      subscription.onInt(static_cast<int32_t>(value->valuedouble));
    } else if (subscription.kind == SubscriptionKind::Bool && cJSON_IsBool(value) &&
               (strcmp(type->valuestring, "bool") == 0 ||
                strcmp(type->valuestring, "boolean") == 0)) {
      subscription.onBool(cJSON_IsTrue(value));
    } else if (subscription.kind == SubscriptionKind::Float &&
               strcmp(type->valuestring, "float") == 0 &&
               cJSON_IsNumber(value) && isfinite(value->valuedouble)) {
      subscription.onFloat(value->valuedouble);
    } else if (subscription.kind == SubscriptionKind::Text &&
               strcmp(type->valuestring, "string") == 0 && cJSON_IsString(value)) {
      const String text(value->valuestring);
      if (text.length() == strlen(value->valuestring)) {
        subscription.onText(text);
      } else {
        reportStreamError("out_of_memory", subscription.id);
      }
    } else if (subscription.kind == SubscriptionKind::List &&
               strcmp(type->valuestring, "list") == 0 && cJSON_IsArray(value)) {
      char* json = cJSON_PrintUnformatted(value);
      if (json) {
        const String text(json);
        const bool complete = text.length() == strlen(json);
        cJSON_free(json);
        if (complete) {
          subscription.onText(text);
        } else {
          reportStreamError("out_of_memory", subscription.id);
        }
      } else {
        reportStreamError("out_of_memory", subscription.id);
      }
    }
    break;
  }
  return true;
}

void Client::releasePublishPayload(PublishRequest& request) {
  const size_t bytes = request.payload.length();
  if (!bytes) return;
  request.payload = static_cast<const char*>(nullptr);
  _publishBytes.fetch_sub(bytes, std::memory_order_relaxed);
}

void Client::controlsTask() {
  uint32_t retryDelayMs = 1000;
  for (;;) {
    if (!_ready || WiFi.status() != WL_CONNECTED || !_heartbeatReady.load() ||
        time(nullptr) < 1700000000) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    // lwIP is initialized by Wi-Fi. Opening any socket from begin() can run
    // before WiFi.begin() and fail inside the ESP32 core.
    if (!openPublishWakeSocket()) {
      if (_debug.load()) {
        Serial.println("Nodexa: could not open the real-time wake socket; retrying.");
      }
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    if (_debug.load()) {
      Serial.println("Nodexa: requesting a real-time connection ticket.");
    }
    String ticket;
    bool subscribed = false;
    std::unique_ptr<PublishRequest> pending;
    if (fetchControlsTicket(ticket) && connectControlsSocket() &&
        sendControlsFrame(0x1, reinterpret_cast<const uint8_t*>(ticket.c_str()),
                          ticket.length())) {
      ticket = static_cast<const char*>(nullptr);
      bool closeSent = false;
      uint32_t sentAt = 0;
      const char* failure = "connection_lost";
      while (_ready && WiFi.status() == WL_CONNECTED &&
             _heartbeatReady.load()) {
        if (subscribed && !pending) {
          PublishRequest* request = nullptr;
          if (xQueueReceive(_publishQueue, &request, 0) == pdPASS) {
            pending.reset(request);
            sentAt = millis();
            const auto& frame = pending->payload;
            const bool sent = sendControlsFrame(0x1,
                reinterpret_cast<const uint8_t*>(frame.c_str()), frame.length());
            releasePublishPayload(*pending);
            if (!sent) break;
          }
        }
        uint32_t waitMs = subscribed ? 90000 : 10000;
        if (pending) {
          const uint32_t elapsed = millis() - sentAt;
          if (elapsed >= kPublishAckTimeoutMs) {
            failure = "ack_timeout";
            break;
          }
          waitMs = kPublishAckTimeoutMs - elapsed;
        }
        bool woke = false;
        if (!waitForControlData(*_controlsTls, waitMs, _publishWakeSocket, &woke)) {
          if (pending && millis() - sentAt >= kPublishAckTimeoutMs) {
            failure = "ack_timeout";
          }
          break;
        }
        if (woke) continue;

        uint8_t opcode = 0;
        size_t payloadLength = 0;
        char small[kInlineFrameBytes];
        std::unique_ptr<char[]> large;
        if (!readControlsFrame(opcode, small, large, payloadLength)) break;
        const char* payload = large ? large.get() : small;
        const auto* bytes = reinterpret_cast<const uint8_t*>(payload);
        if (opcode == 0x8) {
          sendControlsFrame(0x8, bytes, payloadLength);
          closeSent = true;
          break;
        }
        if (opcode == 0x9 && !sendControlsFrame(0xA, bytes, payloadLength)) break;
        if (opcode == 0x1 &&
            !handleControlsMessage(payload, large, subscribed, pending)) {
          break;
        }
      }
      _streamReady.store(false);
      if (!closeSent && _controlsTls->connected()) sendControlsFrame(0x8, nullptr, 0);
      if (pending && pending->callback) pending->callback(false, failure);
    }
    _controlsTls->stop();
    if (subscribed) retryDelayMs = 1000;
    if (_lastStreamUpgradeStatus.load() == 429 && retryDelayMs < 5000) {
      retryDelayMs = 5000;
    }
    const uint32_t jitterMs = esp_random() % 500;
    if (_debug.load()) {
      Serial.printf(
          "Nodexa: real-time connection closed or failed; ticket HTTP %d, "
          "WebSocket HTTP %d; retrying in %lu ms.\n",
          _lastStreamTicketStatus.load(), _lastStreamUpgradeStatus.load(),
          static_cast<unsigned long>(retryDelayMs + jitterMs));
    }
    vTaskDelay(pdMS_TO_TICKS(retryDelayMs + jitterMs));
    retryDelayMs = retryDelayMs < 16000 ? retryDelayMs * 2 : 30000;
  }
}

Response Client::heartbeat() {
  return sendHeartbeat(true);
}

Response Client::sendHeartbeat(bool readBody) {
  Response response;
  if (!_ready) {
    response.body = "Nodexa client is not configured.";
    if (_debug.load()) Serial.println("Nodexa: heartbeat skipped; client is not configured.");
    return response;
  }
  if (WiFi.status() != WL_CONNECTED) {
    response.body = "Wi-Fi is not connected.";
    if (_debug.load()) Serial.println("Nodexa: heartbeat skipped; Wi-Fi is disconnected.");
    return response;
  }

  const MutexLock lock(_httpMutex, portMAX_DELAY);
  if (!lock || WiFi.status() != WL_CONNECTED) return response;

  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(_timeoutMs);
  http.setTimeout(_timeoutMs);
  if (_debug.load()) Serial.println("Sending heartbeat...");
  if (!http.begin(_httpTls, _heartbeatEndpoint)) {
    response.body = "Could not start the Nodexa heartbeat request.";
    if (_debug.load()) {
      Serial.println("Failed to send heartbeat. Retrying...");
      Serial.println("Heartbeat request could not be opened.");
    }
    return response;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Nodexa-Secret", _accountSecret);
  const uint32_t internalHeapTotalBytes = ESP.getHeapSize();
  const uint32_t internalHeapFreeBytes = ESP.getFreeHeap();
  const uint32_t psramTotalBytes = ESP.getPsramSize();
  const uint32_t psramFreeBytes = ESP.getFreePsram();
  const uint64_t heapTotalBytes =
      static_cast<uint64_t>(internalHeapTotalBytes) + psramTotalBytes;
  const uint64_t internalHeapUsedBytes =
      internalHeapTotalBytes > internalHeapFreeBytes
          ? internalHeapTotalBytes - internalHeapFreeBytes
          : 0;
  const uint64_t psramUsedBytes = psramTotalBytes > psramFreeBytes
      ? psramTotalBytes - psramFreeBytes
      : 0;
  const uint64_t heapUsedBytes = internalHeapUsedBytes + psramUsedBytes;

  const uint64_t cpuSampleTimestampUs =
      static_cast<uint64_t>(esp_timer_get_time());
  uint64_t cpuIdleRuntimeCore0 = 0;
  uint64_t cpuIdleRuntimeCore1 = 0;
  bool hasCpuUsage = false;
  uint8_t cpuUsagePercent = 0;
#if NODEXA_HAS_CPU_RUNTIME_STATS
  cpuIdleRuntimeCore0 = static_cast<uint64_t>(
      ulTaskGetIdleRunTimeCounterForCore(0));
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
  cpuIdleRuntimeCore1 = static_cast<uint64_t>(
      ulTaskGetIdleRunTimeCounterForCore(1));
#endif
  if (_cpuSampleTimestampUs > 0 &&
      cpuSampleTimestampUs > _cpuSampleTimestampUs) {
    const uint64_t elapsedUs = cpuSampleTimestampUs - _cpuSampleTimestampUs;
#if defined(CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U32) && \
    CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U32
    const uint64_t idleCore0Us = static_cast<uint32_t>(cpuIdleRuntimeCore0) -
        static_cast<uint32_t>(_cpuIdleRuntimeCore0);
    const uint64_t idleCore1Us = static_cast<uint32_t>(cpuIdleRuntimeCore1) -
        static_cast<uint32_t>(_cpuIdleRuntimeCore1);
#else
    const uint64_t idleCore0Us = cpuIdleRuntimeCore0 - _cpuIdleRuntimeCore0;
    const uint64_t idleCore1Us = cpuIdleRuntimeCore1 - _cpuIdleRuntimeCore1;
#endif
    bool sampleValid = true;
#if defined(CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U32) && \
    CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U32
    sampleValid = elapsedUs < UINT32_MAX;
#endif
    const uint64_t coreCount = CONFIG_FREERTOS_NUMBER_OF_CORES;
    const uint64_t totalRuntimeUs = elapsedUs * coreCount;
    const uint64_t idleRuntimeUs = idleCore0Us + idleCore1Us;
    if (sampleValid && totalRuntimeUs > 0 &&
        idleRuntimeUs <= totalRuntimeUs) {
      const uint64_t busyRuntimeUs = totalRuntimeUs - idleRuntimeUs;
      cpuUsagePercent = static_cast<uint8_t>(
          (busyRuntimeUs * 100 + totalRuntimeUs / 2) / totalRuntimeUs);
      hasCpuUsage = true;
    }
  }
#endif

  const String firmware = _firmwareVersion.length() == 0
      ? String("null") : Batch::quoteJson(_firmwareVersion);
  String heartbeatPayload = String("{\"firmware_version\":") + firmware +
      ",\"uptime_ms\":" + String(static_cast<unsigned long long>(esp_timer_get_time() / 1000)) +
      ",\"heap_total_bytes\":" + String(static_cast<unsigned long long>(heapTotalBytes)) +
      ",\"heap_used_bytes\":" + String(static_cast<unsigned long long>(heapUsedBytes)) +
      ",\"chip_model\":" + Batch::quoteJson(chipModel()) +
      ",\"chip_revision\":" + String(chipRevision()) +
      ",\"ota_slot_bytes\":" + String(static_cast<unsigned long>(otaStorageBytes())) +
      ",\"local_ip\":" + Batch::quoteJson(WiFi.localIP().toString()) +
      ",\"heartbeat_interval_seconds\":" + String((_heartbeatIntervalMs + 999) / 1000) +
      ",\"library_version\":\"0.11.10\"";
  if (hasCpuUsage) {
    heartbeatPayload += String(",\"cpu_usage_percent\":") +
        String(static_cast<unsigned>(cpuUsagePercent));
  }
#ifdef ARDUINO_BOARD
  heartbeatPayload += String(",\"board_profile\":") + Batch::quoteJson(ARDUINO_BOARD);
#endif
  heartbeatPayload += "}";
  response.statusCode = http.POST(
      reinterpret_cast<uint8_t*>(heartbeatPayload.begin()), heartbeatPayload.length());
  if (readBody) {
    if (response.statusCode > 0) {
      detail::readHttpBody(http, response.body, kMaxRequestBytes);
    } else {
      response.body = HTTPClient::errorToString(response.statusCode);
    }
  }
  http.end();

#if NODEXA_HAS_CPU_RUNTIME_STATS
  if (response.ok()) {
    _cpuSampleTimestampUs = cpuSampleTimestampUs;
    _cpuIdleRuntimeCore0 = cpuIdleRuntimeCore0;
    _cpuIdleRuntimeCore1 = cpuIdleRuntimeCore1;
  }
#endif

  if (_debug.load()) {
    Serial.println(response.ok()
                       ? "Heartbeat sent!"
                       : "Failed to send heartbeat. Retrying...");
    Serial.printf("Heartbeat HTTP status: %d\n", response.statusCode);
  }

  return response;
}

Response Client::write(const Batch& batch, const char* timestamp) {
  Response response;
  if (!_ready) {
    response.body = "Nodexa client is not configured.";
    return response;
  }
  if (WiFi.status() != WL_CONNECTED || !_heartbeatReady.load()) {
    response.body = "Waiting for a successful Nodexa heartbeat.";
    if (_debug.load()) {
      Serial.println("Nodexa: value write skipped; server connection is not ready.");
    }
    return response;
  }
  String payload = batch.toPayload(timestamp);
  if (payload.length() == 0) {
    response.body = "Batch is empty or invalid.";
    return response;
  }
  if (payload.length() > kMaxRequestBytes) {
    response.body = "Batch exceeds Nodexa's 16 KiB request limit.";
    return response;
  }

  return postBatch(payload, true, portMAX_DELAY);
}

Response Client::postBatch(String& payload, bool readBody,
                           TickType_t waitForTransport) {
  Response response;
  if (!connected()) return response;
  const MutexLock lock(_httpMutex, waitForTransport);
  if (!lock) return response;
  if (!connected()) {
    if (readBody) response.body = "Waiting for a successful Nodexa heartbeat.";
    return response;
  }
  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(_timeoutMs);
  http.setTimeout(_timeoutMs);
  if (_debug.load()) {
    Serial.printf("Nodexa: sending HTTPS value batch (%u bytes).\n",
                  static_cast<unsigned>(payload.length()));
  }
  if (!http.begin(_httpTls, _endpoint)) {
    response.body = "Could not start the HTTPS request.";
    if (_debug.load()) {
      Serial.println("Nodexa: could not open the HTTPS value request.");
    }
    return response;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Nodexa-Secret", _accountSecret);
  response.statusCode = http.POST(reinterpret_cast<uint8_t*>(payload.begin()),
                                  payload.length());
  if (readBody) {
    if (response.statusCode > 0) {
      detail::readHttpBody(http, response.body, kMaxRequestBytes);
    } else {
      response.body = HTTPClient::errorToString(response.statusCode);
    }
  }
  http.end();
  if (_debug.load()) {
    Serial.printf("Nodexa: value batch response HTTP %d.\n",
                  response.statusCode);
  }
  return response;
}

bool Client::isTenDigitProjectId(const String& projectId) {
  if (projectId.length() != 10) return false;
  for (size_t i = 0; i < projectId.length(); ++i) {
    if (projectId[i] < '0' || projectId[i] > '9') return false;
  }
  return true;
}

}  // namespace Nodexa
