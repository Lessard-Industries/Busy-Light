# 🔴 Busy Light

An ESP32-based workplace availability indicator system that automatically displays your meeting status through colored LEDs by integrating with Microsoft 365 calendars.

---

## 📋 Overview

The Busy Light project turns a simple ESP32 microcontroller into a smart, always-updated meeting status indicator. Each device polls a Google Apps Script endpoint (which reads your M365 calendar ICS feed) and lights the appropriate LED color based on your current availability.

Five devices are deployed across home and office environments, all monitored in real time through a cloud-hosted Node-RED dashboard.

---

## 💡 LED Status Colors

| Color | Meaning |
|-------|---------|
| 🔴 Red | In a meeting / Busy |
| 🟢 Green | Available |
| 🔵 Blue | Remote work day |
| ⚫ Off | Outside business hours (8:30 AM – 4:30 PM) |

---

## 🏗️ System Architecture

```
Microsoft 365 Calendar (ICS Feed)
        │
        ▼
Google Apps Script (Calendar Parser)
        │
        ▼ HTTPS
ESP32 Devices (×5) ──── MQTT ────► HiveMQ Cloud
        │                               │
        ▼                               ▼
  LED Indicators              Node-RED (Oracle Cloud)
                                        │
                                        ▼
                              Web Dashboard + Monitoring
                                        │
                                        ▼
                            Raspberry Pi Zero (e-ink display)
```

### Components

| Component | Role |
|-----------|------|
| **ESP32** (×5) | LED busy light nodes |
| **Google Apps Script** | Parses M365 ICS calendar feed, returns JSON |
| **HiveMQ Cloud** | MQTT broker for device telemetry and control |
| **Node-RED (Oracle Cloud)** | Dashboard, monitoring, and device control |
| **Raspberry Pi Zero** | Always-on e-ink status display |

---

## ⚙️ ESP32 Firmware Features

- **Automatic calendar integration** via M365 ICS feed (polled every 3 minutes)
- **MQTT telemetry** — heartbeat, status, mode, and meeting info published every 10 seconds
- **WiFiManager** for flexible network configuration (supports multiple SSIDs)
- **Over-the-Air (OTA) updates** — no physical access required for firmware updates
- **Business hours logic** — lights off automatically outside 8:30 AM–4:30 PM
- **Morning prep** — pre-downloads today's schedule at 7:00 AM
- **Manual override** — physical buttons let you manually set any color
- **Special light modes** — Disco, Double Disco, Time Travel, Color Circle (long-press buttons)
- **Scheduled daily reboot** — for reliability and fresh connections
- **Blackout tracking** — remembers override state across reboots using Preferences storage
- **Friendly device names** — received via MQTT retained messages
- **mDNS support** — accessible by hostname (e.g., `BusyLight-Office-1.local`)
- **Local web interface** — view status and trigger actions via browser

### Device-Specific Pin Layouts

Each of the five devices has a unique pin configuration due to iterative hardware improvements. Pin assignments are defined per `DEVICE_ID` in the firmware configuration.

---

## 📁 Repository Structure

```
├── Busy_Light_x_x_x.ino      # ESP32 Arduino firmware
├── secrets.h                  # WiFi, MQTT credentials (not committed)
├── Google_Script/             # Google Apps Script for calendar parsing
│   └── Code.gs
├── Node-RED/                  # Node-RED flow exports
│   └── flow.json
├── Raspberry_Pi/              # Optional e-ink monitoring station
│   └── busy_light_eink.py     # MQTT-driven Waveshare e-ink display
└── README.md
```

> **Note:** `secrets.h` is excluded from version control. See [Configuration](#-configuration) below.

---

## 🔧 Configuration

Create a `secrets.h` file in the same directory as the `.ino` file:

```cpp
// secrets.h
#define WIFI_SSID_1     "YourHomeSSID"
#define WIFI_PASSWORD_1 "YourHomePassword"
#define WIFI_SSID_2     "YourOfficeSSID"
#define WIFI_PASSWORD_2 "YourOfficePassword"

#define MQTT_SERVER   "your-hivemq-host.hivemq.cloud"
#define MQTT_PORT     8883
#define MQTT_USER     "your-mqtt-username"
#define MQTT_PASSWORD "your-mqtt-password"

#define CALENDAR_API_URL "https://script.google.com/macros/s/YOUR_SCRIPT_ID/exec"

#define DEVICE_ID 1   // Change for each device (1–5)
```

---

## 🛠️ Hardware

### Required per Device
- ESP32 development board
- 5× LEDs (Red, Green, Blue, Yellow, White)
- 5× momentary push buttons
- Resistors (appropriate for your LEDs)
- USB power supply

### Optional (Monitoring Station)
- Raspberry Pi Zero W
- Waveshare 2.13" e-ink display (V4, HAT form factor)

---

## 📦 Software Dependencies

### ESP32 Firmware (PlatformIO / Arduino IDE)
- `WiFiManager`
- `PubSubClient` (MQTT)
- `ArduinoJson`
- `WiFiClientSecure`
- `ArduinoOTA`
- ESP32 Arduino Core **2.0.17** (Note: 3.x versions have LED PWM compatibility issues)


## 🔁 Node-RED Dashboard & Orchestration

This repository includes an export of the Node-RED flows used to monitor,
aggregate, and control all Busy Light devices in real time.

The Node-RED instance acts as the system's **central coordinator**, providing:

- Real-time MQTT status aggregation across all devices
- Online/offline detection using heartbeats and LWT
- Web-based dashboard for monitoring and manual control
- Per-device status pages (LED state, mode, IP, firmware version)
- Master control panel for sending color and mode commands

### Importing the Node-RED Flow

1. Open your Node-RED editor
2. Menu (☰) → Import
3. Upload `Node-RED/flow.json`
4. Update the MQTT broker settings as needed
5. Deploy

No credentials are included in the exported flow.

### Security Notes

- The exported Node-RED flow **does not include any credentials**
- MQTT usernames, passwords, and TLS material must be configured locally
- Broker hostnames may appear in the flow but are not secrets
- Google Apps Script URLs and calendar feeds are **not** embedded in Node-RED

### Raspberry Pi Zero (e-ink display)

An optional Raspberry Pi Zero W subscribes to MQTT and displays the
real-time status of all Busy Light devices on a 2.13" Waveshare e-ink display.

Features:
- Live online/offline status for all devices
- Busy / Free / Remote indicators
- Firmware version display
- MQTT connection, temperature, and uptime stats
- MQTT credentials should be configured directly in the script or via environment variables.

Configuration (MQTT credentials) is required and not included in the repo.


```bash
sudo apt install python3-pil python3-numpy python3-requests python3-spidev
pip3 install paho-mqtt
git clone https://github.com/waveshare/e-Paper.git
```
## 🧩 Google Apps Script (Calendar API)

The ESP32 devices do not talk directly to Microsoft 365. Instead, they poll a lightweight **Google Apps Script Web App** that:

1. Fetches your Microsoft 365 calendar **ICS feed**
2. Parses events (including recurring meetings)
3. Applies business rules (business hours, weekends, “REMOTE” days, “OFF” blocks)
4. Returns a compact JSON payload the ESP32 can consume

This script lives in: `Google_Script/Code.gs`

### ✅ Features

- **ICS Fetch + Parse**  
  Pulls the raw `.ics` feed from Microsoft 365 and extracts events, including:
  - Regular events
  - All-day events
  - Recurring events via `RRULE`
  - Modified/moved recurring instances using `RECURRENCE-ID` exceptions

- **Aggressive Caching (3 minutes)**  
  Uses Apps Script `CacheService` to reduce calendar fetches and keep responses fast.

- **Weekend + Business Hours Logic**  
  Automatically returns `off_hours` on weekends, and outside the configured window:
  **8:30 AM – 4:30 PM** (configurable constants in the script).

- **Remote Work Day Detection**  
  Flags a day as `remote` when it finds an event containing `REMOTE`
  scheduled before 9:00 AM on the current day.

- **Busy / Free Determination + 1-Minute Grace Window**  
  Marks you as `busy` during active meetings, including a 1-minute pre-start grace period.

- **Special "OFF" Appointment Handling**  
  If an active meeting contains `OFF` in the title, the API returns `off_hours`
  (useful for PTO blocks, appointments, etc.).

- **Next Meeting Preview**  
  Returns the next upcoming meeting title/time (within business hours),
  allowing the ESP32/Node-RED dashboard to show what’s coming.

- **Helpful Debug Payload**  
  Includes a `debug_info` section with:
  - total parsed events
  - cache age
  - script + cache version

### 📦 JSON Response Format

Example (shape only):

```json
{
  "status": "busy | free | remote | off_hours | error",
  "timestamp": "2026-02-26T01:23:45.678Z",
  "business_hours": {
    "start": 8.5,
    "end": 16.5,
    "current_hour": 13.25
  },
  "remote_day": false,
  "current_meeting": {
    "title": "Standup",
    "end_time": "2026-02-26 10:30:00"
  },
  "next_meeting": {
    "time": "2026-02-26 11:00:00",
    "title": "Planning"
  },
  "debug_info": {
    "total_events": 12,
    "cache_age_seconds": 42,
    "script_version": "8.0.5",
    "cache_version": "10"
  }
}
```
---

## 🚀 Getting Started

### 1. Google Apps Script
- Create a new Google Apps Script project
- Paste the contents of `Google_Script/Code.gs`
- Deploy as a Web App (execute as yourself, accessible to anyone)
- Copy the deployment URL into your firmware as `CALENDAR_API_URL`

### 2. MQTT Broker
- Sign up for [HiveMQ Cloud](https://www.hivemq.com/mqtt-cloud-broker/) (free tier available)
- Create credentials and add them to your firmware configuration

### 3. Flash ESP32 Firmware
- Open project in PlatformIO (VS Code) or Arduino IDE
- Create `secrets.h` with your credentials
- Set `DEVICE_ID` for each device (1–5)
- Flash firmware via USB or OTA

### 4. Node-RED Dashboard (Optional)
- Deploy Node-RED on Oracle Cloud (always-free tier)
- Import `Node-RED/flow.json`
- Update MQTT broker settings to match your HiveMQ credentials

### 5. Raspberry Pi Zero e-ink Display (Optional)
- Flash Raspberry Pi OS Bookworm (32-bit)
- Enable SPI via `sudo raspi-config` → Interface Options → SPI
- Install dependencies (see above)
- Copy `busy_light_status.py` to `/home/pi/`
- Set up systemd service for autostart on boot

---

## 📡 MQTT Topic Structure

```
busylight/device{ID}/status        → free | busy | remote | unknown
busylight/device{ID}/mode          → NORMAL | OFF_HOURS | REMOTE | DISCO | etc.
busylight/device{ID}/active_led    → RED | GREEN | BLUE | YELLOW | WHITE | NONE
busylight/device{ID}/ip            → Device IP address (retained)
busylight/device{ID}/version       → Firmware version (retained)
busylight/device{ID}/name          → Friendly name (retained, set externally)
busylight/device{ID}/meeting/title → Current meeting title
busylight/device{ID}/lwt           → Last Will Testament (offline detection)
```

---

## 🔄 How It Works

1. On boot, the ESP32 connects to WiFi, syncs time via NTP, and connects to HiveMQ
2. At 7:00 AM, it pre-fetches today's full calendar schedule
3. During business hours, it re-checks the calendar every 3 minutes
4. The appropriate LED lights based on current calendar status
5. Status and meeting data are published to MQTT every 10 seconds
6. Node-RED subscribes to MQTT and displays real-time status on the dashboard
7. The Pi Zero e-ink display subscribes to MQTT and refreshes every 60 seconds

---

## 🏢 Deployment Notes

- Designed for enterprise environments where admin WiFi rights may be unavailable
- WiFiManager handles captive portal setup on first boot — no hardcoded SSID required
- OTA updates eliminate the need for physical access to deployed devices
- Cloud services (HiveMQ, Oracle Cloud) provide reliable, maintenance-free operation

---

## 📝 Version History

| Version | Notes |
|---------|-------|
| 8.0.x | PlatformIO migration, separate TLS clients, secrets.h config |
| 7.7.x | HiveMQ Cloud MQTT, Oracle Cloud Node-RED, friendly device names |
| 7.4.x | Blackout tracking, scheduled reboots, enhanced WiFi reconnection |
| 3.x | Raspberry Pi hub architecture (now retired) |

---

## 🤝 Acknowledgements

Developed with assistance from Claude (Anthropic), ChatGPT (OpenAI), and Gemini (Google). Special thanks to Jim for embedded systems expertise during debugging sessions.

---

## 📄 License

MIT License — feel free to adapt this project for your own workplace.
