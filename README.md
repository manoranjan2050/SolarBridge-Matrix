<p align="center">
  <b>SolarBridge-Matrix</b>
</p>

Another tiny satellite display for [Solar Bridge](https://github.com/manoranjan2050/Solar-Bridge-Flin-Fution-JKBMS) —
an ESP8266 + MAX7219 8x32 LED dot-matrix display that polls the same `/api/state` endpoint the
web dashboard, [Android app](https://github.com/manoranjan2050/SolarBridgeApp) and
[SolarBridge-LCD](https://github.com/manoranjan2050/SolarBridge-LCD) use, and scrolls live solar,
load, battery, grid, backup-time and charge-time readings across the matrix.

**Status: built, flashed, and confirmed showing live data.**

## What it shows

7 messages, scrolling continuously in order:

| Message | Example |
|---|---|
| Solar | `SOLAR 1024W   TODAY 10.4KWH` |
| Load | `LOAD 920W   24%` |
| Battery | `BATTERY 76%   CHG 6.2A` |
| Grid | `GRID 760W   MODE LINE/GRID` |
| Battery packs | `PACK1 92%   PACK2 88%   TOTAL 183.7AH` |
| Backup time | `BACKUP 9.6H  @1011W` (or `BACKUP -- (NO LOAD)`) |
| Charge time | `CHARGE 2.1H  @740W` (or `CHARGE: FULL` / `CHARGE: -- NOT CHARGING`) |

Same live-calculated backup/charge time formulas as SolarBridge-LCD — see that repo's README for
the math.

### Alerts

The backend's alert engine (grid lost/restored, battery low, high temperature, overload, battery
full, inverter fault cleared, ...) interrupts the scroll rotation once when a *new* alert fires —
`WARNING: GRID POWER LOST`, `INFO: GRID POWER RESTORED`, etc — then normal rotation resumes even
if the underlying condition is still active. A real hard inverter fault
(`inverter_fault_status == "fault"`) takes over the scroll continuously until it clears.

## Hardware

| Part | Notes |
|---|---|
| ESP8266 dev board | NodeMCU or Wemos D1 Mini |
| MAX7219 8x32 dot-matrix | 4x cascaded 8x8 FC-16 modules |

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

4 cascaded 8x8 modules can pull well over 500mA at default brightness — more than a USB port
reliably supplies alongside the ESP8266 itself. If the board stops responding to `esptool`/USB
uploads entirely (`Failed to connect... No serial data received`) once the matrix is wired and
powered, that's very likely why: a brownout during boot, not a code or driver problem. Confirmed
on this exact build — flashing failed consistently with the matrix's VCC connected and succeeded
immediately once it was disconnected. For anything beyond a quick USB-powered bench test, run the
matrix's VCC from a separate 5V supply (sharing GND with the ESP8266) instead of off USB, and drop
`P.setIntensity()` in the sketch if you still see resets at full brightness.

## Installation

### 1. Get the code

```bash
git clone https://github.com/manoranjan2050/SolarBridge-Matrix.git
cd SolarBridge-Matrix
```

### 2. Wire the hardware

Connect the matrix to the ESP8266 per the wiring table above. Double-check VCC is going to
**5V**, not 3.3V — 4 cascaded modules draw more current than a single ESP8266 3.3V regulator can
reliably supply.

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
- `LittleFS` and `ESP8266HTTPClient` ship with the ESP8266 Arduino core

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
