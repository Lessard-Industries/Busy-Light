// ============================================================================
// Busy Light Project - Version 8.0.4
// ESP32 with M365 Calendar Integration via HiveMQ Cloud MQTT
// Conversion to platform.io
// Lessard Industries, 2026
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include "secrets.h"

// ============================================================================
// CONFIGURATION - Edit these values as needed
// ============================================================================

// Device ID - Change this for each device (1-5)
#define DEVICE_ID 2

// MQTT Broker (HiveMQ Cloud)
const char* MQTT_SERVER = SECRET_MQTT_SERVER;
const int MQTT_PORT = SECRET_MQTT_PORT;
const char* MQTT_USER = SECRET_MQTT_USER;
const char* MQTT_PASSWORD = SECRET_MQTT_PASSWORD;

// Calendar API (Google Apps Script endpoint)
const char* CALENDAR_API_URL = SECRET_CALENDAR_API_URL;

// Time Configuration
const char* NTP_SERVER = "pool.ntp.org";
const long GMT_OFFSET_SEC = -5 * 3600;  // EST
const int DAYLIGHT_OFFSET_SEC = 0;      // Manually adjust when needed

// Business Hours (device-side enforcement for precise on/off)
const float BUSINESS_START_HOUR = 8.5;   // 8:30 AM
const float BUSINESS_END_HOUR = 16.5;    // 4:30 PM
const float MORNING_PREP_HOUR = 7.0;     // 7:00 AM - check for REMOTE day

// Timing Intervals
const unsigned long CALENDAR_CHECK_INTERVAL_MS = 3 * 60 * 1000;  // 3 minutes
const unsigned long MQTT_PUBLISH_INTERVAL_MS = 10000;            // 10 seconds heartbeat
const unsigned long MIN_API_CALL_INTERVAL_MS = 5000;             // Rate limit protection
const int MAX_CALENDAR_FAILURES = 3;

// Hardware Pin Assignments (varies by device due to iterative builds)
#if DEVICE_ID == 1
  const int LED_PINS[] = {23, 22, 21, 19, 18};
  const int BUTTON_PINS[] = {5, 4, 16, 17, 25};
#elif DEVICE_ID == 2
  const int LED_PINS[] = {32, 33, 25, 26, 27};
  const int BUTTON_PINS[] = {23, 22, 21, 19, 18};
#elif DEVICE_ID == 3
  const int LED_PINS[] = {33, 25, 26, 27, 14};
  const int BUTTON_PINS[] = {23, 22, 21, 19, 18};
#elif DEVICE_ID == 4
  const int LED_PINS[] = {32, 33, 25, 26, 27};
  const int BUTTON_PINS[] = {23, 22, 21, 19, 18};
#elif DEVICE_ID == 5
  const int LED_PINS[] = {27, 32, 33, 25, 26};
  const int BUTTON_PINS[] = {23, 22, 21, 19, 18};
#endif

// LED/PWM Settings
const int NUM_LEDS = 5;
const int PWM_FREQUENCY = 20000;
const int PWM_RESOLUTION = 8;

// Visual Effect Speeds
const int DISCO_FADE_AMOUNT = 2;
const int DISCO_UPDATE_DELAY = 15;
const int TIMETRAVEL_FADE_DELAY = 5;
const int CIRCLE_FADE_AMOUNT = 5;
const int CIRCLE_UPDATE_DELAY = 10;
const int PULSE_SPEED = 5;
const int PULSE_UPDATE_INTERVAL = 15;
const int FLASH_DURATION_MS = 100;
const int GAP_DURATION_MS = 100;

// ============================================================================
// DERIVED CONSTANTS (auto-generated from config)
// ============================================================================

const char* FIRMWARE_VERSION = "8.0.6";
const String DEVICE_HOSTNAME = "BusyLight-Office-" + String(DEVICE_ID);

// MQTT Topics
const String MQTT_BASE_TOPIC = "busylight/device" + String(DEVICE_ID);
const String MQTT_STATUS_TOPIC = MQTT_BASE_TOPIC + "/status";
const String MQTT_MODE_TOPIC = MQTT_BASE_TOPIC + "/mode";
const String MQTT_IP_TOPIC = MQTT_BASE_TOPIC + "/ip";
const String MQTT_LOG_TOPIC = MQTT_BASE_TOPIC + "/log";
const String MQTT_COMMAND_TOPIC = MQTT_BASE_TOPIC + "/command";
const String MQTT_ALL_COMMAND_TOPIC = "busylight/all/command";

// Friendly name
Preferences prefs;
String friendlyName = "";

// ============================================================================
// ENUMS AND STRUCTURES
// ============================================================================

enum Mode { NORMAL, DISCO, DOUBLE_DISCO, TIMETRAVEL, COLOR_CIRCLE, REMOTE, OFF_HOURS };
enum ButtonState { IDLE, PRESSED, HELD };

struct FadeState {
  bool fadingIn;
  int duty;
};

struct Button {
  const uint8_t PIN;
  const unsigned long DEBOUNCE_DELAY = 50;
  const unsigned long LONG_PRESS_DURATION = 1500;
  bool lastReading = HIGH;
  bool debouncedState = HIGH;
  unsigned long lastDebounceTime = 0;
  unsigned long pressStartTime = 0;
  bool heldTriggered = false;

  Button(uint8_t pin) : PIN(pin) {}

  void setup() {
    pinMode(PIN, INPUT_PULLUP);
  }

  ButtonState getState() {
    bool currentReading = digitalRead(PIN);
    if (currentReading != lastReading) lastDebounceTime = millis();
    lastReading = currentReading;

    if ((millis() - lastDebounceTime) > DEBOUNCE_DELAY) {
      if (currentReading != debouncedState) {
        debouncedState = currentReading;
        if (debouncedState == LOW) {
          pressStartTime = millis();
          heldTriggered = false;
          return PRESSED;
        }
      }
    }

    if (debouncedState == LOW && !heldTriggered) {
      if ((millis() - pressStartTime) > LONG_PRESS_DURATION) {
        heldTriggered = true;
        return HELD;
      }
    }

    if (debouncedState == HIGH) {
      heldTriggered = false;
    }
    return IDLE;
  }
};

// ============================================================================
// GLOBAL STATE
// ============================================================================

// MQTT
WiFiClientSecure mqttSecure;
PubSubClient mqttClient(mqttSecure);
WiFiClientSecure httpSecure;
unsigned long lastMqttReconnect = 0;
unsigned long lastMqttPublish = 0;

// Mode and Display
Mode currentMode = NORMAL;
const char* LED_COLOR_NAMES[] = {"RED", "GREEN", "BLUE", "YELLOW", "WHITE"};
const char* SPECIAL_MODE_NAMES[] = {"DOUBLE_DISCO", "TIMETRAVEL", "COLOR_CIRCLE", "DISCO", "NORMAL"};
const Mode BUTTON_MODE_MAP[] = {DOUBLE_DISCO, TIMETRAVEL, COLOR_CIRCLE, DISCO, NORMAL};

// Hardware
Button buttons[] = {
  Button(BUTTON_PINS[0]), Button(BUTTON_PINS[1]), Button(BUTTON_PINS[2]),
  Button(BUTTON_PINS[3]), Button(BUTTON_PINS[4])
};
int activeLed = -1;

// Visual Effects State
int discoState = 0, discoDuty = 0, discoLed1 = -1, discoLed2 = -1;
FadeState fadeStates[NUM_LEDS];
int circleCurrentLedIndex = 0, circleDuty = 0;
unsigned long lastFadeStepTime = 0, lastCircleStepTime = 0, lastTimetravelStep = 0;

// Warning Pulse
bool warningPulseActive = false;
int pulseBrightness = 0;
bool pulseDirection = true;

// Acknowledgement Flash (for long-press white feedback)
bool flashActive = false;
unsigned long flashStartTime = 0;
bool gapActive = false;
unsigned long gapStartTime = 0;

// Meeting and Alarm State
bool preemptiveCheckDone = false;
bool meetingInProgress = false;
bool alarmIsForEnd = false;

// System State
bool timeSynced = false;
bool autoMode = true;
time_t lastLoggedSecond = 0;
unsigned long lastCalendarCheck = 0;
unsigned long lastAPICallTime = 0;
time_t nextMeetingStartTime = 0;
int calendarCheckFailures = 0;

// Blackout tracking
unsigned long lastMqttSuccess = 0;
unsigned long lastCalendarSuccess = 0;
unsigned long wifiLostTime = 0;
int reconnectAttempts = 0;
bool apTriggered = false;

// Daily Trigger Flags
bool isRemoteDay = false;
bool morningPrepDone = false;
bool businessHoursStartDone = false;
bool businessHoursEndDone = false;

// Calendar Data (updated from API, can override device defaults)
String currentStatus = "unknown";
String nextMeetingTimeStr = "";
String nextMeetingTitle = "";
String currentMeetingTitle = "";
float apiBusinessHoursStart = 8.5;
float apiBusinessHoursEnd = 16.5;

// Forward declaration
void updateLightFromCalendar(bool verboseLog = false);
void publishMQTTStatus(bool force = false);
void publishBlackoutSummary();
void switchToSpecialMode(int buttonIndex);
void saveFriendlyName(String name);

// ============================================================================
// LOGGING
// ============================================================================

void logMessage(String message) {
  // Serial: errors and critical events only (before MQTT available)
  if (message.indexOf("ERROR") != -1 || 
      message.indexOf("failed") != -1 ||
      message.indexOf("Connected") != -1 ||
      message.indexOf("===") != -1) {
    Serial.println(message);
  }
  
  // MQTT: all messages once connected
  if (mqttClient.connected()) {
    mqttClient.publish(MQTT_LOG_TOPIC.c_str(), message.c_str());
  }
}

// ============================================================================
// WIFI EVENT HANDLER
// ============================================================================

unsigned long lastWifiDisconnectLog = 0;
unsigned long lastMqttFailLog = 0;
int wifiDisconnectReason = 0;


void setupWiFiEvents() {
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
      case WIFI_EVENT_STA_DISCONNECTED:
        wifiDisconnectReason = info.wifi_sta_disconnected.reason;
        // Only log once per minute to avoid spam
        if (millis() - lastWifiDisconnectLog > 60000) {
          Serial.println("[WIFI] Disconnected - Reason: " + String(wifiDisconnectReason));
          lastWifiDisconnectLog = millis();
        }
        break;
      case WIFI_EVENT_STA_CONNECTED:
        Serial.println("[WIFI] Connected to AP");
        break;
      case IP_EVENT_STA_GOT_IP:
        Serial.println("[WIFI] Got IP: " + WiFi.localIP().toString());
        break;
      case IP_EVENT_STA_LOST_IP:
        Serial.println("[WIFI] Lost IP address");
        break;
    }
  });
}

// ============================================================================
// LED CONTROL
// ============================================================================

void updateLeds() {
  for (int i = 0; i < NUM_LEDS; i++) {
    ledcWrite(i, (i == activeLed) ? 255 : 0);
  }
  publishMQTTStatus(true);
}

void turnAllLedsOff() {
  activeLed = -1;
  for (int i = 0; i < NUM_LEDS; i++) {
    ledcWrite(i, 0);
  }
  publishMQTTStatus(true);
}

void turnAllLedsOn() {
  for (int i = 0; i < NUM_LEDS; i++) {
    ledcWrite(i, 255);
  }
}

// Boot status indicators
void blinkBlue() {
  ledcWrite(2, 255);
  delay(500);
  ledcWrite(2, 0);
  delay(500);
}

void blinkRedError() {
  for (int i = 0; i < 5; i++) {
    ledcWrite(0, 255);
    delay(200);
    ledcWrite(0, 0);
    delay(200);
  }
}

void flashWhite() {
  ledcWrite(4, 255);
  delay(200);
  ledcWrite(4, 0);
}

// Acknowledge sequence for long-press white (auto mode resume)
void startAcknowledgeSequence() {
  turnAllLedsOff();
  gapActive = true;
  gapStartTime = millis();
}

// ============================================================================
// MQTT FUNCTIONS
// ============================================================================

void publishMQTTStatus(bool force) {
  if (!mqttClient.connected()) return;
  
  unsigned long now = millis();
  if (!force && (now - lastMqttPublish < MQTT_PUBLISH_INTERVAL_MS)) return;
  
  lastMqttPublish = now;
  
  // Status
  mqttClient.publish(MQTT_STATUS_TOPIC.c_str(), currentStatus.c_str(), false);
  
  // Mode
  String mode;
  switch (currentMode) {
    case NORMAL: mode = "NORMAL"; break;
    case DISCO: mode = "DISCO"; break;
    case DOUBLE_DISCO: mode = "DOUBLE_DISCO"; break;
    case TIMETRAVEL: mode = "TIMETRAVEL"; break;
    case COLOR_CIRCLE: mode = "COLOR_CIRCLE"; break;
    case REMOTE: mode = "REMOTE"; break;
    case OFF_HOURS: mode = "OFF_HOURS"; break;
  }
  mqttClient.publish(MQTT_MODE_TOPIC.c_str(), mode.c_str(), false);
  
  // Active LED color
  String activeLedTopic = MQTT_BASE_TOPIC + "/active_led";
  String activeLedColor = "NONE";
  if (currentMode == NORMAL || currentMode == REMOTE) {
    if (activeLed >= 0 && activeLed < NUM_LEDS) {
      activeLedColor = LED_COLOR_NAMES[activeLed];
    }
  } else if (currentMode == OFF_HOURS) {
    activeLedColor = "OFF";
  } else {
    activeLedColor = "EFFECT";
  }
  mqttClient.publish(activeLedTopic.c_str(), activeLedColor.c_str(), false);
  
  // IP and version (only on force/boot)
  if (force) {
    mqttClient.publish(MQTT_IP_TOPIC.c_str(), WiFi.localIP().toString().c_str(), true);
    String versionTopic = MQTT_BASE_TOPIC + "/version";
    mqttClient.publish(versionTopic.c_str(), FIRMWARE_VERSION, true);
  }
}

void publishMeetingData() {
  if (!mqttClient.connected()) return;
  
  String baseTopic = MQTT_BASE_TOPIC + "/meeting/";
  bool isCurrent = (currentStatus == "busy" && !currentMeetingTitle.isEmpty());
  
  if (isCurrent) {
    mqttClient.publish((baseTopic + "title").c_str(), currentMeetingTitle.c_str());
    mqttClient.publish((baseTopic + "current").c_str(), "true");
    mqttClient.publish((baseTopic + "date").c_str(), "Now");
    mqttClient.publish((baseTopic + "start").c_str(), "Now");
    mqttClient.publish((baseTopic + "end").c_str(), "Soon");
  } else if (!nextMeetingTitle.isEmpty() && !nextMeetingTimeStr.isEmpty()) {
    mqttClient.publish((baseTopic + "title").c_str(), nextMeetingTitle.c_str());
    mqttClient.publish((baseTopic + "current").c_str(), "false");
    mqttClient.publish((baseTopic + "start").c_str(), nextMeetingTimeStr.c_str());
    
    // Calculate date string
    time_t now = time(nullptr);
    struct tm now_tm, meeting_tm;
    localtime_r(&now, &now_tm);
    localtime_r(&nextMeetingStartTime, &meeting_tm);
    
    String dateStr;
    if (now_tm.tm_yday == meeting_tm.tm_yday) {
      dateStr = "Today";
    } else if (now_tm.tm_yday + 1 == meeting_tm.tm_yday) {
      dateStr = "Tomorrow";
    } else {
      char date_buf[20];
      strftime(date_buf, sizeof(date_buf), "%a %m/%d", &meeting_tm);
      dateStr = String(date_buf);
    }
    mqttClient.publish((baseTopic + "date").c_str(), dateStr.c_str());
  } else {
    // No meetings
    mqttClient.publish((baseTopic + "title").c_str(), "");
    mqttClient.publish((baseTopic + "current").c_str(), "false");
    mqttClient.publish((baseTopic + "date").c_str(), "");
    mqttClient.publish((baseTopic + "start").c_str(), "");
  }
}

void connectMQTT() {
  if (mqttClient.connected()) return;
  
  unsigned long now = millis();
  if (now - lastMqttReconnect < 5000) return;
  lastMqttReconnect = now;
  
  logMessage("Connecting to MQTT...");
  
  String clientId = "BusyLight-" + String(DEVICE_ID);
  String lwtTopic = MQTT_BASE_TOPIC + "/lwt";
  
  if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD,
                         lwtTopic.c_str(), 0, true, "offline")) {
    logMessage("MQTT Connected!");

    lastMqttSuccess = millis();

    mqttClient.publish(lwtTopic.c_str(), "online", true);
    mqttClient.subscribe(MQTT_COMMAND_TOPIC.c_str());
    mqttClient.subscribe(MQTT_ALL_COMMAND_TOPIC.c_str());
    publishMQTTStatus(true);
    publishBlackoutSummary();

    // Publish friendly name if set
    if (friendlyName.length() > 0) {
      String nameTopic = MQTT_BASE_TOPIC + "/friendly_name";
      mqttClient.publish(nameTopic.c_str(), friendlyName.c_str(), true);
    }
  } else {
    if (millis() - lastMqttFailLog > 60000) {
      Serial.println("MQTT connection failed: " + String(mqttClient.state()));
      lastMqttFailLog = millis();
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  
  // Ignore JSON status messages from hub
  if (message.startsWith("{")) return;
  
  logMessage("MQTT Command: " + message);
  
  if (message == "DISCO") {
    autoMode = false;
    switchToSpecialMode(3);
  } 
  else if (message == "DOUBLE_DISCO") {
    autoMode = false;
    switchToSpecialMode(0);
  }
  else if (message == "TIMETRAVEL") {
    autoMode = false;
    switchToSpecialMode(1);
  }
  else if (message == "COLOR_CIRCLE") {
    autoMode = false;
    switchToSpecialMode(2);
  }
  else if (message == "AUTO") {
    logMessage("MQTT: Resuming auto mode");
    autoMode = true;
    warningPulseActive = false;
    currentMode = NORMAL;
    updateLightFromCalendar(true);
  }
  else if (message == "RED") {
    autoMode = false;
    currentMode = NORMAL;
    activeLed = 0;
    updateLeds();
  }
  else if (message == "GREEN") {
    autoMode = false;
    currentMode = NORMAL;
    activeLed = 1;
    updateLeds();
  }
  else if (message == "BLUE") {
    autoMode = false;
    currentMode = NORMAL;
    activeLed = 2;
    updateLeds();
  }
  else if (message == "YELLOW") {
    autoMode = false;
    currentMode = NORMAL;
    activeLed = 3;
    updateLeds();
  }
  else if (message == "WHITE") {
    autoMode = false;
    currentMode = NORMAL;
    activeLed = 4;
    updateLeds();
  }
  else if (message == "OFF") {
    autoMode = false;
    currentMode = NORMAL;
    turnAllLedsOff();
  } 
  else if (message.startsWith("SETNAME:")) {
    String newName = message.substring(8);
    newName.trim();
    if (newName.length() > 0 && newName.length() <= 32) {
      saveFriendlyName(newName);
      logMessage("Name updated to: " + newName);
    } else {
      logMessage("Invalid name (must be 1-32 chars)");
    }
  }
  else if (message == "PUBLISH_STATUS") {
    logMessage("Status request received");
    publishMQTTStatus(true);
    publishMeetingData();
  }
}

// ============================================================================
// TIME SYNCHRONIZATION
// ============================================================================

void syncTime() {
  if (WiFi.status() != WL_CONNECTED || timeSynced) return;

  logMessage("Syncing time...");
  blinkBlue();
  
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);

  struct tm timeinfo;
  int retries = 0;
  while (!getLocalTime(&timeinfo) && retries < 10) {
    blinkBlue();
    retries++;
  }

  if (getLocalTime(&timeinfo)) {
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%m/%d %l:%M:%S %p", &timeinfo);
    logMessage("Time synced: " + String(time_str));
    timeSynced = true;
    flashWhite();
  } else {
    logMessage("Time sync failed");
    blinkRedError();
  }
}

// ============================================================================
// CALENDAR API
// ============================================================================

void updateLightFromCalendar(bool verboseLog) {
  unsigned long currentTime = millis();
  
  if (currentTime - lastAPICallTime < MIN_API_CALL_INTERVAL_MS) {
    logMessage("Skipping - rate limit");
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    logMessage("No WiFi");
    calendarCheckFailures++;
    return;
  }

  if (verboseLog) logMessage("Checking calendar...");
  lastAPICallTime = currentTime;

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(20000);
  http.setReuse(false);
  
  yield();
  delay(50);

  httpSecure.setInsecure();
  if (!http.begin(httpSecure, CALENDAR_API_URL)) {
    logMessage("HTTP begin failed");
    return;
  }

  yield();
  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {
    logMessage("HTTP error: " + String(httpCode));
    if (httpCode == 429) {
      logMessage("Rate limited");
      lastAPICallTime = currentTime + 50000;
    }
    calendarCheckFailures++;
    http.end();
    return;
  }

  int payloadLength = http.getSize();
  if (payloadLength > 1800) {
    logMessage("Payload too large");
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, payload);

  if (error) {
    logMessage("JSON error: " + String(error.c_str()));
    calendarCheckFailures++;
    return;
  }

  calendarCheckFailures = 0;
  lastCalendarSuccess = millis();

  // Parse response
  currentStatus = doc["status"] | "unknown";
  isRemoteDay = doc["remote_day"] | false;

  const char* timeStrNext = doc["next_meeting"]["time"];
  const char* timeStrEnd = doc["current_meeting"]["end_time"];

  currentMeetingTitle = doc["current_meeting"]["title"] | "";
  nextMeetingTitle = doc["next_meeting"]["title"] | "";

  if (doc["business_hours"]["start"]) apiBusinessHoursStart = doc["business_hours"]["start"];
  if (doc["business_hours"]["end"]) apiBusinessHoursEnd = doc["business_hours"]["end"];

  // Set up alarm
  const char* timeStringForAlarm = nullptr;
  nextMeetingTimeStr = "";
  alarmIsForEnd = false;
  static time_t lastLoggedAlarm = 0;

  if (!isRemoteDay && autoMode) {
    if (currentStatus == "busy" && timeStrEnd) {
      timeStringForAlarm = timeStrEnd;
      alarmIsForEnd = true;
      if (verboseLog) logMessage("Alarm: meeting END");
    } else if (currentStatus == "free" && timeStrNext) {
      timeStringForAlarm = timeStrNext;

      struct tm temp_tm;
      memset(&temp_tm, 0, sizeof(struct tm));
      if (strptime(timeStrNext, "%Y-%m-%d %H:%M:%S", &temp_tm) != nullptr) {
        char temp_time_str[10];
        strftime(temp_time_str, sizeof(temp_time_str), "%l:%M %p", &temp_tm);
        nextMeetingTimeStr = String(temp_time_str);
      }
      if (verboseLog) logMessage("Alarm: meeting START");
    }
  }

  // Parse alarm time
  nextMeetingStartTime = 0;
  if (timeStringForAlarm) {
    struct tm timeinfo;
    memset(&timeinfo, 0, sizeof(struct tm));
    if (strptime(timeStringForAlarm, "%Y-%m-%d %H:%M:%S", &timeinfo) != nullptr) {
      time_t t = mktime(&timeinfo) - DAYLIGHT_OFFSET_SEC;
      nextMeetingStartTime = t;

      if (verboseLog || nextMeetingStartTime != lastLoggedAlarm) {
        struct tm local_tm;
        localtime_r(&nextMeetingStartTime, &local_tm);
        char time_str[20];
        strftime(time_str, sizeof(time_str), "%l:%M %p", &local_tm);
        logMessage("Alarm set: " + String(time_str));
        lastLoggedAlarm = nextMeetingStartTime;
      }
    }
  }

  // Update LED based on status
  if (autoMode) {
    warningPulseActive = false;
    turnAllLedsOff();

    if (currentStatus == "busy") {
      currentMode = NORMAL;
      activeLed = 0;
      updateLeds();
    } else if (currentStatus == "free") {
      currentMode = NORMAL;
      activeLed = 1;
      updateLeds();
    } else if (currentStatus == "off_hours") {
      currentMode = OFF_HOURS;
    } else if (currentStatus == "remote" || isRemoteDay) {
      currentMode = REMOTE;
      activeLed = 2;
      updateLeds();
    }

    // Log status
    if (verboseLog) {
      struct tm now_tm;
      time_t now = time(nullptr);
      localtime_r(&now, &now_tm);
      char now_time_str[10];
      strftime(now_time_str, sizeof(now_time_str), "%l:%M %p", &now_tm);

      String log_line = "Status: " + currentStatus + " (" + String(now_time_str) + ")";
      if (isRemoteDay) {
        log_line += " | REMOTE";
      } else if (currentStatus == "busy") {
        log_line += " | " + currentMeetingTitle;
      } else if (nextMeetingStartTime != 0) {
        log_line += " | Next: " + nextMeetingTimeStr;
      }
      logMessage(log_line);
    }
    
    publishMeetingData();
  }

  // Handle failures
  if (calendarCheckFailures >= MAX_CALENDAR_FAILURES) {
    logMessage("Too many failures - reconnecting WiFi");
    WiFi.disconnect();
    delay(1000);
    WiFi.reconnect();
    calendarCheckFailures = 0;
  }
}

// ============================================================================
// VISUAL EFFECTS
// ============================================================================

void warningPulseEffect() {
  static unsigned long lastUpdate = 0;
  unsigned long now = millis();
  if (now - lastUpdate < PULSE_UPDATE_INTERVAL) return;
  lastUpdate = now;

  if (pulseDirection) {
    pulseBrightness += PULSE_SPEED;
    if (pulseBrightness >= 255) {
      pulseBrightness = 255;
      pulseDirection = false;
    }
  } else {
    pulseBrightness -= PULSE_SPEED;
    if (pulseBrightness <= 0) {
      pulseBrightness = 0;
      pulseDirection = true;
    }
  }
  ledcWrite(0, pulseBrightness);  // Red LED
}

void initializeTimetravel() {
  for (int i = 0; i < NUM_LEDS; i++) {
    fadeStates[i].duty = random(0, 256);
    fadeStates[i].fadingIn = random(0, 2) == 0;
  }
}

void discoEffect() {
  unsigned long now = millis();
  if (now - lastFadeStepTime < DISCO_UPDATE_DELAY) return;
  lastFadeStepTime = now;

  if (discoLed1 == -1) discoLed1 = random(0, NUM_LEDS);

  if (discoState == 0) {
    discoDuty += DISCO_FADE_AMOUNT;
    if (discoDuty >= 255) {
      discoDuty = 255;
      discoState = 1;
    }
  } else {
    discoDuty -= DISCO_FADE_AMOUNT;
    if (discoDuty <= 0) {
      discoDuty = 0;
      discoState = 0;
      discoLed1 = random(0, NUM_LEDS);
    }
  }

  for (int i = 0; i < NUM_LEDS; i++) {
    ledcWrite(i, (i == discoLed1) ? discoDuty : 0);
  }
}

void doubleDiscoEffect() {
  unsigned long now = millis();
  if (now - lastFadeStepTime < DISCO_UPDATE_DELAY) return;
  lastFadeStepTime = now;

  if (discoLed1 == -1) {
    discoLed1 = random(0, NUM_LEDS);
    do { discoLed2 = random(0, NUM_LEDS); } while (discoLed2 == discoLed1);
  }

  if (discoState == 0) {
    discoDuty += DISCO_FADE_AMOUNT;
    if (discoDuty >= 255) {
      discoDuty = 255;
      discoState = 1;
    }
  } else {
    discoDuty -= DISCO_FADE_AMOUNT;
    if (discoDuty <= 0) {
      discoDuty = 0;
      discoState = 0;
      discoLed1 = random(0, NUM_LEDS);
      do { discoLed2 = random(0, NUM_LEDS); } while (discoLed2 == discoLed1);
    }
  }

  for (int i = 0; i < NUM_LEDS; i++) {
    ledcWrite(i, (i == discoLed1 || i == discoLed2) ? discoDuty : 0);
  }
}

void timetravelEffect() {
  unsigned long now = millis();
  if (now - lastTimetravelStep < TIMETRAVEL_FADE_DELAY) return;
  lastTimetravelStep = now;

  for (int i = 0; i < NUM_LEDS; i++) {
    if (fadeStates[i].fadingIn) {
      fadeStates[i].duty += 3;
      if (fadeStates[i].duty >= 255) {
        fadeStates[i].duty = 255;
        fadeStates[i].fadingIn = false;
      }
    } else {
      fadeStates[i].duty -= 2;
      if (fadeStates[i].duty <= 0) {
        fadeStates[i].duty = 0;
        fadeStates[i].fadingIn = true;
      }
    }
    ledcWrite(i, fadeStates[i].duty);
  }
}

void colorCircleEffect() {
  unsigned long now = millis();
  if (now - lastCircleStepTime < CIRCLE_UPDATE_DELAY) return;
  lastCircleStepTime = now;

  int prevLed = (circleCurrentLedIndex == 0) ? NUM_LEDS - 1 : circleCurrentLedIndex - 1;

  if (circleDuty >= 255) {
    ledcWrite(prevLed, 0);
    ledcWrite(circleCurrentLedIndex, 255);
    circleDuty = 0;
    circleCurrentLedIndex = (circleCurrentLedIndex + 1) % NUM_LEDS;
    return;
  }

  circleDuty += CIRCLE_FADE_AMOUNT;
  ledcWrite(circleCurrentLedIndex, circleDuty);
  ledcWrite(prevLed, 255 - circleDuty);

  for (int i = 0; i < NUM_LEDS; i++) {
    if (i != circleCurrentLedIndex && i != prevLed) {
      ledcWrite(i, 0);
    }
  }
}

// ============================================================================
// BUTTON AND MODE HANDLING
// ============================================================================

void switchToSpecialMode(int buttonIndex) {
  activeLed = -1;
  turnAllLedsOff();
  warningPulseActive = false;
  currentMode = BUTTON_MODE_MAP[buttonIndex];
  logMessage("Mode: " + String(SPECIAL_MODE_NAMES[buttonIndex]));

  switch (currentMode) {
    case DISCO:
    case DOUBLE_DISCO:
      discoState = 0;
      discoDuty = 0;
      discoLed1 = -1;
      discoLed2 = -1;
      break;
    case TIMETRAVEL:
      initializeTimetravel();
      break;
    case COLOR_CIRCLE:
      circleCurrentLedIndex = 0;
      circleDuty = 0;
      break;
    default:
      break;
  }
  publishMQTTStatus(true);
}

void handleNormalMode() {
  for (int i = 0; i < NUM_LEDS; i++) {
    ButtonState s = buttons[i].getState();
    
    if (s == HELD) {
      if (i == 4) {  // White button long-press = resume auto mode
        logMessage("Resuming auto mode");
        autoMode = true;
        warningPulseActive = false;

        if (isRemoteDay) {
          currentMode = REMOTE;
          activeLed = 2;
          updateLeds();
        } else {
          startAcknowledgeSequence();
        }
        return;
      }
      // Other buttons long-press = special mode
      logMessage("Long press: " + String(SPECIAL_MODE_NAMES[i]));
      autoMode = false;
      switchToSpecialMode(i);
      return;
    }
    
    if (s == PRESSED) {
      logMessage("Manual: " + String(LED_COLOR_NAMES[i]));
      autoMode = false;
      warningPulseActive = false;
      currentMode = NORMAL;
      activeLed = (activeLed == i) ? -1 : i;
      updateLeds();
      return;
    }
  }
}

void handleModeExit() {
  for (int i = 0; i < NUM_LEDS; i++) {
    if (buttons[i].getState() == PRESSED) {
      logMessage("Exiting special mode");
      currentMode = NORMAL;
      if (autoMode) {
        updateLightFromCalendar();
      } else {
        turnAllLedsOff();
      }
      return;
    }
  }
}

// ============================================================================
// TIME-BASED TRIGGERS
// ============================================================================

void checkMorningPrep() {
  if (morningPrepDone) return;

  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  float currentHour = timeinfo.tm_hour + (timeinfo.tm_min / 60.0);

  if (currentHour >= MORNING_PREP_HOUR && currentHour < BUSINESS_START_HOUR) {
    logMessage("=== MORNING PREP ===");
    updateLightFromCalendar();
    morningPrepDone = true;
  }
}

void checkBusinessHoursStart() {
  if (businessHoursStartDone) return;

  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  float currentHour = timeinfo.tm_hour + (timeinfo.tm_min / 60.0);

  if (currentHour >= BUSINESS_START_HOUR && currentHour < (BUSINESS_START_HOUR + 0.1)) {
    logMessage("=== BUSINESS HOURS START ===");
    autoMode = true;
    updateLightFromCalendar();
    businessHoursStartDone = true;
  }
}

void checkBusinessHoursEnd() {
  if (businessHoursEndDone) return;

  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  float currentHour = timeinfo.tm_hour + (timeinfo.tm_min / 60.0);

  if (currentHour >= BUSINESS_END_HOUR) {
    logMessage("=== BUSINESS HOURS END ===");
    autoMode = true;
    turnAllLedsOff();
    currentMode = OFF_HOURS;
    nextMeetingStartTime = 0;
    warningPulseActive = false;
    businessHoursEndDone = true;
  }
}

void resetDailyFlags() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  static bool midnightResetDone = false;
  
  if (timeinfo.tm_hour == 0 && timeinfo.tm_min == 0) {
    if (!midnightResetDone) {
      morningPrepDone = false;
      businessHoursStartDone = false;
      businessHoursEndDone = false;
      isRemoteDay = false;
      midnightResetDone = true;
      logMessage("Daily flags reset");
    }
  } else {
    midnightResetDone = false;
  }
}

void checkScheduledReboot() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  // Reboot at 6:55 AM daily (once only per day)
  if (timeinfo.tm_hour == 6 && timeinfo.tm_min == 55) {
    // Check if we already rebooted today using NVS
    int today = (timeinfo.tm_year + 1900) * 10000 + (timeinfo.tm_mon + 1) * 100 + timeinfo.tm_mday;
    prefs.begin("busylight", true);
    int lastRebootDay = prefs.getInt("rebootDay", 0);
    prefs.end();

    if (lastRebootDay != today) {
      // Save today's date before rebooting
      prefs.begin("busylight", false);
      prefs.putInt("rebootDay", today);
      prefs.end();

      logMessage("=== SCHEDULED REBOOT ===");
      delay(100);
      ESP.restart();
    }
  }
}

void handleBootCatchup() {
  if (!timeSynced) return;

  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  float currentHour = timeinfo.tm_hour + (timeinfo.tm_min / 60.0);

  logMessage("Boot at " + String(timeinfo.tm_hour) + ":" +
             String(timeinfo.tm_min < 10 ? "0" : "") + String(timeinfo.tm_min));

  if (currentHour < MORNING_PREP_HOUR) {
    logMessage("Before work - lights OFF");
    turnAllLedsOff();
  } else if (currentHour < BUSINESS_START_HOUR) {
    logMessage("Morning prep on boot");
    updateLightFromCalendar();
    morningPrepDone = true;
    turnAllLedsOff();
  } else if (currentHour < BUSINESS_END_HOUR) {
    logMessage("Business hours - full catchup");
    updateLightFromCalendar();
    morningPrepDone = true;
    businessHoursStartDone = true;
    autoMode = true;
  } else {
    logMessage("After hours - lights OFF");
    turnAllLedsOff();
    currentMode = OFF_HOURS;
    morningPrepDone = true;
    businessHoursStartDone = true;
    businessHoursEndDone = true;
  }
}

// ============================================================================
// Save Blackout Data System
// ============================================================================

void saveBlackoutData() {
  prefs.begin("blackout", false);
  prefs.putULong("lastMqtt", lastMqttSuccess);
  prefs.putULong("lastCal", lastCalendarSuccess);
  prefs.putULong("wifiLost", wifiLostTime);
  prefs.putInt("reconnects", reconnectAttempts);
  prefs.putBool("apTriggered", apTriggered);
  prefs.putULong("savedAt", millis());
  prefs.end();
}

void publishBlackoutSummary() {
  prefs.begin("blackout", true);  // Read-only
  unsigned long savedLastMqtt = prefs.getULong("lastMqtt", 0);
  unsigned long savedLastCal = prefs.getULong("lastCal", 0);
  unsigned long savedWifiLost = prefs.getULong("wifiLost", 0);
  int savedReconnects = prefs.getInt("reconnects", 0);
  bool savedApTriggered = prefs.getBool("apTriggered", false);
  unsigned long savedAt = prefs.getULong("savedAt", 0);
  prefs.end();
  
  // Only publish if there was a blackout (savedAt > 0 means we saved data before losing power/rebooting)
  if (savedAt == 0) return;
  
  // Calculate offline duration
  unsigned long offlineDuration = 0;
  if (savedWifiLost > 0 && savedAt > savedWifiLost) {
    offlineDuration = (savedAt - savedWifiLost) / 1000;  // Convert to seconds
  }
  
  logMessage("=== RECONNECT SUMMARY ===");
  if (offlineDuration > 0) {
    logMessage("Offline: " + String(offlineDuration / 3600) + "h " + String((offlineDuration % 3600) / 60) + "m");
  }
  logMessage("Reconnect attempts: " + String(savedReconnects));
  if (savedApTriggered) {
    logMessage("AP was triggered: Yes");
  }
  logMessage("=========================");
  
  // Clear the blackout data after publishing
  prefs.begin("blackout", false);
  prefs.clear();
  prefs.end();
}

// ============================================================================
// WIFI SETUP
// ============================================================================

void setupWiFi() {
    // Random delay to stagger multiple ESP32 boots (0-5 seconds)
    randomSeed(ESP.getEfuseMac());
    int delayMs = random(0, 5000);
    logMessage("Startup delay: " + String(delayMs) + "ms");
    delay(delayMs);

    WiFi.setHostname(DEVICE_HOSTNAME.c_str());
    WiFi.mode(WIFI_STA);

    // Try predefined WiFi networks before falling back to WiFiManager
    const char* wifiNetworks[][2] = {
        { SECRET_WIFI_SSID,  SECRET_WIFI_PASSWORD  },
        { SECRET_WIFI_SSID2, SECRET_WIFI_PASSWORD2 },
    };
    const int numNetworks = sizeof(wifiNetworks) / sizeof(wifiNetworks[0]);

    for (int n = 0; n < numNetworks; n++) {
        logMessage("Trying WiFi " + String(n + 1) + ": " + String(wifiNetworks[n][0]));
        WiFi.begin(wifiNetworks[n][0], wifiNetworks[n][1]);

        // Slow green blink while trying to connect
        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 20) {  // ~10 seconds
            ledcWrite(1, 255);  // Green on
            delay(250);
            ledcWrite(1, 0);    // Green off
            delay(250);
            attempts++;
        }

        if (WiFi.status() == WL_CONNECTED) {
            logMessage("Connected to WiFi: " + String(wifiNetworks[n][0]));
            logMessage("IP address: " + WiFi.localIP().toString());
            reconnectAttempts = 0;
            apTriggered = false;
            return;
        }

        WiFi.disconnect();
        delay(100);
    }

    // All predefined networks failed, fall back to WiFiManager
    logMessage("All predefined WiFi networks failed, starting WiFiManager...");

    WiFiManager wm;

    // Set timeout for config portal (2 minutes)
    wm.setConfigPortalTimeout(120);

    // Callback when entering AP mode
    wm.setAPCallback([](WiFiManager* myWiFiManager) {
        logMessage("=== WIFI CONFIG MODE ===");
        logMessage("Connect to: BusyLightAP");
        logMessage("Configure WiFi at 192.168.4.1");

        apTriggered = true;
        saveBlackoutData();

        // Distinctive: 3 bursts of 3 fast yellow blinks
        for (int burst = 0; burst < 3; burst++) {
            for (int i = 0; i < 3; i++) {
                ledcWrite(3, 255);  // Yellow on
                delay(100);
                ledcWrite(3, 0);    // Yellow off
                delay(100);
            }
            delay(500);  // Pause between bursts
        }
    });

    // Track reconnect attempts
    reconnectAttempts++;
    saveBlackoutData();

    // Slow yellow blink while trying to connect
    ledcWrite(3, 255);
    delay(250);
    ledcWrite(3, 0);
    delay(750);

    String apName;
    if (friendlyName.length() > 0) {
      apName = friendlyName + " Setup";
    } else {
      apName = "BusyLight-" + String(DEVICE_ID) + "-Setup";
    }
    if (!wm.autoConnect(apName.c_str())) {
        logMessage("Failed to connect - timeout. Restarting...");
        saveBlackoutData();
        // Red error blink
        for (int i = 0; i < 5; i++) {
            ledcWrite(0, 255);
            delay(100);
            ledcWrite(0, 0);
            delay(100);
        }
        delay(3000);
        ESP.restart();
    }

    // Connected! Reset reconnect counter
    reconnectAttempts = 0;
    apTriggered = false;

    logMessage("WiFi connected to: " + WiFi.SSID());
    logMessage("IP address: " + WiFi.localIP().toString());
}

// ============================================================================
// DEVICE NAMING
// ============================================================================

void loadFriendlyName() {
  prefs.begin("busylight", true);  // Read-only
  friendlyName = prefs.getString("fname", "");
  prefs.end();
  
  if (friendlyName.length() > 0) {
    logMessage("Friendly name: " + friendlyName);
  }
}

void saveFriendlyName(String name) {
  prefs.begin("busylight", false);  // Read-write
  prefs.putString("fname", name);
  prefs.end();
  friendlyName = name;
  logMessage("Saved friendly name: " + name);
  
  // Publish to MQTT
  String topic = MQTT_BASE_TOPIC + "/friendly_name";
  mqttClient.publish(topic.c_str(), name.c_str(), true);
}

// ============================================================================
// SETUP AND LOOP
// ============================================================================

void setup() {
  Serial.begin(115200);
  mqttSecure.setInsecure();
  httpSecure.setInsecure();
  Serial.println("===================================");
  Serial.println("  Busy Light v" + String(FIRMWARE_VERSION));
  Serial.println("===================================");

  //Device Naming
  loadFriendlyName();
  // Initialize LEDs and buttons
  for (int i = 0; i < NUM_LEDS; i++) {
    buttons[i].setup();
    ledcSetup(i, PWM_FREQUENCY, PWM_RESOLUTION);
    ledcAttachPin(LED_PINS[i], i);
  }
  turnAllLedsOff();

  // Setup WiFi event monitoring
  setupWiFiEvents();

  // Connect to WiFi
  setupWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    // Setup MQTT
    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
    connectMQTT();
    publishBlackoutSummary();
    
    // Sync time
    delay(2000);
    syncTime();
    delay(2000);
    syncTime();

    // Catch up based on current time
    handleBootCatchup();
    
    // Ready indicator
    flashWhite();
  }

  logMessage("Ready");
}

void loop() {
  // Track WiFi loss
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiLostTime == 0) {
      wifiLostTime = millis();
      saveBlackoutData();
    }
  } else {
    wifiLostTime = 0;
  }
  
  // MQTT handling
  if (!mqttClient.connected()) {
    connectMQTT();
  }
  mqttClient.loop();
  publishMQTTStatus(false);

  // Acknowledge flash sequence (non-blocking)
  if (gapActive && millis() - gapStartTime >= GAP_DURATION_MS) {
    gapActive = false;
    flashActive = true;
    flashStartTime = millis();
    ledcWrite(4, 255);
  }

  if (flashActive && millis() - flashStartTime >= FLASH_DURATION_MS) {
    flashActive = false;
    ledcWrite(4, 0);
    updateLightFromCalendar(true);
    lastCalendarCheck = millis();
  }

  // Daily triggers
  resetDailyFlags();
  checkScheduledReboot();
  checkMorningPrep();
  checkBusinessHoursStart();
  checkBusinessHoursEnd();

  unsigned long currentTime = millis();
  time_t currentUnixTime = time(nullptr);

  // Retry time sync if needed
  if (!timeSynced && WiFi.status() == WL_CONNECTED) {
    syncTime();
  }

  if (autoMode) {
    // Scheduled calendar check
    if (currentTime - lastCalendarCheck >= CALENDAR_CHECK_INTERVAL_MS) {
      if (!isRemoteDay) {
        updateLightFromCalendar(false);
      }
      lastCalendarCheck = currentTime;
    }

    handleNormalMode();

    // Meeting logic (only if not remote day)
    if (nextMeetingStartTime != 0 && !isRemoteDay) {
      long timeUntilTarget = nextMeetingStartTime - currentUnixTime;

      // Meeting start/end trigger
      if (timeUntilTarget <= 0) {
        if (!alarmIsForEnd && !meetingInProgress) {
          meetingInProgress = true;
          warningPulseActive = false;
          logMessage("Meeting started");
          updateLightFromCalendar();
          turnAllLedsOff();
          activeLed = 0;
          ledcWrite(activeLed, 255);
        } else if (alarmIsForEnd && meetingInProgress) {
          logMessage("Meeting ended");
          updateLightFromCalendar();
          meetingInProgress = false;
          preemptiveCheckDone = false;
          warningPulseActive = false;
          nextMeetingStartTime = 0;
          alarmIsForEnd = false;
        }
      }

      // Preemptive check (5 min before)
      if (!alarmIsForEnd && timeUntilTarget <= 300 && timeUntilTarget > 60 && !preemptiveCheckDone) {
        logMessage("Preemptive check");
        updateLightFromCalendar();
        lastCalendarCheck = currentTime;
        preemptiveCheckDone = true;
      }

      // Warning pulse (1 min before)
      if (!alarmIsForEnd && timeUntilTarget <= 60 && timeUntilTarget > 0) {
        if (!warningPulseActive) {
          logMessage("Meeting in " + String(timeUntilTarget) + "s");
          warningPulseActive = true;
          turnAllLedsOff();
          activeLed = 0;
          pulseBrightness = 0;
          pulseDirection = true;
        }
        warningPulseEffect();
      }

      // Countdown logging
      if (timeUntilTarget > 0 && timeUntilTarget <= 15 && (timeUntilTarget % 5 == 0)) {
        if (currentUnixTime > lastLoggedSecond) {
          logMessage((alarmIsForEnd ? "Ends in " : "Starts in ") + String(timeUntilTarget) + "s");
          lastLoggedSecond = currentUnixTime;
        }
      }
    } else {
      // Reset flags if no meeting
      preemptiveCheckDone = false;
      meetingInProgress = false;
      warningPulseActive = false;
      alarmIsForEnd = false;

      // Safety check: if showing busy but no timer, re-check
      if (autoMode && currentMode == NORMAL && activeLed == 0 &&
          (currentTime - lastCalendarCheck >= 30000)) {
        logMessage("Safety re-check");
        updateLightFromCalendar();
        lastCalendarCheck = currentTime;
      }
    }
  } else {
    // Manual mode
    if (currentMode != NORMAL) {
      handleModeExit();
      switch (currentMode) {
        case DISCO: discoEffect(); break;
        case DOUBLE_DISCO: doubleDiscoEffect(); break;
        case TIMETRAVEL: timetravelEffect(); break;
        case COLOR_CIRCLE: colorCircleEffect(); break;
        default: break;
      }
    } else {
      handleNormalMode();
    }
  }

  yield();
  delay(1);
}