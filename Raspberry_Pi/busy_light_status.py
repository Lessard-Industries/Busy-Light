"""
Busy Light E-Ink Status Display
Subscribes to HiveMQ Cloud MQTT broker
Displays system status on 2.13" e-ink display
"""

import sys
import os
import time
import json
import ssl
from datetime import datetime
import paho.mqtt.client as mqtt

# Add e-Paper library path
epd_path = '/home/pi/e-Paper/RaspberryPi_JetsonNano/python/lib'
if os.path.exists(epd_path):
    sys.path.append(epd_path)

from waveshare_epd import epd2in13_V4
from PIL import Image, ImageDraw, ImageFont

# ============================================================================
# CONFIGURATION
# ============================================================================

# HiveMQ Cloud Configuration
MQTT_BROKER = "your-hivemq-cluster.hivemq.cloud"
MQTT_PORT = 8883
MQTT_USERNAME = "your-mqtt-username"
MQTT_PASSWORD = "your-mqtt-password"
MQTT_TOPIC = "busylight/#"

# Display settings
DISPLAY_WIDTH = 122
DISPLAY_HEIGHT = 250
UPDATE_INTERVAL = 60  # seconds

# ============================================================================
# DEVICE STATE TRACKING
# ============================================================================

class DeviceState:
    def __init__(self):
        self.devices = {
            'device1': {'online': False, 'status': '--', 'mode': '--', 'last_seen': 0, 'version': '--', 'friendly_name': ''},
            'device2': {'online': False, 'status': '--', 'mode': '--', 'last_seen': 0, 'version': '--', 'friendly_name': ''},
            'device3': {'online': False, 'status': '--', 'mode': '--', 'last_seen': 0, 'version': '--', 'friendly_name': ''},
            'device4': {'online': False, 'status': '--', 'mode': '--', 'last_seen': 0, 'version': '--', 'friendly_name': ''},
            'device5': {'online': False, 'status': '--', 'mode': '--', 'last_seen': 0, 'version': '--', 'friendly_name': ''},
        }
        self.mqtt_connected = False
        self.last_update = None

    def update_device(self, device_id, data_type, value):
        """Update device state from MQTT message"""
        if device_id not in self.devices:
            return

        device = self.devices[device_id]
        device['last_seen'] = time.time()

        if data_type == 'lwt':
            device['online'] = (value == 'online')
        elif data_type == 'status':
            device['status'] = value
        elif data_type == 'mode':
            device['mode'] = value
        elif data_type == 'version':
            device['version'] = value
        elif data_type == 'friendly_name':
            device['friendly_name'] = value

    def check_timeouts(self):
        """Mark devices offline if no message in 30 seconds"""
        now = time.time()
        for device in self.devices.values():
            if device['last_seen'] > 0 and (now - device['last_seen']) > 30:
                device['online'] = False

# ============================================================================
# MQTT CALLBACKS
# ============================================================================

state = DeviceState()

def on_connect(client, userdata, flags, rc):
    """Called when connected to MQTT broker"""
    if rc == 0:
        print(f"[{datetime.now().strftime('%H:%M:%S')}] Connected to MQTT broker")
        state.mqtt_connected = True
        client.subscribe(MQTT_TOPIC)
        print(f"[{datetime.now().strftime('%H:%M:%S')}] Subscribed to {MQTT_TOPIC}")
    else:
        print(f"[{datetime.now().strftime('%H:%M:%S')}] Connection failed, code: {rc}")
        state.mqtt_connected = False

def on_disconnect(client, userdata, rc):
    """Called when disconnected from MQTT broker"""
    print(f"[{datetime.now().strftime('%H:%M:%S')}] Disconnected from MQTT broker")
    state.mqtt_connected = False

def on_message(client, userdata, msg):
    """Called when MQTT message received"""
    try:
        topic_parts = msg.topic.split('/')
        if len(topic_parts) < 3:
            return

        # Parse topic: busylight/device2/status
        device_id = topic_parts[1]  # "device2" or "2"
        data_type = topic_parts[2]   # "status", "mode", "lwt", "version", "friendly_name", etc.

        # Convert numeric device IDs to device# format
        if not device_id.startswith('device'):
            device_id = 'device' + device_id

        # Get payload value
        value = msg.payload.decode('utf-8')

        # Update device state
        state.update_device(device_id, data_type, value)

    except Exception as e:
        print(f"[{datetime.now().strftime('%H:%M:%S')}] Error processing message: {e}")

# ============================================================================
# E-INK DISPLAY RENDERING
# ============================================================================

def render_display(epd):
    """Render current state to e-ink display - three column layout with border"""
    try:
        # Create blank image
        image = Image.new('1', (DISPLAY_HEIGHT, DISPLAY_WIDTH), 255)
        draw = ImageDraw.Draw(image)
        font = ImageFont.load_default()

        # Draw border around entire display
        draw.rectangle([(0, 0), (DISPLAY_HEIGHT-1, DISPLAY_WIDTH-1)], outline=0, width=2)

        # Check for timeouts
        state.check_timeouts()

        # Define three columns
        col1_x = 6    # All 5 devices
        col2_x = 100  # Firmware versions
        col3_x = 175  # System stats

        y = 6

        # Header - spans all columns
        now = datetime.now().strftime("%m/%d %I:%M%p")
        online_count = sum(1 for d in state.devices.values() if d['online'])
        draw.text((col1_x, y), f"BusyLight {online_count}/5", font=font, fill=0)
        y += 14

        # Separator line
        draw.line([(4, y), (DISPLAY_HEIGHT-4, y)], fill=0, width=1)
        y += 4

        # Column headers
        draw.text((col1_x, y), "Device:", font=font, fill=0)
        draw.text((col2_x, y), "Ver:", font=font, fill=0)
        y += 13

        # COLUMN 1: All 5 devices with friendly names
        col1_y = y
        for i in range(1, 6):
            device_id = f'device{i}'
            device = state.devices[device_id]

            # Use friendly name if set, otherwise device number
            name = device.get('friendly_name', '')
            if name:
                # Truncate to 8 chars for display
                name = name[:8]
            else:
                name = f"Dev{i}"

            if device['online']:
                status_map = {
                    'free': 'F',
                    'busy': 'B',
                    'remote': 'R',
                    'off_hours': 'O'
                }
                status = status_map.get(device['status'][:10].lower(), '-')
                line = f"{name}: {status}"
            else:
                line = f"{name}: --"

            draw.text((col1_x, col1_y), line, font=font, fill=0)
            col1_y += 13

        # COLUMN 2: Firmware versions
        col2_y = y

        for i in range(1, 6):
            device_id = f'device{i}'
            device = state.devices[device_id]

            if device['online']:
                version = device['version']
                # Shorten version (e.g., "7.7.1" -> "7.7.1")
                if len(version) > 5:
                    version = version[:5]
                draw.text((col2_x, col2_y), version, font=font, fill=0)
            else:
                draw.text((col2_x, col2_y), "--", font=font, fill=0)

            col2_y += 13

        # COLUMN 3: System stats (starts after column headers)
        col3_y = y

        # MQTT connection
        draw.text((col3_x, col3_y), "MQTT:", font=font, fill=0)
        col3_y += 13
        mqtt_status = "OK" if state.mqtt_connected else "FAIL"
        draw.text((col3_x, col3_y), mqtt_status, font=font, fill=0)
        col3_y += 16

        # CPU Temperature
        try:
            with open('/sys/class/thermal/thermal_zone0/temp', 'r') as f:
                temp_raw = int(f.read().strip())
                temp_c = temp_raw / 1000.0
                temp_text = f"{temp_c:.0f}C"
        except:
            temp_text = "--"

        draw.text((col3_x, col3_y), "Temp:", font=font, fill=0)
        col3_y += 13
        draw.text((col3_x, col3_y), temp_text, font=font, fill=0)
        col3_y += 16

        # Pi Uptime
        try:
            with open('/proc/uptime', 'r') as f:
                uptime_seconds = float(f.read().split()[0])
                hours = int(uptime_seconds // 3600)
                if hours < 24:
                    uptime_text = f"{hours}h"
                else:
                    days = hours // 24
                    uptime_text = f"{days}d"
        except:
            uptime_text = "--"

        draw.text((col3_x, col3_y), "Up:", font=font, fill=0)
        col3_y += 13
        draw.text((col3_x, col3_y), uptime_text, font=font, fill=0)

        # Display the image
        epd.display(epd.getbuffer(image))
        print(f"[{datetime.now().strftime('%H:%M:%S')}] Display updated")

    except Exception as e:
        print(f"[{datetime.now().strftime('%H:%M:%S')}] Display error: {e}")

# ============================================================================
# MAIN PROGRAM
# ============================================================================

def main():
    print("=" * 50)
    print("BusyLight E-Ink Status Display")
    print("=" * 50)

    # Initialize e-ink display
    try:
        print("Initializing e-ink display...")
        epd = epd2in13_V4.EPD()
        epd.init()
        epd.Clear(0xFF)
        print("Display initialized")
    except Exception as e:
        print(f"Display initialization failed: {e}")
        return

    # Setup MQTT client
    client = mqtt.Client()
    client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)
    client.tls_set(cert_reqs=ssl.CERT_NONE)
    client.tls_insecure_set(True)
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message

    # Connect to MQTT broker
    print(f"Connecting to MQTT broker: {MQTT_BROKER}:{MQTT_PORT}")
    try:
        client.connect(MQTT_BROKER, MQTT_PORT, 60)
    except Exception as e:
        print(f"MQTT connection failed: {e}")
        return

    # Start MQTT loop in background
    client.loop_start()

    # Main update loop
    last_display_update = 0

    try:
        print("Entering main loop...")
        while True:
            current_time = time.time()

            # Update display every UPDATE_INTERVAL seconds
            if current_time - last_display_update >= UPDATE_INTERVAL:
                render_display(epd)
                last_display_update = current_time

            # Sleep briefly
            time.sleep(1)

    except KeyboardInterrupt:
        print("\nShutting down...")
        client.loop_stop()
        client.disconnect()
        epd.sleep()
        print("Cleanup complete")

if __name__ == '__main__':
    main()