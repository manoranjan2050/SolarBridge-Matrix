/*
 * MatrixDiagnostic — standalone test sketch for the MAX7219 8x8 x4 (8x32
 * total) panel, no WiFi/API/anything else involved. Purpose: let you watch
 * the real panel and report back exactly what each numbered test looks
 * like, so the main SolarBridge-Matrix firmware gets flashed with settings
 * that are actually confirmed correct instead of guessed.
 *
 * Wiring (same as SolarBridge-Matrix): CLK->D5, DIN->D7, CS->D6.
 * Hardware: 4x cascaded 8x8 modules = 8 rows x 32 columns total.
 *
 * Each test runs for a fixed time and prints "[TEST n] name" to Serial
 * (115200 baud) right when it starts — but you don't need the serial
 * monitor open, just watch the panel and note what test number you were
 * on when you tell me what you saw (count them in order as they change).
 */

#include <SPI.h>
#include <MD_MAX72xx.h>
#include <MD_Parola.h>

// If everything below looks wrong, this is the first thing to try
// changing: MD_MAX72XX::FC16_HW, PAROLA_HW, or ICSTATION_HW.
// GENERIC_HW confirmed correct on this panel (FC16_HW showed mirrored text).
#define HARDWARE_TYPE MD_MAX72XX::GENERIC_HW
#define MAX_DEVICES 4     // confirmed hardware: 4x 8x8 = 8x32 total
#define CS_PIN D6

MD_Parola P = MD_Parola(HARDWARE_TYPE, CS_PIN, MAX_DEVICES);
MD_MAX72XX *mx;

void banner(const char *name) {
  Serial.println();
  Serial.print("[TEST] ");
  Serial.println(name);
}

// ── Test 1: single dot sweeping left to right across all 32 columns. ────
// Watch closely: does ONE dot travel smoothly all the way across all 4
// panels in a single pass (confirms correct cascade wiring + column
// order), or does something odd happen (dot jumps/repeats/only covers
// part of the panel, or all 4 panels show the same moving dot at once)?
void testSweep() {
  banner("1: dot sweep, all 32 columns, left to right");
  for (uint16_t col = 0; col < MAX_DEVICES * 8; col++) {
    mx->clear();
    mx->setPoint(4, col, true);
    mx->update();
    delay(120);
  }
  mx->clear();
  mx->update();
}

// ── Test 2: full-panel fill, so you can confirm all 4 modules actually
// light up (rules out a dead/disconnected module before judging anything
// else). ───────────────────────────────────────────────────────────────
void testFill() {
  banner("2: full fill (all LEDs on), then all off");
  mx->clear();
  for (uint16_t col = 0; col < MAX_DEVICES * 8; col++)
    for (uint8_t row = 0; row < 8; row++)
      mx->setPoint(row, col, true);
  mx->update();
  delay(2000);
  mx->clear();
  mx->update();
  delay(500);
}

// ── Test 3: the NORMAL (big, built-in) Parola font — single digits. ─────
void testBigDigits() {
  banner("3: big font, digits 0-9 one at a time");
  for (char c = '0'; c <= '9'; c++) {
    char buf[2] = {c, 0};
    P.displayClear();
    P.displayText(buf, PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
    while (!P.displayAnimate()) {}
    delay(500);
  }
}

// ── Test 4: big font, a realistic short string, to see how much of it
// actually fits across 32 columns without scrolling. ────────────────────
void testBigString() {
  banner("4: big font, \"099%\" then \"0551W\" static");
  const char *msgs[] = {"099%", "0551W", "12:34"};
  for (auto m : msgs) {
    P.displayClear();
    P.displayText(m, PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
    while (!P.displayAnimate()) {}
    delay(2500);
  }
}

// ── Tiny 3x5 font (bit order fixed: bit4=top row, matches
// Led_Matrix_Clock's puttinychar() exactly) — same table as the main
// firmware, kept in sync manually. ───────────────────────────────────────
const uint8_t TINY_FONT[][3] PROGMEM = {
  {0x00, 0x00, 0x00},
  {0x1F, 0x14, 0x1F}, {0x1F, 0x15, 0x0A}, {0x1F, 0x11, 0x11}, {0x1F, 0x11, 0x0E},
  {0x1F, 0x15, 0x11}, {0x1F, 0x14, 0x10}, {0x1F, 0x11, 0x17}, {0x1F, 0x04, 0x1F},
  {0x11, 0x1F, 0x11}, {0x03, 0x01, 0x1F}, {0x1F, 0x04, 0x1B}, {0x1F, 0x01, 0x01},
  {0x1F, 0x08, 0x1F}, {0x1F, 0x10, 0x0F}, {0x1F, 0x11, 0x1F}, {0x1F, 0x14, 0x1C},
  {0x1C, 0x14, 0x1F}, {0x1F, 0x16, 0x1D}, {0x1D, 0x15, 0x17}, {0x10, 0x1F, 0x10},
  {0x1F, 0x01, 0x1F}, {0x1E, 0x01, 0x1E}, {0x1F, 0x02, 0x1F}, {0x1B, 0x04, 0x1B},
  {0x1C, 0x07, 0x1C}, {0x13, 0x15, 0x19},
  {0x1F, 0x11, 0x1F}, {0x00, 0x00, 0x1F}, {0x17, 0x15, 0x1D}, {0x11, 0x15, 0x1F},
  {0x1C, 0x04, 0x1F}, {0x1D, 0x15, 0x17}, {0x1F, 0x15, 0x17}, {0x10, 0x10, 0x1F},
  {0x1F, 0x15, 0x1F}, {0x1D, 0x15, 0x1F},
  {0x04, 0x04, 0x04}, {0x00, 0x0A, 0x00}, {0x11, 0x04, 0x11},
};
uint8_t tinyGlyphIndex(char c) {
  c = toupper(c);
  if (c == ' ') return 0;
  if (c >= 'A' && c <= 'Z') return 1 + (c - 'A');
  if (c >= '0' && c <= '9') return 27 + (c - '0');
  if (c == '-') return 37;
  if (c == ':') return 38;
  if (c == '%') return 39;
  return 0;
}

void drawTiny(const String &text, uint8_t spacing) {
  mx->clear();
  const uint8_t glyphW = 3, step = glyphW + spacing, rowOffset = 1;
  int totalW = text.length() * step - spacing;
  int startCol = (MAX_DEVICES * 8 - totalW) / 2;
  if (startCol < 0) startCol = 0;
  for (size_t i = 0; i < text.length(); i++) {
    uint8_t idx = tinyGlyphIndex(text[i]);
    for (uint8_t col = 0; col < glyphW; col++) {
      uint8_t bits = pgm_read_byte(&TINY_FONT[idx][col]);
      for (uint8_t row = 0; row < 5; row++) {
        if (bits & (0x10 >> row)) mx->setPoint(row + rowOffset, startCol + i * step + col, true);
      }
    }
  }
  mx->update();
}

// ── Test 5: tiny font, single digits, big and obvious. ───────────────────
void testTinyDigits() {
  banner("5: tiny font, digits 0-9 one at a time");
  for (char c = '0'; c <= '9'; c++) {
    drawTiny(String(c), 1);
    delay(500);
  }
}

// ── Test 6: tiny font, realistic short strings at spacing=1 (the current
// default). ───────────────────────────────────────────────────────────────
void testTinyString() {
  banner("6: tiny font spacing=1, \"099%\" then \"0551W\"");
  drawTiny("099%", 1);
  delay(2500);
  drawTiny("0551W", 1);
  delay(2500);
}

// ── Test 7: tiny font at 3 different spacings, same text each time, so
// you can compare how tight/loose the letters look. ──────────────────────
void testTinySpacingCompare() {
  banner("7a: tiny font spacing=0 (tightest)");
  drawTiny("099%", 0);
  delay(2500);
  banner("7b: tiny font spacing=1");
  drawTiny("099%", 1);
  delay(2500);
  banner("7c: tiny font spacing=2 (loosest)");
  drawTiny("099%", 2);
  delay(2500);
}

// ── Test 8: tiny font, how many digits actually fit across all 32
// columns at once (should be about 8 at spacing=1). ──────────────────────
void testTinyFit() {
  banner("8: tiny font, \"01234567\" — does it all fit?");
  drawTiny("01234567", 1);
  delay(3000);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== MatrixDiagnostic ===");
  Serial.printf("HARDWARE_TYPE=GENERIC_HW  MAX_DEVICES=%d (8x%d total)\n", MAX_DEVICES, MAX_DEVICES * 8);
  Serial.println("Watch the panel. Tests repeat in a loop every ~35s.");

  P.begin();
  P.setIntensity(6);
  mx = P.getGraphicObject();
  mx->clear();
  mx->update();
  delay(500);
}

void loop() {
  testSweep();
  testFill();
  testBigDigits();
  testBigString();
  testTinyDigits();
  testTinyString();
  testTinySpacingCompare();
  testTinyFit();
  banner("--- loop complete, restarting from test 1 ---");
  delay(1000);
}
