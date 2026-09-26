/*
 * Solar Bridge Matrix Display
 * ────────────────────────────
 * A tiny satellite display for the Solar Bridge inverter/BMS monitoring
 * system (github.com/manoranjan2050/Solar-Bridge-Flin-Fution-JKBMS).
 * Polls the same /api/state endpoint the web dashboard, Android app and
 * SolarBridge-LCD use, and scrolls solar / load / battery / grid / backup
 * & charge time readings across a MAX7219 8x32 LED dot-matrix display.
 *
 * Hardware: ESP8266 (NodeMCU / Wemos D1 Mini) + MAX7219 8x32 dot matrix
 * (4x cascaded 8x8 FC-16 modules), hardware SPI:
 *   CLK -> D5 (SCK)   DIN -> D7 (MOSI)   CS -> D6
 *
 * First boot (or held-FLASH-button reset) opens a WiFi setup portal —
 * connect to it from a phone, no code changes needed. See README.md.
 *
 * Libraries (Library Manager): WiFiManager (tzapu), ArduinoJson (>=6.19),
 * MD_MAX72XX + MD_Parola (majicdesigns), LittleFS (bundled with the
 * ESP8266 core).
 */

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <ArduinoOTA.h>
#include <SPI.h>
#include <MD_MAX72xx.h>
#include <MD_Parola.h>

// Optional, gitignored — lets you hardcode WiFi/server/token for a fast
// local flash without going through the captive portal each time. See
// secrets.h.example. Never commit a real secrets.h.
#if __has_include("secrets.h")
#include "secrets.h"
#endif

// ── MAX7219 matrix wiring — hardware SPI (CLK=D5/SCK, DIN=D7/MOSI are
// fixed by the ESP8266's SPI peripheral), CS is the only pin we choose. ──
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES 4
#define CS_PIN D6

MD_Parola P = MD_Parola(HARDWARE_TYPE, CS_PIN, MAX_DEVICES);

// If your text comes out mirrored/garbled, your modules aren't FC-16 —
// try MD_MAX72XX::GENERIC_HW, PAROLA_HW or ICSTATION_HW instead.

const uint8_t SCROLL_SPEED = 40;   // ms per column step — lower = faster
const textEffect_t SCROLL_IN = PA_SCROLL_LEFT;
const textEffect_t SCROLL_OUT = PA_SCROLL_LEFT;

// ── Config persisted via WiFiManager's custom parameters ───────────────
#define CONFIG_PATH "/config.json"

#ifndef DEFAULT_SERVER_URL
#define DEFAULT_SERVER_URL "https://solar.manoranjan.dev"
#endif
#ifndef DEFAULT_API_TOKEN
#define DEFAULT_API_TOKEN ""
#endif
#ifndef DEFAULT_WIFI_SSID
#define DEFAULT_WIFI_SSID ""
#endif
#ifndef DEFAULT_WIFI_PASS
#define DEFAULT_WIFI_PASS ""
#endif
// Optional second (backup) network — tried if the primary one fails to
// connect at boot, and alternated with on every reconnect attempt while
// running. Leave undefined/empty if you only have one network.
#ifndef DEFAULT_WIFI_SSID2
#define DEFAULT_WIFI_SSID2 ""
#endif
#ifndef DEFAULT_WIFI_PASS2
#define DEFAULT_WIFI_PASS2 ""
#endif
// OTA (over-the-air) update password — set this in secrets.h so re-flashing
// over WiFi requires it. Left blank, OTA still works but anyone on the same
// network could push firmware to the board.
#ifndef DEFAULT_OTA_PASSWORD
#define DEFAULT_OTA_PASSWORD ""
#endif

char serverUrl[96] = DEFAULT_SERVER_URL;
char apiToken[64] = DEFAULT_API_TOKEN;
char pollSecondsStr[4] = "5";
uint32_t pollIntervalMs = 5000;

// ── State polled from /api/state ────────────────────────────────────────
struct SolarState {
  bool valid = false;
  float pvPower = 0, pvToday = 0;
  float loadPower = 0, loadPercent = 0;
  float batterySoc = 0;
  float batteryCurrent = 0;
  float gridPower = 0;
  float pack1Soc = 0, pack2Soc = 0;
  float totalRemainingAh = 0;
  float totalDesignAh = 0;
  float batteryVoltage = 0;
  String deviceMode = "--";
  String faultStatus = "ok";
  String faultText = "";
  String alertLevel = "";
  String alertMessage = "";
  double alertTs = 0;
};
SolarState state;

unsigned long lastPoll = 0;

// ── Alert one-shot tracking ──────────────────────────────────────────────
// Same idea as SolarBridge-LCD: a new alert (ts changed) interrupts the
// scroll rotation once, then normal rotation resumes even if the
// underlying condition (e.g. still on battery) is still active.
double lastAlertTs = 0;
bool alertPending = false;

// ── Config load/save (LittleFS, so credentials survive re-flashing) ─────
void loadConfig() {
  if (!LittleFS.begin()) return;
  if (!LittleFS.exists(CONFIG_PATH)) return;
  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) return;
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, f) == DeserializationError::Ok) {
    strlcpy(serverUrl, doc["server"] | serverUrl, sizeof(serverUrl));
    strlcpy(apiToken, doc["token"] | apiToken, sizeof(apiToken));
    strlcpy(pollSecondsStr, doc["poll"] | pollSecondsStr, sizeof(pollSecondsStr));
  }
  f.close();
}

void saveConfig() {
  StaticJsonDocument<256> doc;
  doc["server"] = serverUrl;
  doc["token"] = apiToken;
  doc["poll"] = pollSecondsStr;
  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) return;
  serializeJson(doc, f);
  f.close();
}

// Matrix font only covers plain ASCII — drop every byte with the high bit
// set so UTF-8 emoji/symbols (⚡, °, ≤, ...) vanish instead of printing as
// garbage glyphs, leaving the plain-ASCII words.
String asciiOnly(const String &in) {
  String out;
  for (size_t i = 0; i < in.length(); i++) {
    uint8_t c = (uint8_t)in[i];
    if (c < 0x80) out += (char)c;
  }
  out.trim();
  return out;
}

void showMessage(const String &msg) {
  Serial.printf("[MATRIX] %s\n", msg.c_str());
  P.displayText(msg.c_str(), PA_LEFT, SCROLL_SPEED, 400, SCROLL_IN, SCROLL_OUT);
}

// ── WiFi: primary + backup network ──────────────────────────────────────
bool connectWiFiBlocking(const char *ssid, const char *pass, unsigned long timeoutMs) {
  WiFi.begin(ssid, pass);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(250);
    Serial.print('.');
  }
  return WiFi.status() == WL_CONNECTED;
}

bool connectWiFi() {
  bool connected = false;
  if (strlen(DEFAULT_WIFI_SSID) > 0) {
    Serial.printf("[WiFi] connecting to primary '%s'...\n", DEFAULT_WIFI_SSID);
    showMessage("WIFI: PRIMARY");
    connected = connectWiFiBlocking(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS, 15000);
  }
  if (!connected && strlen(DEFAULT_WIFI_SSID2) > 0) {
    Serial.printf("\n[WiFi] primary failed, trying backup '%s'...\n", DEFAULT_WIFI_SSID2);
    showMessage("WIFI: BACKUP");
    connected = connectWiFiBlocking(DEFAULT_WIFI_SSID2, DEFAULT_WIFI_PASS2, 15000);
  }
  if (!connected && strlen(DEFAULT_WIFI_SSID) == 0 && strlen(DEFAULT_WIFI_SSID2) == 0) {
    showMessage("CONNECTING WIFI");
    WiFi.begin();  // last WiFi creds saved by the SDK, if any
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
      delay(250);
      Serial.print('.');
    }
    connected = WiFi.status() == WL_CONNECTED;
  }
  return connected;
}

// Non-blocking: called from loop() while disconnected, alternating between
// primary and backup every WIFI_RETRY_MS so a dead primary AP doesn't keep
// the board from ever trying the backup one.
unsigned long lastWifiRetry = 0;
uint8_t wifiRetrySlot = 0;
const unsigned long WIFI_RETRY_MS = 30000;

void retryWiFiIfNeeded() {
  if (WiFi.status() == WL_CONNECTED) return;
  unsigned long now = millis();
  if (now - lastWifiRetry < WIFI_RETRY_MS) return;
  lastWifiRetry = now;

  wifiRetrySlot = 1 - wifiRetrySlot;
  if (wifiRetrySlot == 0 && strlen(DEFAULT_WIFI_SSID) > 0) {
    Serial.printf("[WiFi] reconnecting to primary '%s'...\n", DEFAULT_WIFI_SSID);
    WiFi.begin(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS);
  } else if (strlen(DEFAULT_WIFI_SSID2) > 0) {
    Serial.printf("[WiFi] reconnecting to backup '%s'...\n", DEFAULT_WIFI_SSID2);
    WiFi.begin(DEFAULT_WIFI_SSID2, DEFAULT_WIFI_PASS2);
  } else {
    WiFi.reconnect();
  }
}

// ── WiFiManager: extra fields for server/token/poll interval ────────────
void runWiFiPortal() {
  WiFiManager wm;
  WiFiManagerParameter p_server("server", "Dashboard URL (https://...)", serverUrl, sizeof(serverUrl) - 1);
  WiFiManagerParameter p_token("token", "Viewer API token", apiToken, sizeof(apiToken) - 1);
  WiFiManagerParameter p_poll("poll", "Poll interval (seconds)", pollSecondsStr, sizeof(pollSecondsStr) - 1);
  wm.addParameter(&p_server);
  wm.addParameter(&p_token);
  wm.addParameter(&p_poll);

  showMessage("SETUP: CONNECT TO SolarBridge-Setup");

  wm.setConfigPortalTimeout(180);
  bool ok = wm.autoConnect("SolarBridge-Setup");

  strlcpy(serverUrl, p_server.getValue(), sizeof(serverUrl));
  strlcpy(apiToken, p_token.getValue(), sizeof(apiToken));
  strlcpy(pollSecondsStr, p_poll.getValue(), sizeof(pollSecondsStr));
  saveConfig();

  if (!ok) {
    showMessage("WIFI SETUP TIMED OUT - RETRYING");
    delay(3000);
    ESP.restart();
  }
}

// ── OTA (flash new firmware over WiFi, no USB cable needed) ─────────────
void setupOTA() {
  ArduinoOTA.setHostname("solarbridge-matrix");
  if (strlen(DEFAULT_OTA_PASSWORD) > 0) ArduinoOTA.setPassword(DEFAULT_OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    Serial.println("[OTA] update starting");
    showMessage("OTA UPDATE STARTING");
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    Serial.printf("[OTA] progress %u%%\n", (done * 100) / total);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("[OTA] update done, rebooting");
    showMessage("OTA DONE - REBOOTING");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] error %u\n", error);
    showMessage("OTA ERROR " + String((unsigned)error));
  });

  ArduinoOTA.begin();
  Serial.printf("[OTA] ready, hostname=solarbridge-matrix ip=%s\n", WiFi.localIP().toString().c_str());
}

// ── Fetch + parse /api/state (filtered, so memory use stays flat no ─────
// matter how many BMS cell fields the real payload has) ────────────────
bool fetchState() {
  if (WiFi.status() != WL_CONNECTED) return false;

  String url = String(serverUrl) + "/api/state";
  std::unique_ptr<BearSSL::WiFiClientSecure> https(new BearSSL::WiFiClientSecure);
  WiFiClient httpPlain;
  HTTPClient http;
  bool isHttps = url.startsWith("https://");

  if (isHttps) {
    https->setInsecure();  // see README: pin a fingerprint if you want stricter TLS
    if (!http.begin(*https, url)) return false;
  } else {
    if (!http.begin(httpPlain, url)) return false;
  }
  http.addHeader("Authorization", String("Bearer ") + apiToken);
  http.setTimeout(8000);

  int code = http.GET();
  Serial.printf("[API] GET %s -> %d\n", url.c_str(), code);
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  StaticJsonDocument<512> filter;
  filter["inverter_pv_power"] = true;
  filter["inverter_pv_energy_today"] = true;
  filter["inverter_ac_out_active_power"] = true;
  filter["inverter_load_percent"] = true;
  filter["bank_battery_soc"] = true;
  filter["inverter_battery_current"] = true;
  filter["inverter_grid_power"] = true;
  filter["inverter_device_mode"] = true;
  filter["inverter_fault_status"] = true;
  filter["inverter_fault_text"] = true;
  filter["alert"] = true;
  filter["bms1_battery_soc"] = true;
  filter["bms2_battery_soc"] = true;
  filter["bank_total_remaining_capacity"] = true;
  filter["bank_total_design_capacity"] = true;
  filter["bank_battery_voltage"] = true;
  filter["inverter_battery_voltage"] = true;

  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("[API] JSON parse failed: %s\n", err.c_str());
    return false;
  }

  state.pvPower = doc["inverter_pv_power"] | 0.0f;
  state.pvToday = doc["inverter_pv_energy_today"] | 0.0f;
  state.loadPower = doc["inverter_ac_out_active_power"] | 0.0f;
  state.loadPercent = doc["inverter_load_percent"] | 0.0f;
  state.batterySoc = doc["bank_battery_soc"] | 0.0f;
  state.batteryCurrent = doc["inverter_battery_current"] | 0.0f;
  state.gridPower = doc["inverter_grid_power"] | 0.0f;
  state.pack1Soc = doc["bms1_battery_soc"] | 0.0f;
  state.pack2Soc = doc["bms2_battery_soc"] | 0.0f;
  state.totalRemainingAh = doc["bank_total_remaining_capacity"] | 0.0f;
  state.totalDesignAh = doc["bank_total_design_capacity"] | 0.0f;
  state.batteryVoltage = doc["bank_battery_voltage"] | (doc["inverter_battery_voltage"] | 0.0f);
  state.deviceMode = String((const char *)(doc["inverter_device_mode"] | "--"));
  state.faultStatus = String((const char *)(doc["inverter_fault_status"] | "ok"));
  state.faultText = String((const char *)(doc["inverter_fault_text"] | ""));
  state.valid = true;

  // The backend's alert engine sends its {level, message, ts} as a nested
  // JSON *string* (it's just mirrored from an MQTT payload) — parse it
  // separately from the outer document.
  const char *alertRaw = doc["alert"] | "";
  if (alertRaw[0] != '\0') {
    StaticJsonDocument<256> adoc;
    if (deserializeJson(adoc, alertRaw) == DeserializationError::Ok) {
      state.alertLevel = String((const char *)(adoc["level"] | ""));
      state.alertMessage = String((const char *)(adoc["message"] | ""));
      state.alertTs = adoc["ts"] | 0.0;
    }
  }

  // Flag a new alert (ts changed) — the scroll rotation shows it once, then
  // resumes normal rotation even if the underlying condition is still
  // active. See notifier.py's edge-triggered alert engine.
  if (state.alertTs != lastAlertTs && state.alertTs > 0) {
    lastAlertTs = state.alertTs;
    alertPending = true;
  }

  Serial.printf("[API] solar=%.0fW load=%.0fW soc=%.0f%% grid=%.0fW mode=%s\n",
                 state.pvPower, state.loadPower, state.batterySoc, state.gridPower, state.deviceMode.c_str());
  if (state.alertMessage.length() > 0) {
    Serial.printf("[ALERT] [%s] %s\n", state.alertLevel.c_str(), state.alertMessage.c_str());
  }
  return true;
}

// ── Build the scrolling message set from the latest state ───────────────
// Compact, mixed-case, zero-padded readouts — shorter strings scroll
// across the 32-column matrix faster and read cleaner than all-caps.
String pad3(float v) {
  int n = (int)fabs(v);
  if (n > 999) n = 999;
  char buf[8];
  snprintf(buf, sizeof(buf), "%03d", n);
  return String(buf);
}

const uint8_t PAGE_COUNT = 7;
String pages[PAGE_COUNT];

void buildPages() {
  pages[0] = "Solar - " + pad3(state.pvPower) + "w";
  pages[1] = "Grid - " + pad3(state.gridPower) + "w";
  pages[2] = "Load - " + pad3(state.loadPower) + "w";
  pages[3] = "Load - " + pad3(state.loadPercent) + "%";
  pages[4] = "Battery " + pad3(state.batterySoc) + "%";
  pages[5] = "P1 " + pad3(state.pack1Soc) + "% P2 " + pad3(state.pack2Soc) + "%";
  pages[6] = "Mode: " + asciiOnly(state.deviceMode);
}

// ── Setup / loop ──────────────────────────────────────────────────────────
uint8_t currentPage = 0;

void setup() {
  Serial.begin(115200);

  P.begin();
  P.setIntensity(4);
  P.displayClear();
  showMessage("SOLAR BRIDGE");

  loadConfig();
  pollIntervalMs = (uint32_t)atoi(pollSecondsStr) * 1000UL;
  if (pollIntervalMs < 2000) pollIntervalMs = 5000;

  WiFi.mode(WIFI_STA);
  bool connected = connectWiFi();
  Serial.printf("\n[WiFi] status=%d (3=connected) ip=%s\n", WiFi.status(), WiFi.localIP().toString().c_str());

  if (!connected || strlen(apiToken) == 0) {
    Serial.println("[WiFi] falling back to setup portal");
    runWiFiPortal();
  }

  setupOTA();

  fetchState();
  lastPoll = millis();
  buildPages();
  showMessage(pages[0]);
}

void loop() {
  unsigned long now = millis();

  ArduinoOTA.handle();

  if (now - lastPoll >= pollIntervalMs) {
    lastPoll = now;
    if (fetchState()) buildPages();
  }

  if (P.displayAnimate()) {
    // A real hard inverter fault takes over the scroll continuously.
    if (state.faultStatus == "fault") {
      showMessage("! FAULT: " + asciiOnly(state.faultText));
    } else if (alertPending) {
      alertPending = false;
      String tag = state.alertLevel == "critical" ? "CRITICAL"
                 : state.alertLevel == "warning"  ? "WARNING"
                                                   : "INFO";
      showMessage(tag + ": " + asciiOnly(state.alertMessage));
    } else {
      currentPage = (currentPage + 1) % PAGE_COUNT;
      showMessage(pages[currentPage]);
    }
  }

  retryWiFiIfNeeded();
}
