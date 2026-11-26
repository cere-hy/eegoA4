#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

struct Rect;
struct ThemeMetrics;
struct WifiCredential;

// Structure to hold WiFi network information
struct WifiNetworkInfo {
  std::string ssid;
  int32_t rssi;
  bool isEncrypted;
  bool hasSavedPassword;             // Whether we have saved credentials for this network
  bool isHiddenPlaceholder = false;  // Synthetic "Add hidden network..." list entry
};

// WiFi selection states
enum class WifiSelectionState {
  AUTO_CONNECTING,    // Trying to connect to the last known network
  SCANNING,           // Scanning for networks
  NETWORK_LIST,       // Displaying available networks
  HIDDEN_SSID_ENTRY,  // Entering SSID for a hidden network
  PASSWORD_ENTRY,     // Entering password for selected network
  CONNECTING,         // Attempting to connect
  CONNECTED,          // Successfully connected
  SAVE_PROMPT,        // Asking user if they want to save the password
  CONNECTION_FAILED,  // Connection failed
  FORGET_PROMPT       // Asking user if they want to forget the network
};

/**
 * WifiSelectionActivity is responsible for scanning WiFi APs and connecting to them.
 * It will:
 * - Enter scanning mode on entry
 * - List available WiFi networks
 * - Allow selection and launch KeyboardEntryActivity for password if needed
 * - Save the password if requested
 * - Call onComplete callback when connected or cancelled
 *
 * The onComplete callback receives true if connected successfully, false if cancelled.
 */
class WifiSelectionActivity final : public Activity, private UiAppHost {
  ButtonNavigator buttonNavigator;

  WifiSelectionState state = WifiSelectionState::SCANNING;
  size_t selectedNetworkIndex = 0;
  std::vector<WifiNetworkInfo> networks;
  // Number of real (scanned) networks, excluding the synthetic hidden-network entry
  size_t realNetworkCount = 0;

  // Row buffers derived from `networks`, rebuilt only when it changes
  // (processWifiScanResults()) instead of on every repaint — buildListScreen()
  // used to re-derive a "+ * ||||" status string per network on every render
  // (cursor move, tap flash, ...).
  std::vector<std::string> networkStatuses;
  std::vector<freeink::ui::ListItem> networkRowItems;
  void rebuildNetworkRowItems();

  // Selected network for connection
  std::string selectedSSID;
  bool selectedRequiresPassword = false;

  // Connection result
  std::string connectedIP;
  std::string connectionError;

  // b65-s3c-pf9c: WiFi/BLE time-division. BLE is torn down on entering this
  // page and resumed by the parent after WiFi is fully recycled. A DEAD state
  // is terminal and must not be retried.
  bool bleStoppedForWifi = false;  // this page performed the L2 teardown
  bool bleDead = false;            // stop/resume failed terminally (reboot hint)

  // Password to potentially save (from keyboard or saved credentials)
  std::string enteredPassword;

  // Cached MAC address string for display
  std::string cachedMacAddress;

  // Whether network was connected using a saved password (skip save prompt)
  bool usedSavedPassword = false;

  // Whether to attempt auto-connect on entry
  const bool allowAutoConnect;

  // b65-s3c-pf9e problem D: when the parent's web server failed to start after
  // an ostensibly successful join, re-enter this page showing a one-shot
  // "connection failed, retry" hint instead of auto-connecting again. Suppresses
  // auto-connect so the user is left on the network list / retry prompt.
  const bool retryHintOnEnter;

  // b65-s3c-pf9l5: display-only gate for the failure hint line. Default true
  // keeps every pre-existing call site behaviour-identical to pf9l4; only
  // requestNetworkSwitch() passes false to suppress the misleading
  // "connection failed, retry" text on a proactive network change. It never
  // feeds the state machine, the auto-connect short-circuit, nor linkHandedOver.
  const bool showFailureHint;

  // Whether we are attempting to auto-connect or auto-scan saved networks.
  bool autoConnecting = false;

  // b65-s3c-pf9i: link ownership handed to the Web server on a successful
  // join.  When true, onExit() must NOT tear WiFi down — the parent Web
  // activity needs the live association/IP.  Kept separate from `state`
  // (which has 20+ render/loop consumers) so a success never depends on a
  // state mutation.  No underscore prefix, mirroring `autoConnecting`.
  bool linkHandedOver = false;

  // True from the Confirm press that stops auto-connect until that button is released.
  bool manualNetworkListRequested = false;

  // Saved SSIDs already attempted during the current auto-connect session.
  std::vector<std::string> autoAttemptedSsids;

  // Save/forget prompt selection (0 = Yes, 1 = No)
  int savePromptSelection = 0;
  int forgetPromptSelection = 0;

  // Connection timeout
  static constexpr unsigned long CONNECTION_TIMEOUT_MS = 15000;
  static constexpr unsigned long AUTO_CONNECTION_TIMEOUT_MS = 7000;
  unsigned long connectionStartTime = 0;

  // b65-s3c-pf9h (P0, observation + judgement): WL_CONNECTED (evt=3) proves
  // GOT_IP fired, but the IP can be gone by the time the parent starts the web
  // server. Gate success on a non-zero localIP() instead of status alone, and
  // hold the page on "getting IP" (15 s sub-window) while logging the IP trail.
  bool waitingForIp = false;
  unsigned long ipWaitStartTime = 0;
  static constexpr unsigned long IP_WAIT_TIMEOUT_MS = 15000;

  // The UiAppHost app hosts the network list and the save/forget prompts
  // (themed rows and dialogs, touch routing); every other state keeps its
  // legacy centered-text rendering.
  // Viewport memory (top/visibleRows) for the network list; `selected` is
  // mirrored from selectedNetworkIndex at build/move time.
  freeink::ui::ListNav listNav;

  static void listScreen(UiScreen& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onScanEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onPromptEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiScreen& screen);
  void buildPromptDialog(UiScreen& screen);

  void renderNetworkList(const Rect* screen, const ThemeMetrics* metrics);
  void renderPasswordEntry(const Rect* screen, const ThemeMetrics* metrics) const;
  void renderConnecting(const Rect* screen, const ThemeMetrics* metrics) const;
  void renderConnected(const Rect* screen, const ThemeMetrics* metrics) const;
  void renderConnectionFailed(const Rect* screen, const ThemeMetrics* metrics) const;

  void startWifiScan(bool autoScan = false);
  void processWifiScanResults();
  void appendHiddenNetworkEntry();
  void selectNetwork(int index);
  void promptHiddenSsid();
  void promptPasswordEntry();
  void attemptConnection();
  void checkConnectionStatus();
  bool tryAutoConnectCredential(const WifiCredential& cred);
  bool tryNextSavedNetworkFromScan();
  void handleAutoConnectFailure();
  void showNetworkListFromAutoConnect();
  bool hasAttemptedAutoSsid(const std::string& ssid) const;
  static std::string getSignalStrengthIndicator(int32_t rssi);

  void onComplete(bool connected);

 public:
  explicit WifiSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool autoConnect = true,
                                 bool retryHintOnEnter = false, bool showFailureHint = true);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
};
