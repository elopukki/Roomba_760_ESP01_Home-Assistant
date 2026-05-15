#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Roomba.h>

// =========================
// USER CONFIG
// =========================
const char* ssid = "SSID";
const char* password = "PAS";

const char* mqtt_server = "IPmqtt";
const int mqtt_port = PORT;
const char* mqtt_user = "USER";
const char* mqtt_pass = "PAS";
const char* mqtt_client_name = "Roomba760ESP01";

// MQTT topics
const char* topic_command = "roomba/commands";
const char* topic_state = "roomba/state";
const char* topic_availability = "roomba/availability";
const char* topic_checkin = "checkIn/roomba";

// GPIO
const uint8_t noSleepPin = 2;   // ESP-01 GPIO2 -> Roomba BRC

// =========================
// GLOBALS
// =========================
WiFiClient espClient;
PubSubClient client(espClient);
Roomba roomba(&Serial, Roomba::Baud115200);

bool bootFlag = true;
unsigned long lastStatusPublish = 0;
unsigned long lastKeepAwake = 0;
unsigned long lastReconnectAttempt = 0;
unsigned long lastCommandAt = 0;

String currentState = "idle";
int batteryPercent = -1;
int chargeState = -1;
bool charging = false;
bool dockVisible = false;
char jsonBuffer[256];
uint8_t tempBuf[10];

// =========================
// HELPERS
// =========================
void pulseBRCOnce(unsigned int lowMs) {
  digitalWrite(noSleepPin, HIGH);
  delay(50);
  digitalWrite(noSleepPin, LOW);
  delay(lowMs);
  digitalWrite(noSleepPin, HIGH);
  delay(50);
}

void awake() {
  pulseBRCOnce(200);
}

void setupRoombaBaud115200() {
  Serial.write(128);
  delay(50);
  Serial.write(129);
  Serial.write(11);
  delay(100);
}

void startOI() {
  awake();
  Serial.write(128);
  delay(50);
}

void safeMode() {
  startOI();
  Serial.write(131);
  delay(50);
}

void stopOI() {
  Serial.write(173);
  delay(50);
}

int16_t readSensor16(uint8_t packetId) {
  roomba.getSensors(packetId, tempBuf, 2);
  return (int16_t)((tempBuf[0] << 8) | tempBuf[1]);
}

uint8_t readSensor8(uint8_t packetId) {
  roomba.getSensors(packetId, tempBuf, 1);
  return tempBuf[0];
}

const char* chargeStateToText(int state) {
  switch (state) {
    case 0: return "not_charging";
    case 1: return "reconditioning";
    case 2: return "full_charging";
    case 3: return "trickle_charging";
    case 4: return "waiting";
    case 5: return "charging_fault";
    default: return "unknown";
  }
}

void publishState() {
  StaticJsonDocument<256> doc;
  doc["state"] = currentState;
  doc["battery"] = batteryPercent;
  doc["charging"] = charging;
  doc["charge_state"] = chargeStateToText(chargeState);
  doc["dock_visible"] = dockVisible;
  doc["last_command_ms"] = millis() - lastCommandAt;

  size_t n = serializeJson(doc, jsonBuffer, sizeof(jsonBuffer));
  client.publish(topic_state, (const uint8_t*)jsonBuffer, n, true);
}

void publishAvailabilityOnline() {
  client.publish(topic_availability, "online", true);
}

void publishCheckin(const char* payload) {
  client.publish(topic_checkin, payload, true);
}

void updateSensors() {
  startOI();
  roomba.start();

  delay(30);
  chargeState = readSensor8(21);       // charging state
  delay(30);

  int16_t charge = readSensor16(25);   // battery charge mAh
  delay(30);

  int16_t capacity = readSensor16(26); // battery capacity mAh
  delay(30);

  uint8_t omniIr = readSensor8(17);    // IR char omni
  delay(30);

  if (capacity > 0 && charge >= 0) {
    batteryPercent = (int)((100L * charge) / capacity);
    if (batteryPercent < 0) batteryPercent = 0;
    if (batteryPercent > 100) batteryPercent = 100;
  }

  charging = (chargeState == 1 || chargeState == 2 || chargeState == 3);
  dockVisible = (omniIr == 160 || omniIr == 164 || omniIr == 168 || omniIr == 172);

  if (currentState == "returning" && charging) {
    currentState = "docked";
  } else if (currentState == "idle" && charging) {
    currentState = "docked";
  }

  publishState();
}

void playLocateSong() {
  safeMode();
  Serial.write(140); // Song
  Serial.write(0);   // song number
  Serial.write(3);   // 3 notes
  Serial.write(72);  // C
  Serial.write(16);
  Serial.write(76);  // E
  Serial.write(16);
  Serial.write(79);  // G
  Serial.write(24);
  delay(50);

  Serial.write(141); // Play
  Serial.write(0);
  delay(50);
}

void startCleaning() {
  awake();
  startOI();
  safeMode();
  Serial.write(135);   // Clean
  delay(50);
  currentState = "cleaning";
  lastCommandAt = millis();
  publishState();
}

void spotCleaning() {
  awake();
  startOI();
  safeMode();
  Serial.write(134);   // Spot
  delay(50);
  currentState = "cleaning";
  lastCommandAt = millis();
  publishState();
}

void pauseCleaning() {
  awake();
  startOI();
  Serial.write(135);   // Clean toggles/pause if cleaning is in progress
  delay(50);
  currentState = "paused";
  lastCommandAt = millis();
  publishState();
}

void stopCleaning() {
  awake();
  startOI();
  safeMode();
  Serial.write(135);   // Clean as pause/stop toggle on many Roomba 600/700
  delay(100);
  currentState = "idle";
  lastCommandAt = millis();
  publishState();
}

void goHome() {
  awake();
  startOI();
  safeMode();
  Serial.write(143);   // Seek Dock
  delay(50);
  currentState = "returning";
  lastCommandAt = millis();
  publishState();
}

void processCommand(const String& cmd) {
  if (cmd == "start") {
    startCleaning();
  } else if (cmd == "pause") {
    pauseCleaning();
  } else if (cmd == "stop") {
    stopCleaning();
  } else if (cmd == "dock" || cmd == "return_to_base") {
    goHome();
  } else if (cmd == "spot" || cmd == "clean_spot") {
    spotCleaning();
  } else if (cmd == "find" || cmd == "locate") {
    playLocateSong();
    publishState();
  } else if (cmd == "status") {
    updateSensors();
  }
}

void callback(char* topic, byte* payload, unsigned int length) {
  String newTopic = String(topic);
  String newPayload;

  for (unsigned int i = 0; i < length; i++) {
    newPayload += (char)payload[i];
  }

  if (newTopic == topic_command) {
    processCommand(newPayload);
  }
}

void setup_wifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
}

bool reconnect() {
  if (client.connected()) return true;

  if (millis() - lastReconnectAttempt < 5000) return false;
  lastReconnectAttempt = millis();

  bool ok = client.connect(
    mqtt_client_name,
    mqtt_user,
    mqtt_pass,
    topic_availability,
    0,
    true,
    "offline"
  );

  if (ok) {
    publishAvailabilityOnline();

    if (bootFlag) {
      publishCheckin("Rebooted");
      bootFlag = false;
    } else {
      publishCheckin("Reconnected");
    }

    client.subscribe(topic_command);
    updateSensors();
  }

  return ok;
}

void setup() {
  pinMode(noSleepPin, OUTPUT);
  digitalWrite(noSleepPin, HIGH);

  Serial.begin(115200);
  delay(200);

  setupRoombaBaud115200();
  setup_wifi();

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  lastCommandAt = millis();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    setup_wifi();
  }

  if (!client.connected()) {
    reconnect();
  } else {
    client.loop();

    unsigned long now = millis();

    if (now - lastStatusPublish > 60000) {
      updateSensors();
      lastStatusPublish = now;
    }

    if (currentState == "idle" || currentState == "docked" || currentState == "paused") {
      if (now - lastKeepAwake > 240000) {
        awake();
        lastKeepAwake = now;
      }
    }
  }

  delay(50);
}