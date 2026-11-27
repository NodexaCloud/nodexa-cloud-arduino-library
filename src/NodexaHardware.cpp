#include "NodexaCloud.h"

#include <esp_chip_info.h>
#include <esp_ota_ops.h>

namespace Nodexa {

String Client::chipModel() const {
  return ESP.getChipModel();
}

uint16_t Client::chipRevision() const {
  esp_chip_info_t info;
  esp_chip_info(&info);
  return info.revision;
}

size_t Client::otaStorageBytes() const {
  const auto* partition = esp_ota_get_next_update_partition(nullptr);
  return partition == nullptr ? 0 : partition->size;
}

}  // namespace Nodexa
