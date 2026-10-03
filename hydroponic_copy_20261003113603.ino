/*
 * Smart Aquarium / Hydroponics — Arduino IDE version
 * Board: LOLIN(WEMOS) D1 R2 & mini  (ESP8266)
 *
 * Пины:
 *   D0  -> PWM  помпа гидропоники
 *   D1  -> PWM  серво (кормушка)
 *   D2  -> I2C SDA
 *   D3  -> Dallas DS18B20
 *   D4  -> I2C SCL
 *   D5  -> PWM  свет аквариум
 *   D6  -> PWM  помпа второй башни (рассада)
 *   D7  -> датчик низкого уровня воды (INPUT_PULLUP)
 *   D8  -> PWM  свет гидропоники
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <Servo.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ==================== НАСТРОЙКИ ====================
const char* WIFI_SSID     = "Wifi";
const char* WIFI_PASSWORD = "pass";
const char* OTA_PASSWORD  = "pass";
const char* HOSTNAME      = "smart-aquarium";

// ==================== ПИНЫ ====================
#define PIN_PWM_PUMP_HYDRO   D0
#define PIN_SERVO            D1
#define PIN_ONEWIRE          D3
#define PIN_PWM_LIGHT_AQUA   D5
#define PIN_PWM_PUMP_TOWER   D6
#define PIN_WATER_LEVEL      D7
#define PIN_PWM_LIGHT_HYDRO  D8

// ==================== ОБЪЕКТЫ ====================
OneWire oneWire(PIN_ONEWIRE);
DallasTemperature ds18b20(&oneWire);
Servo feedServo;
ESP8266WebServer server(80);

// ==================== СОСТОЯНИЯ ====================
bool  pumpTowerRunning  = false;
unsigned long pumpTowerStopAt = 0;

bool  feedRunning       = false;
unsigned long feedStepAt = 0;
int   feedStep          = 0;

bool  waterLevelLow     = false;

unsigned long lastTempRead  = 0;
unsigned long lastWifiPrint = 0;
float waterTempC = NAN;

// ==================== СЕРВО (кормушка) ====================
// Позиции: -10% ≈ покой, -60% ≈ открыто (эмуляция level из ESPHome)
// Servo.write принимает 0..180. ESPHome level: -100%..100% -> 0..180
int levelToAngle(float levelPercent) {
  // -100% -> 0°, 0% -> 90°, +100% -> 180°
  float a = (levelPercent + 100.0f) * 0.9f;
  if (a < 0)   a = 0;
  if (a > 180) a = 180;
  return (int)a;
}

void servoWriteLevel(float level) {
  feedServo.attach(PIN_SERVO);
  feedServo.write(levelToAngle(level));
}

void servoDetach() {
  feedServo.detach();
}

// ==================== PWM ХЕЛПЕРЫ ====================
void setPwm(uint8_t pin, float percent) {
  if (percent < 0)   percent = 0;
  if (percent > 100) percent = 100;
  int duty = (int)(percent / 100.0f * 1023.0f);
  analogWrite(pin, duty);
}

// ==================== КОРМУШКА ====================
void startFeed() {
  feedRunning = true;
  feedStep = 0;
  feedStepAt = millis();
  Serial.println("[FEED] start");
}

void feedLoop() {
  if (!feedRunning) return;
  unsigned long now = millis();
  switch (feedStep) {
    case 0:  // открыть
      servoWriteLevel(-60.0f);
      feedStep = 1;
      feedStepAt = now + 1000;
      break;
    case 1:
      if (now >= feedStepAt) {
        servoWriteLevel(-10.0f);
        feedStep = 2;
        feedStepAt = now + 1000;
      }
      break;
    case 2:
      if (now >= feedStepAt) {
        servoDetach();
        feedRunning = false;
        feedStep = 3;
        Serial.println("[FEED] done");
      }
      break;
    default:
      feedRunning = false;
      break;
  }
}

// ==================== ПОМПА ВТОРОЙ БАШНИ ====================
void startPumpTower() {
  if (pumpTowerRunning) return;
  pumpTowerRunning = true;
  setPwm(PIN_PWM_PUMP_TOWER, 90.0f);
  pumpTowerStopAt = millis() + 15000UL;
  Serial.println("[PUMP TOWER] on");
}

void pumpTowerLoop() {
  if (pumpTowerRunning && millis() >= pumpTowerStopAt) {
    setPwm(PIN_PWM_PUMP_TOWER, 0.0f);
    pumpTowerRunning = false;
    Serial.println("[PUMP TOWER] off");
  }
}

// ==================== ВЕБ-СЕРВЕР ====================
String htmlPage() {
  String s = F("<!DOCTYPE html><html><head><meta charset='utf-8'>");
  s += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  s += F("<title>Smart Aquarium</title></head><body>");
  s += F("<h2>Smart Aquarium</h2>");
  s += F("<p>Uptime: "); s += String(millis() / 1000); s += F(" s</p>");
  s += F("<p>Water temp: ");
  if (isnan(waterTempC)) s += F("n/a");
  else { s += String(waterTempC, 2); s += F(" &deg;C"); }
  s += F("</p>");
  s += F("<p>Water level low: "); s += waterLevelLow ? F("YES") : F("no"); s += F("</p>");
  s += F("<p>Pump tower: "); s += pumpTowerRunning ? F("ON") : F("off"); s += F("</p>");
  s += F("<hr>");
  s += F("<form action='/feed' method='get'><button>Feed fish</button></form>");
  s += F("<form action='/pump' method='get'><button>Run pump tower 15s</button></form>");
  s += F("<form action='/restart' method='get'><button>Restart</button></form>");
  s += F("<hr><form action='/light' method='get'>");
  s += F("Hydro light %: <input name='hydro' type='number' min='0' max='100' value='0'>");
  s += F("<input type='submit'></form>");
  s += F("<form action='/light' method='get'>");
  s += F("Aqua light %: <input name='aqua' type='number' min='0' max='100' value='0'>");
  s += F("<input type='submit'></form>");
  s += F("</body></html>");
  return s;
}

void handleRoot()  { server.send(200, "text/html", htmlPage()); }
void handleFeed()  { startFeed();        server.sendHeader("Location", "/"); server.send(303); }
void handlePump()  { startPumpTower();   server.sendHeader("Location", "/"); server.send(303); }
void handleRestart() {
  server.send(200, "text/plain", "Restarting...");
  delay(300);
  ESP.restart();
}
void handleLight() {
  if (server.hasArg("hydro")) setPwm(PIN_PWM_LIGHT_HYDRO, server.arg("hydro").toFloat());
  if (server.hasArg("aqua"))  setPwm(PIN_PWM_LIGHT_AQUA,  server.arg("aqua").toFloat());
  server.sendHeader("Location", "/");
  server.send(303);
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println(F("=== Smart Aquarium boot ==="));

  // PWM
  pinMode(PIN_PWM_PUMP_HYDRO,  OUTPUT);
  pinMode(PIN_PWM_PUMP_TOWER,  OUTPUT);
  pinMode(PIN_PWM_LIGHT_HYDRO, OUTPUT);
  pinMode(PIN_PWM_LIGHT_AQUA,  OUTPUT);
  analogWriteRange(1023);
  setPwm(PIN_PWM_PUMP_HYDRO,  0);
  setPwm(PIN_PWM_PUMP_TOWER,  0);
  setPwm(PIN_PWM_LIGHT_HYDRO, 0);
  setPwm(PIN_PWM_LIGHT_AQUA,  0);

  // Уровень воды
  pinMode(PIN_WATER_LEVEL, INPUT_PULLUP);

  // Dallas
  ds18b20.begin();

  // Servo — исходное положение как в on_boot
  servoWriteLevel(-10.0f);
  delay(1000);
  servoDetach();

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.hostname(HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print(F("WiFi connecting"));
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 20000) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("WiFi OK, IP: "));
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("WiFi FAILED — работаем без сети"));
  }

  // OTA
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() { Serial.println(F("OTA start")); });
  ArduinoOTA.onEnd([]()   { Serial.println(F("\nOTA end")); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("OTA error %u\n", e); });
  ArduinoOTA.begin();

  // Web
  server.on("/",        handleRoot);
  server.on("/feed",    handleFeed);
  server.on("/pump",    handlePump);
  server.on("/restart", handleRestart);
  server.on("/light",   handleLight);
  server.begin();
  MDNS.begin(HOSTNAME);
  Serial.println(F("HTTP server started"));
}

// ==================== LOOP ====================
void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  feedLoop();
  pumpTowerLoop();

  // Датчик уровня воды — с антидребезгом 200 мс (как в ESPHome)
  static bool lastRaw = HIGH;
  static unsigned long lastChange = 0;
  bool raw = digitalRead(PIN_WATER_LEVEL);
  if (raw != lastRaw) {
    lastRaw = raw;
    lastChange = millis();
  }
  if (millis() - lastChange > 200) {
    waterLevelLow = (lastRaw == LOW);  // INPUT_PULLUP: LOW = вода низко
  }

  // Dallas каждые 30 сек
  if (millis() - lastTempRead > 30000UL) {
    lastTempRead = millis();
    ds18b20.requestTemperatures();
    float t = ds18b20.getTempCByIndex(0);
    if (t != DEVICE_DISCONNECTED) waterTempC = t;
    Serial.printf("[TEMP] %.2f C\n", waterTempC);
  }

  // Периодический вывод статуса
  if (millis() - lastWifiPrint > 60000UL) {
    lastWifiPrint = millis();
    Serial.printf("[STATUS] uptime=%lus wifi=%ddBm temp=%.2f level=%s\n",
                  millis() / 1000,
                  WiFi.RSSI(),
                  waterTempC,
                  waterLevelLow ? "LOW" : "ok");
  }

  delay(5);
}