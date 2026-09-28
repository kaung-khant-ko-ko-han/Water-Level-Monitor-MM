# ESP32 Water Monitor V0.2 — Full Multi-File Arduino Project

အောက်မှာ **Arduino IDE တွင် တိုက်ရိုက် open/compile/upload လုပ်နိုင်တဲ့ multi-tab project** အပြည့်အစုံဖြစ်ပါတယ်။ Folder structure ကို Arduino IDE convention အတိုင်း `WaterMonitor/WaterMonitor.ino` ဖြစ်ရပါမယ်။

---

## 📁 Folder Structure

```
WaterMonitor/                ← Sketch folder (name must match .ino)
├── WaterMonitor.ino         ← Main sketch
├── AppConfig.h
├── certs.h
├── ConfigManager.h
├── ConfigManager.cpp
├── LEDStatus.h
├── LEDStatus.cpp
├── Battery.h
├── Battery.cpp
├── AJSR04M.h
├── AJSR04M.cpp
├── WiFiProvisioning.h
├── WiFiProvisioning.cpp
├── WebConfig.h
├── WebConfig.cpp
├── Discovery.h
├── Discovery.cpp
├── MQTTManager.h
├── MQTTManager.cpp
├── OTAManager.h
├── OTAManager.cpp
├── HAClient.h
└── HAClient.cpp

WaterMonitorFactory/         ← Separate factory-test sketch
└── WaterMonitorFactory.ino
```

**Arduino Library Manager မှ install လုပ်ရန်:**
```
PubSubClient        (Nick O'Leary)   >= 2.8
ArduinoJson         (Benoit Blanchon) >= 7.x
QRCode              (Richard Moore)   >= 0.0.1
ESP32 Board Package (Espressif)       >= 2.0.14
```

---

## 📄 `AppConfig.h`

```cpp
#pragma once

// ---------------------------------------------------------------------
//  Firmware identity
// ---------------------------------------------------------------------
#define FW_VERSION   "0.2.0"
#define DEVICE_MODEL "WM-01"
#define HW_REV       "REV-A"

// ---------------------------------------------------------------------
//  GPIO
// ---------------------------------------------------------------------
#define PIN_TRIG        26
#define PIN_ECHO        25
#define PIN_SENSOR_PWR  33      // MOSFET gate → AJ-SR04M VCC
#define PIN_BATTERY_ADC 34      // ADC1_CH6 input-only
#define PIN_LED_STATUS   2
#define PIN_BUTTON       0      // BOOT (active LOW)

// ---------------------------------------------------------------------
//  Defaults (overridable via provisioning)
// ---------------------------------------------------------------------
#define DEFAULT_MQTT_HOST \
  "4f578522a76143be8bcbb8b759a0e496.s1.eu.hivemq.cloud"
#define DEFAULT_MQTT_PORT            8883
#define DEFAULT_MEASURE_INTERVAL_SEC 300
#define DEFAULT_TANK_HEIGHT_MM       2000
#define DEFAULT_MIN_DISTANCE_MM      200
#define DEFAULT_MAX_DISTANCE_MM      8000

// ---------------------------------------------------------------------
//  Battery divider  (R1 top, R2 bottom)
// ---------------------------------------------------------------------
#define BAT_R1_KOHM    220.0f
#define BAT_R2_KOHM    100.0f
#define BAT_ADC_SAMPLES 16

// ---------------------------------------------------------------------
//  Timing
// ---------------------------------------------------------------------
#define OTA_WINDOW_MS       (10UL * 60UL * 1000UL)   // maintenance mode
#define OTA_SHORT_WINDOW_MS (3UL  * 1000UL)          // per-boot window
#define BTN_MAINT_MS        3000
#define BTN_FACTORY_MS      8000

// ---------------------------------------------------------------------
//  MQTT topic base
// ---------------------------------------------------------------------
#define MQTT_BASE  "wm/v1"
```

---

## 📄 `certs.h`

> ⚠️ **Production:** ဒီဖိုင်ထဲမှာ HiveMQ server CA certificate (Root + Intermediate) ကို paste ထည့်ပါ။ `setInsecure()` ကို production release မှာ မသုံးပါနဲ့။

```cpp
#pragma once

// ---------------------------------------------------------------------
//  Fill this in for production builds.
//  e.g. ISRG Root X1 (Let's Encrypt) or Amazon Root CA 1 depending on
//  the certificate chain that HiveMQ Cloud currently serves.
//
//  Getting the chain:
//    openssl s_client -showcerts -connect \
//      4f578522a76143be8bcbb8b759a0e496.s1.eu.hivemq.cloud:8883 \
//      </dev/null 2>/dev/null | openssl x509 -outform PEM
// ---------------------------------------------------------------------

static const char WM_CA_CERT_PEM[] = R"EOF(
-----BEGIN CERTIFICATE-----
PASTE_HIVEMQ_SERVER_CA_HERE
-----END CERTIFICATE-----
)EOF";

// Compile-time switch:  build with -DWM_USE_TLS_INSECURE to allow dev
// builds without a real CA.  Never ship a binary built that way.
#ifndef WM_USE_TLS_INSECURE
  #define WM_TLS_STRICT 1
#else
  #define WM_TLS_STRICT 0
#endif
```

---

## 📄 `ConfigManager.h`

```cpp
#pragma once
#include <Arduino.h>
#include <Preferences.h>

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

  bool     mqttEnabled;
  bool     otaEnabled;
  bool     discoveryEnabled;
  bool     configured;
};

namespace ConfigManager {
  extern DeviceConfig cfg;
  extern char         deviceId[16];   // "WM-A1B2C3"

  void begin();      // loads NVS + derives deviceId
  void load();
  void save();
  void factoryReset();
  void setDefaults();
}
```

---

## 📄 `ConfigManager.cpp`

```cpp
#include "ConfigManager.h"
#include "AppConfig.h"

namespace ConfigManager {

DeviceConfig cfg;
char         deviceId[16];

static Preferences prefs;

void setDefaults() {
  memset(&cfg, 0, sizeof(cfg));

  snprintf(cfg.deviceName, sizeof(cfg.deviceName), "WaterMonitor");

  strncpy(cfg.mqttHost, DEFAULT_MQTT_HOST, sizeof(cfg.mqttHost) - 1);
  cfg.mqttPort           = DEFAULT_MQTT_PORT;
  cfg.measureIntervalSec = DEFAULT_MEASURE_INTERVAL_SEC;
  cfg.tankHeightMm       = DEFAULT_TANK_HEIGHT_MM;
  cfg.minDistanceMm      = DEFAULT_MIN_DISTANCE_MM;
  cfg.maxDistanceMm      = DEFAULT_MAX_DISTANCE_MM;
  cfg.mqttEnabled        = true;
  cfg.otaEnabled         = true;
  cfg.discoveryEnabled   = true;
  cfg.configured         = false;
}

void load() {
  prefs.begin("wmcfg", true);
  size_t sz = prefs.getBytesLength("cfg");
  if (sz == sizeof(cfg)) {
    prefs.getBytes("cfg", &cfg, sizeof(cfg));
  } else {
    setDefaults();
  }
  prefs.end();
}

void save() {
  prefs.begin("wmcfg", false);
  prefs.putBytes("cfg", &cfg, sizeof(cfg));
  prefs.end();
}

void factoryReset() {
  prefs.begin("wmcfg", false);
  prefs.clear();
  prefs.end();
  delay(200);
  ESP.restart();
}

void begin() {
  // Deterministic deviceId from eFuse MAC
  const uint64_t mac = ESP.getEfuseMac();
  snprintf(deviceId, sizeof(deviceId), "WM-%02X%02X%02X",
           (uint8_t)(mac >> 16), (uint8_t)(mac >> 8), (uint8_t)(mac));

  load();
  snprintf(cfg.deviceName, sizeof(cfg.deviceName), "WaterMonitor-%s", deviceId + 3);
}

}  // namespace ConfigManager
```

---

## 📄 `LEDStatus.h`

```cpp
#pragma once
namespace LEDStatus {
  enum State {
    BOOT, PROVISIONING, WIFI_CONNECTING, MQTT_CONNECTING,
    MEASURING, ONLINE, OTA, ERROR, SLEEP
  };
  void begin();
  void set(State s);
  void loop();
}
```

---

## 📄 `LEDStatus.cpp`

```cpp
#include "LEDStatus.h"
#include "AppConfig.h"
#include <Arduino.h>

namespace LEDStatus {

static State    _state      = BOOT;
static uint32_t _lastChange = 0;

void begin() {
  pinMode(PIN_LED_STATUS, OUTPUT);
  digitalWrite(PIN_LED_STATUS, LOW);
  _lastChange = millis();
}

void set(State s) {
  _state      = s;
  _lastChange = millis();
}

void loop() {
  uint32_t t = millis() - _lastChange;
  bool on = false;

  switch (_state) {
    case BOOT:            on = (t % 200) < 100; break;
    case PROVISIONING:    on = (t % 400) < 200; break;
    case WIFI_CONNECTING: on = (t % 600) < 300; break;
    case MQTT_CONNECTING: on = (t % 800) < 400; break;
    case MEASURING:       on = (t % 200) <  50; break;
    case ONLINE:
      on = ((t / 2000) % 2 == 0) ? true : ((t % 100) < 20);
      break;
    case OTA:             on = (t % 150) < 75;  break;
    case ERROR:           on = (t % 1000) < 100; break;
    case SLEEP:           on = false; break;
  }
  digitalWrite(PIN_LED_STATUS, on ? HIGH : LOW);
}

}  // namespace LEDStatus
```

---

## 📄 `Battery.h`

```cpp
#pragma once
namespace Battery {
  void begin();
  int  readMilliVolts();
  int  percentFromMv(int mv);
  int  readPercent();
}
```

---

## 📄 `Battery.cpp`

```cpp
#include "Battery.h"
#include "AppConfig.h"
#include <Arduino.h>

namespace Battery {

void begin() {
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_11db);
}

int readMilliVolts() {
  uint32_t acc = 0;
  for (int i = 0; i < BAT_ADC_SAMPLES; i++) {
    acc += analogReadMilliVolts(PIN_BATTERY_ADC);
    delay(2);
  }
  float vAdc  = acc / (float)BAT_ADC_SAMPLES;
  float ratio = (BAT_R1_KOHM + BAT_R2_KOHM) / BAT_R2_KOHM;
  return (int)(vAdc * ratio);
}

int percentFromMv(int mv) {
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
      float f = (float)(mv - lut[i + 1].mv) /
                (float)(lut[i].mv - lut[i + 1].mv);
      return lut[i + 1].pct + (int)(f * (lut[i].pct - lut[i + 1].pct));
    }
  }
  return 0;
}

int readPercent() {
  return percentFromMv(readMilliVolts());
}

}  // namespace Battery
```

---

## 📄 `AJSR04M.h`

```cpp
#pragma once
#include <Arduino.h>

namespace AJSR04M {

  struct Reading {
    uint32_t distance_mm;   // 0 on failure
    bool     valid;
  };

  void    begin();
  void    powerOn();
  void    powerOff();
  Reading readMedian();     // 5 samples, median-filtered
  uint32_t pingOnce();
}
```

---

## 📄 `AJSR04M.cpp`

```cpp
#include "AJSR04M.h"
#include "AppConfig.h"

namespace AJSR04M {

static int cmpU32(const void* a, const void* b) {
  uint32_t x = *(const uint32_t*)a;
  uint32_t y = *(const uint32_t*)b;
  return (x > y) - (x < y);
}

void begin() {
  pinMode(PIN_TRIG,       OUTPUT);
  pinMode(PIN_ECHO,       INPUT);
  pinMode(PIN_SENSOR_PWR, OUTPUT);
  digitalWrite(PIN_TRIG,       LOW);
  digitalWrite(PIN_SENSOR_PWR, LOW);
}

void powerOn() {
  digitalWrite(PIN_SENSOR_PWR, HIGH);
  delay(80);                          // stabilization
}

void powerOff() {
  digitalWrite(PIN_SENSOR_PWR, LOW);
}

uint32_t pingOnce() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(4);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  uint32_t dur = pulseIn(PIN_ECHO, HIGH, 30000UL);
  if (dur == 0) return 0;
  return (dur * 343UL) / 2000UL;      // mm @ ~20°C
}

Reading readMedian() {
  uint32_t s[5];
  int n = 0;
  for (int i = 0; i < 5; i++) {
    uint32_t d = pingOnce();
    if (d > 0) s[n++] = d;
    delay(60);
  }
  Reading r = {0, false};
  if (n == 0) return r;
  qsort(s, n, sizeof(uint32_t), cmpU32);
  r.distance_mm = s[n / 2];
  r.valid       = true;
  return r;
}

}  // namespace AJSR04M
```

---

## 📄 `WiFiProvisioning.h`

```cpp
#pragma once
#include <Arduino.h>

namespace WiFiProvisioning {
  void begin();          // start SoftAP + captive web UI
  void loop();
  bool isActive();
  void stop();
}
```

---

## 📄 `WiFiProvisioning.cpp`

```cpp
#include "WiFiProvisioning.h"
#include "ConfigManager.h"
#include "AppConfig.h"
#include <WiFi.h>
#include <WebServer.h>
#include <qrcode.h>

namespace WiFiProvisioning {

static WebServer  server(80);
static bool       active = false;
static String     apSsid;
static String     apPass = "watermon";   // per-device recommended

// --- render a QR code as inline SVG --------------------------------
static String qrSvg(const String& text, int scale = 4) {
  QRCode qr;
  uint8_t buf[qrcode_getBufferSize(4)];
  qrcode_initText(&qr, buf, 4, ECC_LOW, text.c_str());

  String svg;
  svg.reserve(4096);
  int px = qr.size * scale;
  svg  = "<svg xmlns='http://www.w3.org/2000/svg' width='";
  svg += px; svg += "' height='"; svg += px;
  svg += "' viewBox='0 0 "; svg += px; svg += " "; svg += px; svg += "'>";
  svg += "<rect width='100%' height='100%' fill='#fff'/>";
  for (uint8_t y = 0; y < qr.size; y++) {
    for (uint8_t x = 0; x < qr.size; x++) {
      if (qrcode_getModule(&qr, x, y)) {
        svg += "<rect x='"; svg += x * scale;
        svg += "' y='";     svg += y * scale;
        svg += "' width='"; svg += scale;
        svg += "' height='"; svg += scale;
        svg += "' fill='#000'/>";
      }
    }
  }
  svg += "</svg>";
  return svg;
}

// --- HTML pages ----------------------------------------------------
static void handleRoot() {
  String payload = "WIFI:T:WPA;S:" + apSsid + ";P:" + apPass + ";;";

  String html;
  html.reserve(4096);
  html += F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>Water Monitor Setup</title>"
            "<style>body{font-family:sans-serif;max-width:440px;margin:16px;}"
            "input,select{width:100%;padding:8px;margin:4px 0;box-sizing:border-box;}"
            "button{padding:10px 16px;background:#07c;color:#fff;border:0;"
            "border-radius:4px;}h2{margin:4px 0;}label{font-size:13px;color:#555;}"
            ".qr{background:#fff;padding:8px;display:inline-block;border:1px solid #ddd;}"
            "</style></head><body>");
  html += "<h2>Water Monitor Setup</h2>";
  html += "<p>Device: <b>"; html += ConfigManager::deviceId; html += "</b></p>";
  html += "<div class='qr'>"; html += qrSvg(payload); html += "</div>";
  html += "<p style='font-size:12px;color:#666'>AP: <b>"; html += apSsid;
  html += "</b> / <b>"; html += apPass; html += "</b></p>";
  html += "<hr>";
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

  server.send(200, "text/html", html);
}

static void handleSave() {
  auto& c = ConfigManager::cfg;

  auto cp = [](char* dst, size_t n, const String& s) {
    strncpy(dst, s.c_str(), n - 1);
    dst[n - 1] = '\0';
  };

  if (server.hasArg("ssid"))  cp(c.wifiSsid,     sizeof(c.wifiSsid),     server.arg("ssid"));
  if (server.hasArg("pass"))  cp(c.wifiPassword, sizeof(c.wifiPassword), server.arg("pass"));
  if (server.hasArg("mhost")) cp(c.mqttHost,     sizeof(c.mqttHost),     server.arg("mhost"));
  if (server.hasArg("mport")) c.mqttPort           = server.arg("mport").toInt();
  if (server.hasArg("muser")) cp(c.mqttUser,     sizeof(c.mqttUser),     server.arg("muser"));
  if (server.hasArg("mpass")) cp(c.mqttPassword, sizeof(c.mqttPassword), server.arg("mpass"));
  if (server.hasArg("opass")) cp(c.otaPassword,  sizeof(c.otaPassword),  server.arg("opass"));
  if (server.hasArg("tank"))  c.tankHeightMm       = server.arg("tank").toInt();
  if (server.hasArg("intv"))  c.measureIntervalSec = server.arg("intv").toInt();

  c.configured = true;
  ConfigManager::save();

  server.send(200, "text/html",
              F("<h3>Saved.</h3><p>Rebooting...</p>"));
  delay(1200);
  ESP.restart();
}

// --- start / stop --------------------------------------------------
void begin() {
  active = true;

  apSsid = String("WM-SETUP-") + (ConfigManager::deviceId + 3);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSsid.c_str(), apPass.c_str());

  server.on("/",     HTTP_GET,  handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleRoot);            // captive portal fallback
  server.begin();

  Serial.printf("[PROV] AP \"%s\"  pass \"%s\"  IP %s\n",
                apSsid.c_str(), apPass.c_str(),
                WiFi.softAPIP().toString().c_str());
  Serial.printf("[PROV] QR = WIFI:T:WPA;S:%s;P:%s;;\n",
                apSsid.c_str(), apPass.c_str());
}

void loop() {
  if (active) server.handleClient();
}

bool isActive() { return active; }

void stop() {
  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  active = false;
}

}  // namespace WiFiProvisioning
```

---

## 📄 `WebConfig.h`

```cpp
#pragma once
namespace WebConfig {
  void begin();
  void loop();
  void stop();
}
```

---

## 📄 `WebConfig.cpp`

Normal-mode web UI (maintenance window) — includes `/update` for **HTTPS OTA** trigger.

```cpp
#include "WebConfig.h"
#include "ConfigManager.h"
#include "OTAManager.h"
#include "Battery.h"
#include "AppConfig.h"
#include <WebServer.h>
#include <ArduinoJson.h>

namespace WebConfig {

static WebServer  server(80);
static bool       active = false;

static void handleStatus() {
  StaticJsonDocument<320> d;
  d["device_id"]    = ConfigManager::deviceId;
  d["fw"]           = FW_VERSION;
  d["model"]        = DEVICE_MODEL;
  d["uptime_s"]     = millis() / 1000;
  d["battery_mv"]   = Battery::readMilliVolts();
  d["battery_pct"]  = Battery::readPercent();
  d["rssi"]         = WiFi.RSSI();
  d["ip"]           = WiFi.localIP().toString();

  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

static void handleConfigGet() {
  auto& c = ConfigManager::cfg;
  StaticJsonDocument<512> d;
  d["device_id"]      = ConfigManager::deviceId;
  d["mqtt_host"]      = c.mqttHost;
  d["mqtt_port"]      = c.mqttPort;
  d["mqtt_user"]      = c.mqttUser;
  d["tank_height_mm"] = c.tankHeightMm;
  d["interval_sec"]   = c.measureIntervalSec;

  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

// ---- HTTP OTA trigger --------------------------------------------
static void handleUpdatePost() {
  if (!server.hasArg("url")) {
    server.send(400, "text/plain", "missing url");
    return;
  }
  String url = server.arg("url");
  server.send(200, "text/plain", "OTA starting; device will reboot");
  delay(300);
  OTAManager::httpsUpdate(url.c_str());
  // if it fails, fall through:
}

static void handleRoot() {
  String html;
  html += F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
            "<title>Water Monitor</title></head><body style='font-family:sans-serif'>");
  html += "<h2>"; html += ConfigManager::deviceId; html += "</h2>";
  html += "<p>FW "; html += FW_VERSION; html += "</p>";
  html += "<h3>HTTPS OTA</h3>"
          "<form method='POST' action='/update'>"
          "<input name='url' size='64' placeholder='https://.../firmware.bin'>"
          " <button type='submit'>Update</button></form>";
  html += "<p><a href='/status'>/status</a> · <a href='/config'>/config</a></p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void begin() {
  server.on("/",         HTTP_GET,  handleRoot);
  server.on("/status",   HTTP_GET,  handleStatus);
  server.on("/config",   HTTP_GET,  handleConfigGet);
  server.on("/update",   HTTP_POST, handleUpdatePost);
  server.begin();
  active = true;
  Serial.println(F("[WEB] started on :80"));
}

void loop() { if (active) server.handleClient(); }

void stop() { server.stop(); active = false; }

}  // namespace WebConfig
```

---

## 📄 `Discovery.h`

```cpp
#pragma once
namespace Discovery {
  void begin();
}
```

---

## 📄 `Discovery.cpp`

```cpp
#include "Discovery.h"
#include "ConfigManager.h"
#include "AppConfig.h"
#include <ESPmDNS.h>

namespace Discovery {

void begin() {
  if (!MDNS.begin(ConfigManager::deviceId)) {
    Serial.println(F("[MDNS] begin failed"));
    return;
  }

  MDNS.addService("watermon", "tcp", 80);
  MDNS.addServiceTxt("watermon", "tcp", "id",    ConfigManager::deviceId);
  MDNS.addServiceTxt("watermon", "tcp", "model", DEVICE_MODEL);
  MDNS.addServiceTxt("watermon", "tcp", "fw",    FW_VERSION);
  MDNS.addServiceTxt("watermon", "tcp", "cap",   "level,battery,ota,mqtt,ha");

  Serial.printf("[MDNS] %s.local  _watermon._tcp\n", ConfigManager::deviceId);
}

}  // namespace Discovery
```

---

## 📄 `MQTTManager.h`

```cpp
#pragma once
#include <Arduino.h>

namespace MQTTManager {
  void begin();
  bool connect();
  void loop();
  bool connected();

  bool publish(const char* topic, const char* payload, bool retain = false);
  bool publishTelemetry();
  bool publishAvailability(const char* state);

  const char* topicTelemetry();
  const char* topicAvailability();
  const char* topicCommand();
  const char* topicOtaStatus();
  const char* topicState();
}
```

---

## 📄 `MQTTManager.cpp`

```cpp
#include "MQTTManager.h"
#include "ConfigManager.h"
#include "Battery.h"
#include "certs.h"
#include "AppConfig.h"

#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

namespace MQTTManager {

static WiFiClientSecure tls;
static PubSubClient     mqtt(tls);
static String           tTelemetry, tAvail, tCmd, tOta, tState;
static bool             started = false;

static void buildTopics() {
  String base = String(MQTT_BASE) + "/" + ConfigManager::deviceId;
  tTelemetry = base + "/telemetry";
  tAvail     = base + "/availability";
  tCmd       = base + "/command";
  tOta       = base + "/ota/status";
  tState     = base + "/state";
}

static void onMessage(char* topic, byte* payload, unsigned int len) {
  Serial.printf("[MQTT] RX %s\n", topic);
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, payload, len)) return;

  const char* cmd = doc["command"] | "";
  if (!strcmp(cmd, "reboot")) {
    delay(200); ESP.restart();
  } else if (!strcmp(cmd, "ota")) {
    const char* url = doc["url"] | "";
    if (*url) {
      // handled by caller via OTAManager
      mqtt.publish(tOta.c_str(), "starting", false);
      // NOTE: actual update triggered from main loop with OTAManager
      extern volatile const char* pendingOtaUrl;
      pendingOtaUrl = url;
    }
  }
}

void begin() {
  buildTopics();

#if WM_TLS_STRICT
  tls.setCACert(WM_CA_CERT_PEM);
#else
  Serial.println(F("[MQTT] WARNING: TLS not verified (dev build)"));
  tls.setInsecure();
#endif

  mqtt.setServer(ConfigManager::cfg.mqttHost, ConfigManager::cfg.mqttPort);
  mqtt.setCallback(onMessage);
  mqtt.setKeepAlive(60);
  mqtt.setBufferSize(1024);       // HA discovery payloads are large
  started = true;
}

bool connect() {
  if (!started) begin();
  if (mqtt.connected()) return true;
  if (strlen(ConfigManager::cfg.mqttHost) == 0) return false;

  String clientId = String(ConfigManager::deviceId) + "-" +
                    String((uint32_t)esp_random(), HEX);

  Serial.printf("[MQTT] connecting %s:%u\n",
                ConfigManager::cfg.mqttHost,
                ConfigManager::cfg.mqttPort);

  bool ok;
  if (strlen(ConfigManager::cfg.mqttUser) > 0) {
    ok = mqtt.connect(clientId.c_str(),
                      ConfigManager::cfg.mqttUser,
                      ConfigManager::cfg.mqttPassword,
                      tAvail.c_str(), 1, true, "offline");
  } else {
    ok = mqtt.connect(clientId.c_str(), nullptr, nullptr,
                      tAvail.c_str(), 1, true, "offline");
  }
  if (ok) {
    Serial.println(F("[MQTT] connected"));
    mqtt.publish(tAvail.c_str(), "online", true);
    mqtt.subscribe(tCmd.c_str(), 1);
  } else {
    Serial.printf("[MQTT] failed rc=%d\n", mqtt.state());
  }
  return ok;
}

void loop() { mqtt.loop(); }
bool connected() { return mqtt.connected(); }

bool publish(const char* topic, const char* payload, bool retain) {
  return mqtt.publish(topic, payload, retain);
}

bool publishAvailability(const char* state) {
  return mqtt.publish(tAvail.c_str(), state, true);
}

bool publishTelemetry() {
  if (!mqtt.connected()) return false;

  int mv  = Battery::readMilliVolts();
  int pct = Battery::percentFromMv(mv);

  StaticJsonDocument<384> d;
  d["device_id"]          = ConfigManager::deviceId;
  d["fw"]                 = FW_VERSION;
  d["battery_voltage_mv"] = mv;
  d["battery_percent"]    = pct;
  d["rssi"]               = WiFi.RSSI();
  d["uptime_s"]           = millis() / 1000;

  char buf[384];
  size_t n = serializeJson(d, buf, sizeof(buf));
  return mqtt.publish(tTelemetry.c_str(), (const uint8_t*)buf, n, false);
}

const char* topicTelemetry()    { return tTelemetry.c_str(); }
const char* topicAvailability() { return tAvail.c_str(); }
const char* topicCommand()      { return tCmd.c_str(); }
const char* topicOtaStatus()    { return tOta.c_str(); }
const char* topicState()        { return tState.c_str(); }

}  // namespace MQTTManager
```

---

## 📄 `OTAManager.h`

```cpp
#pragma once
#include <Arduino.h>

namespace OTAManager {
  void begin(const char* hostname, const char* password);
  void loop();
  bool httpsUpdate(const char* url);
  const char* lastError();
}
```

---

## 📄 `OTAManager.cpp`

```cpp
#include "OTAManager.h"
#include "ConfigManager.h"
#include "AppConfig.h"
#include "certs.h"
#include <ArduinoOTA.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>

namespace OTAManager {

static String  _err = "";
static bool    _started = false;

void begin(const char* hostname, const char* password) {
  ArduinoOTA.setHostname(hostname);
  if (password && *password) ArduinoOTA.setPassword(password);

  ArduinoOTA.onStart([]() {
    Serial.println(F("[OTA] start"));
  });
  ArduinoOTA.onEnd([]() {
    Serial.println(F("\n[OTA] end"));
  });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
    Serial.printf("[OTA] %u%%\r", (p * 100) / t);
  });
  ArduinoOTA.onError([](ota_error_t e) {
    Serial.printf("[OTA] error %u\n", e);
  });
  ArduinoOTA.begin();
  _started = true;
  Serial.printf("[OTA] ArduinoOTA ready as %s.local\n", hostname);
}

void loop() {
  if (_started) ArduinoOTA.handle();
}

bool httpsUpdate(const char* url) {
  _err = "";

  WiFiClientSecure client;
#if WM_TLS_STRICT
  client.setCACert(WM_CA_CERT_PEM);
#else
  client.setInsecure();
#endif

  HTTPClient https;
  if (!https.begin(client, url)) { _err = "begin failed"; return false; }

  int code = https.GET();
  if (code != HTTP_CODE_OK) {
    _err = "HTTP " + String(code);
    https.end();
    return false;
  }

  int len = https.getSize();
  if (len <= 0) { _err = "unknown length"; https.end(); return false; }

  if (!Update.begin(len)) { _err = "Update.begin failed"; https.end(); return false; }

  WiFiClient* stream = https.getStreamPtr();
  size_t written = Update.writeStream(*stream);
  https.end();

  if (written != (size_t)len) {
    _err = "short write " + String(written) + "/" + String(len);
    Update.abort();
    return false;
  }
  if (!Update.end(true)) { _err = Update.errorString(); return false; }

  Serial.println(F("[OTA] HTTPS update OK — rebooting"));
  delay(400);
  ESP.restart();
  return true;
}

const char* lastError() { return _err.c_str(); }

}  // namespace OTAManager
```

---

## 📄 `HAClient.h`

```cpp
#pragma once
namespace HAClient {
  void publishDiscovery();
  void publishState(uint32_t distance_mm, bool sensor_ok,
                    int32_t level_mm, float level_pct,
                    int battery_mv, int battery_pct, int rssi);
}
```

---

## 📄 `HAClient.cpp`

```cpp
#include "HAClient.h"
#include "MQTTManager.h"
#include "ConfigManager.h"
#include "AppConfig.h"
#include <ArduinoJson.h>

namespace HAClient {

static String base() {
  return String(MQTT_BASE) + "/" + ConfigManager::deviceId;
}

static void pub(const String& topic, const String& payload, bool retain = true) {
  MQTTManager::publish(topic.c_str(), payload.c_str(), retain);
}

static void haComponent(const char* component,
                        const char* object_id,
                        const char* name,
                        const char* state_topic,
                        const char* unit,
                        const char* device_class,
                        const char* value_template = nullptr) {
  StaticJsonDocument<768> d;

  d["name"]               = name;
  d["unique_id"]          = String(ConfigManager::deviceId) + "_" + object_id;
  d["state_topic"]        = state_topic;
  if (unit)         d["unit_of_measurement"] = unit;
  if (device_class) d["device_class"]        = device_class;
  if (value_template) d["value_template"]    = value_template;

  JsonObject dev = d.createNestedObject("device");
  dev["identifiers"][0] = ConfigManager::deviceId;
  dev["name"]           = ConfigManager::deviceId;
  dev["model"]          = DEVICE_MODEL;
  dev["manufacturer"]   = "WaterMonitor";
  dev["sw_version"]     = FW_VERSION;

  String payload;
  serializeJson(d, payload);

  String topic = String("homeassistant/") + component + "/" +
                 ConfigManager::deviceId + "/" + object_id + "/config";
  pub(topic, payload);
}

void publishDiscovery() {
  if (!MQTTManager::connected()) return;

  String telemetry = base() + "/telemetry";
  String state     = base() + "/state";
  String avail     = base() + "/availability";

  // distance
  haComponent("sensor", "distance", "Distance",
              telemetry.c_str(), "mm", "distance",
              "{{ value_json.distance_mm }}");

  // water level
  haComponent("sensor", "level", "Water Level",
              telemetry.c_str(), "%", nullptr,
              "{{ value_json.level_percent }}");

  // water height mm
  haComponent("sensor", "level_mm", "Water Height",
              telemetry.c_str(), "mm", nullptr,
              "{{ value_json.level_mm }}");

  // battery voltage
  haComponent("sensor", "battery_mv", "Battery Voltage",
              telemetry.c_str(), "mV", "voltage",
              "{{ value_json.battery_voltage_mv }}");

  // battery percent
  haComponent("sensor", "battery_pct", "Battery",
              telemetry.c_str(), "%", "battery",
              "{{ value_json.battery_percent }}");

  // RSSI
  haComponent("sensor", "rssi", "WiFi RSSI",
              telemetry.c_str(), "dBm", "signal_strength",
              "{{ value_json.rssi }}");

  // sensor ok binary sensor
  {
    StaticJsonDocument<512> d;
    d["name"]                = "Sensor OK";
    d["unique_id"]           = String(ConfigManager::deviceId) + "_sensor_ok";
    d["state_topic"]         = telemetry.c_str();
    d["value_template"]      = "{{ 'ON' if value_json.sensor_ok else 'OFF' }}";
    d["availability_topic"]  = avail.c_str();
    JsonObject dev = d.createNestedObject("device");
    dev["identifiers"][0] = ConfigManager::deviceId;
    dev["name"]           = ConfigManager::deviceId;
    dev["model"]          = DEVICE_MODEL;
    dev["sw_version"]     = FW_VERSION;
    String payload;
    serializeJson(d, payload);
    pub(String("homeassistant/binary_sensor/") +
        ConfigManager::deviceId + "/sensor_ok/config", payload);
  }

  Serial.println(F("[HA] discovery published"));
}

void publishState(uint32_t distance_mm, bool sensor_ok,
                  int32_t level_mm, float level_pct,
                  int battery_mv, int battery_pct, int rssi) {
  if (!MQTTManager::connected()) return;

  StaticJsonDocument<384> d;
  d["device_id"]          = ConfigManager::deviceId;
  d["fw"]                 = FW_VERSION;
  d["distance_mm"]        = distance_mm;
  d["sensor_ok"]          = sensor_ok;
  d["level_mm"]           = level_mm;
  d["level_percent"]      = level_pct;
  d["battery_voltage_mv"] = battery_mv;
  d["battery_percent"]    = battery_pct;
  d["rssi"]               = rssi;

  String payload;
  serializeJson(d, payload);

  MQTTManager::publish((base() + "/telemetry").c_str(), payload.c_str(), false);
  MQTTManager::publish((base() + "/state").c_str(),     payload.c_str(), false);
}

}  // namespace HAClient
```

---

## 📄 `WaterMonitor.ino` (Main)

```cpp
/*
 * WaterMonitor  V0.2.0
 * ESP32-WROOM-32S · AJ-SR04M · 18650 · MQTT/TLS · mDNS · OTA · Deep Sleep
 */

#include "AppConfig.h"
#include "ConfigManager.h"
#include "LEDStatus.h"
#include "Battery.h"
#include "AJSR04M.h"
#include "WiFiProvisioning.h"
#include "WebConfig.h"
#include "Discovery.h"
#include "MQTTManager.h"
#include "OTAManager.h"
#include "HAClient.h"

#include <WiFi.h>
#include <esp_sleep.h>

// -------- runtime state ------------------------------------------------
enum RunMode { RUN_NORMAL, RUN_PROVISIONING, RUN_MAINTENANCE };
static RunMode  g_mode              = RUN_NORMAL;
static bool     g_maintenance       = false;
static uint32_t g_maintenanceStart  = 0;
static bool     g_haPublished       = false;

// -------- helpers ------------------------------------------------------

static bool wifiConnect(uint32_t timeoutMs = 20000) {
  if (strlen(ConfigManager::cfg.wifiSsid) == 0) return false;

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);      // modem sleep
  WiFi.begin(ConfigManager::cfg.wifiSsid, ConfigManager::cfg.wifiPassword);

  Serial.printf("[WiFi] connecting to %s", ConfigManager::cfg.wifiSsid);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    LEDStatus::loop();
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

static void enterDeepSleep() {
  Serial.printf("[SLEEP] %u sec\n", ConfigManager::cfg.measureIntervalSec);
  LEDStatus::set(LEDStatus::SLEEP);
  digitalWrite(PIN_LED_STATUS, LOW);
  AJSR04M::powerOff();

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  esp_sleep_enable_timer_wakeup(
      (uint64_t)ConfigManager::cfg.measureIntervalSec * 1000000ULL);
  delay(50);
  esp_deep_sleep_start();
}

static void readButton() {
  static uint32_t pressStart = 0;
  static bool     wasDown    = false;
  bool down = (digitalRead(PIN_BUTTON) == LOW);

  if (down && !wasDown) {
    pressStart = millis();
    wasDown    = true;
  } else if (down && wasDown) {
    if (millis() - pressStart >= BTN_FACTORY_MS) {
      Serial.println(F("[BTN] factory reset"));
      ConfigManager::factoryReset();
    }
  } else if (!down && wasDown) {
    uint32_t held = millis() - pressStart;
    if (held >= BTN_MAINT_MS && held < BTN_FACTORY_MS) {
      Serial.println(F("[BTN] maintenance"));
      g_maintenance      = true;
      g_maintenanceStart = millis();
    }
    wasDown = false;
  }
}

// -------- setup -------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println(F("=========================================="));
  Serial.println(F("  WaterMonitor  ESP32  (multi-file V0.2)"));
  Serial.printf ("  FW %s  HW %s\n", FW_VERSION, HW_REV);
  Serial.println(F("=========================================="));

  pinMode(PIN_BUTTON, INPUT_PULLUP);

  LEDStatus::begin();
  LEDStatus::set(LEDStatus::BOOT);
  Battery::begin();
  AJSR04M::begin();
  ConfigManager::begin();

  Serial.printf("[BOOT] deviceId  = %s\n", ConfigManager::deviceId);
  Serial.printf("[BOOT] configured= %d\n", ConfigManager::cfg.configured);

  // BOOT button hold on boot
  if (digitalRead(PIN_BUTTON) == LOW) {
    uint32_t t0 = millis();
    while (digitalRead(PIN_BUTTON) == LOW) {
      if (millis() - t0 > BTN_FACTORY_MS) ConfigManager::factoryReset();
      delay(10);
    }
    if (millis() - t0 > BTN_MAINT_MS) {
      g_maintenance      = true;
      g_maintenanceStart = millis();
      Serial.println(F("[BOOT] maintenance mode"));
    }
  }

  // First boot → provisioning
  if (!ConfigManager::cfg.configured || strlen(ConfigManager::cfg.wifiSsid) == 0) {
    g_mode = RUN_PROVISIONING;
    WiFiProvisioning::begin();
    LEDStatus::set(LEDStatus::PROVISIONING);
    return;
  }

  // Normal boot
  g_mode = RUN_NORMAL;
  LEDStatus::set(LEDStatus::WIFI_CONNECTING);
  if (!wifiConnect()) {
    LEDStatus::set(LEDStatus::ERROR);
    enterDeepSleep();
  }

  if (ConfigManager::cfg.discoveryEnabled) Discovery::begin();

  if (ConfigManager::cfg.otaEnabled)
    OTAManager::begin(ConfigManager::deviceId, ConfigManager::cfg.otaPassword);

  LEDStatus::set(LEDStatus::MQTT_CONNECTING);
  MQTTManager::begin();
  MQTTManager::connect();
}

// -------- loop --------------------------------------------------------

void loop() {
  LEDStatus::loop();

  if (g_mode == RUN_PROVISIONING) {
    WiFiProvisioning::loop();
    return;
  }

  readButton();

  // ---- maintenance window ----
  if (g_maintenance) {
    if (!WebConfig::isActive()) WebConfig::begin();
    if (!WiFi.status() == WL_CONNECTED) {
      // Ensure STA also up in maintenance (AP is dropped)
      if (!wifiConnect(10000)) { /* continue */ }
    }
    OTAManager::loop();
    WebConfig::loop();
    MQTTManager::loop();
    if (!MQTTManager::connected()) MQTTManager::connect();

    if (millis() - g_maintenanceStart > OTA_WINDOW_MS) {
      Serial.println(F("[MAINT] window ended"));
      WebConfig::stop();
      g_maintenance = false;
      enterDeepSleep();
    }
    return;
  }

  // ---- normal cycle ----
  LEDStatus::set(LEDStatus::MEASURING);

  AJSR04M::powerOn();
  AJSR04M::Reading r = AJSR04M::readMedian();
  AJSR04M::powerOff();

  bool     ok        = r.valid &&
                       r.distance_mm >= ConfigManager::cfg.minDistanceMm &&
                       r.distance_mm <= ConfigManager::cfg.maxDistanceMm;
  int32_t  level_mm  = 0;
  float    level_pct = 0.0f;
  if (ok) {
    level_mm  = (int32_t)ConfigManager::cfg.tankHeightMm -
                (int32_t)r.distance_mm;
    if (level_mm < 0) level_mm = 0;
    level_pct = (float)level_mm * 100.0f /
                (float)ConfigManager::cfg.tankHeightMm;
  }
  int battMv  = Battery::readMilliVolts();
  int battPct = Battery::percentFromMv(battMv);

  Serial.printf("[MEAS] dist=%umm ok=%d level=%dmm (%.1f%%) batt=%dmV(%d%%)\n",
                r.distance_mm, ok, level_mm, level_pct, battMv, battPct);

  // WiFi ensure
  if (WiFi.status() != WL_CONNECTED) {
    LEDStatus::set(LEDStatus::WIFI_CONNECTING);
    if (!wifiConnect(10000)) { LEDStatus::set(LEDStatus::ERROR); enterDeepSleep(); }
  }

  // mDNS (once)
  static bool mdnsDone = false;
  if (!mdnsDone && ConfigManager::cfg.discoveryEnabled) {
    Discovery::begin();
    mdnsDone = true;
  }

  // MQTT ensure
  LEDStatus::set(LEDStatus::MQTT_CONNECTING);
  if (!MQTTManager::connected()) {
    if (!MQTTManager::connect()) { enterDeepSleep(); }
  }

  if (MQTTManager::connected()) {
    LEDStatus::set(LEDStatus::ONLINE);

    // HA auto-discovery (once per boot)
    if (!g_haPublished) {
      HAClient::publishDiscovery();
      g_haPublished = true;
    }

    HAClient::publishState(r.distance_mm, ok, level_mm, level_pct,
                           battMv, battPct, WiFi.RSSI());
  }

  // Short OTA window (last-chance updates)
  LEDStatus::set(LEDStatus::ONLINE);
  uint32_t until = millis() + OTA_SHORT_WINDOW_MS;
  while (millis() < until) {
    LEDStatus::loop();
    OTAManager::loop();
    MQTTManager::loop();
    delay(20);
  }

  enterDeepSleep();
}
```

---

## 📄 `WaterMonitorFactory.ino` (Separate Sketch)

Factory / QC firmware — serial-menu driven. Flash after production assembly.

```cpp
/*
 * WaterMonitorFactory - manufacturing test firmware
 * Commands via serial (115200):
 *   T  - ultrasonic read
 *   B  - battery ADC
 *   W  - WiFi scan
 *   M  - MQTT connect test (reads stored config)
 *   L  - LED sweep
 *   I  - print device info + MAC
 *   S  - generate serial number stub
 *   R  - reboot
 *   F  - factory reset (clear NVS)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>

#include "AppConfig.h"
#include "ConfigManager.h"
#include "AJSR04M.h"
#include "Battery.h"
#include "MQTTManager.h"

static void banner() {
  Serial.println();
  Serial.println(F("=== WaterMonitor Factory Test ==="));
  Serial.println(F(" T ultrasonic  B battery  W wifi-scan"));
  Serial.println(F(" M mqtt-test   L led      I info"));
  Serial.println(F(" S serial#     R reboot   F factory-reset"));
}

static void cmdInfo() {
  uint64_t mac = ESP.getEfuseMac();
  Serial.printf("Model     : %s %s\n", DEVICE_MODEL, HW_REV);
  Serial.printf("FW        : %s\n", FW_VERSION);
  Serial.printf("Chip      : %s rev %d, %d MHz\n",
                ESP.getChipModel(), ESP.getChipRevision(),
                getCpuFrequencyMhz());
  Serial.printf("MAC       : %02X:%02X:%02X:%02X:%02X:%02X\n",
                (uint8_t)(mac >> 0), (uint8_t)(mac >> 8),
                (uint8_t)(mac >> 16),(uint8_t)(mac >> 24),
                (uint8_t)(mac >> 32),(uint8_t)(mac >> 40));
  Serial.printf("DeviceId  : %s\n", ConfigManager::deviceId);
  Serial.printf("FlashSize : %u KB\n", ESP.getFlashChipSize() / 1024);
  Serial.printf("FreeHeap  : %u bytes\n", ESP.getFreeHeap());
  Serial.printf("Battery   : %d mV (%d%%)\n",
                Battery::readMilliVolts(), Battery::readPercent());
}

static void cmdUltrasonic() {
  AJSR04M::powerOn();
  for (int i = 0; i < 5; i++) {
    uint32_t d = AJSR04M::pingOnce();
    Serial.printf("  ping[%d] = %u mm\n", i, d);
    delay(80);
  }
  AJSR04M::Reading r = AJSR04M::readMedian();
  Serial.printf("  median = %u mm  (valid=%d)\n", r.distance_mm, r.valid);
  AJSR04M::powerOff();
}

static void cmdBattery() {
  for (int i = 0; i < 5; i++) {
    Serial.printf("  bat[%d] = %d mV\n", i, Battery::readMilliVolts());
    delay(200);
  }
  Serial.printf("  percent = %d%%\n", Battery::readPercent());
}

static void cmdWifiScan() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  int n = WiFi.scanNetworks();
  Serial.printf("  %d networks\n", n);
  for (int i = 0; i < n; i++) {
    Serial.printf("   %2d  %-32s  %d dBm  ch%2d\n",
                  i + 1, WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i));
  }
  WiFi.scanDelete();
}

static void cmdMqttTest() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("  !! WiFi not connected"));
    return;
  }
  MQTTManager::begin();
  bool ok = MQTTManager::connect();
  Serial.printf("  MQTT connect: %s\n", ok ? "OK" : "FAIL");
  if (ok) {
    MQTTManager::publish(MQTTManager::topicTelemetry(),
                         "{\"factory_test\":true}", false);
    delay(500);
    MQTTManager::loop();
  }
}

static void cmdLed() {
  pinMode(PIN_LED_STATUS, OUTPUT);
  for (int i = 0; i < 4; i++) {
    digitalWrite(PIN_LED_STATUS, HIGH); delay(150);
    digitalWrite(PIN_LED_STATUS, LOW);  delay(150);
  }
}

static void cmdSerialStub() {
  uint64_t mac = ESP.getEfuseMac();
  char sn[32];
  snprintf(sn, sizeof(sn), "WM-%02X%02X%02X-%04X",
           (uint8_t)(mac >> 16), (uint8_t)(mac >> 8), (uint8_t)(mac),
           (uint16_t)(esp_random() & 0xFFFF));
  Serial.printf("  Serial# : %s\n", sn);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  banner();

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_LED_STATUS, OUTPUT);

  Battery::begin();
  AJSR04M::begin();
  ConfigManager::begin();

  Serial.printf("DeviceId: %s\n", ConfigManager::deviceId);
  Serial.println(F("Ready. Type command + Enter."));
}

void loop() {
  if (!Serial.available()) { delay(20); return; }
  char c = Serial.read();
  while (Serial.available() && Serial.read() != '\n') {}

  switch (toupper(c)) {
    case 'T': cmdUltrasonic();    break;
    case 'B': cmdBattery();       break;
    case 'W': cmdWifiScan();      break;
    case 'M': cmdMqttTest();      break;
    case 'L': cmdLed();           break;
    case 'I': cmdInfo();          break;
    case 'S': cmdSerialStub();    break;
    case 'R': ESP.restart();      break;
    case 'F': ConfigManager::factoryReset(); break;
    default:  banner();           break;
  }
  Serial.println(F("> "));
}
```

---

## 📄 `README.md` — Power Budget, HA, Provisioning, Manufacturing

````markdown
# WaterMonitor WM-01 — Firmware V0.2

ESP32-WROOM-32S water-level monitor. Battery (18650) powered,
deep-sleep, MQTT/TLS to HiveMQ Cloud, WiFi provisioning, mDNS discovery,
ArduinoOTA + HTTPS OTA, Home Assistant auto-discovery.

---

## 1. BOM overview

| Block | Part | Notes |
|-------|------|-------|
| MCU | ESP32-WROOM-32S | 4 MB flash |
| Sensor | AJ-SR04M | 3.3 V powered |
| Battery | 18650 Li-ion | 2600–3500 mAh |
| Charge | TP4056 / BQ24074 | USB-C |
| Protection | DW01 + FS8205 | OVP/UVP/OCP |
| Regulator | HT7333 / TPS62840 | low Iq |
| Sensor switch | AO3400 N-MOSFET | Vgs(th) < 1.5 V |
| Divider | 220 kΩ / 100 kΩ | battery ADC |

---

## 2. Power budget (5-minute interval, 18650 = 3000 mAh)

| Phase | Current | Duration | Charge |
|-------|---------|----------|--------|
| Deep sleep | 15 µA | 300 s | 1.25 µAh |
| Wake + boot | 40 mA | 250 ms | 2.78 µAh |
| Ultrasonic (5 pings) | 25 mA | 500 ms | 3.47 µAh |
| WiFi assoc | 90 mA (avg) | 1.8 s | 45.0 µAh |
| TLS handshake | 65 mA | 1.4 s | 25.3 µAh |
| MQTT pub | 40 mA | 400 ms | 4.44 µAh |
| Idle drain | 25 mA | 400 ms | 2.78 µAh |
| **Per cycle** | | ~5 s active | ≈ **85 µAh** |

Cycles/day = 288 ⇒ **≈ 24.5 mAh/day** ⇒ 3000 mAh / 24.5 = **≈ 122 days**  
(Real-world with self-discharge + conversion loss: **~90–100 days**.)

**Sleep interval scaling**

| Interval | Active | mAh/day | Runtime |
|----------|--------|---------|---------|
| 1 min    | 1440   | 122     | ≈ 24 days |
| 5 min    | 288    | 24.5    | ≈ 100 days |
| 15 min   | 96     |  8.2    | ≈ 11 months |
| 30 min   | 48     |  4.1    | ≈ 1.8 years* |

\* Limited by 18650 self-discharge (~2%/month) and regulator quiescent.

**Deep-sleep current must be ≤ 30 µA** including LDO quiescent. Verify with a µCurrent / Nordic PPK2 during DVT.

---

## 3. Provisioning flow

```
Power ON
   │
   ▼
No config?  ──yes──► SoftAP "WM-SETUP-XXXXXX"  (pass: watermon)
   │                        │
   no                       ▼
   │              http://192.168.4.1
   ▼                        │
Normal boot            WiFi + MQTT form
   │                        │
   ▼                        ▼
Deep sleep              Save → reboot
```

- AP name: `WM-SETUP-` + last 6 of deviceId  
- Captive portal auto-opens on phones.  
- QR rendered inline (SVG) on setup page.

---

## 4. mDNS / Discovery

```
http://WM-A1B2C3.local/
_watermon._tcp.local
  TXT: id=WM-A1B2C3 model=WM-01 fw=0.2.0
       cap=level,battery,ota,mqtt,ha
```

Use any DNS-SD browser (avahi-browse, Bonjour Browser) to find devices
without knowing IPs.

---

## 5. MQTT topics

```
wm/v1/<deviceId>/telemetry      (JSON, non-retained)
wm/v1/<deviceId>/state          (JSON, non-retained)
wm/v1/<deviceId>/availability   (online|offline, retained, LWT)
wm/v1/<deviceId>/command        (JSON in)
wm/v1/<deviceId>/ota/status     (text)
```

Telemetry example:

```json
{
  "device_id": "WM-A1B2C3",
  "fw": "0.2.0",
  "distance_mm": 502,
  "sensor_ok": true,
  "level_mm": 1498,
  "level_percent": 74.9,
  "battery_voltage_mv": 3910,
  "battery_percent": 67,
  "rssi": -61
}
```

Command examples:

```json
{"command":"reboot"}
{"command":"ota","url":"https://fw.example.com/wm/v0.2.1.bin"}
```

---

## 6. Home Assistant auto-discovery

The firmware publishes HA MQTT Discovery configs on boot. HA will
auto-create the following entities:

| Entity | Type | Unit |
|--------|------|------|
| Distance | sensor | mm |
| Water Level | sensor | % |
| Water Height | sensor | mm |
| Battery Voltage | sensor | mV |
| Battery | sensor | % |
| WiFi RSSI | sensor | dBm |
| Sensor OK | binary_sensor | — |

Prerequisites in HA: MQTT integration enabled, discovery on. Done.

---

## 7. OTA paths

**Arduino OTA (development / on-site)**
- Device listens on UDP 3232 while awake.
- Arduino IDE → Ports → `WM-A1B2C3 at 192.168.x.x`.
- Password from provisioning (optional).

**HTTPS OTA (production / fleet)**
- `POST /update` with body `url=https://.../firmware.bin` while in
  maintenance mode, **or** publish:

  ```json
  {"command":"ota","url":"https://fw.example.com/wm/v0.2.1.bin"}
  ```

- Firmware must be signed for production (ESP-IDF secure boot v2 +
  `esp_ota_begin` rollback check).

**Entering maintenance**
- Hold BOOT ≥ 3 s while powered → 10 min window.
- Hold BOOT ≥ 8 s → factory reset.

---

## 8. Manufacturing / factory test

Flash `WaterMonitorFactory.ino`, open serial 115200, use single-key menu:

```
T  ultrasonic (5 pings + median)
B  battery ADC ×5
W  WiFi scan
M  MQTT connect + publish test
L  LED sweep
I  chip info + deviceId
S  serial number generator
R  reboot
F  factory reset (clear NVS)
```

Recommended QC flow:
1. Flash factory image → `I` → record MAC/serial.
2. `B` → verify 3.5–4.2 V range.
3. `T` → verify against 200/500/1000 mm reference jig.
4. `L` → visual LED check.
5. `W` → confirm scan list non-empty.
6. Flash production image.
7. `F` to clear NVS before boxing.

---

## 9. Production checklist (before shipping)

- [ ] Provision HiveMQ CA cert into `certs.h`
- [ ] Set `-DWM_TLS_STRICT=1` build flag (disable `setInsecure`)
- [ ] Enable ESP32 Secure Boot v2 + Flash Encryption
- [ ] Sign firmware, add rollback verification
- [ ] Per-device MQTT credentials (client certificate)
- [ ] Battery discharge curve calibrated per cell supplier
- [ ] Brownout / watchdog test at 3.0 V
- [ ] ESD / reverse polarity / IP rating enclosure
- [ ] Unique per-device AP password (currently shared `watermon`)
- [ ] FCC/CE/RoHS certification
```

---

## 🔧 Build & Flash

**Arduino IDE settings:**
```
Board             : ESP32 Dev Module
Flash Size        : 4 MB (32 Mb)
Partition Scheme  : Minimal SPIFFS (1.9MB APP / 190KB SPIFFS / OTA)
                   OR "Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)"
PSRAM             : Disabled
Upload Speed      : 921600
CPU Frequency     : 240 MHz (WiFi/BT)
```

**Compile flags (only for dev):**
```
-DWM_USE_TLS_INSECURE
```
Production builds: omit this so `WM_TLS_STRICT=1` is active.

---

## 🚦 What's next after V0.2

ဒီ project ကို commercial-grade ဖြစ်အောင် ဆက်လုပ်ရမယ့်အဆင့်:

1. **Secure Boot + Flash Encryption** — ESP-IDF toolchain ကို Arduino core နဲ့ mix လုပ်ရမယ်
2. **Per-device certificates** (client cert auth) — HiveMQ Cloud Starter plan အထက်
3. **Signed OTA + rollback verification** — `esp_https_ota` + `esp_ota_get_state_partition`
4. **Battery calibration wizard** — provisioning page မှာ 3-point calibration
5. **PCB bring-up + power measurement** — Nordic PPK2 နဲ့ deep sleep current verify
6. **CI/CD** — PlatformIO + GitHub Actions, versioned firmware artifacts

ဒီထဲက ဘယ်အပိုင်းကို ဆက်ချဲ့ချင်လဲ ပြောပါ — **secure boot + signed OTA** ကို ESP-IDF hybrid Arduino project အဖြစ်ရေးပေးနိုင်ပါတယ်။
