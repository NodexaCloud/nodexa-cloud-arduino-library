#include "NodexaCloud.h"
#include "NodexaHttpResponse.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <cJSON.h>
#include <esp_ota_ops.h>
#include <cstring>

namespace Nodexa {
namespace {

constexpr char kOtaApi[] = "https://api.nodexa.it.com/api/v1/";
constexpr char kReceiptKey[] = "pending";
std::atomic_flag managedOtaBusy = ATOMIC_FLAG_INIT;

struct ManagedOtaLock {
  ~ManagedOtaLock() { managedOtaBusy.clear(); }
};

bool parseVersion(const char* version, uint64_t& order) {
  if (!version) return false;
  order = 0;
  for (unsigned part = 0; part < 3; ++part) {
    if (*version < '0' || *version > '9') return false;
    if (*version == '0' && version[1] >= '0' && version[1] <= '9') return false;
    unsigned value = 0;
    unsigned digits = 0;
    while (*version >= '0' && *version <= '9') {
      if (++digits > 4) return false;
      value = value * 10 + (*version++ - '0');
    }
    order = order * 10000 + value;
    if (part < 2 && *version++ != '.') return false;
  }
  return *version == '\0';
}

bool validVersion(const char* version) {
  uint64_t unused = 0;
  return parseVersion(version, unused);
}

bool newerVersion(const String& candidate, const String& current) {
  uint64_t offered = 0;
  uint64_t installed = 0;
  return parseVersion(candidate.c_str(), offered) &&
         parseVersion(current.c_str(), installed) && offered > installed;
}

bool validBoard(const char* board) {
  if (!board) return false;
  const size_t length = strlen(board);
  if (length == 0 || length > 64) return false;
  for (size_t i = 0; i < length; ++i) {
    const char c = board[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')) return false;
  }
  return true;
}

bool validReleaseId(const String& id) {
  if (id.length() != 36) return false;
  for (unsigned i = 0; i < id.length(); ++i) {
    const char c = id[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
    } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

bool validArtifact(const FirmwareUpdate& offer) {
  const String prefix = String(kOtaApi) + "ota/artifacts/" + offer.id + "?ticket=";
  return validReleaseId(offer.id) && validVersion(offer.version.c_str()) &&
         offer.url.startsWith(prefix) && offer.url.length() > prefix.length() &&
         offer.sha256.length() == 64 && offer.sizeBytes > 0 &&
         offer.sizeBytes <= 4U * 1024 * 1024;
}

const char* jsonText(const cJSON* object, const char* name) {
  const cJSON* field = cJSON_GetObjectItemCaseSensitive(object, name);
  return cJSON_IsString(field) ? field->valuestring : "";
}

struct Receipt {
  String id;
  String version;
  char next = 's';  // Next report: staged, booted, or confirmed.
};

bool saveReceipt(Preferences& preferences, const String& scope,
                 const Receipt& receipt) {
  const String record =
      scope + "|" + receipt.id + "|" + receipt.version + "|" + receipt.next;
  const size_t expected = scope.length() + receipt.id.length() + receipt.version.length() + 4;
  return record.length() == expected &&
         preferences.putString(kReceiptKey, record) == expected;
}

bool parseReceipt(const String& record, const String& scope, Receipt& receipt) {
  if (record.length() <= scope.length() || !record.startsWith(scope) ||
      record[scope.length()] != '|') return false;
  const int start = scope.length() + 1;
  const int versionStart = record.indexOf('|', start);
  if (versionStart < 0) return false;
  const int phaseStart = record.indexOf('|', versionStart + 1);
  if (phaseStart < 0 || static_cast<size_t>(phaseStart + 2) != record.length()) return false;
  receipt.id = record.substring(start, versionStart);
  receipt.version = record.substring(versionStart + 1, phaseStart);
  receipt.next = record[phaseStart + 1];
  return validReleaseId(receipt.id) && validVersion(receipt.version.c_str()) &&
         (receipt.next == 's' || receipt.next == 'b' || receipt.next == 'c');
}

}  // namespace

String Client::otaScope() const {
  return _accountId + "/" + _projectId + "/" + _macAddress;
}

OtaResult Client::postOta(const char* action, const String& payload, String* response) {
  if (!connected()) return {"not_connected"};
  String mac = _macAddress;
  mac.toLowerCase();
  const String url = String(kOtaApi) + _accountId + "/projects/" + _projectId +
                     "/devices/" + mac + "/ota/" + action;
  WiFiClientSecure tls;
  tls.useBuiltinCACertBundle();
  HTTPClient http;
  const uint16_t configuredTimeout = _timeoutMs.load();
  const uint16_t timeout = configuredTimeout ? configuredTimeout : 10000;
  tls.setHandshakeTimeout((static_cast<unsigned long>(timeout) + 999) / 1000);
  http.setConnectTimeout(timeout);
  http.setTimeout(timeout);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  if (!http.begin(tls, url)) return {"ota_connection_failed"};
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Nodexa-Secret", _accountSecret);
  const int status = http.POST(payload);
  if (status != HTTP_CODE_OK) return {"ota_request_failed", status};
  if (response && !detail::readHttpBody(http, *response, 4096)) {
    return {"invalid_ota_response_body"};
  }
  return {};
}

OtaResult Client::checkForFirmwareUpdate(FirmwareUpdate& update, const char* board) {
  update = {};
  if (!validBoard(board)) return {"invalid_board"};
  if (!validVersion(_firmwareVersion.c_str())) return {"invalid_firmware_version"};
  const auto* slot = esp_ota_get_next_update_partition(nullptr);
  const auto* running = esp_ota_get_running_partition();
  if (!slot || !running || slot->address == running->address ||
      !esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr)) {
    return {"no_ota_partition"};
  }
  const String payload = String("{\"current_version\":") + Batch::quoteJson(_firmwareVersion) +
      ",\"board\":" + Batch::quoteJson(board) + ",\"flash_bytes\":" + ESP.getFlashChipSize() +
      ",\"slot_bytes\":" + static_cast<unsigned long>(slot->size) + "}";
  String response;
  const auto result = postOta("check", payload, &response);
  if (!result.ok()) return result;
  if (!detail::jsonWithinDepth(response.c_str())) return {"invalid_ota_response"};
  std::unique_ptr<cJSON, decltype(&cJSON_Delete)> document(
      cJSON_Parse(response.c_str()), cJSON_Delete);
  const cJSON* offer = cJSON_GetObjectItemCaseSensitive(document.get(), "update");
  if (cJSON_IsNull(offer)) return {};
  if (!cJSON_IsObject(offer)) return {"invalid_ota_response"};
  const cJSON* bytes = cJSON_GetObjectItemCaseSensitive(offer, "size_bytes");
  if (!cJSON_IsNumber(bytes) || bytes->valueint <= 0 ||
      bytes->valuedouble != bytes->valueint) return {"invalid_ota_response"};
  FirmwareUpdate candidate;
  candidate.id = jsonText(offer, "id");
  candidate.version = jsonText(offer, "version");
  candidate.url = jsonText(offer, "url");
  candidate.sha256 = jsonText(offer, "sha256");
  candidate.sizeBytes = bytes->valueint;
  if (!validArtifact(candidate) || candidate.sizeBytes > slot->size ||
      !newerVersion(candidate.version, _firmwareVersion)) {
    return {"invalid_ota_response"};
  }
  update = std::move(candidate);
  return {};
}

OtaResult Client::updateFirmware(OtaProgressCallback onProgress) {
#ifdef ARDUINO_BOARD
  return updateFirmware(ARDUINO_BOARD, onProgress);
#else
  return {"ota_board_required"};
#endif
}

OtaResult Client::updateFirmware(const char* board, OtaProgressCallback onProgress) {
  const auto* running = esp_ota_get_running_partition();
  const auto* boot = esp_ota_get_boot_partition();
  if (running && boot && running->address != boot->address) {
    return {"restart_required"};
  }

  FirmwareUpdate update;
  const auto check = checkForFirmwareUpdate(update, board);
  if (!check.ok() || !update.available()) return check;
  return installFirmwareUpdate(update, onProgress, board);
}

OtaResult Client::reportOta(const String& id, const char* state) {
  const String payload = String("{\"release_id\":") + Batch::quoteJson(id) +
      ",\"state\":" + Batch::quoteJson(state) + ",\"running_version\":" +
      Batch::quoteJson(_firmwareVersion) + "}";
  return postOta("status", payload);
}

OtaResult Client::installFirmwareUpdate(const FirmwareUpdate& update) {
  return installFirmwareUpdate(update, nullptr);
}

OtaResult Client::installFirmwareUpdate(const FirmwareUpdate& update,
                                        OtaProgressCallback onProgress) {
#ifdef ARDUINO_BOARD
  return installFirmwareUpdate(update, onProgress, ARDUINO_BOARD);
#else
  return installFirmwareUpdate(update, onProgress, nullptr);
#endif
}

OtaResult Client::installFirmwareUpdate(const FirmwareUpdate& update,
                                        OtaProgressCallback onProgress,
                                        const char* board) {
  if (!validArtifact(update)) return {"invalid_ota_offer"};
  if (!connected()) return {"not_connected"};
  if (!validVersion(_firmwareVersion.c_str())) return {"invalid_firmware_version"};
  if (!newerVersion(update.version, _firmwareVersion)) return {"ota_version_not_newer"};
  if (managedOtaBusy.test_and_set()) return {"ota_busy"};
  const ManagedOtaLock lock;

  Preferences preferences;
  if (!preferences.begin("nodexa-ota")) return {"ota_checkpoint_unavailable"};
  if (preferences.isKey(kReceiptKey)) return {"ota_checkpoint_pending"};
  const String scope = otaScope();
  Receipt receipt{update.id, update.version};
  if (!saveReceipt(preferences, scope, receipt)) return {"ota_checkpoint_unavailable"};

  const auto result = stageFirmwareImage(
      update.url.c_str(), update.sha256.c_str(), update.sizeBytes, onProgress,
      &update, board);
  if (!result.ok()) {
    if (result.error && (strcmp(result.error, "invalid_firmware_image") == 0 ||
                         strcmp(result.error, "sha256_mismatch") == 0)) {
      reportOta(receipt.id, "failed");
    }
    preferences.remove(kReceiptKey);
    return result;
  }
  if (reportOta(receipt.id, "staged").ok()) {
    receipt.next = 'b';
    saveReceipt(preferences, scope, receipt);
  }
  // A lost acknowledgement can be retried from the saved checkpoint after boot.
  return result;
}

OtaResult Client::confirmFirmwareUpdate() {
  if (!ready()) return {"not_connected"};
  if (managedOtaBusy.test_and_set()) return {"ota_busy"};
  const ManagedOtaLock lock;
  Preferences preferences;
  if (!preferences.begin("nodexa-ota")) return {"ota_checkpoint_unavailable"};
  if (!preferences.isKey(kReceiptKey)) return {};
  Receipt receipt;
  const String scope = otaScope();
  if (!parseReceipt(preferences.getString(kReceiptKey), scope, receipt)) {
    return {"invalid_ota_checkpoint"};
  }
  const auto* running = esp_ota_get_running_partition();
  const auto* boot = esp_ota_get_boot_partition();
  if (!running || !boot) return {"no_ota_partition"};
  if (boot->address != running->address) return {"restart_required"};
  if (!validVersion(_firmwareVersion.c_str())) return {"invalid_firmware_version"};

  if (receipt.version != _firmwareVersion) {
    // An interrupted download can leave a checkpoint without a staged image.
    if (receipt.next != 's') {
      const auto failed = reportOta(receipt.id, "failed");
      if (!failed.ok() && failed.detail != 404) return failed;
    }
    return preferences.remove(kReceiptKey) ? OtaResult{} :
        OtaResult{"ota_checkpoint_unavailable"};
  }
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    const auto error = esp_ota_mark_app_valid_cancel_rollback();
    if (error != ESP_OK) return {"ota_boot_confirmation_failed", error};
  }

  while (true) {
    const char* next = receipt.next == 's' ? "staged" :
                       receipt.next == 'b' ? "booted" : "confirmed";
    const auto reported = reportOta(receipt.id, next);
    if (!reported.ok()) {
      if (reported.detail == 404) {
        // A release may be cancelled after this device already staged it. The
        // stale lifecycle receipt must not block checks for newer active releases.
        if (!preferences.remove(kReceiptKey)) return {"ota_checkpoint_unavailable"};
        if (debug()) {
          Serial.println("Nodexa OTA: clearing receipt for a release no longer on the server.");
        }
        return {};
      }
      return reported;
    }
    if (receipt.next == 'c') break;
    receipt.next = receipt.next == 's' ? 'b' : 'c';
    if (!saveReceipt(preferences, scope, receipt)) return {"ota_checkpoint_unavailable"};
  }
  return preferences.remove(kReceiptKey) ? OtaResult{} :
      OtaResult{"ota_checkpoint_unavailable"};
}

}  // namespace Nodexa
