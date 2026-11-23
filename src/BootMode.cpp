#include "BootMode.h"

#include <Logging.h>
#include <nvs.h>
#include <nvs_flash.h>

namespace bootmode {

void set(Mode mode) {
  nvs_handle_t handle;
  const esp_err_t opened = nvs_open(kNamespace, NVS_READWRITE, &handle);
  if (opened != ESP_OK) {
    LOG_ERR("BOOTM", "nvs_open(boot_mode) failed: 0x%X", static_cast<unsigned>(opened));
    return;
  }
  esp_err_t err = nvs_set_u8(handle, kKey, static_cast<uint8_t>(mode));
  if (err == ESP_OK) {
    // Immediate commit — a power cut right after esp_restart() must still show
    // the new value on the next boot. Same pattern as DeviceSecret.cpp.
    err = nvs_commit(handle);
  }
  nvs_close(handle);
  if (err != ESP_OK) {
    LOG_ERR("BOOTM", "boot_mode write/commit failed: 0x%X", static_cast<unsigned>(err));
    return;
  }
  LOG_INF("BOOTM", "boot_mode=%u committed", static_cast<unsigned>(mode));
}

Mode consume() {
  nvs_handle_t handle;
  const esp_err_t opened = nvs_open(kNamespace, NVS_READWRITE, &handle);
  if (opened != ESP_OK) {
    LOG_ERR("BOOTM", "nvs_open(boot_mode) failed: 0x%X; assuming NORMAL", static_cast<unsigned>(opened));
    return Mode::NORMAL;
  }

  uint8_t raw = static_cast<uint8_t>(Mode::NORMAL);
  const esp_err_t read = nvs_get_u8(handle, kKey, &raw);
  const Mode observed = (read == ESP_OK && raw == static_cast<uint8_t>(Mode::WIFI)) ? Mode::WIFI : Mode::NORMAL;

  // Consume: a WIFI request is one-shot. Write it back to NORMAL immediately so
  // a later power cut during this session can never re-enter WiFi-transfer mode.
  if (observed == Mode::WIFI) {
    const esp_err_t err = nvs_set_u8(handle, kKey, static_cast<uint8_t>(Mode::NORMAL)) == ESP_OK
                              ? nvs_commit(handle)
                              : ESP_FAIL;
    if (err == ESP_OK) {
      LOG_INF("BOOTM", "boot_mode=WIFI consumed; reset to NORMAL");
    } else {
      LOG_ERR("BOOTM", "boot_mode consume commit failed: 0x%X", static_cast<unsigned>(err));
    }
  }
  nvs_close(handle);
  return observed;
}

}  // namespace bootmode
