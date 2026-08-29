# Device 5 (PIXEL build) — WiFi RX failure: findings & work ahead

**Status:** Root cause found and proven. LEDs currently disabled by a diagnostic flag
so the unit stays online. Remaining task: make the WS2812 LEDs work **without**
re-breaking WiFi reception.

---

## TL;DR

The WS2812/NeoPixel **RMT LED subsystem** on device 5's ESP32 causes **~63% inbound
WiFi packet loss**. That single fault produced every symptom (sluggish/unresponsive,
MQTT lag, dashboard "behind" then dropping, failed DNS, hard-to-associate WiFi).

Proven by a controlled ping race on the same LAN, same moment:

| Build | device 5 (.148) RX loss | device 1 (.134) control |
|---|---|---|
| Normal (NeoPixel/RMT active) | **63%** (22/60) | 0% |
| NeoPixel/RMT **disabled** | **0%** (60/60) | 0% |

With the LED driver disabled the unit joins WiFi on its own (no AP portal),
MQTT connects and **holds**, `loopMax` stays 0–9 ms, DNS resolves, and the
Node-RED dashboard tracks it. Re-enabling the current LED driver brings the
packet loss back.

---

## What was ruled OUT (don't re-chase these)

All of these were investigated and disproven with evidence:

- **The network / LAN** — device 1 on the *same* network at the *same* time had **0%**
  loss. Network is fine.
- **The DNS server (Pi-hole on HomePi 192.168.68.5)** — healthy; resolved the broker
  10/10 instantly; **receives and answers** device 5's queries (474/24h, 0 refused).
  The replies just don't reach device 5.
- **Pi-hole rate-limiting** — limit is 1000/60s/client; device 5's worst-ever minute
  was 216, whole day 474. Zero rate-limit events logged. Disproven.
- **IP conflict on .148** — router DHCP + ARP + Pi-hole all show a single Espressif MAC
  `20:E7:C8:BB:00:1C` (hostname "Ed-Pixel-8" is device 5's own stealth name). No conflict.
- **WiFi modem-sleep** — added `WiFi.setSleep(false)`; did **not** fix the loss.
- **Weak signal / RF distance** — fails at **RSSI −52** (excellent). Pinning a closer
  mesh node didn't help.
- **Power / LED current draw** — pulled the 12-px strip's 5 V (leaving 3 px); still failed.
- **Firmware loop freeze** — real, but a *separate* bug (see "Changes already made");
  fixed independently and not the cause of the packet loss.

## What was CONFIRMED

Device 5 can **transmit** (its DNS queries reach Pi-hole; gratuitous ARP populates the
LAN) but **loses ~63% of inbound unicast** (DNS replies, ICMP echo, TLS data). Disabling
the NeoPixel/RMT subsystem removes the loss entirely. Therefore the fault is in the LED
driver path, which is the only thing different in this build vs. the working PWM units 1–4.

**Extended-run confirmation (RMT disabled):** ~8 minutes of continuous serial showed
`mqtt=1` held the entire time, `loopMax` mostly 3–16 ms, RSSI −52…−66, and **no**
`hostByName DNS Failed` / `MQTT connection failed`. The fix holds over time, not just in a
60 s snapshot.

**One residual minor blip (not the main fault, likely pre-existing on all devices):** an
occasional TLS read error on the MQTT socket —
`ssl_client.cpp:37 _handle_error(): [data_to_read():361]: (-76) UNKNOWN ERROR CODE (004C)`
— fires roughly every 1–3 min and causes a brief ~3.6–5.5 s `loopMax` spike, but MQTT
**stays connected** through it. Low priority; worth a look only after the LED fix (could be
the broker/TLS session, or a blocking read on the hiccup worth making non-blocking).

---

## Hardware context (device 5)

- ESP32 DevKit (`env:esp32doit-devkit-v1`), platform **pinned `espressif32@6.9.0`
  (ESP32 Arduino 2.0.17) — do NOT upgrade** (breaks LED timing on the other projects too).
- WS2812B, **two** data lines through an **SN74AHCT125N** level shifter:
  - `PIXEL_PIN_BALL = 16` → ping-pong ball, 3 px
  - `PIXEL_PIN_TUBE = 17` → tube, 12 px
  - `PIXEL_BRIGHTNESS = 191` (75%)
- LED driver: **Adafruit NeoPixel** (uses the ESP32 **RMT** peripheral for `show()`).
- Factory MAC `20:E7:C8:BB:00:1C`, DHCP address `.148` (dynamic lease, not reserved).

---

## Changes already made to `pixel/src/main.cpp` (this session)

Keep these — they're correct and independent of the LED fix:

1. **`lightShow()` batching.** `lightWrite()` now only stages the blended color + sets a
   dirty flag; `lightShow()` transmits both strips **once per loop** (was: every
   `lightWrite`, i.e. up to 10 `show()` per visual update). Blocking boot/WiFi blink
   sequences call `lightShow()` explicitly.
2. **Non-blocking `syncTime()`.** Was retrying NTP for ~88 s **while blocking the loop**,
   and being called every loop until time synced → 90–140 s freezes (`loopMax=139856ms`
   observed). Now: kick off background SNTP once, then poll `getLocalTime(…, 500)` at most
   every 30 s. MQTT uses `setInsecure()` so it never needs the clock. This killed the
   catastrophic freezes (loopMax now single-digit ms).
3. **Timeouts capped:** `http.setTimeout(20000 → 5000)`; `mqttSecure`/`httpSecure`
   `.setHandshakeTimeout(6)` — a lossy link can't hang a connect for many seconds.
4. **`WiFi.setSleep(false)`** after `WiFi.mode(WIFI_STA)` — didn't fix the RX loss but is
   good practice; keep unless you have a reason not to.
5. **Diagnostics (temporary):** boot prints `Reset reason:`; a `[DIAG] up=.. heap=.. rssi=..
   wifi=.. mqtt=.. dns=.. loopMax=..` line every 5 s. Handy for the LED-fix testing; remove
   before final.
6. **Backed OUT:** an earlier DNS-override attempt used `WiFi.config()` which pins a static
   IP — it broke reconnects. Fully removed. Do **not** reintroduce static IP; the unit is
   pure DHCP.

### The diagnostic flag you must resolve

```c
#define PIXEL_RXTEST_DISABLE 1   // in the DEVICE_ID==5 config block
```
When `1`, `lightSetup()` and `lightShow()` skip **all** NeoPixel/RMT init and use (LEDs stay
dark) — this is the state that gives 0% packet loss. **A real build needs this back to `0`,
but that reintroduces the WiFi break until the LED driver is fixed.** That is the task.

---

## Work ahead (the task)

**Goal:** LEDs fully functional AND device 5 packet loss ~0% / MQTT stable (match device 1).

### Step 1 — Pinpoint the mechanism (software vs electrical vs pins)

Run the same ping race (`ping -n 60 192.168.68.148` vs `.134` control) under each:

- **Re-enable the driver, strips physically UNPLUGGED** from 16/17.
  - Loss returns → it's the **RMT software/peripheral** → fix in code (Step 2a).
  - Loss stays 0%, returns only when strips are re-plugged → it's **electrical/EMI** from
    the strips / level-shifter / long data lines near the PCB antenna (Step 2b).
- Also suspect **GPIO16/17 specifically.** On some ESP32 modules those pins are tied to
  PSRAM; if this module is a WROVER variant, driving 16/17 could be the whole problem.
  Worth trying **different data pins** (e.g. 4/13/25/26 — pick free, non-strapping GPIOs).

### Step 2a — If RMT software

Switch off Adafruit NeoPixel's RMT path to a WiFi-friendly driver, keeping the same
`lightWrite`/`lightShow` API:
- **NeoPixelBus** with an **I2S/DMA** ESP32 method (DMA-driven, coexists with WiFi far
  better than RMT), or
- **FastLED** (test its ESP32 output method), or
- ESP-IDF `led_strip` with RMT configured carefully.
Re-measure packet loss with LEDs active; must be ~0%.

### Step 2b — If electrical/EMI

- Move data lines off 16/17 (see above) and shorten them.
- Series resistor (~330 Ω) at each data output, decoupling cap at the strips.
- Route the LED wiring / strips **away from the ESP32 PCB antenna** (the striped end).
- Keep the level-shifter and its wiring off the antenna area.

### Success criteria
With `PIXEL_RXTEST_DISABLE 0` and LEDs displaying real status:
- `ping -n 60 192.168.68.148` → ~0% loss (matches device 1).
- `[DIAG]` shows `mqtt=1` held, no `hostByName DNS Failed`, `loopMax` single-digit ms.
- Joins on the predefined path without the AP portal; Node-RED tracks it live.
Then remove the temporary `[DIAG]`/reset-reason logging and the `PIXEL_RXTEST_DISABLE`
scaffold.

---

## Unrelated fix also done this session (context, not part of the task)

The Node-RED dashboard's **activity-log panels were blank for ALL devices** since the
Oracle→lessard-cloud migration. Cause: the 5 per-device "Aggregate Data" functions did
`data.logs.unshift(...)` where `data.logs` was `undefined` (the `check_online` branch
seeded context without a `logs` array), so every `/log` message threw. Guarded with
`if (!Array.isArray(data.logs)) data.logs = [];` + `logs:[]` in the initializer, in all 5
functions. Fixed in `Node-RED/flow.json` (repo, source of truth) and deployed live to
Node-RED on lessard-cloud (backup `flows.json.bak-20260828-225515`). Logs now flow.

Also: device 5's Apps Script calendar endpoint was returning **403** (deployment not
authorized) — Ed re-authorized it; now 200. It still needs `setupICSUrl()` run in that
Apps Script project to set the ICS feed (currently returns "ICS URL not configured").
