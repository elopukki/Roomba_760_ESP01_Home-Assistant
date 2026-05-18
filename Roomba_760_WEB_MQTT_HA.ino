#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Roomba.h>

// =====================
// USER CONFIG
// =====================
const char* ssid = "SSID";
const char* password = "PASS";

const char* mqtt_server = "IPmqtt";
const int mqtt_port = 1883;
const char* mqtt_user = "LOGIN";
const char* mqtt_pass = "PASS";
const char* mqtt_client_name = "Roomba760ESP01";

const char* topic_command = "roomba/commands";
const char* topic_state = "roomba/state";
const char* topic_availability = "roomba/availability";
const char* topic_checkin = "checkIn/roomba";

const uint8_t noSleepPin = 2; // ESP-01 GPIO2 -> Roomba BRC

// =====================
// GLOBALS
// =====================
WiFiClient espClient;
PubSubClient client(espClient);
ESP8266WebServer server(80);
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

// =====================
// LOW-LEVEL HELPERS
// =====================
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

// =====================
// ROOMBA ACTIONS
// =====================
void publishState();

void startCleaning() {
  awake();
  startOI();
  safeMode();
  Serial.write(135); // Clean
  delay(100);
  currentState = "cleaning";
  lastCommandAt = millis();
  publishState();
}

void pauseCleaning() {
  awake();
  startOI();
  safeMode();
  Serial.write(135); // toggle/pause on many 600/700 series
  delay(100);
  currentState = "paused";
  lastCommandAt = millis();
  publishState();
}

void stopCleaning() {
  awake();
  startOI();
  safeMode();
  Serial.write(173); // on your 760 this is usually more useful than 173
  delay(100);
  currentState = "idle";
  lastCommandAt = millis();
  publishState();
}

void goHome() {
  awake();
  startOI();
  safeMode();
  Serial.write(143); // Seek Dock
  delay(100);
  currentState = "returning";
  lastCommandAt = millis();
  publishState();
}

void spotCleaning() {
  awake();
  startOI();
  safeMode();
  Serial.write(134); // Spot
  delay(100);
  currentState = "cleaning";
  lastCommandAt = millis();
  publishState();
}

void playLocateSong() {
  safeMode();
  Serial.write(140); // Song
  Serial.write(0);
  Serial.write(3);
  Serial.write(72);
  Serial.write(16);
  Serial.write(76);
  Serial.write(16);
  Serial.write(79);
  Serial.write(24);
  delay(50);
  Serial.write(141); // Play
  Serial.write(0);
  delay(50);
}

void processCommand(const String& cmd) {
  if (cmd == "start") startCleaning();
  else if (cmd == "pause") pauseCleaning();
  else if (cmd == "stop") stopCleaning();
  else if (cmd == "dock" || cmd == "return_to_base") goHome();
  else if (cmd == "spot" || cmd == "clean_spot") spotCleaning();
  else if (cmd == "find" || cmd == "locate") playLocateSong();
  else if (cmd == "status") {
    // just update sensors
  }
}

// =====================
// MQTT
// =====================
void publishAvailabilityOnline() {
  client.publish(topic_availability, (const uint8_t*)"online", 6, true);
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

void updateSensors() {
  startOI();
  roomba.start();

  delay(30);
  chargeState = readSensor8(21);   // charge state
  delay(30);

  int16_t charge = readSensor16(25);   // battery charge mAh
  delay(30);

  int16_t capacity = readSensor16(26);  // battery capacity mAh
  delay(30);

  uint8_t omniIr = readSensor8(17);     // simple presence hint

  if (capacity > 0 && charge >= 0) {
    batteryPercent = (int)((100L * charge) / capacity);
    if (batteryPercent < 0) batteryPercent = 0;
    if (batteryPercent > 100) batteryPercent = 100;
  }

  charging = (chargeState == 1 || chargeState == 2 || chargeState == 3);
  dockVisible = (omniIr != 0);

  if ((currentState == "returning" || currentState == "idle") && charging) {
    currentState = "docked";
  }

  publishState();
}

bool reconnectMqtt() {
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
      client.publish(topic_checkin, (const uint8_t*)"Rebooted", 8, true);
      bootFlag = false;
    } else {
      client.publish(topic_checkin, (const uint8_t*)"Reconnected", 11, true);
    }

    client.subscribe(topic_command);
    updateSensors();
  }

  return ok;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String newTopic = String(topic);
  String newPayload;
  for (unsigned int i = 0; i < length; i++) newPayload += (char)payload[i];

  if (newTopic == topic_command) {
    processCommand(newPayload);
  }
}

// =====================
// WEB UI
// =====================
String htmlEscape(const String& s) {
  String r = s;
  r.replace("&", "&amp;");
  r.replace("<", "&lt;");
  r.replace(">", "&gt;");
  return r;
}

void handleRoot() {
  String html;
  html.reserve(3500);

  html += "<!doctype html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta charset='utf-8'>";
  html += "<title>Roomba 760</title>";
  html += "<style>";
  html += "body{font-family:Arial,sans-serif;background:#f2f2f2;margin:0;padding:16px;text-align:center;}";
  html += ".card{max-width:420px;margin:0 auto;background:#fff;border-radius:24px;box-shadow:0 8px 24px rgba(0,0,0,.12);padding:18px;}";
  html += "h1{margin:8px 0 0 0;font-size:28px;}";
  html += ".state{font-size:44px;font-weight:700;margin:24px 0 6px;}";
  html += ".sub{color:#666;font-size:16px;margin-bottom:20px;}";
  html += ".btns{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-top:20px;}";
  html += ".btn{display:block;padding:18px 10px;border-radius:18px;text-decoration:none;font-size:20px;font-weight:700;color:#111;background:#e9e9e9;}";
  html += ".btn.primary{background:#dff3e3;}";
  html += ".btn.danger{background:#fde2e1;}";
  html += ".btn.blue{background:#e1ecfd;}";
  html += ".btn.gray{background:#efefef;}";
  html += ".full{grid-column:1/-1;}";
  html += ".small{font-size:14px;color:#666;margin-top:12px;}";
  html += "</style></head><body>";
  html += "<div class='card'>";
  html += "<h1>Roomba 760</h1>";
  html += "<div class='state' id='state'>" + htmlEscape(currentState) + "</div>";
  html += "<div class='sub'>Battery: <span id='battery'>" + String(batteryPercent) + "</span>%</div>";
  html += "<div class='sub'>Charge: <span id='charge'>" + htmlEscape(String(chargeStateToText(chargeState))) + "</span></div>";
  html += "<div class='btns'>";
  html += "<a class='btn primary' href='/api/start'>Start</a>";
  html += "<a class='btn gray' href='/api/pause'>Pause</a>";
  html += "<a class='btn danger' href='/api/stop'>Stop</a>";
  html += "<a class='btn blue' href='/api/dock'>Dock</a>";
  html += "<a class='btn full' href='/api/spot'>Spot</a>";
  html += "<a class='btn full' href='/api/find'>Find</a>";
  html += "</div>";
  html += "<div class='small'>";
  html += "WiFi: " + WiFi.localIP().toString() + "<br>";
  html += "MQTT: " + String(client.connected() ? "online" : "offline");
  html += "</div>";
  html += "<script>";
  html += "async function upd(){try{let r=await fetch('/api/state');let j=await r.json();";
  html += "document.getElementById('state').innerText=j.state;";
  html += "document.getElementById('battery').innerText=j.battery;";
  html += "document.getElementById('charge').innerText=j.charge_state;";
  html += "}catch(e){}} setInterval(upd,5000); upd();";
  html += "</script>";
  html += "</div></body></html>";

  server.send(200, "text/html; charset=utf-8", html);
}

void handleApiState() {
  StaticJsonDocument<256> doc;
  doc["state"] = currentState;
  doc["battery"] = batteryPercent;
  doc["charging"] = charging;
  doc["charge_state"] = chargeStateToText(chargeState);
  doc["dock_visible"] = dockVisible;
  doc["ip"] = WiFi.localIP().toString();

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json; charset=utf-8", out);
}

void redirectHome() {
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void setupWeb() {
  server.on("/", handleRoot);
  server.on("/api/state", handleApiState);

  server.on("/api/start", []() { startCleaning(); redirectHome(); });
  server.on("/api/pause", []() { pauseCleaning(); redirectHome(); });
  server.on("/api/stop",  []() { stopCleaning(); redirectHome(); });
  server.on("/api/dock",  []() { goHome(); redirectHome(); });
  server.on("/api/spot",  []() { spotCleaning(); redirectHome(); });
  server.on("/api/find",  []() { processCommand("find"); redirectHome(); });

  server.begin();
}

// =====================
// WIFI / MDNS
// =====================
void setup_wifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
}

void setup_mdns() {
  if (MDNS.begin("roomba")) {
    MDNS.addService("http", "tcp", 80);
  }
}

// =====================
// SETUP / LOOP
// =====================
void setupRoombaBaud115200() {
  Serial.write(129);
  delay(20);
  Serial.write(11);
  delay(100);
}
void setup() {
  pinMode(noSleepPin, OUTPUT);
  digitalWrite(noSleepPin, HIGH);

  Serial.begin(115200);
  delay(200);

  setup_wifi();
  setup_mdns();

  setupRoombaBaud115200();

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);

  setupWeb();

  lastCommandAt = millis();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    setup_wifi();
  }

  if (MDNS.isRunning()) {
    MDNS.update();
  }

  if (!client.connected()) {
    reconnectMqtt();
  } else {
    client.loop();
  }

  server.handleClient();

  unsigned long now = millis();

  if (now - lastStatusPublish > 60000) {
    updateSensors();
    lastStatusPublish = now;
  }

  // if (currentState == "idle" || currentState == "docked" || currentState == "paused") {
  //  if (now - lastKeepAwake > 240000) {
  //    awake();
  //    lastKeepAwake = now;
  //  }
  // }

  delay(20);
}
