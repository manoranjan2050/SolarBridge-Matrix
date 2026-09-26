/*
 * Solar Bridge Matrix Display
 * ────────────────────────────
 * A tiny satellite display for the Solar Bridge inverter/BMS monitoring
 * system (github.com/manoranjan2050/Solar-Bridge-Flin-Fution-JKBMS).
 * Polls the same /api/state endpoint the web dashboard, Android app and
 * SolarBridge-LCD use, and shows solar / grid / load / battery / pack /
 * mode readings across a MAX7219 8x32 LED dot-matrix display, each field
 * on one screen ("S-0551W", "P1-092%", ...) held for 5 seconds. Mode is
 * the exception — its value is too unpredictable to abbreviate, so it
 * gets a label screen + value screen.
 *
 * Hardware: ESP8266 (NodeMCU / Wemos D1 Mini) + MAX7219 8x32 dot matrix
 * (4x cascaded 8x8 FC-16 modules), hardware SPI:
 *   CLK -> D5 (SCK)   DIN -> D7 (MOSI)   CS -> D6
 * Onboard LED (D4) lights up as soon as the board has power, as a simple
 * "it's alive" indicator independent of WiFi/matrix state.
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
#include <ESP8266WebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <ArduinoOTA.h>
#include <SPI.h>
#include <MD_MAX72xx.h>
#include <MD_Parola.h>
#include <time.h>

// Optional, gitignored — lets you hardcode WiFi/server/token for a fast
// local flash without going through the captive portal each time. See
// secrets.h.example. Never commit a real secrets.h.
#if __has_include("secrets.h")
#include "secrets.h"
#endif

// ── MAX7219 matrix wiring — hardware SPI (CLK=D5/SCK, DIN=D7/MOSI are
// fixed by the ESP8266's SPI peripheral), CS is the only pin we choose. ──
// FC16_HW confirmed correct row (up/down) orientation on this panel, but
// mirrors columns left-right — GENERIC_HW was the opposite (rows flipped,
// columns correct), neither alone is right for this board. Rather than a
// 5th guess at a named type, we keep FC16_HW and cancel the mirror
// ourselves: flipTinyCol() below, plus mx->transform(..., TFLR) after
// every Parola-drawn frame (see flipParolaFrame()).
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES 4    // 8x32 = 4 cascaded 8x8 modules (confirmed via MatrixDiagnostic)
#define CS_PIN D6

MD_Parola P = MD_Parola(HARDWARE_TYPE, CS_PIN, MAX_DEVICES);

// Onboard LED — lights up as soon as the board has power (active LOW).
#define POWER_LED_PIN LED_BUILTIN

// Display settings — all runtime-configurable from the web settings page
// (see setupWebServer()), persisted to LittleFS, with these as defaults.
uint8_t displayIntensity = 4;      // brightness, 0-15
uint16_t scrollSpeedMs = 40;       // ms per column step for scrolling text — lower = faster
uint16_t holdMs = 5000;            // each field stays on screen this long
uint8_t tinySpacing = 1;           // blank columns between tiny-font characters

// ── Tiny 3x5 font — ported from manoranjan2050/Led_Matrix_Clock's "Small"
// mode (Library/FontLEDClock/FontLEDClock.h, mytinyfont). Half the height
// of MD_Parola's built-in font, so short fields read clean without
// crowding the panel. Drawn directly via MD_MAX72XX's low-level pixel API
// (same technique as the library's own "Parola_Mixed" example) so it can
// sit alongside Parola's normal scrolling text for longer alert messages.
// 3 columns per glyph, bit0 = top row .. bit4 = row 5, in the same logical
// row/col space P.displayText() already draws in correctly on this panel.
const uint8_t TINY_FONT[][3] PROGMEM = {
  {0x00, 0x00, 0x00},  // space (index 0)
  {0x1F, 0x14, 0x1F}, {0x1F, 0x15, 0x0A}, {0x1F, 0x11, 0x11}, {0x1F, 0x11, 0x0E},
  {0x1F, 0x15, 0x11}, {0x1F, 0x14, 0x10}, {0x1F, 0x11, 0x17}, {0x1F, 0x04, 0x1F},
  {0x11, 0x1F, 0x11}, {0x03, 0x01, 0x1F}, {0x1F, 0x04, 0x1B}, {0x1F, 0x01, 0x01},
  {0x1F, 0x08, 0x1F}, {0x1F, 0x10, 0x0F}, {0x1F, 0x11, 0x1F}, {0x1F, 0x14, 0x1C},
  {0x1C, 0x14, 0x1F}, {0x1F, 0x16, 0x1D}, {0x1D, 0x15, 0x17}, {0x10, 0x1F, 0x10},
  {0x1F, 0x01, 0x1F}, {0x1E, 0x01, 0x1E}, {0x1F, 0x02, 0x1F}, {0x1B, 0x04, 0x1B},
  {0x1C, 0x07, 0x1C}, {0x13, 0x15, 0x19},                                          // A-Z (1-26)
  {0x1F, 0x11, 0x1F}, {0x00, 0x00, 0x1F}, {0x17, 0x15, 0x1D}, {0x11, 0x15, 0x1F},
  {0x1C, 0x04, 0x1F}, {0x1D, 0x15, 0x17}, {0x1F, 0x15, 0x17}, {0x10, 0x10, 0x1F},
  {0x1F, 0x15, 0x1F}, {0x1D, 0x15, 0x1F},                                          // 0-9 (27-36)
  {0x04, 0x04, 0x04},  // '-' (37)
  {0x00, 0x0A, 0x00},  // ':' (38)
  {0x11, 0x04, 0x11},  // '%' (39)
};
uint8_t tinyGlyphIndex(char c) {
  c = toupper(c);
  if (c == ' ') return 0;
  if (c >= 'A' && c <= 'Z') return 1 + (c - 'A');
  if (c >= '0' && c <= '9') return 27 + (c - '0');
  if (c == '-') return 37;
  if (c == ':') return 38;
  if (c == '%') return 39;
  return 0;  // unknown -> blank
}

// Draws text.c_str() in the tiny font, centered, replacing whatever P
// (Parola) last drew — call mx->update() so it actually reaches the panel.
//
// Bit order matches Led_Matrix_Clock's own puttinychar() exactly: row 0
// (top) is bit 4 (the *high* bit, tested as `dots & (16 >> row)`), not
// bit 0 — the reverse of what a first read of the byte suggests. Getting
// this backwards (bit0=top) is what produced garbage/sparse output on the
// panel: most glyphs aren't vertically symmetric, so a bit-reversed
// pattern looks like near-random dots, not just a mirrored letter.
//
// Columns are flipped (MAX_DEVICES*8-1-col) to cancel FC16_HW's left-right
// mirroring on this panel — confirmed via MatrixDiagnostic's hardware-type
// sweep: FC16_HW has the correct row/vertical orientation, just mirrored
// columns, and no single named hardware type on this board gets both
// right. Flipping the *final* absolute column (not the glyph-internal
// layout) mirrors the whole rendered string in one step — both the
// glyph shapes and their left-right order — which is exactly what
// cancels a hardware mirror.
void drawTiny(const String &text) {
  MD_MAX72XX *mx = P.getGraphicObject();
  mx->clear();

  const uint8_t glyphW = 3, step = glyphW + tinySpacing;
  const uint8_t rowOffset = 1;  // centers the 5-row glyph in the 8-row panel
  const uint16_t lastCol = MAX_DEVICES * 8 - 1;
  int totalW = text.length() * step - tinySpacing;
  int startCol = (MAX_DEVICES * 8 - totalW) / 2;
  if (startCol < 0) startCol = 0;

  for (size_t i = 0; i < text.length(); i++) {
    uint8_t idx = tinyGlyphIndex(text[i]);
    for (uint8_t col = 0; col < glyphW; col++) {
      uint8_t bits = pgm_read_byte(&TINY_FONT[idx][col]);
      for (uint8_t row = 0; row < 5; row++) {
        if (bits & (0x10 >> row)) {
          mx->setPoint(row + rowOffset, lastCol - (startCol + i * step + col), true);
        }
      }
    }
  }
  mx->update();
  Serial.printf("[MATRIX] %s\n", text.c_str());
}


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

// Runtime-configurable WiFi (via the web settings page) — starts from the
// secrets.h/compiled defaults, but the settings page overrides and
// persists these to LittleFS, so WiFi can be changed without reflashing.
char wifiSsid[32]  = DEFAULT_WIFI_SSID;
char wifiPass[64]  = DEFAULT_WIFI_PASS;
char wifiSsid2[32] = DEFAULT_WIFI_SSID2;
char wifiPass2[64] = DEFAULT_WIFI_PASS2;

// Clock: NTP + timezone, settable from the web page. utcOffsetMinutes can
// be negative; e.g. India (IST) is +330.
int utcOffsetMinutes = 330;
char ntpServer[48] = "pool.ntp.org";
bool showClockPage = true;
bool timeSynced = false;

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
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, f) == DeserializationError::Ok) {
    strlcpy(serverUrl, doc["server"] | serverUrl, sizeof(serverUrl));
    strlcpy(apiToken, doc["token"] | apiToken, sizeof(apiToken));
    strlcpy(pollSecondsStr, doc["poll"] | pollSecondsStr, sizeof(pollSecondsStr));
    strlcpy(wifiSsid, doc["ssid1"] | wifiSsid, sizeof(wifiSsid));
    strlcpy(wifiPass, doc["pass1"] | wifiPass, sizeof(wifiPass));
    strlcpy(wifiSsid2, doc["ssid2"] | wifiSsid2, sizeof(wifiSsid2));
    strlcpy(wifiPass2, doc["pass2"] | wifiPass2, sizeof(wifiPass2));
    strlcpy(ntpServer, doc["ntp"] | ntpServer, sizeof(ntpServer));
    utcOffsetMinutes = doc["tz"] | utcOffsetMinutes;
    showClockPage = doc["clock"] | showClockPage;
    displayIntensity = doc["bright"] | displayIntensity;
    scrollSpeedMs = doc["scrollms"] | scrollSpeedMs;
    holdMs = doc["holdms"] | holdMs;
    tinySpacing = doc["spacing"] | tinySpacing;
  }
  f.close();
}

void saveConfig() {
  StaticJsonDocument<512> doc;
  doc["server"] = serverUrl;
  doc["token"] = apiToken;
  doc["poll"] = pollSecondsStr;
  doc["ssid1"] = wifiSsid;
  doc["pass1"] = wifiPass;
  doc["ssid2"] = wifiSsid2;
  doc["pass2"] = wifiPass2;
  doc["ntp"] = ntpServer;
  doc["tz"] = utcOffsetMinutes;
  doc["clock"] = showClockPage;
  doc["bright"] = displayIntensity;
  doc["scrollms"] = scrollSpeedMs;
  doc["holdms"] = holdMs;
  doc["spacing"] = tinySpacing;
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

// Short, fixed-width fields (Solar/Grid/Load/Battery/etc) — these always
// fit within the 96-column display, so just print them and hold, no
// scrolling. This is also what fixes the scroll "lag": there's no ongoing
// animation for a blocking WiFi/HTTP call to stutter mid-motion.
void showStatic(const String &msg) {
  Serial.printf("[MATRIX] %s\n", msg.c_str());
  P.displayText(msg.c_str(), PA_CENTER, scrollSpeedMs, holdMs, PA_PRINT, PA_NO_EFFECT);
}

// Longer, unpredictable-length text (alerts, fault messages) — scroll
// since it may not fit in 96 columns.
void showScrolling(const String &msg) {
  Serial.printf("[MATRIX] %s\n", msg.c_str());
  P.displayText(msg.c_str(), PA_LEFT, scrollSpeedMs, 400, PA_SCROLL_LEFT, PA_SCROLL_LEFT);
}

// Parola's own font rendering goes through the same FC16_HW column
// mapping our tiny font does, so it mirrors too — cancel it the same way,
// by flipping the whole panel buffer immediately after each frame Parola
// draws. Call once per P.displayAnimate() call, every time, not just when
// it returns true (every call draws a frame during a scroll).
void flipParolaFrame() {
  MD_MAX72XX *mx = P.getGraphicObject();
  mx->transform(0, MAX_DEVICES - 1, MD_MAX72XX::TFLR);
  mx->update();
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
  if (strlen(wifiSsid) > 0) {
    Serial.printf("[WiFi] connecting to primary '%s'...\n", wifiSsid);
    showStatic("WIFI: PRIMARY");
    connected = connectWiFiBlocking(wifiSsid, wifiPass, 15000);
  }
  if (!connected && strlen(wifiSsid2) > 0) {
    Serial.printf("\n[WiFi] primary failed, trying backup '%s'...\n", wifiSsid2);
    showStatic("WIFI: BACKUP");
    connected = connectWiFiBlocking(wifiSsid2, wifiPass2, 15000);
  }
  if (!connected && strlen(wifiSsid) == 0 && strlen(wifiSsid2) == 0) {
    showStatic("CONNECTING WIFI");
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
  if (wifiRetrySlot == 0 && strlen(wifiSsid) > 0) {
    Serial.printf("[WiFi] reconnecting to primary '%s'...\n", wifiSsid);
    WiFi.begin(wifiSsid, wifiPass);
  } else if (strlen(wifiSsid2) > 0) {
    Serial.printf("[WiFi] reconnecting to backup '%s'...\n", wifiSsid2);
    WiFi.begin(wifiSsid2, wifiPass2);
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

  showStatic("SETUP: CONNECT TO SolarBridge-Setup");

  wm.setConfigPortalTimeout(180);
  bool ok = wm.autoConnect("SolarBridge-Setup");

  strlcpy(serverUrl, p_server.getValue(), sizeof(serverUrl));
  strlcpy(apiToken, p_token.getValue(), sizeof(apiToken));
  strlcpy(pollSecondsStr, p_poll.getValue(), sizeof(pollSecondsStr));
  saveConfig();

  if (!ok) {
    showStatic("WIFI SETUP TIMED OUT - RETRYING");
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
    showStatic("OTA UPDATE STARTING");
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    Serial.printf("[OTA] progress %u%%\n", (done * 100) / total);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("[OTA] update done, rebooting");
    showStatic("OTA DONE - REBOOTING");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] error %u\n", error);
    showStatic("OTA ERROR " + String((unsigned)error));
  });

  ArduinoOTA.begin();
  Serial.printf("[OTA] ready, hostname=solarbridge-matrix ip=%s\n", WiFi.localIP().toString().c_str());
}

// ── NTP clock ─────────────────────────────────────────────────────────────
void syncTime() {
  configTime(utcOffsetMinutes * 60, 0, ntpServer);
  Serial.printf("[Clock] syncing via %s, UTC offset %d min\n", ntpServer, utcOffsetMinutes);
}

bool checkTimeSynced() {
  if (timeSynced) return true;
  if (time(nullptr) > 1700000000) {  // sane epoch => NTP has landed
    timeSynced = true;
    Serial.println("[Clock] time synced");
  }
  return timeSynced;
}

String getClockText() {
  time_t now = time(nullptr);
  struct tm *t = localtime(&now);
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", t->tm_hour, t->tm_min);
  return String(buf);
}

// ── Web settings page — browse to the board's IP to change WiFi, the ────
// Solar Bridge server/token, poll interval and clock/timezone without
// reflashing. Saving always reboots so every change re-applies cleanly.
ESP8266WebServer webServer(80);

String settingsPageHtml() {
  String h;
  h.reserve(4096);
  h += F("<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>SolarBridge-Matrix Setup</title><style>"
         "body{font-family:system-ui,sans-serif;background:#0c0b09;color:#e9e2d6;max-width:480px;"
         "margin:0 auto;padding:20px 16px 60px}"
         "h1{font-size:19px;margin:0 0 4px}p.sub{color:#a89c88;font-size:13px;margin:0 0 22px}"
         "fieldset{border:1px solid #332c1f;border-radius:10px;margin-bottom:16px;padding:14px 16px}"
         "legend{color:#ffb020;font-size:12px;letter-spacing:.06em;text-transform:uppercase;padding:0 6px}"
         "label{display:block;font-size:12.5px;color:#a89c88;margin:10px 0 4px}"
         "input,select{width:100%;box-sizing:border-box;background:#17140f;border:1px solid #332c1f;"
         "color:#e9e2d6;border-radius:7px;padding:9px 10px;font-size:14px}"
         "small{color:#7d715f;font-size:11.5px}"
         ".row{display:flex;gap:10px}.row>div{flex:1}"
         "button{width:100%;margin-top:18px;background:#ffb020;color:#0c0b09;border:none;"
         "border-radius:8px;padding:13px;font-size:15px;font-weight:600}"
         "</style></head><body><h1>SolarBridge-Matrix</h1>"
         "<p class='sub'>Live settings — saving reboots the board to apply.</p>"
         "<form method='POST' action='/save'>");

  h += F("<fieldset><legend>WiFi</legend>"
         "<label>Primary SSID</label><input name='ssid1' value='");
  h += wifiSsid;
  h += F("'><label>Primary password</label><input name='pass1' type='password' value='");
  h += wifiPass;
  h += F("'><label>Backup SSID (optional)</label><input name='ssid2' value='");
  h += wifiSsid2;
  h += F("'><label>Backup password</label><input name='pass2' type='password' value='");
  h += wifiPass2;
  h += F("'></fieldset>");

  h += F("<fieldset><legend>Solar Bridge API</legend>"
         "<label>Dashboard URL</label><input name='server' value='");
  h += serverUrl;
  h += F("'><label>Viewer API token</label><input name='token' value='");
  h += apiToken;
  h += F("'><label>Poll interval (seconds)</label><input name='poll' type='number' min='2' value='");
  h += pollSecondsStr;
  h += F("'></fieldset>");

  h += F("<fieldset><legend>Clock</legend>"
         "<label><input type='checkbox' name='clock' value='1' style='width:auto' ");
  h += (showClockPage ? F("checked") : F(""));
  h += F("> Show a clock page in the rotation</label>"
         "<label>Timezone offset from UTC (minutes)</label>"
         "<input name='tz' type='number' value='");
  h += String(utcOffsetMinutes);
  h += F("'><small>India (IST) = 330 &middot; UK = 0 &middot; US Eastern = -300</small>"
         "<label>NTP server</label><input name='ntp' value='");
  h += ntpServer;
  h += F("'></fieldset>");

  h += F("<fieldset><legend>Display</legend>"
         "<label>Brightness (0-15)</label>"
         "<input name='bright' type='number' min='0' max='15' value='");
  h += String(displayIntensity);
  h += F("'><div class='row'><div><label>Field hold time (seconds)</label>"
         "<input name='holds' type='number' min='1' step='0.5' value='");
  h += String(holdMs / 1000.0, 1);
  h += F("'></div><div><label>Alert scroll speed (ms/step)</label>"
         "<input name='scroll' type='number' min='10' max='200' value='");
  h += String(scrollSpeedMs);
  h += F("'></div></div>"
         "<small>Lower scroll speed = faster. Applies to the longer scrolling "
         "alert/fault messages, not the static fields.</small>"
         "<label>Tiny-font character spacing (columns)</label>"
         "<input name='spacing' type='number' min='0' max='4' value='");
  h += String(tinySpacing);
  h += F("'><small>Blank columns between characters in the field rotation — 0 is "
         "tightest/smallest-looking, higher spreads letters out more.</small>"
         "</fieldset>"
         "<button type='submit'>Save &amp; reboot</button></form></body></html>");
  return h;
}

void handleRoot() {
  webServer.send(200, "text/html", settingsPageHtml());
}

void handleSave() {
  if (webServer.hasArg("ssid1")) strlcpy(wifiSsid, webServer.arg("ssid1").c_str(), sizeof(wifiSsid));
  if (webServer.hasArg("pass1")) strlcpy(wifiPass, webServer.arg("pass1").c_str(), sizeof(wifiPass));
  if (webServer.hasArg("ssid2")) strlcpy(wifiSsid2, webServer.arg("ssid2").c_str(), sizeof(wifiSsid2));
  if (webServer.hasArg("pass2")) strlcpy(wifiPass2, webServer.arg("pass2").c_str(), sizeof(wifiPass2));
  if (webServer.hasArg("server")) strlcpy(serverUrl, webServer.arg("server").c_str(), sizeof(serverUrl));
  if (webServer.hasArg("token")) strlcpy(apiToken, webServer.arg("token").c_str(), sizeof(apiToken));
  if (webServer.hasArg("poll")) strlcpy(pollSecondsStr, webServer.arg("poll").c_str(), sizeof(pollSecondsStr));
  if (webServer.hasArg("ntp")) strlcpy(ntpServer, webServer.arg("ntp").c_str(), sizeof(ntpServer));
  if (webServer.hasArg("tz")) utcOffsetMinutes = webServer.arg("tz").toInt();
  showClockPage = webServer.hasArg("clock");
  if (webServer.hasArg("bright")) displayIntensity = constrain(webServer.arg("bright").toInt(), 0, 15);
  if (webServer.hasArg("holds")) holdMs = (uint16_t)(webServer.arg("holds").toFloat() * 1000);
  if (webServer.hasArg("scroll")) scrollSpeedMs = constrain(webServer.arg("scroll").toInt(), 10, 200);
  if (webServer.hasArg("spacing")) tinySpacing = constrain(webServer.arg("spacing").toInt(), 0, 4);

  saveConfig();
  webServer.send(200, "text/html",
    "<body style='font-family:sans-serif;background:#0c0b09;color:#e9e2d6;padding:40px;text-align:center'>"
    "<h2>Saved</h2><p>Rebooting to apply&hellip;</p></body>");
  delay(400);
  ESP.restart();
}

void setupWebServer() {
  webServer.on("/", HTTP_GET, handleRoot);
  webServer.on("/save", HTTP_POST, handleSave);
  webServer.begin();
  Serial.printf("[Web] settings page ready at http://%s/\n", WiFi.localIP().toString().c_str());
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

// ── Build the per-field readouts from the latest state ───────────────────
// Zero-padded, no spaces around the dash — e.g. "Grid-0000W" — each one
// held on screen for HOLD_MS before the next.
String pad3(float v) {
  int n = (int)fabs(v);
  if (n > 999) n = 999;
  char buf[8];
  snprintf(buf, sizeof(buf), "%03d", n);
  return String(buf);
}

String pad4(float v) {
  int n = (int)fabs(v);
  if (n > 9999) n = 9999;
  char buf[8];
  snprintf(buf, sizeof(buf), "%04d", n);
  return String(buf);
}

// 32 columns fits ~8 tiny-font characters at the default spacing — not
// enough for a full word label plus a 4-digit value ("Solar-0551W" is 11
// chars), but a short mnemonic tag fits everything on one screen:
// "SO-0551W" and "BAT-076%" are both exactly 8. Mode is the one
// exception — its value ("Battery", "Line/Grid", "Power saving", ...) is
// too unpredictable to abbreviate meaningfully, so it keeps a label
// screen + value screen.
const uint8_t PAGE_COUNT = 9;
String pages[PAGE_COUNT];
uint8_t totalPages() { return (showClockPage && timeSynced) ? PAGE_COUNT + 1 : PAGE_COUNT; }

// Truncates to fit an 8-char tiny-font screen (mode names like "Line/Grid"
// or "Power saving" would otherwise run off the 32-column panel).
String fit8(const String &s) {
  String out = asciiOnly(s);
  if (out.length() > 8) out = out.substring(0, 8);
  return out;
}

void buildPages() {
  pages[0] = "SO-" + pad4(state.pvPower) + "W";
  pages[1] = "GR-" + pad4(state.gridPower) + "W";
  pages[2] = "LD-" + pad4(state.loadPower) + "W";
  pages[3] = "LD-" + pad3(state.loadPercent) + "%";
  pages[4] = "BAT-" + pad3(state.batterySoc) + "%";
  pages[5] = "P1-" + pad3(state.pack1Soc) + "%";
  pages[6] = "P2-" + pad3(state.pack2Soc) + "%";
  pages[7] = "MODE";
  pages[8] = fit8(state.deviceMode);
}

// ── Setup / loop ──────────────────────────────────────────────────────────
// Two display modes: ROTATION draws each field statically in the tiny font
// on its own HOLD_MS timer; SCROLLING hands the panel to Parola's normal
// font + scroll animation for a longer alert/fault message, then hands
// control back to ROTATION once the scroll finishes.
enum DisplayMode { MODE_ROTATION, MODE_SCROLLING };
DisplayMode displayMode = MODE_ROTATION;
uint8_t currentPage = 0;
unsigned long holdUntil = 0;

void setup() {
  Serial.begin(115200);

  // Onboard LED lights up as soon as the board has power — active LOW,
  // so LOW = on. Independent of WiFi/matrix state, just a power indicator.
  pinMode(POWER_LED_PIN, OUTPUT);
  digitalWrite(POWER_LED_PIN, LOW);

  loadConfig();

  P.begin();
  P.setIntensity(displayIntensity);
  P.setCharSpacing(1);   // spacing for the normal font (boot/WiFi/OTA messages, alerts)
  P.displayClear();
  showStatic("SOLAR BRIDGE");

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
  setupWebServer();
  syncTime();

  fetchState();
  lastPoll = millis();
  buildPages();
  drawTiny(pages[0]);
  holdUntil = millis() + holdMs;
}

// currentPage < PAGE_COUNT is a data field; == PAGE_COUNT is the clock.
void drawCurrentPage() {
  if (currentPage < PAGE_COUNT) drawTiny(pages[currentPage]);
  else drawTiny(getClockText());
}

unsigned long lastTimeCheck = 0;

void loop() {
  unsigned long now = millis();

  ArduinoOTA.handle();
  webServer.handleClient();

  if (now - lastPoll >= pollIntervalMs) {
    lastPoll = now;
    if (fetchState()) buildPages();
  }

  if (!timeSynced && now - lastTimeCheck >= 3000) {
    lastTimeCheck = now;
    checkTimeSynced();
  }

  if (displayMode == MODE_ROTATION) {
    if (now >= holdUntil) {
      // A real hard inverter fault takes over the scroll continuously.
      if (state.faultStatus == "fault") {
        displayMode = MODE_SCROLLING;
        showScrolling("! FAULT: " + asciiOnly(state.faultText));
      } else if (alertPending) {
        alertPending = false;
        String tag = state.alertLevel == "critical" ? "CRITICAL"
                   : state.alertLevel == "warning"  ? "WARNING"
                                                     : "INFO";
        displayMode = MODE_SCROLLING;
        showScrolling(tag + ": " + asciiOnly(state.alertMessage));
      } else {
        currentPage = (currentPage + 1) % totalPages();
        drawCurrentPage();
        holdUntil = now + holdMs;
      }
    }
  } else {  // MODE_SCROLLING
    bool scrollDone = P.displayAnimate();
    flipParolaFrame();
    if (scrollDone) {
      displayMode = MODE_ROTATION;
      drawCurrentPage();
      holdUntil = now + holdMs;
    }
  }

  retryWiFiIfNeeded();
}
