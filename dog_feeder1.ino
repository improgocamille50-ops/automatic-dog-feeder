/*
  Automatic Dog Feeder with Scheduled Feeding and Portion Control
  Board : ESP32 (any DevKit)      IDE : Arduino IDE
  Library to install (Library Manager): "ESP32Servo" by Kevin Harrington

  Wiring
    Servo signal (orange) -> D13 (GPIO 13)
    Servo power  (red)    -> VIN (5V)
    Servo ground (brown)  -> GND (shared with the ESP32)

  How a feeding works
    The servo opens the gate to 180 degrees, holds it for FIXED_OPEN_MS (2 s),
    then swings back to 0 degrees over CLOSE_SWEEP_MS (1 s).
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESP32Servo.h>
#include <Preferences.h>
#include <time.h>

// ---------- Your settings ----------
const char* WIFI_SSID = "HOME WIFI";
const char* WIFI_PASS = "PASSWORD";

const int   SERVO_PIN         = 13;
// Gate swings from 0 (closed) to 180 (open). If it is backwards, swap these two numbers.
const int   ANGLE_CLOSED      = 0;
const int   ANGLE_OPEN        = 90;
const float GRAMS_PER_SECOND  = 25.0;   // calibrate this
const unsigned long FIXED_OPEN_MS  = 5000;  // gate stays open this long (2 seconds). Set to 0 to use the portion size instead.
const unsigned long CLOSE_SWEEP_MS = 200;  // gate swings back to closed over this long (1 second)
const int   HOPPER_CAPACITY_G = 5000;
const int   MIN_PORTION_G     = 10;
const int   MAX_PORTION_G     = 300;
const long  GMT_OFFSET_SEC    = 8 * 3600;  // Philippines = UTC+8

// ---------- Data ----------
#define MAX_SCHEDULES 8
#define MAX_LOG 20

struct Schedule {
  bool used;
  bool enabled;
  uint8_t hour;
  uint8_t minute;
  uint16_t grams;
  int16_t lastYday;   // day of year already fed, prevents double feeding
};

struct LogEntry {
  time_t t;
  uint16_t grams;
  bool manual;
};

Schedule schedules[MAX_SCHEDULES];
LogEntry logs[MAX_LOG];
int logCount = 0;

Servo gate;
WebServer server(80);
Preferences prefs;

int remainingG = HOPPER_CAPACITY_G;
int servoAngle = ANGLE_CLOSED;
bool feeding = false;
bool closing = false;              // true while the gate is swinging closed
unsigned long closeStartMs = 0;
unsigned long feedEndMs = 0;
unsigned long detachAtMs = 0;   // when to stop sending the servo signal after closing
unsigned long lastScheduleCheck = 0;

// ---------- Storage ----------
void saveSchedules() { prefs.putBytes("sched", schedules, sizeof(schedules)); }
void saveRemaining() { prefs.putInt("remain", remainingG); }

void loadSettings() {
  memset(schedules, 0, sizeof(schedules));
  if (prefs.getBytesLength("sched") == sizeof(schedules)) {
    prefs.getBytes("sched", schedules, sizeof(schedules));
  }
  remainingG = prefs.getInt("remain", HOPPER_CAPACITY_G);
}

// ---------- Feeding ----------
void addLog(int grams, bool manual) {
  for (int i = MAX_LOG - 1; i > 0; i--) logs[i] = logs[i - 1];
  logs[0] = { time(nullptr), (uint16_t)grams, manual };
  if (logCount < MAX_LOG) logCount++;
}

// Returns "" on success, otherwise an error message.
String startFeed(int grams, bool manual) {
  if (feeding) return "Already dispensing";
  if (grams < MIN_PORTION_G || grams > MAX_PORTION_G)
    return "Portion must be " + String(MIN_PORTION_G) + "-" + String(MAX_PORTION_G) + " g";
  if (grams > remainingG) return "Hopper low: only " + String(remainingG) + " g left";

  feeding = true;
  closing = false;
  unsigned long openMs = FIXED_OPEN_MS > 0 ? FIXED_OPEN_MS : (unsigned long)((grams / GRAMS_PER_SECOND) * 1000.0);
  feedEndMs = millis() + openMs;
  detachAtMs = 0;
  if (!gate.attached()) gate.attach(SERVO_PIN, 500, 2400);
  servoAngle = ANGLE_OPEN;
  gate.write(servoAngle);
  remainingG -= grams;
  saveRemaining();
  addLog(grams, manual);
  Serial.printf("Dispensing %d g (%s)\n", grams, manual ? "manual" : "schedule");
  return "";
}

void updateFeeding() {
  // 1) Open time is over: start swinging the gate closed
  if (feeding && !closing && (long)(millis() - feedEndMs) >= 0) {
    closing = true;
    closeStartMs = millis();
    Serial.println("Closing gate...");
  }
  // 2) Move the gate smoothly from open to closed over CLOSE_SWEEP_MS
  if (closing) {
    unsigned long elapsed = millis() - closeStartMs;
    if (elapsed >= CLOSE_SWEEP_MS) {
      servoAngle = ANGLE_CLOSED;
      gate.write(servoAngle);
      closing = false;
      feeding = false;
      detachAtMs = millis() + 700;   // give the servo time to settle
      Serial.println("Gate closed");
    } else {
      servoAngle = ANGLE_OPEN + (int)(((long)(ANGLE_CLOSED - ANGLE_OPEN) * (long)elapsed) / (long)CLOSE_SWEEP_MS);
      gate.write(servoAngle);
    }
  }
  if (detachAtMs && (long)(millis() - detachAtMs) >= 0) {
    gate.detach();                   // stop the signal so the servo stays still and quiet
    detachAtMs = 0;
  }
}

int todayGrams() {
  struct tm now;
  if (!getLocalTime(&now, 0)) return 0;
  int total = 0;
  for (int i = 0; i < logCount; i++) {
    struct tm l;
    time_t t = logs[i].t;
    localtime_r(&t, &l);
    if (l.tm_year == now.tm_year && l.tm_yday == now.tm_yday) total += logs[i].grams;
  }
  return total;
}

void checkSchedules() {
  struct tm now;
  if (!getLocalTime(&now, 0)) return;  // clock not synced yet
  for (int i = 0; i < MAX_SCHEDULES; i++) {
    Schedule &s = schedules[i];
    if (!s.used || !s.enabled) continue;
    if (s.hour == now.tm_hour && s.minute == now.tm_min && s.lastYday != now.tm_yday) {
      String err = startFeed(s.grams, false);
      if (err != "Already dispensing") {   // retry next second only if busy
        s.lastYday = now.tm_yday;
        saveSchedules();
      }
    }
  }
}

// ---------- HTTP helpers ----------
void sendJson(int code, const String &body) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", body);
}
void sendError(int code, const String &msg) {
  sendJson(code, "{\"error\":\"" + msg + "\"}");
}

// ---------- API handlers ----------
void handleStatus() {
  struct tm now;
  char clockStr[16] = "--:--:--";
  if (getLocalTime(&now, 0)) strftime(clockStr, sizeof(clockStr), "%H:%M:%S", &now);
  String j = "{";
  j += "\"time\":\"" + String(clockStr) + "\",";
  j += "\"epoch\":" + String((long)time(nullptr)) + ",";
  j += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  j += "\"feeding\":" + String(feeding ? "true" : "false") + ",";
  j += "\"fixedMs\":" + String(FIXED_OPEN_MS) + ",";
  j += "\"servo\":" + String(feeding ? 90 : 0) + ",";   // display only: 90 = open, 0 = closed
  j += "\"remaining\":" + String(remainingG) + ",";
  j += "\"capacity\":" + String(HOPPER_CAPACITY_G) + ",";
  j += "\"today\":" + String(todayGrams());
  j += "}";
  sendJson(200, j);
}

void handleSchedules() {
  String j = "[";
  bool first = true;
  for (int i = 0; i < MAX_SCHEDULES; i++) {
    if (!schedules[i].used) continue;
    if (!first) j += ",";
    first = false;
    j += "{\"id\":" + String(i) +
         ",\"enabled\":" + String(schedules[i].enabled ? "true" : "false") +
         ",\"hour\":" + String(schedules[i].hour) +
         ",\"minute\":" + String(schedules[i].minute) +
         ",\"grams\":" + String(schedules[i].grams) + "}";
  }
  j += "]";
  sendJson(200, j);
}

void handleLog() {
  String j = "[";
  for (int i = 0; i < logCount; i++) {
    if (i) j += ",";
    j += "{\"t\":" + String((long)logs[i].t) + ",\"grams\":" + String(logs[i].grams) +
         ",\"src\":\"" + (logs[i].manual ? "manual" : "schedule") + "\"}";
  }
  j += "]";
  sendJson(200, j);
}

void handleFeed() {
  String err = startFeed(server.arg("grams").toInt(), true);
  if (err.length()) sendError(400, err); else sendJson(200, "{\"ok\":true}");
}

void handleAdd() {
  int h = server.arg("hour").toInt();
  int m = server.arg("minute").toInt();
  int g = server.arg("grams").toInt();
  if (!server.hasArg("hour") || h < 0 || h > 23 || m < 0 || m > 59) return sendError(400, "Invalid time");
  if (g < MIN_PORTION_G || g > MAX_PORTION_G) return sendError(400, "Invalid portion");
  for (int i = 0; i < MAX_SCHEDULES; i++) {
    if (!schedules[i].used) {
      schedules[i] = { true, true, (uint8_t)h, (uint8_t)m, (uint16_t)g, -1 };
      saveSchedules();
      return sendJson(200, "{\"ok\":true}");
    }
  }
  sendError(400, "Schedule list is full (8 max)");
}

void handleDelete() {
  int id = server.arg("id").toInt();
  if (id < 0 || id >= MAX_SCHEDULES || !schedules[id].used) return sendError(404, "Feeding not found");
  schedules[id].used = false;
  saveSchedules();
  sendJson(200, "{\"ok\":true}");
}

void handleToggle() {
  int id = server.arg("id").toInt();
  if (id < 0 || id >= MAX_SCHEDULES || !schedules[id].used) return sendError(404, "Feeding not found");
  schedules[id].enabled = !schedules[id].enabled;
  saveSchedules();
  sendJson(200, "{\"ok\":true}");
}

void handleRefill() {
  remainingG = HOPPER_CAPACITY_G;
  saveRemaining();
  sendJson(200, "{\"ok\":true}");
}

void handleNotFound() { sendError(404, "Unknown endpoint"); }

// ---------- Setup / loop ----------
void setup() {
  Serial.begin(115200);

  gate.setPeriodHertz(50);
  gate.attach(SERVO_PIN, 500, 2400);
  gate.write(ANGLE_CLOSED);
  delay(700);
  gate.detach();   // idle with no signal until the next feeding

  prefs.begin("feeder", false);
  loadSettings();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println();
  Serial.print("Dashboard IP address: ");
  Serial.println(WiFi.localIP());   // type this into the dashboard's Connection settings

  configTime(GMT_OFFSET_SEC, 0, "pool.ntp.org", "time.nist.gov");

  server.on("/api/status", handleStatus);
  server.on("/api/schedules", handleSchedules);
  server.on("/api/log", handleLog);
  server.on("/api/feed", handleFeed);
  server.on("/api/schedule/add", handleAdd);
  server.on("/api/schedule/delete", handleDelete);
  server.on("/api/schedule/toggle", handleToggle);
  server.on("/api/refill", handleRefill);
  server.onNotFound(handleNotFound);
  server.begin();
}

void loop() {
  server.handleClient();
  updateFeeding();
  if (millis() - lastScheduleCheck >= 1000) {
    lastScheduleCheck = millis();
    checkSchedules();
  }
}