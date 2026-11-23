#pragma once

// b65-s3c-pf9d: WiFi-transfer lightweight mode.
//
// Skip decisions are a DUAL judgment: a compile-time switch (so the feature can
// be compiled out with -DPF9D_WIFI_TRANSFER_MODE=0) AND a runtime flag set once
// at boot from the consumed NVS boot_mode. Every skip site goes through the
// single WIFI_TRANSFER_ACTIVE() helper — do not scatter raw ifs.
#ifndef PF9D_WIFI_TRANSFER_MODE
#define PF9D_WIFI_TRANSFER_MODE 1
#endif

namespace wifitransfer {

// Set once in setup() after bootmode::consume() returns WIFI AND the reset was
// a software restart (the normal entry path).
void activate();

// Runtime latch (true only for the lifetime of a WiFi-transfer boot).
bool active();

// b65-s3c-pf9d clause 11: a stale WIFI boot_mode reached on a non-software
// restart (power cut before/during entry) means the previous WiFi session did
// not exit cleanly. markAbnormalExit() records it in setup; consume* clears it
// on first read so the hint shows exactly once on Home.
void markAbnormalExit();
bool consumeAbnormalExit();

// Compile-time portion of the dual judgment.
constexpr bool compiledEnabled() { return PF9D_WIFI_TRANSFER_MODE != 0; }

}  // namespace wifitransfer

// The only skip predicate call sites should use.
#define WIFI_TRANSFER_ACTIVE() (::wifitransfer::compiledEnabled() && ::wifitransfer::active())
