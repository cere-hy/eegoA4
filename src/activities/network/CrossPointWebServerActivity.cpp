#include "CrossPointWebServerActivity.h"

#include <BlePushGatt.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>
#include <WiFi.h>
#include <esp_err.h>
#include <esp_heap_caps.h>  // internal-RAM-only gate probes (no PSRAM contamination)
#include <esp_system.h>

#include "BootMode.h"
#include "WifiTransferMode.h"
#include "network/WifiIpProbe.h"  // b65-s3c-pf9h: millisecond-slice IP probe
#include <esp_netif.h>      // b65-s3c-pf9f: STA IP-presence pre-begin check
#include <esp_wifi.h>       // esp_wifi_disconnect/stop/deinit recycle ladder

#include <cstddef>

#include "MappedInputManager.h"
#include "NetworkModeSelectionActivity.h"
#include "SilentRestart.h"
#include "WifiSelectionActivity.h"
#include "activities/network/CalibreConnectActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/PluginEvents.h"
#include "util/QrUtils.h"
#include "util/TaskWatchdog.h"

namespace {
// AP Mode configuration
constexpr const char* AP_SSID = "CrossPoint-Reader";
constexpr const char* AP_PASSWORD = nullptr;  // Open network for ease of use
constexpr const char* AP_HOSTNAME = "crosspoint";
constexpr uint8_t AP_CHANNEL = 1;
constexpr uint8_t AP_MAX_CONNECTIONS = 4;
constexpr int QR_CODE_WIDTH = 198;
constexpr int QR_CODE_HEIGHT = 198;

// DNS server for captive portal (redirects all DNS queries to our IP)
DNSServer* dnsServer = nullptr;
constexpr uint16_t DNS_PORT = 53;

// b65-s3c-pf9b-4a internal-heap probes. ESP.getFreeHeap()/esp_get_free_heap_size()
// include PSRAM and cannot answer the WiFi contiguous-block question.
static inline unsigned wifiInternalFree() {
  return static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}
static inline unsigned wifiInternalMax() {
  return static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

// b65-s3c-pf9f: same STA IP-presence test as CrossPointWebServer::begin(), so
// the pre-begin guard and the real gate agree (WiFi.status() is not reliable
// after the scan-cache path on this core). netif view first, Arduino fallback.
static bool actStaHasIp(IPAddress* outIp) {
  IPAddress ip;
  // "WIFI_STA_DEF" literal: this core has no ESP_NETIF_WIFI_STA_IF_KEY macro.
  esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (sta != nullptr) {
    esp_netif_ip_info_t info{};
    if (esp_netif_get_ip_info(sta, &info) == ESP_OK && info.ip.addr != 0) {
      ip = IPAddress(static_cast<uint32_t>(info.ip.addr));
    }
  }
  if (ip == IPAddress(0, 0, 0, 0)) ip = WiFi.localIP();
  if (outIp != nullptr) *outIp = ip;
  return ip != IPAddress(0, 0, 0, 0);
}

void stopDnsServer() {
  if (!dnsServer) return;

  dnsServer->stop();
  delete dnsServer;
  dnsServer = nullptr;
}

void restartMdns(const char* hostname, const char* tag) {
  // b65-s3c-pf9h: ESPmDNS 3.x internally calls esp_netif_* to bind the service
  // to an interface address.  Bracket MDNS.begin() to see whether THIS call is
  // where a previously-present IP disappears.  Observation only — no behaviour
  // change inside MDNS itself.
  wifi_ip_probe::emit("web_before_mdns_end");
  MDNS.end();
  wifi_ip_probe::emit("web_before_mdns_begin");
  if (MDNS.begin(hostname)) {
    wifi_ip_probe::emit("web_after_mdns_begin_ok");
    LOG_DBG(tag, "mDNS started: http://%s.local/", hostname);
  } else {
    wifi_ip_probe::emit("web_after_mdns_begin_fail");
    LOG_DBG(tag, "WARNING: mDNS failed to start");
  }
}

// 0..4 bars from RSSI (dBm), with 3 dBm hysteresis on currentBars to suppress flicker.
int barsForRssi(int rssi, int currentBars) {
  static constexpr int RISE_DBM[] = {-85, -75, -65, -55};
  static constexpr int FALL_DBM[] = {-88, -78, -68, -58};
  int bars = std::clamp(currentBars, 0, 4);
  while (bars < 4 && rssi >= RISE_DBM[bars]) bars++;
  while (bars > 0 && rssi < FALL_DBM[bars - 1]) bars--;
  return bars;
}
}  // namespace

void CrossPointWebServerActivity::onEnter() {
  Activity::onEnter();

  // b65-s3c-pf9b-4a gate point #0: before SD-font cache release mutates heap.
  logPrintf("INF", "WIFI", "[WIFI] gate point=webActOnEnter free=%u maxalloc=%u(internal)", wifiInternalFree(),
            wifiInternalMax());

  LOG_DBG("WEBACT", "Free heap at onEnter: %d bytes", ESP.getFreeHeap());

  // Heap-critical transition: WiFi (~45KB) plus the web server have to fit in
  // what's left of the ~380KB parts. SD-font caches retained for the CJK UI
  // fallback (mini glyph/kern arenas, ligature tables) are rebuildable on
  // demand — release them up front instead of aborting in startWebServer()
  // when the heap comes up short (observed on X3 with a Korean SD font).
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->releaseSdFontCaches();
    LOG_DBG("WEBACT", "Free heap after SD font cache release: %d bytes", ESP.getFreeHeap());
  }

  // Reset state
  state = WebServerActivityState::MODE_SELECTION;
  networkMode = NetworkMode::JOIN_NETWORK;
  isApMode = false;
  connectedIP.clear();
  connectedSSID.clear();
  lastHandleClientTime = 0;
  requestUpdate();

  if (startInJoinNetwork) {
    // Came back from the heap-defrag reboot on a pristine heap: skip the mode
    // picker and go straight into Join Network (onNetworkModeSelected won't
    // reboot again while this flag is set).
    onNetworkModeSelected(NetworkMode::JOIN_NETWORK);
    return;
  }

  // Launch network mode selection subactivity
  LOG_DBG("WEBACT", "Launching NetworkModeSelectionActivity...");
  startActivityForResult(std::make_unique<NetworkModeSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             onGoHome();
                           } else {
                             onNetworkModeSelected(std::get<NetworkModeResult>(result.data).mode);
                           }
                         });
}

void CrossPointWebServerActivity::onExit() {
  Activity::onExit();

  // b65-s3c-pf9d clause 10: in WiFi-transfer mode, "Done" simply resets the
  // boot flag and reboots. NEVER call esp_wifi_deinit() here — pf9b-4a proved it
  // corrupts the Arduino WiFi state machine (0x3001 NOT_INIT on the next boot).
  // A reboot returns to a pristine NORMAL-mode heap with BLE re-initialized by
  // setup(). This single interception covers every exit path (Back/Home/
  // upload-exit), so no per-call-site branch is needed.
  if (WIFI_TRANSFER_ACTIVE()) {
    // Clause 11 sync: destroy the web server first so any in-flight upload's
    // HalFile is closed (SdFat close() flushes the FAT entry + data to card)
    // BEFORE we reboot. Never call esp_wifi_deinit() (pf9b-4a: 0x3001 NOT_INIT).
    webServer.reset();
    bootmode::set(bootmode::Mode::NORMAL);
    LOG_INF("WIFI", "wifi-transfer done: files closed, boot_mode=NORMAL; restarting (no esp_wifi_deinit)");
    esp_restart();
    return;  // unreachable
  }

  LOG_DBG("WEBACT", "Free heap at onExit start: %d bytes", ESP.getFreeHeap());

  state = WebServerActivityState::SHUTTING_DOWN;

  // Application layer FIRST: DNS -> mDNS -> Web/WS server, then WiFi. No
  // handler may touch a deinitialised WiFi driver.
  stopDnsServer();
  MDNS.end();
  webServer.reset();

  // Web uploads may have installed or removed plugins; the session now ends in
  // place, so re-read event subscriptions here.
  pluginevents::refreshSubscriptions();

  // b65-s3c-pf9b-4a recycle ladder + b65-s3c-pf9c safe resume point. BLE is
  // resumed ONLY after WiFi is fully recycled, never at WiFi-page exit.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    esp_wifi_disconnect();
    logPrintf("INF", "WIFI", "[WIFI] recycle step=disconnect free=%u maxalloc=%u", wifiInternalFree(),
              wifiInternalMax());

    esp_wifi_stop();
    logPrintf("INF", "WIFI", "[WIFI] recycle step=stop free=%u maxalloc=%u", wifiInternalFree(),
              wifiInternalMax());

    const esp_err_t deinitRc = esp_wifi_deinit();
    logPrintf("INF", "WIFI", "[WIFI] recycle step=deinit free=%u maxalloc=%u", wifiInternalFree(),
              wifiInternalMax());
    logPrintf("INF", "WIFI", "[WIFI] recycle deinit rc=0x%X", static_cast<unsigned>(deinitRc));
  } else {
    logPrintf("INF", "WIFI", "[WIFI] recycle skipped (WiFi already NULL) free=%u maxalloc=%u", wifiInternalFree(),
              wifiInternalMax());
  }

  if (blepushgatt::wifiBleState() == blepushgatt::WifiBleState::STOPPED_FOR_WIFI) {
    const bool ok = blepushgatt::resumeForWifi();
    logPrintf("INF", "WFBLE", "[WFBLE] resume-after-wifi-recycle ok=%d state=%d", ok ? 1 : 0,
              static_cast<int>(blepushgatt::wifiBleState()));
  } else {
    logPrintf("INF", "WFBLE", "[WFBLE] resume-after-wifi-recycle skipped; state=%d",
              static_cast<int>(blepushgatt::wifiBleState()));
  }

  LOG_DBG("WEBACT", "Free heap at onExit end: %d bytes", ESP.getFreeHeap());
}

void CrossPointWebServerActivity::onNetworkModeSelected(const NetworkMode mode) {
  // Join Network brings up WiFi + the web server + TLS relays, whose working set
  // needs a large *contiguous* block. After reading/browsing the heap is
  // fragmented enough that tight boards (X3) abort mid-activation. Reboot once
  // into a pristine heap and land straight back here; the flag prevents a second
  // reboot on that return. No-op on touch boards, which fall through.
  if (mode == NetworkMode::JOIN_NETWORK && !startInJoinNetwork) {
    silentRestartToJoinNetwork();  // does not return on non-touch boards
  }

  // b65-s3c-pf9b-4a gate point #1: after the possible defrag reboot and before
  // WiFi.mode(WIFI_STA)/subactivity startup touch the heap.
  logPrintf("INF", "WIFI", "[WIFI] gate point=joinBranch free=%u maxalloc=%u(internal)", wifiInternalFree(),
            wifiInternalMax());

  const char* modeName = "Join Network";
  if (mode == NetworkMode::CONNECT_CALIBRE) {
    modeName = "Connect to Calibre";
  } else if (mode == NetworkMode::CREATE_HOTSPOT) {
    modeName = "Create Hotspot";
  } else if (mode == NetworkMode::OPDS_BROWSER) {
    modeName = "OPDS Browser";
#if FREEINK_CAP_USB_MSC
  } else if (mode == NetworkMode::USB_DRIVE) {
    modeName = "USB Drive";
#endif
  }
  LOG_DBG("WEBACT", "Network mode selected: %s", modeName);

  // The OPDS browser joins Wi-Fi itself. Wi-Fi is still on if the user backed
  // out of Join Network; turn it off so onExit has no WiFi to recycle.
  if (mode == NetworkMode::OPDS_BROWSER) {
    if (WiFi.getMode() != WIFI_MODE_NULL) {
      WiFi.disconnect(false);
      WiFi.mode(WIFI_OFF);
    }
    activityManager.goToBrowser();
    return;
  }

#if FREEINK_CAP_USB_MSC
  if (mode == NetworkMode::USB_DRIVE) {
    activityManager.goToUsbDrive();
    return;
  }
#endif

  networkMode = mode;
  isApMode = (mode == NetworkMode::CREATE_HOTSPOT);

  if (mode == NetworkMode::CONNECT_CALIBRE) {
    startActivityForResult(
        std::make_unique<CalibreConnectActivity>(renderer, mappedInput), [this](const ActivityResult& result) {
          state = WebServerActivityState::MODE_SELECTION;

          startActivityForResult(std::make_unique<NetworkModeSelectionActivity>(renderer, mappedInput),
                                 [this](const ActivityResult& result) {
                                   if (result.isCancelled) {
                                     onGoHome();
                                   } else {
                                     onNetworkModeSelected(std::get<NetworkModeResult>(result.data).mode);
                                   }
                                 });
        });
    return;
  }

  if (mode == NetworkMode::JOIN_NETWORK) {
    // STA mode - launch WiFi selection
    LOG_DBG("WEBACT", "Turning on WiFi (STA mode)...");
    const bool staModeOk = WiFi.mode(WIFI_STA);
    logPrintf("INF", "WIFI", "[WIFI] session point=modeSta rc=%d free=%u maxalloc=%u(internal)",
              static_cast<int>(staModeOk ? 0 : 1), wifiInternalFree(), wifiInternalMax());

    state = WebServerActivityState::WIFI_SELECTION;
    LOG_DBG("WEBACT", "Launching WifiSelectionActivity...");
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& wifi = std::get<WifiResult>(result.data);
                               connectedIP = wifi.ip;
                               connectedSSID = wifi.ssid;
                             }
                             onWifiSelectionComplete(!result.isCancelled);
                           });
  } else {
    // AP mode - start access point
    state = WebServerActivityState::AP_STARTING;
    requestUpdate();
    startAccessPoint();
  }
}

void CrossPointWebServerActivity::onWifiSelectionComplete(const bool connected) {
  // b65-s3c-pf9h: bracket the completion callback — the first slice after
  // WifiSelectionActivity::onExit() in the 12 ms window.
  wifi_ip_probe::emit("web_cb_enter");
  LOG_DBG("WEBACT", "WifiSelectionActivity completed, connected=%d", connected);

  if (connected) {
    // Get connection info before exiting subactivity
    isApMode = false;

    // b65-s3c-pf9h (P1, semantics + likely workaround): mDNS used to start
    // HERE, unconditionally, before any IP check — on the pf9g field run
    // "mDNS started" printed while localIP() was already 0.0.0.0.  ESPmDNS 3.x
    // binds to a netif address, so starting it pre-IP is wrong (and a suspect
    // in the 12 ms loss window).  The STA mDNS start is now deferred into
    // startWebServer(), AFTER its IP poll confirms a non-zero address.
    wifi_ip_probe::emit("web_cb_ip_defer_mdns");

    // Start the web server
    startWebServer();
  } else {
    // User cancelled - go back to mode selection
    state = WebServerActivityState::MODE_SELECTION;

    startActivityForResult(std::make_unique<NetworkModeSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) {
                               onGoHome();
                             } else {
                               onNetworkModeSelected(std::get<NetworkModeResult>(result.data).mode);
                             }
                           });
  }
}

void CrossPointWebServerActivity::startAccessPoint() {
  LOG_DBG("WEBACT", "Starting Access Point mode...");
  LOG_DBG("WEBACT", "Free heap before AP start: %d bytes", ESP.getFreeHeap());

  // Configure and start the AP
  WiFi.mode(WIFI_AP);
  delay(100);

  // Start soft AP
  bool apStarted;
  if (AP_PASSWORD && strlen(AP_PASSWORD) >= 8) {
    apStarted = WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, false, AP_MAX_CONNECTIONS);
  } else {
    // Open network (no password)
    apStarted = WiFi.softAP(AP_SSID, nullptr, AP_CHANNEL, false, AP_MAX_CONNECTIONS);
  }

  if (!apStarted) {
    LOG_ERR("WEBACT", "ERROR: Failed to start Access Point!");
    onGoHome();
    return;
  }

  delay(100);  // Wait for AP to fully initialize

  // Get AP IP address
  const IPAddress apIP = WiFi.softAPIP();
  char ipStr[16];
  snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", apIP[0], apIP[1], apIP[2], apIP[3]);
  connectedIP = ipStr;
  connectedSSID = AP_SSID;

  LOG_DBG("WEBACT", "Access Point started!");
  LOG_DBG("WEBACT", "SSID: %s", AP_SSID);
  LOG_DBG("WEBACT", "IP: %s", connectedIP.c_str());

  // Start mDNS for hostname resolution
  restartMdns(AP_HOSTNAME, "WEBACT");

  // Start DNS server for captive portal behavior
  // This redirects all DNS queries to our IP, making any domain typed resolve to us
  stopDnsServer();
  dnsServer = new DNSServer();
  dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer->start(DNS_PORT, "*", apIP);
  LOG_DBG("WEBACT", "DNS server started for captive portal");

  LOG_DBG("WEBACT", "Free heap after AP start: %d bytes", ESP.getFreeHeap());

  // Start the web server
  startWebServer();
}

void CrossPointWebServerActivity::startWebServer() {
  LOG_DBG("WEBACT", "Starting web server...");

  // b65-s3c-pf9f (P0): belt-and-braces pre-begin guard, aligned with the real
  // gate in CrossPointWebServer::begin().  WiFi.status() proved unreliable
  // after the scan-cache path (stale 6 across the whole window in the field),
  // so wait up to 3 s (6x500 ms) for a non-zero STA IP and log every sample as
  // "P0 ip poll i/6: ip=x.x.x.x" — the exact format begin() uses, so field logs
  // from the two layers can be compared directly.
  if (!isApMode) {
    constexpr int kIpPolls = 6;
    constexpr unsigned long kIpPollMs = 500;
    IPAddress ip;
    bool haveIp = actStaHasIp(&ip);
    for (int poll = 1; poll <= kIpPolls && !haveIp; ++poll) {
      LOG_DBG("WEBACT", "P0 ip poll %d/%d: ip=%s", poll, kIpPolls, ip.toString().c_str());
      delay(kIpPollMs);
      haveIp = actStaHasIp(&ip);
    }
    LOG_DBG("WEBACT", "P0 ip poll final ip=%s", ip.toString().c_str());

    // b65-s3c-pf9h (P1, semantics + likely workaround): start the STA mDNS
    // only AFTER the IP poll confirms a non-zero address.  This is the slice's
    // last boundary before webServer->begin(); bracketing MDNS.begin() inside
    // restartMdns() shows whether the call itself drops the IP.
    wifi_ip_probe::emit("web_before_mdns_gate");
    if (haveIp) {
      restartMdns(AP_HOSTNAME, "WEBACT");
    } else {
      LOG_DBG("WEBACT", "pf9h: IP poll found no address, skip mDNS (server gate will triage)");
      wifi_ip_probe::emit("web_mdns_skipped_no_ip");
    }
  }

  // Repeat the release right before the allocation: the WiFi selection screen
  // rendered since onEnter(), and a CJK SSID repopulates the SD-font caches.
  if (auto* fcm = renderer.getFontCacheManager()) {
    LOG_DBG("WEBACT", "Free heap before SD font cache release: %d bytes", ESP.getFreeHeap());
    fcm->releaseSdFontCaches();
    LOG_DBG("WEBACT", "Free heap before server alloc: %d bytes", ESP.getFreeHeap());
  }

  // Create the web server instance
  webServer.reset(new CrossPointWebServer());
  // An upload holds loop() inside handleClient(), so input is sampled on each received chunk instead. On a slow
  // link chunks come ~0.5 s apart, so a button found down gets a second sample past the SDK's 5 ms debounce.
  webServer->setUploadCancelCheck([this] {
    mappedInput.update(true);
    if (gpio.rawInputActive()) {
      delay(6);
      mappedInput.update(true);
    }
    leaveRequested = leaveRequested || mappedInput.isPressed(MappedInputManager::Button::Back) ||
                     mappedInput.wasPressed(MappedInputManager::Button::Back) || mappedInput.wasHomeGesture();
    return leaveRequested;
  });
  webServer->begin();

  if (webServer->isRunning()) {
    state = WebServerActivityState::SERVER_RUNNING;
    wifiSessionStartedAt = millis();
    LOG_DBG("WEBACT", "Web server started successfully");
    lastWifiBars = isApMode ? 0 : barsForRssi(WiFi.RSSI(), 0);

    // The device is online: deliver queued plugin events through their
    // manifest handlers. STA only (AP mode has no internet route); bounded by
    // the drain's per-call event budget so serving is not noticeably delayed.
    if (!isApMode) {
      pluginevents::drain(&renderer);
    }

    // Force an immediate render since we're transitioning from a subactivity
    // that had its own rendering task. We need to make sure our display is shown.
    requestUpdate();
  } else {
    LOG_ERR("WEBACT", "ERROR: Failed to start web server!");
    webServer.reset();

    // b65-s3c-pf9e problem D: in WiFi-transfer mode a failed server start must
    // NOT fall through to the home/exit path, whose WIFI_TRANSFER_ACTIVE()
    // branch commits boot_mode=NORMAL and reboots. Instead stay on the WiFi
    // selection page with a retry hint so the user can pick again or return.
    // The reboot is reserved for the user's explicit Done/exit later.
    if (WIFI_TRANSFER_ACTIVE() && !isApMode) {
      LOG_INF("WEBACT", "P1 server start failed in WiFi-transfer mode: staying on WiFi selection for retry (no reboot)");
      state = WebServerActivityState::WIFI_SELECTION;
      startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput,
                                                                     /*autoConnect=*/true,
                                                                     /*retryHintOnEnter=*/true),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 const auto& wifi = std::get<WifiResult>(result.data);
                                 connectedIP = wifi.ip;
                                 connectedSSID = wifi.ssid;
                                 onWifiSelectionComplete(true);
                               } else {
                                 onWifiSelectionComplete(false);
                               }
                             });
      return;
    }

    // Non-WiFi-transfer modes keep the original back-on-error behaviour.
    onGoHome();
  }
}

void CrossPointWebServerActivity::loop() {
  // b65-s3c-pf9d clause 11: an idle WiFi-transfer session auto-returns to
  // NORMAL after 30 minutes (b65-s3c-pf9j, was 10) of no physical input.
  // A plain reboot restores a pristine NORMAL heap; esp_wifi_deinit() is
  // deliberately not called.
  if (WIFI_TRANSFER_ACTIVE() && state == WebServerActivityState::SERVER_RUNNING && wifiSessionStartedAt != 0) {
    if (gpio.rawInputActive()) {
      wifiSessionStartedAt = millis();  // user is present — restart the idle clock
    }
    if ((millis() - wifiSessionStartedAt) >= WIFI_IDLE_RESTART_MS) {
      webServer.reset();  // flush/close any in-flight file before reboot
      LOG_INF("WIFI", "wifi-transfer idle 30 min: boot_mode=NORMAL; restarting");
      bootmode::set(bootmode::Mode::NORMAL);
      esp_restart();
      return;
    }
  }

  // Handle different states
  if (state == WebServerActivityState::SERVER_RUNNING) {
    // Handle DNS requests for captive portal (AP mode only)
    if (isApMode && dnsServer) {
      dnsServer->processNextRequest();
    }

    // STA mode: Monitor WiFi connection health
    if (!isApMode && webServer && webServer->isRunning()) {
      static unsigned long lastWifiCheck = 0;
      if (millis() - lastWifiCheck > 2000) {  // Check every 2 seconds
        lastWifiCheck = millis();
        const wl_status_t wifiStatus = WiFi.status();
        // Driver auto-reconnect handles retries; abandon (via onGoHome) only
        // after WIFI_ABANDON_MS, otherwise the activity freezes on a blip.
        bool repaint = false;
        if (wifiStatus != WL_CONNECTED) {
          if (consecutiveDisconnects == 0) {
            firstDisconnectAt = millis();
            repaint = true;
          }
          consecutiveDisconnects++;
          LOG_DBG("WEBACT", "WiFi not connected (status=%d, consecutive=%d, total=%lu ms)", wifiStatus,
                  consecutiveDisconnects, millis() - firstDisconnectAt);
          if (millis() - firstDisconnectAt > WIFI_ABANDON_MS) {
            LOG_DBG("WEBACT", "WiFi unavailable for >%lu s; returning to network selection", WIFI_ABANDON_MS / 1000UL);
            state = WebServerActivityState::SHUTTING_DOWN;
            onGoHome();
            return;
          }
        } else {
          if (consecutiveDisconnects > 0) {
            LOG_DBG("WEBACT", "WiFi recovered after %d failed checks (%lu ms)", consecutiveDisconnects,
                    millis() - firstDisconnectAt);
            repaint = true;
          }
          consecutiveDisconnects = 0;
          firstDisconnectAt = 0;
          const int rssi = WiFi.RSSI();
          if (rssi < -75) {
            LOG_DBG("WEBACT", "Warning: Weak WiFi signal: %d dBm", rssi);
          }
          const int bars = barsForRssi(rssi, lastWifiBars);
          if (bars != lastWifiBars) {
            lastWifiBars = bars;
            repaint = true;
          }
        }
        if (repaint) requestUpdate();
      }
    }

    // Handle web server requests - maximize throughput with watchdog safety
    if (webServer && webServer->isRunning()) {
      const unsigned long timeSinceLastHandleClient = millis() - lastHandleClientTime;

      // Log if there's a significant gap between handleClient calls (>100ms)
      if (lastHandleClientTime > 0 && timeSinceLastHandleClient > 100) {
        LOG_DBG("WEBACT", "WARNING: %lu ms gap since last handleClient", timeSinceLastHandleClient);
      }

      // Reset watchdog BEFORE processing - HTTP header parsing can be slow
      resetTaskWatchdogIfSubscribed();

      // Process HTTP requests in tight loop for maximum throughput
      // More iterations = more data processed per main loop cycle
      constexpr int MAX_ITERATIONS = 500;
      for (int i = 0; i < MAX_ITERATIONS && webServer->isRunning(); i++) {
        webServer->handleClient();
        if (leaveRequested) {
          onGoHome();
          return;
        }
        // Reset watchdog every 32 iterations
        if ((i & 0x1F) == 0x1F) {
          resetTaskWatchdogIfSubscribed();
        }
        // Yield and check for exit button every 64 iterations
        if ((i & 0x3F) == 0x3F) {
          yield();
          // Pump input inside this blocking loop so exit events remain responsive.
          mappedInput.update(true);
          // Home remains available now; other configured actions are deferred
          // to the next main-loop pass.
          if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasHomeGesture()) {
            onGoHome();
            return;
          }
        }
      }
      lastHandleClientTime = millis();
    }

    // Also check outside the request-processing loop.
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasHomeGesture()) {
      onGoHome();
      return;
    }
  }
}

void CrossPointWebServerActivity::render(RenderLock&&) {
  // Only render our own UI when server is running
  // Subactivities handle their own rendering
  if (state == WebServerActivityState::SERVER_RUNNING || state == WebServerActivityState::AP_STARTING) {
    renderer.clearScreen();
    const auto& metrics = UITheme::getInstance().getMetrics();
    const auto pageWidth = renderer.getScreenWidth();
    const auto pageHeight = renderer.getScreenHeight();

    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                   WIFI_TRANSFER_ACTIVE() ? tr(STR_MODE_WIFI_TITLE)
                                          : (isApMode ? tr(STR_HOTSPOT_MODE) : tr(STR_FILE_TRANSFER)),
                   nullptr);

    if (state == WebServerActivityState::SERVER_RUNNING) {
      GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                        connectedSSID.c_str());
      renderServerRunning();
    } else {
      const auto height = renderer.getLineHeight(UI_10_FONT_ID);
      const auto top = (pageHeight - height) / 2;
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_STARTING_HOTSPOT));
    }
    renderer.displayBuffer();
  }
}

void CrossPointWebServerActivity::renderServerRunning() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  if (!isApMode) {
    renderWifiIndicator(metrics.topPadding + metrics.headerHeight);
  }

  int startY = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing * 2;
  int height10 = renderer.getLineHeight(UI_10_FONT_ID);
  if (isApMode) {
    // AP mode display
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, startY, tr(STR_CONNECT_WIFI_HINT), true,
                      EpdFontFamily::BOLD);
    startY += height10 + metrics.verticalSpacing * 2;

    // Show QR code for Wifi
    // follows spec at https://github.com/zxing/zxing/wiki/Barcode-Contents#wi-fi-network-config-android-ios-11
    const std::string wifiConfig = std::string("WIFI:T:nopass;S:") + connectedSSID + ";;";
    const Rect qrBoundsWifi(metrics.contentSidePadding, startY, QR_CODE_WIDTH, QR_CODE_HEIGHT);
    QrUtils::drawQrCode(renderer, qrBoundsWifi, wifiConfig);

    // Show network name
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding + QR_CODE_WIDTH + metrics.verticalSpacing, startY + 80,
                      connectedSSID.c_str());

    startY += QR_CODE_HEIGHT + 2 * metrics.verticalSpacing;

    // Show primary URL (hostname)
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, startY, tr(STR_OPEN_URL_HINT), true,
                      EpdFontFamily::BOLD);
    startY += height10 + metrics.verticalSpacing * 2;

    std::string hostnameUrl = std::string("http://") + AP_HOSTNAME + ".local/";
    std::string ipUrl = tr(STR_OR_HTTP_PREFIX) + connectedIP + "/";

    // Show QR code for URL
    const Rect qrBoundsUrl(metrics.contentSidePadding, startY, QR_CODE_WIDTH, QR_CODE_HEIGHT);
    QrUtils::drawQrCode(renderer, qrBoundsUrl, hostnameUrl);

    // Show IP address as fallback
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding + QR_CODE_WIDTH + metrics.verticalSpacing, startY + 80,
                      hostnameUrl.c_str());
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding + QR_CODE_WIDTH + metrics.verticalSpacing, startY + 100,
                      ipUrl.c_str());
  } else {
    startY += metrics.verticalSpacing * 2;

    // STA mode display (original behavior)
    // std::string ipInfo = "IP Address: " + connectedIP;
    renderer.drawCenteredText(UI_10_FONT_ID, startY, tr(STR_OPEN_URL_HINT), true, EpdFontFamily::BOLD);
    startY += height10;
    renderer.drawCenteredText(UI_10_FONT_ID, startY, tr(STR_SCAN_QR_HINT), true, EpdFontFamily::BOLD);
    startY += height10 + metrics.verticalSpacing * 2;

    // Show QR code for URL
    std::string webInfo = "http://" + connectedIP + "/";
    const Rect qrBounds((pageWidth - QR_CODE_WIDTH) / 2, startY, QR_CODE_WIDTH, QR_CODE_HEIGHT);
    QrUtils::drawQrCode(renderer, qrBounds, webInfo);
    startY += QR_CODE_HEIGHT + metrics.verticalSpacing * 2;

    // Show web server URL prominently
    renderer.drawCenteredText(UI_10_FONT_ID, startY, webInfo.c_str(), true);
    startY += height10 + 5;

    // Also show hostname URL
    std::string hostnameUrl = std::string(tr(STR_OR_HTTP_PREFIX)) + AP_HOSTNAME + ".local/";
    renderer.drawCenteredText(SMALL_FONT_ID, startY, hostnameUrl.c_str(), true);
  }

  if (WIFI_TRANSFER_ACTIVE()) {
    // b65-s3c-pf9d: fixed hint under the transfer page — push is paused for the
    // whole session; completing the transfer reboots back to NORMAL mode.
    const int hintY = renderer.getScreenHeight() - renderer.getLineHeight(SMALL_FONT_ID) - 20;
    renderer.drawCenteredText(SMALL_FONT_ID, hintY, tr(STR_MODE_WIFI_HINT), true);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void CrossPointWebServerActivity::renderWifiIndicator(int subHeaderTop) const {
  constexpr int BAR_COUNT = 4;
  constexpr int BAR_WIDTH = 4;
  constexpr int BAR_GAP = 2;
  constexpr int ICON_HEIGHT = 14;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int iconWidth = BAR_COUNT * BAR_WIDTH + (BAR_COUNT - 1) * BAR_GAP;
  const int iconRight = renderer.getScreenWidth() - metrics.contentSidePadding;
  const int iconLeft = iconRight - iconWidth;
  const int iconBottom = subHeaderTop + metrics.tabBarHeight - metrics.verticalSpacing;

  const bool wifiUp = (WiFi.status() == WL_CONNECTED) && (consecutiveDisconnects == 0);
  if (wifiUp) {
    for (int i = 0; i < BAR_COUNT; i++) {
      const int barHeight = (i + 1) * ICON_HEIGHT / BAR_COUNT;
      const int x = iconLeft + i * (BAR_WIDTH + BAR_GAP);
      const int y = iconBottom - barHeight;
      if (i < lastWifiBars) {
        renderer.fillRect(x, y, BAR_WIDTH, barHeight, true);
      } else {
        renderer.drawRect(x, y, BAR_WIDTH, barHeight, true);
      }
    }
  } else {
    const int xSize = ICON_HEIGHT;
    const int x0 = iconRight - xSize;
    const int y0 = iconBottom - xSize;
    renderer.drawLine(x0, y0, x0 + xSize, y0 + xSize, 2, true);
    renderer.drawLine(x0, y0 + xSize, x0 + xSize, y0, 2, true);
  }
}
