#pragma once

#include <cstdint>

// b65-s3c-pf9d: one-shot boot mode persisted in NVS. The flag is written
// immediately before esp_restart() and consumed (read-then-reset) at boot so a
// stale WIFI value can never strand the device in WiFi-transfer mode.
namespace bootmode {

enum class Mode : uint8_t {
  NORMAL = 0,
  WIFI = 1,
};

// NVS namespace/key (via native nvs_open/nvs_set_u8/nvs_commit). Kept short and
// stable; this value survives reboots and power loss. Do not rename without a migration.
constexpr const char* kNamespace = "crosspoint";
constexpr const char* kKey = "boot_mode";

// Write the mode and commit immediately (no deferred flush — a power cut right
// after must still be observed on the next boot).
void set(Mode mode);

// Read once at startup. If the stored value is WIFI, it is reset to NORMAL and
// committed before returning, so this call is destructive/consuming: a second
// call in the same boot returns NORMAL. Use the returned value to decide the
// boot branch.
Mode consume();

}  // namespace bootmode
