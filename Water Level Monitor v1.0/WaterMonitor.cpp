/*
 * =====================================================================
 *  WaterMonitor - ESP32 Automatic Water Level Monitor
 *  V0.1.1  (compile-fixed for ESP32 core 3.x)
 *  Target  : ESP32-WROOM-32S
 *  Sensor  : AJ-SR04M (3.3 V)
 *  Battery : 18650 + divider on GPIO34
 * =====================================================================
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>              // ✅ ADDED
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <Update.h>
#include <esp_sleep.h>

// ---------- PIN MAP ----------
#define PIN_TRIG          26
#define PIN_ECHO          25
#define PIN_SENSOR_PWR    33
#define PIN_BATTERY_ADC   34
#define PIN_LED_STATUS     2
#define PIN_BUTTON         0

#define FW_VERSION        "0.1.1"
#define DEVICE_MODEL      "WM-01"

#define DEFAULT_MQTT_HOST "4f578522a76143be8bcbb8b759a0e496.s1.eu.hivemq.cloud"
#define DEFAULT_MQTT_PORT 8883

#define DEFAULT_MEASURE_INTERVAL_SEC  300
#define DEFAULT_TANK_HEIGHT_MM        2000
#define DEFAULT_MIN_DISTANCE_MM       200
#define DEFAULT_MAX_DISTANCE_MM       8000

#define BAT_R1_KOHM       220.0f
#define BAT_R2_KOHM       100.0f
#define BAT_ADC_SAMPLES   16

#define OTA_WINDOW_MS     (10UL * 60UL * 1000UL)
#define BTN_MAINT_MS      3000
#define BTN_FACTORY_MS    8000

enum LedState {
  LED_BOOT, LED_PROVISIONING, LED_WIFI_CONNECTING,
  LED_MQTT_CONNECTING, LED_MEASURING, LED_ONLINE,
  LED_OTA, LED_ERROR, LED_SLEEP
};
enum OpMode { MODE_NORMAL, MODE_PROVISIONING, MODE_MAINTENANCE };

// ---------- CONFIG ----------
struct DeviceConfig {
  char     deviceName[32];
  char     wifiSsid[64];
  char     wifiPassword[64];
  char     mqttHost[128];
  uint16_t mqttPort;
  char     mqttUser[64];
  char     mqttPassword[64];
  char     otaPassword[32];
  uint32_t measureIntervalSec;
  uint16_t tankHeightMm;
  uint16_t minDistanceMm;
  uint16_t maxDistanceMm;
  bool     configured;
};

static DeviceConfig cfg;
static Preferences  prefs;
static char         deviceId[16];

static WiFiClientSecure wifiClientSecure;
static PubSubClient     mqtt(wifiClientSecure);
static WebServer        httpServer(80);

static OpMode   currentMode      = MODE_NORMAL;
static LedState ledState         = LED_BOOT;
static uint32_t ledLastChange    = 0;

static uint32_t bootMillis       = 0;
static bool     inMaintenance    = false;
static uint32_t maintenanceStart = 0;

static bool     sensorOk         = false;
static uint32_t lastDistanceMm   = 0;
static int      lastBatteryMv    = 0;
static int      lastBatteryPct   = 0;

// ---------- CONFIG MANAGER ----------
static void loadDefaults() {
  memset(&cfg, 0, sizeof(cfg));
  const uint64_t mac = ESP.getEfuseMac();
  snprintf(deviceId, sizeof(deviceId), "WM-%02X%02X%02X",
           (uint8_t)(mac >> 16), (uint8_t)(mac >> 8), (uint8_t)(mac));
  snprintf(cfg.deviceName, sizeof(cfg.deviceName), "WaterMonitor-%s", deviceId + 3);
  strncpy(cfg.mqttHost, DEFAULT_MQTT_HOST, sizeof(cfg.mqttHost) - 1);
  cfg.mqttPort           = DEFAULT_MQTT_PORT;
  cfg.measureIntervalSec = DEFAULT_MEASURE_INTERVAL_SEC;
  cfg.tankHeightMm       = DEFAULT_TANK_HEIGHT_MM;
  cfg.minDistanceMm      = DEFAULT_MIN_DISTANCE_MM;
  cfg.maxDistanceMm      = DEFAULT_MAX_DISTANCE_MM;
  cfg.configured         = false;
}

static void loadConfig() {
  prefs.begin("wmcfg", true);
  size_t sz = prefs.getBytesLength("cfg");
  if (sz == sizeof(cfg)) {
    prefs.getBytes("cfg", &cfg, sizeof(cfg));
    Serial.println(F("[CFG] loaded from NVS"));
  } else {
    loadDefaults();
    Serial.println(F("[CFG] defaults"));
  }
  const uint64_t mac = ESP.getEfuseMac();
  snprintf(deviceId, sizeof(deviceId), "WM-%02X%02X%02X",
           (uint8_t)(mac >> 16), (uint8_t)(mac >> 8), (uint8_t)(mac));
  prefs.end();
}

static void saveConfig() {
  prefs.begin("wmcfg", false);
  prefs.putBytes("cfg", &cfg, sizeof(cfg));
  prefs.end();
  Serial.println(F("[CFG] saved"));
}

static void factoryReset() {
  prefs.begin("wmcfg", false);
  prefs.clear();
  prefs.end();
  Serial.println(F("[CFG] factory reset"));
  delay(300);
  ESP.restart();
}

// ---------- LED ----------
static void setLed(LedState s) { ledState = s; ledLastChange = millis(); }

static void ledTask() {
  uint32_t t = millis() - ledLastChange;
  bool on = false;
  switch (ledState) {
    case LED_BOOT:             on = (t % 200) < 100; break;
    case LED_PROVISIONING:     on = (t % 400) < 200; break;
    case LED_WIFI_CONNECTING:  on = (t % 600) < 300; break;
    case LED_MQTT_CONNECTING:  on = (t % 800) < 400; break;
    case LED_MEASURING:        on = (t % 200) <  50; break;
    case LED_ONLINE:           on = ((t / 1000) % 2) == 0 ? true : ((t % 100) < 20); break;
    case LED_OTA:              on = (t % 150) <  75; break;
    case LED_ERROR:            on = (t % 1000) < 100; break;
    case LED_SLEEP:            on = false; break;
  }
  digitalWrite(PIN_LED_STATUS, on ? HIGH : LOW);
}

// ---------- BATTERY ----------
static int readBatteryMilliVolts() {
  uint32_t acc = 0;
  for (int i = 0; i < BAT_ADC_SAMPLES; i++) {
    acc += analogReadMilliVolts(PIN_BATTERY_ADC);
    delay(2);
  }
  float vAdc  = acc / (float)BAT_ADC_SAMPLES;
  float ratio = (BAT_R1_KOHM + BAT_R2_KOHM) / BAT_R2_KOHM;
  return (int)(vAdc * ratio);
}

static int batteryPercentFromMv(int mv) {
  struct P { int mv; int pct; };
  static const P lut[] = {
    {4200,100},{4100,90},{4000,80},{3900,65},{3800,50},
    {3700, 35},{3600,20},{3500,10},{3300, 0}
  };
  const int N = sizeof(lut) / sizeof(lut[0]);
  if (mv >= lut[0].mv)     return 100;
  if (mv <= lut[N - 1].mv) return 0;
  for (int i = 0; i < N - 1; i++) {
    if (mv <= lut[i].mv && mv >= lut[i + 1].mv) {
      float f = (float)(mv - lut[i + 1].mv) / (float)(lut[i].mv - lut[i + 1].mv);
      return lut[i + 1].pct + (int)(f * (lut[i].pct - lut[i + 1].pct));
    }
  }
  return 0;
}

// ---------- AJ-SR04M ----------
static void sensorPower(bool on) {
  digitalWrite(PIN_SENSOR_PWR, on ? HIGH : LOW);
  if (on) delay(80);
}

static uint32_t ultrasonicPingOnce() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(4);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  uint32_t dur = pulseIn(PIN_ECHO, HIGH, 30000UL);
  if (dur == 0) return 0;
  return dur * 343UL / 2000UL;
}

static int cmpU32(const void* a, const void* b) {
  uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
  return (x > y) - (x < y);
}

static uint32_t ultrasonicReadMedian() {
  uint32_t s[5];
  int n = 0;
  for (int i = 0; i < 5; i++) {
    uint32_t d = ultrasonicPingOnce();
    if (d > 0) s[n++] = d;
    delay(60);
  }
  if (n == 0) return 0;
  qsort(s, n, sizeof(uint32_t), cmpU32);
  return s[n / 2];
}

// ---------- mDNS ----------  ✅ FIXED
static void startDiscovery() {
  if (!MDNS.begin(deviceId)) {
    Serial.println(F("[MDNS] failed"));
    return;
  }
  MDNS.addService("watermon", "tcp", 80);

  // ✅ Use String() wrapper to disambiguate overloads on ESP32 core 3.x
  MDNS.addServiceTxt(String("watermon"), String("tcp"), String("id"),    String(deviceId));
  MDNS.addServiceTxt(String("watermon"), String("tcp"), String("model"), String(DEVICE_MODEL));
  MDNS.addServiceTxt(String("watermon"), String("tcp"), String("fw"),    String(FW_VERSION));
  MDNS.addServiceTxt(String("watermon"), String("tcp"), String("cap"),   String("level,battery,ota,mqtt"));

  Serial.printf("[MDNS] %s.local  _watermon._tcp\n", deviceId);
}

// ---------- WIFI ----------
static bool wifiConnect(uint32_t timeoutMs = 20000) {
  if (strlen(cfg.wifiSsid) == 0) return false;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);
  WiFi.begin(cfg.wifiSsid, cfg.wifiPassword);
  Serial.printf("[WiFi] connecting to %s", cfg.wifiSsid);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    ledTask();
    delay(200);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] OK  IP=%s  RSSI=%d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }
  Serial.println(F("[WiFi] timeout"));
  return false;
}

// ---------- MQTT ----------
static String topicTelemetry()   { return String("wm/v1/") + deviceId + "/telemetry"; }
static String topicAvailability(){ return String("wm/v1/") + deviceId + "/availability"; }
static String topicCommand()     { return String("wm/v1/") + deviceId + "/command"; }

static void mqttCallback(char* topic, byte* payload, unsigned int len) {
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, payload, len)) return;
  const char* cmd = doc["command"] | "";
  if (!strcmp(cmd, "reboot"))      { delay(200); ESP.restart(); }
  else if (!strcmp(cmd, "maintenance")) { inMaintenance = true; maintenanceStart = millis(); }
}

static bool mqttConnect() {
  if (strlen(cfg.mqttHost) == 0) return false;
  wifiClientSecure.setInsecure();
  mqtt.setServer(cfg.mqttHost, cfg.mqttPort);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(60);
  mqtt.setBufferSize(512);

  String clientId = String(deviceId) + "-" + String((uint32_t)esp_random(), HEX);
  String willTopic = topicAvailability();

  bool ok;
  if (strlen(cfg.mqttUser) > 0) {
    ok = mqtt.connect(clientId.c_str(), cfg.mqttUser, cfg.mqttPassword,
                      willTopic.c_str(), 1, true, "offline");
  } else {
    ok = mqtt.connect(clientId.c_str(), nullptr, nullptr,
                      willTopic.c_str(), 1, true, "offline");
  }
  if (ok) {
    mqtt.publish(willTopic.c_str(), "online", true);
    mqtt.subscribe(topicCommand().c_str(), 1);
    Serial.println(F("[MQTT] connected"));
  } else {
    Serial.printf("[MQTT] fail rc=%d\n", mqtt.state());
  }
  return ok;
}

static void mqttPublishTelemetry() {
  if (!mqtt.connected()) return;
  StaticJsonDocument<384> doc;
  doc["device_id"]          = deviceId;
  doc["fw"]                 = FW_VERSION;
  doc["sensor_ok"]          = sensorOk;
  doc["distance_mm"]        = lastDistanceMm;
  if (sensorOk) {
    int32_t level = (int32_t)cfg.tankHeightMm - (int32_t)lastDistanceMm;
    if (level < 0) level = 0;
    doc["level_mm"]      = level;
    doc["level_percent"] = (float)level * 100.0f / (float)cfg.tankHeightMm;
  }
  doc["battery_voltage_mv"] = lastBatteryMv;
  doc["battery_percent"]    = lastBatteryPct;
  doc["rssi"]               = WiFi.RSSI();

  char buf[384];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  mqtt.publish(topicTelemetry().c_str(), (const uint8_t*)buf, n, false);
  Serial.printf("[MQTT] TX %u bytes\n", (unsigned)n);
}

// ---------- MEASUREMENT ----------
static void runMeasurement() {
  setLed(LED_MEASURING);
  sensorPower(true);
  uint32_t d = ultrasonicReadMedian();
  sensorPower(false);
  lastDistanceMm = d;
  sensorOk = (d >= cfg.minDistanceMm && d <= cfg.maxDistanceMm);
  lastBatteryMv  = readBatteryMilliVolts();
  lastBatteryPct = batteryPercentFromMv(lastBatteryMv);
  Serial.printf("[MEAS] dist=%umm ok=%d batt=%dmV (%d%%)\n",
                lastDistanceMm, sensorOk, lastBatteryMv, lastBatteryPct);
}

// ---------- ARDUINO OTA ----------
static void startArduinoOTA() {
  ArduinoOTA.setHostname(deviceId);
  if (strlen(cfg.otaPassword) > 0) ArduinoOTA.setPassword(cfg.otaPassword);
  ArduinoOTA.onStart([]() { setLed(LED_OTA); Serial.println(F("[OTA] start")); });
  ArduinoOTA.onEnd([]()   { Serial.println(F("[OTA] end")); });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
    Serial.printf("[OTA] %u%%\r", (p * 100) / t);
  });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[OTA] err %u\n", e); });
  ArduinoOTA.begin();
  Serial.println(F("[OTA] ready"));
}

// ---------- HTTPS OTA ----------  ✅ FIXED (HTTPClient include added)
static bool httpsUpdate(const String& url) {
  setLed(LED_OTA);
  wifiClientSecure.setInsecure();

  HTTPClient https;
  if (!https.begin(wifiClientSecure, url)) return false;

  int code = https.GET();
  if (code != HTTP_CODE_OK) { https.end(); return false; }

  int len = https.getSize();
  if (len <= 0) { https.end(); return false; }
  if (!Update.begin(len)) { https.end(); return false; }

  WiFiClient* stream = https.getStreamPtr();
  size_t written = Update.writeStream(*stream);
  https.end();

  if (written != (size_t)len) { Update.abort(); return false; }
  if (!Update.end(true))     { return false; }

  Serial.println(F("[OTA] HTTPS OK -> reboot"));
  delay(300);
  ESP.restart();
  return true;
}

// ---------- PROVISIONING ----------
static void handleRoot() {
  String html = F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Water Monitor Setup</title>"
    "<style>body{font-family:sans-serif;max-width:440px;margin:16px;}"
    "input{width:100%;padding:8px;margin:4px 0;box-sizing:border-box;}"
    "button{padding:10px 16px;background:#07c;color:#fff;border:0;border-radius:4px;}"
    "label{font-size:13px;color:#555;}</style></head><body>");
  html += "<h2>Water Monitor Setup</h2><p>Device: <b>";
  html += deviceId; html += "</b></p>";
  html += "<form method='POST' action='/save'>";
  html += "<label>WiFi SSID</label><input name='ssid' required>";
  html += "<label>WiFi Password</label><input name='pass' type='password'>";
  html += "<label>MQTT Host</label><input name='mhost' value='";
  html += DEFAULT_MQTT_HOST; html += "'>";
  html += "<label>MQTT Port</label><input name='mport' value='8883'>";
  html += "<label>MQTT User</label><input name='muser'>";
  html += "<label>MQTT Pass</label><input name='mpass' type='password'>";
  html += "<label>OTA Password</label><input name='opass'>";
  html += "<label>Tank Height (mm)</label><input name='tank' value='2000'>";
  html += "<label>Interval (sec)</label><input name='intv' value='300'>";
  html += "<button type='submit'>Save &amp; Reboot</button>";
  html += "</form></body></html>";
  httpServer.send(200, "text/html", html);
}

static void handleSave() {
  auto cp = [](char* dst, size_t n, const String& s) {
    strncpy(dst, s.c_str(), n - 1);
    dst[n - 1] = '\0';
  };
  if (httpServer.hasArg("ssid"))  cp(cfg.wifiSsid,     sizeof(cfg.wifiSsid),     httpServer.arg("ssid"));
  if (httpServer.hasArg("pass"))  cp(cfg.wifiPassword, sizeof(cfg.wifiPassword), httpServer.arg("pass"));
  if (httpServer.hasArg("mhost")) cp(cfg.mqttHost,     sizeof(cfg.mqttHost),     httpServer.arg("mhost"));
  if (httpServer.hasArg("mport")) cfg.mqttPort           = httpServer.arg("mport").toInt();
  if (httpServer.hasArg("muser")) cp(cfg.mqttUser,     sizeof(cfg.mqttUser),     httpServer.arg("muser"));
  if (httpServer.hasArg("mpass")) cp(cfg.mqttPassword, sizeof(cfg.mqttPassword), httpServer.arg("mpass"));
  if (httpServer.hasArg("opass")) cp(cfg.otaPassword,  sizeof(cfg.otaPassword),  httpServer.arg("opass"));
  if (httpServer.hasArg("tank"))  cfg.tankHeightMm       = httpServer.arg("tank").toInt();
  if (httpServer.hasArg("intv"))  cfg.measureIntervalSec = httpServer.arg("intv").toInt();

  cfg.configured = true;
  saveConfig();
  httpServer.send(200, "text/html", F("<h3>Saved.</h3><p>Rebooting...</p>"));
  delay(1200);
  ESP.restart();
}

static void startProvisioning() {
  currentMode = MODE_PROVISIONING;
  setLed(LED_PROVISIONING);

  char apSsid[40];
  snprintf(apSsid, sizeof(apSsid), "WM-SETUP-%s", deviceId + 3);
  const char* apPass = "watermon";

  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSsid, apPass);
  Serial.printf("[PROV] AP %s  IP=%s\n", apSsid, WiFi.softAPIP().toString().c_str());

  httpServer.on("/",     HTTP_GET,  handleRoot);
  httpServer.on("/save", HTTP_POST, handleSave);
  httpServer.onNotFound(handleRoot);
  httpServer.begin();

  Serial.printf("[PROV] QR WIFI:T:WPA;S:%s;P:%s;;\n", apSsid, apPass);
}

// ---------- BUTTON ----------
static void buttonTask() {
  static uint32_t pressStart = 0;
  static bool     wasDown    = false;
  bool down = (digitalRead(PIN_BUTTON) == LOW);
  if (down && !wasDown) {
    pressStart = millis();
    wasDown    = true;
  } else if (down && wasDown) {
    if (millis() - pressStart >= BTN_FACTORY_MS) factoryReset();
  } else if (!down && wasDown) {
    uint32_t held = millis() - pressStart;
    if (held >= BTN_MAINT_MS && held < BTN_FACTORY_MS) {
      inMaintenance      = true;
      maintenanceStart   = millis();
      Serial.println(F("[BTN] maintenance"));
    }
    wasDown = false;
  }
}

// ---------- SLEEP ----------
static void enterDeepSleep() {
  Serial.printf("[SLEEP] %u sec\n", cfg.measureIntervalSec);
  setLed(LED_SLEEP);
  digitalWrite(PIN_LED_STATUS, LOW);
  sensorPower(false);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_sleep_enable_timer_wakeup((uint64_t)cfg.measureIntervalSec * 1000000ULL);
  delay(50);
  esp_deep_sleep_start();
}

// ---------- SETUP ----------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println(F("========================================"));
  Serial.println(F("  WaterMonitor  ESP32  V0.1.1"));
  Serial.printf ("  FW %s  Model %s\n", FW_VERSION, DEVICE_MODEL);
  Serial.println(F("========================================"));

  pinMode(PIN_TRIG,       OUTPUT);
  pinMode(PIN_ECHO,       INPUT);
  pinMode(PIN_SENSOR_PWR, OUTPUT);
  pinMode(PIN_LED_STATUS, OUTPUT);
  pinMode(PIN_BUTTON,     INPUT_PULLUP);
  digitalWrite(PIN_SENSOR_PWR, LOW);
  digitalWrite(PIN_TRIG,       LOW);

  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_11db);

  loadConfig();
  Serial.printf("[BOOT] deviceId=%s  configured=%d\n", deviceId, cfg.configured);

  if (digitalRead(PIN_BUTTON) == LOW) {
    uint32_t t0 = millis();
    while (digitalRead(PIN_BUTTON) == LOW) {
      if (millis() - t0 > BTN_FACTORY_MS) factoryReset();
      delay(10);
    }
    if (millis() - t0 > BTN_MAINT_MS) {
      inMaintenance    = true;
      maintenanceStart = millis();
    }
  }

  if (!cfg.configured || strlen(cfg.wifiSsid) == 0) {
    startProvisioning();
    return;
  }

  bootMillis = millis();
  setLed(LED_WIFI_CONNECTING);
  if (!wifiConnect()) {
    setLed(LED_ERROR);
    enterDeepSleep();
  }
  startDiscovery();
  startArduinoOTA();
  setLed(LED_MQTT_CONNECTING);
  mqttConnect();
}

// ---------- LOOP ----------
void loop() {
  ledTask();

  if (currentMode == MODE_PROVISIONING) {
    httpServer.handleClient();
    return;
  }

  buttonTask();

  if (inMaintenance) {
    ArduinoOTA.handle();
    httpServer.handleClient();
    if (millis() - maintenanceStart > OTA_WINDOW_MS) {
      inMaintenance = false;
      enterDeepSleep();
    }
    return;
  }

  runMeasurement();

  if (WiFi.status() != WL_CONNECTED) {
    setLed(LED_WIFI_CONNECTING);
    if (!wifiConnect(10000)) {
      setLed(LED_ERROR);
      enterDeepSleep();
    }
  }

  static bool mdnsStarted = false;
  if (!mdnsStarted) { startDiscovery(); mdnsStarted = true; }

  if (!mqtt.connected()) {
    setLed(LED_MQTT_CONNECTING);
    if (!mqttConnect()) enterDeepSleep();
  }
  mqtt.loop();

  setLed(LED_ONLINE);
  mqttPublishTelemetry();

  uint32_t until = millis() + 3000;
  while (millis() < until) {
    ArduinoOTA.handle();
    mqtt.loop();
    ledTask();
    delay(20);
  }

  enterDeepSleep();
}
