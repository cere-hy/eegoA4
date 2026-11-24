#pragma once

// b65-s3c-pf9h (P0, observation): millisecond-slice IP probe.
//
// The pf9g field log showed `connectEvent evt=3` (== WiFi.status()==WL_CONNECTED,
// which on Arduino-ESP32 3.3.11 is set only on ARDUINO_EVENT_WIFI_STA_GOT_IP)
// followed, ~12 ms later, by the first `P0 ip poll ... ip=0.0.0.0`.  The IP
// vanished inside a 12 ms window that spans WifiSelectionActivity::onExit(),
// the completion callback and MDNS.begin().  To find which statement drops it,
// this probe prints a full five-tuple at each boundary:
//
//   [PROBE] <point> localIP=<x> status=<n> netif_ip=<x> ap_info_rc=<n>
//
// Field names are deliberately distinct from the `evt=` used by
// `connectEvent` (that one is wl_status_t, NOT an Arduino event id), and from
// the new `arduino_evt=<id> <name>` full event stream.  Uses logPrintf("INF")
// directly so the probe is emitted regardless of LOG_LEVEL.
//
// No behaviour change: this only reads netif / AP-info / status.

#include <WiFi.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <Logging.h>

namespace wifi_ip_probe {

// Read the STA IP straight from the IDF netif (authoritative), mirroring
// CrossPointWebServer::staHasIp().  "WIFI_STA_DEF" is the IDF default-STA
// netif key literal; this core defines no ESP_NETIF_WIFI_STA_IF_KEY macro.
inline IPAddress staNetifIp(esp_err_t* outRc = nullptr) {
  IPAddress ip;  // 0.0.0.0
  esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_err_t rc = ESP_ERR_INVALID_STATE;
  if (sta != nullptr) {
    esp_netif_ip_info_t info{};
    rc = esp_netif_get_ip_info(sta, &info);
    if (rc == ESP_OK && info.ip.addr != 0) {
      ip = IPAddress(static_cast<uint32_t>(info.ip.addr));
    }
  }
  if (outRc != nullptr) *outRc = rc;
  return ip;
}

// b65-s3c-pf9i (P2): `level` lets the two constant-on-success probes
// (ws_gotip_check / ws_before_complete) be demoted to DBG so the success path
// is not noisy; the two verdict probes (ws_before_disconnect /
// ws_after_disconnect) and every other point stay INF.  Default INF keeps all
// other call sites byte-for-byte identical in behaviour.
inline void emit(const char* point, const char* level = "INF") {
  IPAddress local = WiFi.localIP();
  IPAddress netif = staNetifIp();
  wifi_ap_record_t apInfo{};
  const esp_err_t apRc = esp_wifi_sta_get_ap_info(&apInfo);
  logPrintf(level, "PROBE",
            "[PROBE] %s localIP=%s status=%d netif_ip=%s ap_info_rc=%d",
            point, local.toString().c_str(), static_cast<int>(WiFi.status()),
            netif.toString().c_str(), static_cast<int>(apRc));
}

}  // namespace wifi_ip_probe
