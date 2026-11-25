#pragma once

#include <functional>
#include <memory>
#include <string>

#include "NetworkModeSelectionActivity.h"
#include "activities/Activity.h"
#include "network/CrossPointWebServer.h"

// Web server activity states
enum class WebServerActivityState {
  MODE_SELECTION,  // Choosing between Join Network and Create Hotspot
  WIFI_SELECTION,  // WiFi selection subactivity is active (for Join Network mode)
  AP_STARTING,     // Starting Access Point mode
  SERVER_RUNNING,  // Web server is running and handling requests
  SHUTTING_DOWN    // Shutting down server and WiFi
};

/**
 * CrossPointWebServerActivity is the entry point for file transfer functionality.
 * It:
 * - First presents a choice between "Join a Network" (STA), "Calibre Wireless", "Create Hotspot" (AP), and
 *   "OPDS Browser" (which hands off to the browser before Wi-Fi starts)
 * - For STA mode: Launches WifiSelectionActivity to connect to an existing network
 * - For AP mode: Creates an Access Point that clients can connect to
 * - Starts the CrossPointWebServer when connected
 * - Handles client requests in its loop() function
 * - Cleans up the server and shuts down WiFi on exit
 */
class CrossPointWebServerActivity final : public Activity {
  WebServerActivityState state = WebServerActivityState::MODE_SELECTION;

  // Network mode
  NetworkMode networkMode = NetworkMode::JOIN_NETWORK;
  bool isApMode = false;
  // Set when re-entered after the heap-defrag reboot: skip mode selection, go
  // straight to Join Network, and don't reboot again.
  bool startInJoinNetwork = false;

  // Web server - owned by this activity
  std::unique_ptr<CrossPointWebServer> webServer;

  // Server status
  std::string connectedIP;
  std::string connectedSSID;  // For STA mode: network name, For AP mode: AP name

  // Performance monitoring
  unsigned long lastHandleClientTime = 0;

  // Set when Back or Home is seen while an upload holds handleClient().
  bool leaveRequested = false;

  // Sustained WiFi-loss tracking; abandon only after WIFI_ABANDON_MS.
  int consecutiveDisconnects = 0;
  unsigned long firstDisconnectAt = 0;
  static constexpr unsigned long WIFI_ABANDON_MS = 5UL * 60UL * 1000UL;

  // b65-s3c-pf9d clause 11: auto-return to NORMAL after an idle WiFi-transfer
  // session. The timer starts when the web server comes up.
  // b65-s3c-pf9j: 10 min -> 30 min (1800000 ms); this constant is used ONLY by
  // the WiFi-transfer SERVER_RUNNING idle loop, not any reader/sleep timeout.
  unsigned long wifiSessionStartedAt = 0;
  static constexpr unsigned long WIFI_IDLE_RESTART_MS = 30UL * 60UL * 1000UL;  // 1800000 ms

  // Cached signal-strength bracket (0..4) for the WiFi indicator.
  int lastWifiBars = 0;

  // b65-s3c-pf9l: one-shot Weak-signal log throttle — log only on state change
  // instead of every 2 s (field: 40+ lines in 379 s drowned other evidence).
  bool lastWeakWarned = false;

  // b65-s3c-pf9l: set while the no-reboot in-session network switch is active.
  // Used only by the return path to avoid re-entering server init twice.
  bool networkSwitchInProgress = false;

  // b65-s3c-pf9l: guard so an in-flight exit confirmation cannot be stacked.
  bool exitConfirmPending = false;

  // b65-s3c-pf9l2b: per-frame snapshot of "an upload is in flight", sampled
  // once at the top of loop(). Grey-out and the Confirm gate read this same
  // value so the button cannot look dimmed yet still execute on press.
  bool uploadingNow = false;

  // b65-s3c-pf9l2b: the single "change network" button rectangle, filled by
  // renderServerRunning() and consumed by the loop() touch hit test. Rendering
  // and hit testing SHARE these exact values — one computation, one {x,y,w,h}.
  struct ButtonRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
  };
  mutable ButtonRect switchButton_{};

  // b65-s3c-pf9l2b: single-rectangle containment. Pure function so host
  // assertions can run the real geometry (centre / four edges / one-line-below).
  static bool buttonHit(const ButtonRect& r, int x, int y) {
    return r.w > 0 && r.h > 0 && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
  }

  void renderServerRunning() const;
  void renderWifiIndicator(int subHeaderTop) const;

  void onNetworkModeSelected(NetworkMode mode);
  void onWifiSelectionComplete(bool connected);
  void startAccessPoint();
  void startWebServer();

  // b65-s3c-pf9l: in-session "change network" without reboot; see .cpp.
  void startNetworkSwitch();
  void onNetworkSwitchResult(const ActivityResult& result);
  void requestExitWithConfirmation();
  // b65-s3c-pf9l2: upload-in-flight switch request — confirm the interruption,
  // then reuse the exact same startNetworkSwitch() path (no second teardown).
  void requestSwitchDuringUpload();
  // b65-s3c-pf9l2b: THE single switch_network dispatch. Both the top-level
  // Confirm key and the single-button touch call this one function; it applies
  // the upload gate once. No separate dispatch is duplicated per input source.
  void requestNetworkSwitch();

 public:
  explicit CrossPointWebServerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       bool startInJoinNetwork = false)
      : Activity("CrossPointWebServer", renderer, mappedInput), startInJoinNetwork(startInJoinNetwork) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return webServer && webServer->isRunning(); }
  bool preventAutoSleep() override { return webServer && webServer->isRunning(); }
};
