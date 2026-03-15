#!/usr/bin/env python3
"""
Busy Light E-Ink Status Display
Subscribes to HiveMQ Cloud MQTT broker
Displays system status on 2.13" e-ink display
"""

import sys
import os
import time
import ssl
from datetime import datetime
import paho.mqtt.client as mqtt  # type: ignore

# Add e-Paper library path
epd_path = '/home/pi/e-Paper/RaspberryPi_JetsonNano/python/lib'
if os.path.exists(epd_path):
    sys.path.append(epd_path)

from waveshare_epd import epd2in13_V4  # type: ignore
from PIL import Image, ImageDraw, ImageFont  # type: ignore

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
UPDATE_INTERVAL = 15  # seconds

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
    """Render current state to e-ink display"""
    try:
        # Create blank image
        image = Image.new('1', (DISPLAY_HEIGHT, DISPLAY_WIDTH), 255)
        draw = ImageDraw.Draw(image)
        font = ImageFont.load_default()

        # Draw border around entire display
        draw.rectangle([(0, 0), (DISPLAY_HEIGHT-1, DISPLAY_WIDTH-1)], outline=0, width=2)

        # Define columns
        col1_x = 6    # Friendly name
        col2_x = 100  # Firmware version
        col3_x = 142  # Status

        y = 6

        # Header
        now = datetime.now().strftime("%m/%d %I:%M%p")
        draw.text((col1_x, y), f"BusyLight  {now}", font=font, fill=0)
        y += 14

        # Separator line
        draw.line([(4, y), (DISPLAY_HEIGHT-4, y)], fill=0, width=1)
        y += 4

        # Column headers
        draw.text((col1_x, y), "Name:", font=font, fill=0)
        draw.text((col2_x, y), "Ver:", font=font, fill=0)
        draw.text((col3_x, y), "Status:", font=font, fill=0)
        y += 13

        # Device rows
        for i in range(1, 6):
            device_id = f'device{i}'
            device = state.devices[device_id]

            name = (device.get('friendly_name', '') or f"Dev{i}")[:14]
            version = device['version'][:6]
            status = device['status']

            draw.text((col1_x, y), name, font=font, fill=0)
            draw.text((col2_x, y), version, font=font, fill=0)
            draw.text((col3_x, y), status, font=font, fill=0)
            y += 13

        # Display the image (full refresh first time, partial after)
        buf = epd.getbuffer(image)
        if render_display.first:
            epd.displayPartBaseImage(buf)
            render_display.first = False
            print(f"[{datetime.now().strftime('%H:%M:%S')}] Display updated (full)")
        else:
            epd.displayPartial(buf)
            print(f"[{datetime.now().strftime('%H:%M:%S')}] Display updated (partial)")

    except Exception as e:
        print(f"[{datetime.now().strftime('%H:%M:%S')}] Display error: {e}")

render_display.first = True

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