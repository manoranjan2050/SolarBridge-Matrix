<p align="center">
  <b>SolarBridge-Matrix</b>
</p>

Another tiny satellite display for [Solar Bridge](https://github.com/manoranjan2050/Solar-Bridge-Flin-Fution-JKBMS) —
an ESP8266 + MAX7219 8x32 LED dot-matrix display that polls the same `/api/state` endpoint the
web dashboard, [Android app](https://github.com/manoranjan2050/SolarBridgeApp) and
[SolarBridge-LCD](https://github.com/manoranjan2050/SolarBridge-LCD) use, and shows live solar,
load, battery, grid and clock readings on the matrix in a tiny 3x5 font ported from
[Led_Matrix_Clock](https://github.com/manoranjan2050/Led_Matrix_Clock)'s "Small" clock mode.
Everything — WiFi, the Solar Bridge server/token, poll interval, clock/timezone, and display
brightness/spacing/timing — is configurable from a settings page served by the board itself, no
reflashing needed.

**Status: built, flashed, and confirmed showing live data with the correct 8x32 hardware.**

## What it shows

32 columns only fits about 8 tiny-font characters, so each metric is a label screen followed by
its value screen, each held for 5 seconds:

| Label | Value |
|---|---|
| `SOLAR` | `0551W` |
| `GRID` | `0000W` |
| `LOAD` | `0454W` |
| `LOAD%` | `024%` |
| `BATTERY` | `076%` |
| `PACK1` | `092%` |
| `PACK2` | `088%` |
| `MODE` | `Battery` |
| — | `14:32` (clock, once NTP has synced — see below) |

Preview the layout, font and timing before flashing anything:
[Matrix Sign Preview](https://claude.ai/artifact/UB5k91nw2EBzJxJeHkSEyR).

### Not sure your panel's settings? Run the diagnostic sketch first

`MatrixDiagnostic/` is a small, self-contained sketch (no WiFi/API, just the matrix) that cycles
through a column sweep, brightness fill, and both fonts at different sizes/spacings, so you can
watch the real panel and confirm module count, wiring, and font settings *before* touching the
main firmware. This is how the 8x32 (not 8x96) module count and the tiny font's row bit-order bug
got nailed down for this exact board — flash it, watch, adjust `MatrixDiagnostic.ino`'s constants
to match what you actually see, then carry those settings over to `SolarBridge-Matrix.ino`.

### Alerts

The backend's alert engine (grid lost/restored, battery low, high temperature, overload, battery
full, inverter fault cleared, ...) interrupts the rotation once when a *new* alert fires —
scrolling `WARNING: GRID POWER LOST`, `INFO: GRID POWER RESTORED`, etc, in the normal (larger)
font since these are full sentences — then normal rotation resumes even if the underlying
condition is still active. A real hard inverter fault (`inverter_fault_status == "fault"`) takes
over the scroll continuously until it clears.

### Web settings page

Browse to the board's IP (printed on boot in the serial log, or check your router's DHCP client
list — hostname `solarbridge-matrix`) to change WiFi (primary + backup), the Solar Bridge
server/token/poll interval, and the clock — timezone offset, NTP server, and whether the clock
page shows at all — without reflashing. Saving reboots the board to apply.

### Clock

Syncs time via NTP (`pool.ntp.org` by default) once WiFi connects. Set your timezone as a UTC
offset in minutes from the settings page (India/IST = `330`, UK = `0`, US Eastern = `-300`). The
clock page only appears in the rotation once a sync has actually landed, so nothing shows `00:00`
before the board knows the real time.

## Hardware

| Part | Notes |
|---|---|
| ESP8266 dev board | NodeMCU or Wemos D1 Mini |
| MAX7219 8x96 dot-matrix | 12x cascaded 8x8 FC-16 modules |

### Wiring (hardware SPI)

| Matrix | ESP8266 (NodeMCU / D1 Mini) | GPIO |
|---|---|---|
| CLK | **D5** | GPIO14 (SCK) |
| CS | **D6** | GPIO12 |
| DIN | **D7** | GPIO13 (MOSI) |
| VCC | 5V (VU / VIN) | — |
| GND | GND | — |

CLK/DIN are the ESP8266's fixed hardware-SPI pins; CS can be (and here is) any free GPIO.

If the text comes out mirrored, flipped, or garbled, your modules aren't the assumed `FC16_HW`
type — open `SolarBridge-Matrix.ino` and change `HARDWARE_TYPE` to `MD_MAX72XX::GENERIC_HW`,
`PAROLA_HW` or `ICSTATION_HW` (the four common cascaded-module wiring variants) and reflash.

### ⚠️ Power it separately from USB once wired up

12 cascaded 8x8 modules can pull well over 1.5A at default brightness — much more than a USB port
reliably supplies alongside the ESP8266 itself. If the board stops responding to `esptool`/USB
uploads at all once the matrix is wired and powered, that's very likely why: a brownout during
boot or mid-transfer, not a code or driver problem. Confirmed repeatedly on this exact build —
flashing failed consistently with the matrix's VCC connected (`No serial data received`,
`Timed out waiting for packet header`, even mid-write `Invalid head of packet`) and succeeded
once it was disconnected. For anything beyond a quick USB-powered bench test, run the matrix's
VCC from a separate 5V supply (sharing GND with the ESP8266) instead of off USB, and drop
`P.setIntensity()` in the sketch if you still see resets at full brightness.

If flashing still fails with the matrix unpowered — port shows present in Device Manager but
opening it throws `PermissionError`/`device is not functioning`, or the chip won't sync even
right after a manual FLASH+RESET — a full physical USB replug (unplug the cable, not just retry
the command) has cleared it every time it's come up. That symptom means the USB-serial link
itself needs a fresh enumeration, which only a real replug forces.

## Installation

### 1. Get the code

```bash
git clone https://github.com/manoranjan2050/SolarBridge-Matrix.git
cd SolarBridge-Matrix
```

### 2. Wire the hardware

Connect the matrix to the ESP8266 per the wiring table above. Double-check VCC is going to
**5V**, not 3.3V — 12 cascaded modules draw far more current than a single ESP8266 3.3V regulator
can reliably supply.

### 3. Flash it

```bash
# from the repo root, with the board plugged in over USB
pip install platformio      # if you don't already have it
platformio run -t upload --upload-port COM7   # or /dev/ttyUSB0 on Linux/Mac
```

`platformio.ini` already pins the board (`nodemcuv2`) and pulls in the required libraries
(`ArduinoJson`, `MD_MAX72XX`, `MD_Parola`) automatically on first build.

### 4. First boot — pair it with your Solar Bridge

1. **Get a read-only viewer token** from your Solar Bridge dashboard: **System** page →
   **Demo / Viewer Access** card. Use the viewer token here, not your admin token.
2. On first boot (or whenever it can't reconnect to WiFi), the board opens its own access point
   named **`SolarBridge-Setup`**. Connect to it from a phone — a setup page usually opens
   automatically, or go to `192.168.4.1` manually. Fill in:
   - Your WiFi network + password (**2.4GHz only**)
   - **Dashboard URL** — `https://solar.yourdomain.com` (Cloudflare Tunnel) or
     `http://solar.local:8080` (LAN only)
   - **Viewer API token** — from step 1
   - **Poll interval** — 5 seconds by default
3. Save. The board reboots, connects, and live data starts scrolling within a few seconds.

### Optional: skip the portal for bench testing

Copy `SolarBridge-Matrix/secrets.h.example` to `SolarBridge-Matrix/secrets.h` and fill in real
WiFi/server/token values — the firmware uses those as defaults and skips the captive portal
entirely. `secrets.h` is gitignored; it never leaves your machine.

You can set a **primary and a backup WiFi network** (`DEFAULT_WIFI_SSID`/`DEFAULT_WIFI_SSID2`).
At boot the board tries the primary first, falls back to the backup if that fails, and — while
running — alternates between the two every 30 seconds if it ever loses the connection.

### Flash over WiFi (OTA), no cable needed

Once this firmware is on the board once via USB, later updates can go out over WiFi:

```bash
platformio run -t upload --upload-port 192.168.1.XXX --upload-flags="--auth=your-ota-password"
```

Use the board's IP (printed on boot in the serial log, `[OTA] ready, ... ip=...`, or your
router's DHCP client list — hostname `solarbridge-matrix`). Set `DEFAULT_OTA_PASSWORD` in
`secrets.h`.

## Libraries

- **WiFiManager** by tzapu
- **ArduinoJson** (>= 6.19) by Benoit Blanchon
- **MD_MAX72XX** + **MD_Parola** by majicDesigns
- `LittleFS`, `ESP8266HTTPClient`, `ESP8266WebServer` and `ArduinoOTA` ship with the ESP8266
  Arduino core

## A note on TLS

The sketch uses `setInsecure()` for HTTPS (no certificate pinning) — simplest to set up and fine
for a read-only viewer token. If your dashboard is only reachable on your LAN, use the plain
`http://solar.local:8080` address instead and skip TLS entirely.

## Related

- [Solar-Bridge-Flin-Fution-JKBMS](https://github.com/manoranjan2050/Solar-Bridge-Flin-Fution-JKBMS) —
  the Raspberry Pi bridge + web dashboard this pairs with.
- [SolarBridgeApp](https://github.com/manoranjan2050/SolarBridgeApp) — the Android companion app.
- [SolarBridge-LCD](https://github.com/manoranjan2050/SolarBridge-LCD) — the 16x2 LCD satellite
  display (same backend, different screen).
- Built by [ElectroIoT](https://electroiot.in)
