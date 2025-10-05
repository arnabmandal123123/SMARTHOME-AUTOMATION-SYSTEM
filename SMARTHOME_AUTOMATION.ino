/*********************************************************************************
 * Professional Home Automation Project: 4-Light ESP8266 Controller
 * Description:
 * This code runs on an ESP8266 to control four light bulbs via active-low 
 * relays. It connects to a public MQTT broker to receive commands from a web UI 
 * and publish status updates. It includes a timer to control all lights.
 *
 * Author: Arnab Mandal
 * Version: 3.0 (Multi-light with robust timer)
 *********************************************************************************/

// --- LIBRARIES ---
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

// --- WIFI CREDENTIALS ---
const char* ssid = "PikU_2.4G";
const char* password = "@RAunak4321";

// --- MQTT BROKER CONFIGURATION ---
const char* mqtt_server = "test.mosquitto.org";
const int mqtt_port = 1883;
const char* clientId = "ESP8266_MultiLight_Client_V2"; // Unique client ID

// --- MQTT TOPICS ---
const char* light_set_topic_wildcard = "homeautomation/project/light/+/set";
const char* light_get_status_topic_wildcard = "homeautomation/project/light/+/getStatus";
const char* light_log_topic = "homeautomation/project/light/log";
const char* light_timer_set_topic = "homeautomation/project/light/setTimer";
const char* light_timer_clear_topic = "homeautomation/project/light/clearTimer";

// --- HARDWARE CONFIGURATION ---
const int relayPins[] = { D1, D2, D3, D4 };
const int NUM_LIGHTS = sizeof(relayPins) / sizeof(int);

// --- NTP (TIME) CLIENT SETUP ---
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000); // IST Offset

// --- GLOBAL VARIABLES ---
WiFiClient espClient;
PubSubClient client(espClient);
bool lightStates[NUM_LIGHTS] = {false}; // Holds the state (ON/OFF) for each light
char msg[128]; // Buffer for formatting messages

// Timer variables for all lights
int onTimeHour = -1, onTimeMinute = -1;
int offTimeHour = -1, offTimeMinute = -1;
bool isTimerActive = false;
int lastMinuteChecked = -1;

// --- INITIAL SETUP ---
void setup() {
  Serial.begin(115200);
  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  for (int i = 0; i < NUM_LIGHTS; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], HIGH); // Initialize to OFF
  }

  timeClient.begin();
}

// --- MAIN LOOP ---
void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();
  
  timeClient.update();
  int currentMinute = timeClient.getMinutes();
  if (currentMinute != lastMinuteChecked) {
    checkTimer();
    lastMinuteChecked = currentMinute;
  }
  delay(100);
}

// --- WIFI & MQTT FUNCTIONS ---

void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    if (client.connect(clientId)) {
      Serial.println("connected");
      // Subscribe to all topics
      client.subscribe(light_set_topic_wildcard);
      client.subscribe(light_get_status_topic_wildcard);
      client.subscribe(light_timer_set_topic);
      client.subscribe(light_timer_clear_topic);
      
      publishLog("ESP8266 connected. Syncing all light statuses.");
      for(int i = 0; i < NUM_LIGHTS; i++) {
        updateLightStatus(i + 1, lightStates[i]);
      }
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      delay(5000);
    }
  }
}

void callback(char* topic, byte* payload, unsigned int length) {
  String topicStr = String(topic);
  String messageStr;
  for (unsigned int i = 0; i < length; i++) {
    messageStr += (char)payload[i];
  }
  
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  Serial.println(messageStr);

  // --- Handle Timer Topics ---
  if (topicStr == light_timer_set_topic) {
    setTimer(messageStr);
    return;
  }
  if (topicStr == light_timer_clear_topic) {
    clearTimer();
    return;
  }

  // --- Parse light number from other topics ---
  // e.g., "homeautomation/project/light/2/set" -> extracts '2'
  String topicPart = topicStr;
  topicPart.remove(0, strlen("homeautomation/project/light/"));
  int lightNumber = topicPart.toInt();

  if (lightNumber < 1 || lightNumber > NUM_LIGHTS) return;
  int lightIndex = lightNumber - 1;

  if (topicStr.endsWith("/set")) {
    setLightState(lightIndex, messageStr == "ON", "user command");
  } else if (topicStr.endsWith("/getStatus")) {
    updateLightStatus(lightNumber, lightStates[lightIndex]);
  }
}


// --- HELPER FUNCTIONS ---

void setLightState(int lightIndex, bool state, String source) {
  digitalWrite(relayPins[lightIndex], state ? LOW : HIGH); // Active low relay
  lightStates[lightIndex] = state;
  
  snprintf(msg, sizeof(msg), "Light %d turned %s by %s.", lightIndex + 1, state ? "ON" : "OFF", source.c_str());
  publishLog(msg);
  updateLightStatus(lightIndex + 1, state);
}

void updateLightStatus(int lightNumber, bool state) {
  char topic[50];
  snprintf(topic, sizeof(topic), "homeautomation/project/light/%d/status", lightNumber);
  client.publish(topic, state ? "ON" : "OFF", true); // Retain message
}

void publishLog(const char* logMessage) {
  client.publish(light_log_topic, logMessage);
}

// --- TIMER FUNCTIONS ---

void setTimer(String payload) {
  StaticJsonDocument<128> doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    publishLog("Error: Failed to parse timer settings.");
    return;
  }
  sscanf(doc["on"], "%d:%d", &onTimeHour, &onTimeMinute);
  sscanf(doc["off"], "%d:%d", &offTimeHour, &offTimeMinute);
  isTimerActive = true;
  snprintf(msg, sizeof(msg), "Timer set for all lights: ON at %02d:%02d, OFF at %02d:%02d.", onTimeHour, onTimeMinute, offTimeHour, offTimeMinute);
  publishLog(msg);
  checkTimer(); // Immediately apply new state
}

void clearTimer() {
  isTimerActive = false;
  onTimeHour = -1;
  publishLog("Timer has been cleared for all lights.");
}

void checkTimer() {
  if (!isTimerActive) return;
  int onTimeInMinutes = onTimeHour * 60 + onTimeMinute;
  int offTimeInMinutes = offTimeHour * 60 + offTimeMinute;
  int currentTimeInMinutes = timeClient.getHours() * 60 + timeClient.getMinutes();
  bool shouldBeOn = false;

  if (onTimeInMinutes < offTimeInMinutes) { // Same-day schedule
    if (currentTimeInMinutes >= onTimeInMinutes && currentTimeInMinutes < offTimeInMinutes) {
      shouldBeOn = true;
    }
  } else if (onTimeInMinutes > offTimeInMinutes) { // Overnight schedule
    if (currentTimeInMinutes >= onTimeInMinutes || currentTimeInMinutes < offTimeInMinutes) {
      shouldBeOn = true;
    }
  }
  
  // Apply state to all lights if it differs from their current state
  for (int i = 0; i < NUM_LIGHTS; i++) {
    if (shouldBeOn && !lightStates[i]) {
      setLightState(i, true, "timer");
    } else if (!shouldBeOn && lightStates[i]) {
      setLightState(i, false, "timer");
    }
  }
}