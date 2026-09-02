# Raspberry Pi Zero W — BusyLight Dashboard Setup

## Hardware

| Component | Detail |
|-----------|--------|
| Board | Raspberry Pi Zero W Rev 1.1 (BCM2835, 512MB RAM) |
| Display | Waveshare 2.13" E-Ink V4 (250x122, black/white) |
| Storage | MicroSD |
| Network | WiFi (wlan0) |

### E-Ink Wiring (SPI + GPIO)

| Display Pin | Pi GPIO (BCM) | Pi Physical Pin |
|-------------|---------------|-----------------|
| VCC | 3.3V | 1 |
| GND | GND | 6 |
| DIN (MOSI) | GPIO 10 | 19 |
| CLK (SCLK) | GPIO 11 | 23 |
| CS | GPIO 8 (CE0) | 24 |
| DC | GPIO 25 | 22 |
| RST | GPIO 17 | 11 |
| BUSY | GPIO 24 | 18 |
| PWR | GPIO 18 | 12 |

## Software

| Component | Version |
|-----------|---------|
| OS | Raspbian 12 (Bookworm) |
| Kernel | 6.12.47+rpt-rpi-v6 |
| Python | 3.11.2 |
| paho-mqtt | 1.6.1 |
| Pillow (PIL) | 9.4.0 |
| gpiozero | 2.0.1 |
| lgpio | 0.2.2 |
| spidev | 20200602 |

### E-Paper Driver

Waveshare e-Paper library installed at:
```
/home/pi/e-Paper/RaspberryPi_JetsonNano/python/lib/waveshare_epd/
```
Driver file: `epd2in13_V4.py`

## Boot Configuration

SPI must be enabled in `/boot/firmware/config.txt`:
```
dtparam=spi=on
```

Other active overlays:
```
dtoverlay=vc4-kms-v3d
dtoverlay=dwc2,dr_mode=host
```

## Network

| Setting | Value |
|---------|-------|
| Hostname | PiZero |
| IP Address | `192.168.68.243` — **static DHCP reservation on the hEX router** (MAC `B8:27:EB:B5:5F:D1`); reserved 2026-07-20, see `C:\Code\netcom\devices\hex\README.md` |
| SSH | Key-based auth, standard netcom key (`~/.ssh/id_ed25519`, comment `ed@LessardServer`) |

SSH from development machine:
```
ssh pi@pizero
```
(Resolved via SSH config at `~/.ssh/config`, `Host pizero` → `HostName 192.168.68.243`)

**Not monitored by netcom-status** — this Pi is intentionally excluded from the netcom push/health-check system. The only netcom-side footprint is the DHCP reservation row in `devices/hex/README.md`. If the Pi ever needs a new reservation or the key is rotated, update it there and in this file — netcom has no automated tracking of it.

### If the IP ever needs to change
1. Find current MAC/lease: `ssh admin@192.168.68.1 "/ip dhcp-server lease print detail where mac-address=B8:27:EB:B5:5F:D1"`
2. Update/re-add the static lease on the router (`/ip dhcp-server lease add ...` or edit in place)
3. Update the IP here and in `devices/hex/README.md`'s reservation table
4. Update `~/.ssh/config`'s `pizero` `HostName`

## Application

### Script Location
```
/home/pi/busy_light_status.py
```

### What It Does
- Subscribes to MQTT broker (HiveMQ Cloud) on topic `busylight/#`
- Receives device status, version, mode, and friendly name from up to 5 ESP32 BusyLight devices
- Renders a dashboard on the e-ink display every 15 seconds

### Display Layout (250x122 pixels)
```
┌──────────────────────────────────────────┐
│ BusyLight  03/14 10:30PM                 │
│──────────────────────────────────────────│
│ Name:          Ver:    Status:            │
│ EdOffice       8.0.1   free               │
│ LivingRoom     8.0.1   busy               │
│ Dev3           --      --                 │
│ Dev4           --      --                 │
│ Dev5           --      --                 │
└──────────────────────────────────────────┘
```

Column positions: Name (x=6, 14 chars), Version (x=100, 6 chars), Status (x=142)

### Refresh Mode
- First render: full refresh via `displayPartBaseImage()` (sets base image)
- Subsequent renders: partial refresh via `displayPartial()` (smooth, no flicker)

### MQTT Credentials
Real credentials are stored only on the Pi in `busy_light_status.py`.
The repo copy uses placeholder values — update on the Pi after each deploy:
```
MQTT_BROKER = "your-hivemq-cluster.hivemq.cloud"
MQTT_USERNAME = "your-mqtt-username"
MQTT_PASSWORD = "your-mqtt-password"
```

## Systemd Service

Service file: `/etc/systemd/system/busylight-display.service`

```ini
[Unit]
Description=Busy Light E-Ink Status Display
After=network-online.target
Wants=network-online.target

[Service]
Environment=PYTHONUNBUFFERED=1
# Wait (up to 60s) for the SPI + GPIO device nodes to appear before starting.
# On a cold boot rpi-gpiomem/spidev come up ~50s in; without this the script
# hit epd.init() too early and crash-looped with "[Errno 2] No such file...".
ExecStartPre=/bin/sh -c 'n=0; until { [ -e /dev/spidev0.0 ] && [ -e /dev/gpiochip0 ]; } || [ "$n" -ge 60 ]; do n=$((n+1)); sleep 1; done'
ExecStart=/usr/bin/python3 /home/pi/busy_light_status.py
WorkingDirectory=/home/pi
StandardOutput=journal
StandardError=journal
Restart=always
RestartSec=10
User=pi

[Install]
WantedBy=multi-user.target
```

> **Cold-boot SPI/GPIO race (fixed 2026-09-02):** the Pi's `rpi-gpiomem`/`spidev`
> nodes don't appear until ~50s into boot, but the service used to start ~12s in and
> crash-loop on `Display initialization failed: [Errno 2] No such file or directory`,
> so the e-ink held a stale frame (looked "connected but wrong devices"). The
> `ExecStartPre` line above blocks the start until both nodes exist. `PYTHONUNBUFFERED=1`
> makes the script's prints show up in `journalctl` in real time instead of buffering.

### Service Commands
```bash
sudo systemctl status busylight-display    # Check status
sudo systemctl restart busylight-display   # Restart after updates
sudo systemctl stop busylight-display      # Stop
sudo systemctl start busylight-display     # Start
sudo journalctl -u busylight-display -f    # Follow logs
```

## Deploying Updates

From the development machine:
```bash
scp Raspberry_Pi/busy_light_status.py pi@pizero:~/busy_light_status.py
ssh pi@pizero "sudo systemctl restart busylight-display"
```
Remember to restore real MQTT credentials on the Pi after copying.
