# Device 5 (PIXEL build) — WiFi RX investigation: RESOLVED 2026-08-29

**Status:** Closed. LEDs enabled (v8.2.3-pixel), 0% packet loss in every configuration,
boot join hardened. This file is kept as the record of what was measured.

## Result

The earlier claim (2026-08-28) that the WS2812/RMT LED driver caused ~63% inbound
packet loss **did not reproduce**. Every test below ran a 60-ping race against device 5
(.148) with device 1 (.134) as a same-moment control, the Adafruit NeoPixel RMT driver
fully active, and the effect/mode confirmed over MQTT (`busylight/device5/mode`).

| Hardware | LED activity | .148 loss | .134 control |
|---|---|---|---|
| ball only (tube data connected, tube 5 V off) | COLOR_CIRCLE (continuous `show()`) | 0/60 | 0/60 |
| ball + tube, tube powered from VIN | COLOR_CIRCLE | 0/60 | 0/60 |
| ball + tube powered | solid WHITE (max current) | 0/60 | 0/60 |
| ball + tube powered | DOUBLE_DISCO | 0/60 | 0/60 |
| ball + tube powered, joined via WiFiManager portal | idle | 2/60, then 0/120 | 0/60, 0/120 |
| LED driver compiled out (previous session's baseline) | dark | 0/20 | 0/20 |

Throughout: `mqtt=1` held, `loopMax` ≤ 11 ms, RSSI −47…−58, no DNS failures.
The 2/60 after a portal-path join was the first seconds after association (DHCP/ARP);
the setup softAP was confirmed **not** left on air (PC WiFi scan).

## The one real, reproducible defect: boot-time auth refusal (fixed in v8.2.3)

Measured with the ESP32's own disconnect reason codes (the handler's 60 s rate-limit
had been swallowing every boot-window reason; fixed):

- 5EBA is not present at home → 4× reason 201 (NO_AP_FOUND) over the 10 s attempt.
- NightHawk, first attempt ~11 s after an abrupt reset/power-cycle → **reason 202
  (AUTH_FAIL) 0.1 s after `begin()`**. The ESP32 does not retry after 202. The old
  loop moved on, so the next real attempt was 20 s later (pass 2) or the 2-min portal.
- Reset after a long-lived session, or after the ~30 s esptool parks the chip → accepted.
- With a *fresh* (spoofed) MAC → accepted first try; on the very next reset with that
  same MAC → 202. So the AP refuses the first auth from a MAC whose previous session it
  still holds; MAC value, hostname (`Ed-Pixel-8` vs stock) and `WiFi.setSleep(false)`
  were each tested and make no difference.
- **Fix:** an SSID that answered-but-refused is re-`begin()`'d every 3 s for up to 30 s
  (an SSID that is simply absent still gets 10 s); up to 6 passes before the portal.
  Verified on three consecutive refused boots: 202 at 11.3 s → connected at 14.3 s →
  MQTT ~25 s. Non-refused boots join at ~11.5 s.
- The PWM build (devices 1–4, 8.1.0) has the same swallowed-reason bug and no retry —
  it recovers via WiFiManager's own connect attempt (~10 s later). A same-fix 8.1.1 for
  the main firmware is a to-do, not urgent.

Device 5 runs its factory MAC (Opus's choice stands); `WiFi.setSleep(false)` removed
(never in the main build, measured irrelevant).

**Power-integrity note:** twice the serial output turned to garbage for ~1 s exactly
when the strips switched (end of the connect blink, the blue/white flashes), and two
of the day's uploads aborted with "serial noise". Strips + shifter on VIN with only a
small ceramic → ground/5 V bounce. Did not cost packets in any test. Ed added a bulk
electrolytic on 5 V (2026-08-29, hot-plugged → one brownout reboot, expected); the
next serial-observed boot had zero garbage bursts and 0/60 loss with LEDs cycling.

## Kept from the previous session (correct, independent of all this)

`lightShow()` batching (one transmit per loop per strip); non-blocking `syncTime()`
(v8.2.2: one bounded 10 s NTP wait at boot with the blue blink so the boot sequence
matches the PWM units, then 2 s background polling — never the old 88 s block);
HTTP timeout 5 s + TLS handshake timeout 6 s; `WiFi.setSleep(false)`; static-IP attempt
fully backed out (unit is pure DHCP). `PIXEL_RXTEST_DISABLE` scaffold and the `[DIAG]`
loop telemetry were removed in v8.2.1/8.2.2; the boot `Reset reason:` print stays.

## Known minor items (not part of this issue)

- Every 3 min the calendar HTTPS GET logs `ssl_client.cpp … (-76)` and blocks the loop
  4–8 s. It is the calendar poll (blocking HTTPS + Google redirect); same on all units.
  Device 5's Apps Script endpoint is healthy (200, `script_version 8.1.0`, correct
  off_hours on a Saturday) — the earlier "ICS URL not configured" note is stale.
- Adafruit NeoPixel 1.15.5 installs/uninstalls the RMT driver on every `show()`. Ugly,
  but measured harmless here; leave it unless a reason appears.

## Hardware (device 5)

ESP32 DevKit (`esp32doit-devkit-v1`), platform pinned `espressif32@6.9.0`. WS2812B via
SN74AHCT125N: GPIO16 → ball (3 px), GPIO17 → tube (12 px), brightness 191. Strips and
shifter powered from the board's VIN; only capacitor is a ceramic across the shifter's
supply pins. Factory MAC `20:E7:C8:BB:00:1C`, DHCP `.148`.
