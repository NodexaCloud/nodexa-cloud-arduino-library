#include "NodexaCloud.h"

#include <cstring>

namespace Nodexa {
namespace {

constexpr uint32_t kConnectionRetryMs = 1000;
constexpr uint32_t kUpdateRetryMs = 30000;
constexpr uint32_t kUpdateCheckIntervalMs = 5 * 60 * 1000;
constexpr uint32_t kOtaTaskStackBytes = 12288;

bool needsRestart(const OtaResult& result) {
  return result.error && std::strcmp(result.error, "restart_required") == 0;
}

}  // namespace

void FirmwareUpdater::setLastResult(const OtaResult& result) {
  portENTER_CRITICAL(&_resultMux);
  _lastResult = result;
  portEXIT_CRITICAL(&_resultMux);
}

OtaResult FirmwareUpdater::lastResult() const {
  portENTER_CRITICAL(&_resultMux);
  const OtaResult result = _lastResult;
  portEXIT_CRITICAL(&_resultMux);
  return result;
}

void FirmwareUpdater::workerTaskEntry(void* argument) {
  auto* updater = static_cast<FirmwareUpdater*>(argument);
  updater->runWorkerTask();
  vTaskDelete(nullptr);
}

void FirmwareUpdater::runWorkerTask() {
  OtaResult result;
  if (!_bootConfirmed) {
    result = _client.confirmFirmwareUpdate();
    if (needsRestart(result)) {
      result.readyToRestart = true;
      setLastResult(result);
      _workerState.store(WorkerState::ReadyToRestart, std::memory_order_release);
      return;
    }
    _bootConfirmed = result.ok();
  }

  if (_bootConfirmed) {
    if (_client.debug()) {
      Serial.println("Nodexa OTA: checking for the latest compatible firmware.");
    }
    result = _client.updateFirmware(_progressCallback);
    if (needsRestart(result)) result.readyToRestart = true;
  }

  setLastResult(result);
  _workerState.store(result.readyToRestart ? WorkerState::ReadyToRestart
                                           : WorkerState::Finished,
                     std::memory_order_release);
}

void FirmwareUpdater::run(Client::OtaProgressCallback onProgress) {
  if (!poll(onProgress) || _client.queuedItems() != 0) return;
  if (_client.debug()) Serial.println("Nodexa OTA: restarting into verified firmware.");
  ESP.restart();
}

bool FirmwareUpdater::poll(Client::OtaProgressCallback onProgress) {
  const WorkerState state = _workerState.load(std::memory_order_acquire);
  if (state == WorkerState::ReadyToRestart) return true;
  if (state == WorkerState::Running) return false;

  if (state == WorkerState::Finished) {
    const OtaResult result = lastResult();
    _lastAttemptMs = millis();
    if (result.ok()) {
      _waitMs = kUpdateCheckIntervalMs;
      if (_client.debug()) {
        Serial.println("Nodexa OTA: no newer compatible firmware; checking again in 5 minutes.");
      }
    } else {
      _waitMs = kUpdateRetryMs;
      if (_client.debug()) {
        Serial.printf("Nodexa OTA: %s (%d)\n", result.error, result.detail);
      }
    }
    _workerState.store(WorkerState::Idle, std::memory_order_release);
    return false;
  }

  const uint32_t now = millis();
  if (now - _lastAttemptMs < _waitMs) return false;
  if (!_client.connected()) {
    _lastAttemptMs = now;
    _waitMs = kConnectionRetryMs;
    return false;
  }

  _lastAttemptMs = now;
  _progressCallback = onProgress;
  _workerState.store(WorkerState::Running, std::memory_order_release);
  if (xTaskCreate(workerTaskEntry, "NodexaOTA", kOtaTaskStackBytes, this,
                  tskIDLE_PRIORITY + 1, nullptr) != pdPASS) {
    setLastResult({"ota_task_start_failed"});
    _workerState.store(WorkerState::Finished, std::memory_order_release);
  }
  return false;
}

}  // namespace Nodexa
