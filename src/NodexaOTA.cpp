#include "NodexaCloud.h"

#include <HTTPClient.h>
#include <algorithm>
#include <cstring>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include <utility>

namespace Nodexa {
namespace {

std::atomic_flag otaBusy = ATOMIC_FLAG_INIT;
constexpr uint32_t kResumeRetryMs = 1000;
constexpr uint32_t kMaximumResumeRetryMs = 10000;

struct OtaLock {
  ~OtaLock() { otaBusy.clear(); }
};

struct OtaWrite {
  esp_ota_handle_t handle = 0;
  bool active = false;

  ~OtaWrite() {
    if (active) esp_ota_abort(handle);
  }
};

struct Sha256 {
  mbedtls_sha256_context context;

  Sha256() { mbedtls_sha256_init(&context); }
  ~Sha256() { mbedtls_sha256_free(&context); }
};

int hexDigit(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

bool parseDigest(const char* hex, uint8_t (&digest)[32]) {
  if (!hex || strlen(hex) != sizeof(digest) * 2) return false;
  for (size_t i = 0; i < sizeof(digest); ++i) {
    const int high = hexDigit(hex[2 * i]);
    const int low = hexDigit(hex[2 * i + 1]);
    if (high < 0 || low < 0) return false;
    digest[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

bool parseDecimal(const String& text, size_t& value) {
  if (text.isEmpty()) return false;
  value = 0;
  const size_t maximum = static_cast<size_t>(-1);
  for (unsigned i = 0; i < text.length(); ++i) {
    const char c = text[i];
    if (c < '0' || c > '9') return false;
    const size_t digit = static_cast<size_t>(c - '0');
    if (value > (maximum - digit) / 10) return false;
    value = value * 10 + digit;
  }
  return true;
}

bool parseContentRange(const String& header, size_t& first, size_t& last,
                       size_t& total) {
  if (!header.startsWith("bytes ")) return false;
  const int dash = header.indexOf('-', 6);
  const int slash = dash < 0 ? -1 : header.indexOf('/', dash + 1);
  if (dash < 0 || slash < 0) return false;
  return parseDecimal(header.substring(6, dash), first) &&
         parseDecimal(header.substring(dash + 1, slash), last) &&
         parseDecimal(header.substring(slash + 1), total) &&
         first <= last && last < total;
}

bool sameRelease(const FirmwareUpdate& left, const FirmwareUpdate& right) {
  return left.id == right.id && left.version == right.version &&
         left.sha256 == right.sha256 && left.sizeBytes == right.sizeBytes;
}

bool retryableHttpStatus(int status) {
  return status < 0 || status == 408 || status == 429 || status >= 500;
}

}  // namespace

OtaResult Client::stageFirmware(const char* httpsUrl, const char* sha256Hex) {
  return stageFirmware(httpsUrl, sha256Hex, nullptr);
}

OtaResult Client::stageFirmware(const char* httpsUrl, const char* sha256Hex,
                                OtaProgressCallback onProgress) {
  return stageFirmwareImage(httpsUrl, sha256Hex, 0, onProgress, nullptr, nullptr);
}

OtaResult Client::stageFirmwareImage(const char* httpsUrl, const char* sha256Hex,
                                     size_t expectedBytes,
                                     OtaProgressCallback onProgress,
                                     const FirmwareUpdate* managedOffer,
                                     const char* board) {
  if (!httpsUrl || strncmp(httpsUrl, "https://", 8) != 0 || !httpsUrl[8]) {
    return {"https_url_required"};
  }
  uint8_t expected[32];
  if (!parseDigest(sha256Hex, expected)) return {"invalid_sha256"};
  if (!connected()) return {"not_connected"};
  if (otaBusy.test_and_set()) return {"ota_busy"};
  const OtaLock lock;
  _otaDownloadPercent.store(0, std::memory_order_relaxed);

  const auto* running = esp_ota_get_running_partition();
  const auto* boot = esp_ota_get_boot_partition();
  const auto* target = esp_ota_get_next_update_partition(nullptr);
  if (!running || !boot || !target || target->address == running->address ||
      !esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr)) {
    return {"no_ota_partition"};
  }
  if (boot->address != running->address) return {"restart_required"};
  if (expectedBytes && expectedBytes > target->size) {
    return {"firmware_too_large", static_cast<int>(expectedBytes)};
  }

  const uint16_t configuredTimeout = _timeoutMs.load();
  const uint16_t timeoutMs = configuredTimeout ? configuredTimeout : 10000;
  String activeUrl(httpsUrl);
  unsigned ticketRefreshes = 0;
  size_t totalBytes = expectedBytes;
  size_t downloadedBytes = 0;
  uint32_t retryDelayMs = kResumeRetryMs;

  Sha256 hash;
  if (const int error = mbedtls_sha256_starts(&hash.context, 0)) {
    return {"sha256_failed", error};
  }
  OtaWrite update;
  auto error = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &update.handle);
  if (error != ESP_OK) return {"flash_begin_failed", error};
  update.active = true;

  uint8_t buffer[1024];
  uint8_t lastPercent = 0;
  if (onProgress) onProgress(0);

  while (totalBytes == 0 || downloadedBytes < totalBytes) {
    while (!connected()) delay(kResumeRetryMs);

    WiFiClientSecure tls;
    tls.useBuiltinCACertBundle();
    tls.setHandshakeTimeout((static_cast<unsigned long>(timeoutMs) + 999) / 1000);
    HTTPClient http;
    http.setConnectTimeout(timeoutMs);
    http.setTimeout(timeoutMs);
    http.setReuse(false);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.setAcceptEncoding("identity");
    const char* headers[] = {"Transfer-Encoding", "Content-Encoding",
                             "Content-Range", "Accept-Ranges"};
    http.collectHeaders(headers, 4);
    if (!http.begin(tls, activeUrl)) {
      if (_debug.load()) {
        Serial.printf("Nodexa OTA: could not open the artifact connection at %lu/%lu bytes; retrying.\n",
                      static_cast<unsigned long>(downloadedBytes),
                      static_cast<unsigned long>(totalBytes));
      }
      http.end();
      delay(retryDelayMs);
      retryDelayMs = std::min(kMaximumResumeRetryMs, retryDelayMs * 2);
      continue;
    }
    if (downloadedBytes > 0) {
      http.addHeader("Range", String("bytes=") +
          String(static_cast<unsigned long>(downloadedBytes)) + "-");
    }

    const int status = http.GET();
    if (status == 401 || status == 403 || status == 404) {
      http.end();
      if (!managedOffer || !board) return {"download_ticket_expired", status};
      if (++ticketRefreshes > 3) return {"download_ticket_refresh_failed", status};

      FirmwareUpdate refreshed;
      const auto refresh = checkForFirmwareUpdate(refreshed, board);
      if (!refresh.ok()) return {"download_ticket_refresh_failed", refresh.detail};
      if (!refreshed.available() || !sameRelease(*managedOffer, refreshed)) {
        return {"ota_release_changed", status};
      }
      activeUrl = std::move(refreshed.url);
      retryDelayMs = kResumeRetryMs;
      continue;
    }
    if (retryableHttpStatus(status)) {
      if (_debug.load()) {
        Serial.printf("Nodexa OTA: connection interrupted at %lu/%lu bytes; retrying.\n",
                      static_cast<unsigned long>(downloadedBytes),
                      static_cast<unsigned long>(totalBytes));
      }
      http.end();
      delay(retryDelayMs);
      retryDelayMs = std::min(kMaximumResumeRetryMs, retryDelayMs * 2);
      continue;
    }

    const int expectedStatus = downloadedBytes == 0 ? HTTP_CODE_OK : HTTP_CODE_PARTIAL_CONTENT;
    if (status != expectedStatus) {
      if (downloadedBytes > 0 && status == HTTP_CODE_OK) {
        return {"range_not_honored", status};
      }
      return {"download_failed", status};
    }

    const int responseBytes = http.getSize();
    const String encoding = http.header("Content-Encoding");
    if (responseBytes <= 0 || http.hasHeader("Transfer-Encoding") ||
        (encoding.length() && !encoding.equalsIgnoreCase("identity"))) {
      return {"firmware_length_required"};
    }

    if (downloadedBytes == 0) {
      if (totalBytes == 0) totalBytes = static_cast<size_t>(responseBytes);
      if (static_cast<size_t>(responseBytes) != totalBytes) {
        return {"firmware_length_mismatch", responseBytes};
      }
    } else {
      size_t rangeStart = 0;
      size_t rangeEnd = 0;
      size_t rangeTotal = 0;
      if (!parseContentRange(http.header("Content-Range"), rangeStart,
                             rangeEnd, rangeTotal) ||
          rangeStart != downloadedBytes || rangeTotal != totalBytes ||
          rangeEnd != totalBytes - 1 ||
          static_cast<size_t>(responseBytes) != totalBytes - downloadedBytes) {
        return {"invalid_content_range", responseBytes};
      }
    }

    auto* stream = http.getStreamPtr();
    if (!stream) {
      if (_debug.load()) {
        Serial.printf("Nodexa OTA: artifact stream unavailable at %lu/%lu bytes; retrying.\n",
                      static_cast<unsigned long>(downloadedBytes),
                      static_cast<unsigned long>(totalBytes));
      }
      http.end();
      delay(retryDelayMs);
      retryDelayMs = std::min(kMaximumResumeRetryMs, retryDelayMs * 2);
      continue;
    }
    int responseRemaining = responseBytes;
    uint32_t lastRead = millis();
    bool interrupted = false;

    while (responseRemaining > 0) {
      const int available = stream->available();
      const int toRead = std::min({available, responseRemaining,
                                   static_cast<int>(sizeof(buffer))});
      const int count = toRead > 0 ? stream->read(buffer, toRead) : 0;
      if (count <= 0) {
        if (count < 0 || !stream->connected() || millis() - lastRead >= timeoutMs) {
          interrupted = true;
          break;
        }
        delay(1);
        continue;
      }

      error = mbedtls_sha256_update(&hash.context, buffer, count);
      if (error != 0) return {"sha256_failed", error};
      error = esp_ota_write(update.handle, buffer, count);
      if (error != ESP_OK) return {"flash_write_failed", error};
      downloadedBytes += static_cast<size_t>(count);
      responseRemaining -= count;

      const auto percent = static_cast<uint8_t>(
          (static_cast<uint64_t>(downloadedBytes) * 100) / totalBytes);
      if (percent != lastPercent) {
        lastPercent = percent;
        _otaDownloadPercent.store(percent, std::memory_order_relaxed);
        if (onProgress) onProgress(percent);
      }
      lastRead = millis();
    }

    http.end();
    if (interrupted && downloadedBytes < totalBytes) {
      if (_debug.load()) {
        Serial.printf("Nodexa OTA: resuming at %lu/%lu bytes.\n",
                      static_cast<unsigned long>(downloadedBytes),
                      static_cast<unsigned long>(totalBytes));
      }
      delay(retryDelayMs);
      retryDelayMs = std::min(kMaximumResumeRetryMs, retryDelayMs * 2);
    }
  }

  uint8_t actual[32];
  error = mbedtls_sha256_finish(&hash.context, actual);
  if (error != 0) return {"sha256_failed", error};
  if (memcmp(expected, actual, sizeof(actual)) != 0) return {"sha256_mismatch"};

  error = esp_ota_end(update.handle);
  update.active = false;
  if (error != ESP_OK) return {"invalid_firmware_image", error};
  error = esp_ota_set_boot_partition(target);
  if (error != ESP_OK) return {"boot_selection_failed", error};
  return {nullptr, 0, true};
}

}  // namespace Nodexa
