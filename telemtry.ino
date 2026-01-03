#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "secrets.h"

// ---------- Network ----------
static const IPAddress ESP_IP(192, 168, 1, 60);
static const IPAddress GATEWAY(192, 168, 1, 1);
static const IPAddress SUBNET(255, 255, 255, 0);
static const IPAddress DNS1(192, 168, 1, 1);

static const uint16_t UDP_PORT = 5005;
WiFiUDP udp;

// ---------- Display ----------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ---------- Button ----------
static const int BTN_PIN = 27;           // your GPIO27
static const uint32_t DEBOUNCE_MS = 35;
static int lastBtnRead = HIGH;
static int stableBtn = HIGH;
static uint32_t lastDebounceMs = 0;

// ---------- Pages ----------
static const uint8_t NUM_PAGES = 5;
static uint8_t uiPage = 0;

// ---------- Telemetry state ----------
struct Telemetry {
  uint32_t seq = 0;
  float rpm = 0;
  int gear = 0;
  float thr = 0;       // 0..1
  float brk = 0;       // 0..1
  float steer = 0;     // -1..1 (unused)
  float fuel_l = 0;    // liters (may be 0 in replay)
  float fuel_pct = 0;  // 0..1
  float speed_kmh = 0;

  int lap = 0;
  int pos = 0;
  int class_pos = 0;

  float lap_cur = 0;   // seconds
  float lap_last = 0;  // seconds
  float lap_best = 0;  // seconds

  uint32_t lastRxMs = 0;
} t;

static uint32_t lastDrawMs = 0;

static bool isLive() {
  return (t.lastRxMs != 0) && ((millis() - t.lastRxMs) < 1500);
}

// ---------- History buffers (graphs) ----------
static const int HIST_LEN = 96;
static uint8_t thrHist[HIST_LEN];
static uint8_t brkHist[HIST_LEN];
static int histHead = 0;
static int histCount = 0;

static void pushHistory(float thr, float brk) {
  thr = constrain(thr, 0.0f, 1.0f);
  brk = constrain(brk, 0.0f, 1.0f);
  thrHist[histHead] = (uint8_t)(thr * 255.0f);
  brkHist[histHead] = (uint8_t)(brk * 255.0f);

  histHead = (histHead + 1) % HIST_LEN;
  if (histCount < HIST_LEN) histCount++;
}

// ---------- Fuel-per-lap stats ----------
static int lastLapSeen = -1;
static float fuelAtLapStart = -1.0f;
static float lastLapFuelUsed = 0.0f;
static float totalFuelUsed = 0.0f;
static int fuelLapsCounted = 0;

static void updateFuelLapStats() {
  // Only meaningful if we have real liters
  if (t.fuel_l <= 0.001f) return;
  if (t.lap <= 0) return;

  if (lastLapSeen < 0) {
    lastLapSeen = t.lap;
    fuelAtLapStart = t.fuel_l;
    return;
  }

  if (t.lap != lastLapSeen) {
    // lap changed => compute previous lap consumption
    float used = fuelAtLapStart - t.fuel_l;

    // Filter out refuel / nonsense (negative or huge spikes)
    if (used > 0.0f && used < 10.0f) {
      lastLapFuelUsed = used;
      totalFuelUsed += used;
      fuelLapsCounted++;
    } else {
      // If we refueled (pit) we reset baseline, but keep existing averages
      lastLapFuelUsed = 0.0f;
    }

    lastLapSeen = t.lap;
    fuelAtLapStart = t.fuel_l;
  }
}

static float avgLapFuelUsed() {
  if (fuelLapsCounted <= 0) return 0.0f;
  return totalFuelUsed / (float)fuelLapsCounted;
}

// ---------- Drawing helpers ----------
static void drawGraphBox(int x, int y, int w, int h, const char* title) {
  display.drawRect(x, y, w, h, SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(x + 2, y - 8);
  display.print(title);
}

static void drawGraphLine(int x, int y, int w, int h, const uint8_t* hist) {
  if (histCount < 2) return;

  int points = w - 2;
  if (points > histCount) points = histCount;

  int start = histHead - points;
  while (start < 0) start += HIST_LEN;

  int prevX = x + 1;
  int prevY = y + h - 2 - (int)((hist[start] / 255.0f) * (h - 3));

  for (int i = 1; i < points; i++) {
    int idx = start + i;
    if (idx >= HIST_LEN) idx -= HIST_LEN;

    int px = x + 1 + i;
    int py = y + h - 2 - (int)((hist[idx] / 255.0f) * (h - 3));
    display.drawLine(prevX, prevY, px, py, SSD1306_WHITE);
    prevX = px;
    prevY = py;
  }
}

static void drawBarSimple(int x, int y, int w, int h, float pct, const char* label) {
  pct = constrain(pct, 0.0f, 1.0f);

  display.setTextSize(1);
  display.setCursor(x, y - 8);
  display.print(label);

  display.drawRect(x, y, w, h, SSD1306_WHITE);
  int fillW = (int)((w - 2) * pct);
  display.fillRect(x + 1, y + 1, fillW, h - 2, SSD1306_WHITE);
}

static void drawGearBox(int x, int y, int w, int h, int gear) {
  display.drawRoundRect(x, y, w, h, 4, SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(x + 4, y + 2);
  display.print("GEAR");
  display.setTextSize(2);
  display.setCursor(x + 8, y + 14);
  display.print(gear);
}

static void fmtTime(char* out, size_t n, float sec) {
  if (sec <= 0.001f) {
    snprintf(out, n, "--:--.---");
    return;
  }
  int ms = (int)(sec * 1000.0f + 0.5f);
  int s = (ms / 1000) % 60;
  int m = (ms / 60000);
  int rem = ms % 1000;
  snprintf(out, n, "%d:%02d.%03d", m, s, rem);
}

static bool initDisplay() {
  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) return true;
  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3D)) return true;
  return false;
}

static void showSplash(const char* line1, const char* line2 = nullptr) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(line1);
  if (line2) display.println(line2);
  display.display();
}

static void connectWiFi() {
  WiFi.disconnect(true, true);
  delay(150);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.persistent(false);

  WiFi.config(ESP_IP, GATEWAY, SUBNET, DNS1);

  showSplash("WiFi: connecting...", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    if (millis() - start > 15000) break;
  }

  if (WiFi.status() == WL_CONNECTED) {
    showSplash("WiFi connected", WiFi.localIP().toString().c_str());
    delay(400);
  } else {
    showSplash("WiFi failed", "Retrying...");
    delay(400);
  }
}

static bool parseCsvPacket(char* buf) {
  // 15-field payload:
  // seq,rpm,gear,thr,brk,steer,fuel_l,fuel_pct,speed_kmh,lap,pos,class_pos,lap_cur,lap_last,lap_best
  char* saveptr = nullptr;
  char* tok = strtok_r(buf, ",", &saveptr);
  if (!tok) return false;

  float v[15];
  int n = 0;
  while (tok && n < 15) {
    v[n++] = strtof(tok, nullptr);
    tok = strtok_r(nullptr, ",", &saveptr);
  }
  if (n < 15) return false;

  t.seq = (uint32_t)v[0];
  t.rpm = v[1];
  t.gear = (int)v[2];
  t.thr = v[3];
  t.brk = v[4];
  t.steer = v[5];
  t.fuel_l = v[6];
  t.fuel_pct = v[7];
  t.speed_kmh = v[8];

  t.lap = (int)v[9];
  t.pos = (int)v[10];
  t.class_pos = (int)v[11];

  t.lap_cur = v[12];
  t.lap_last = v[13];
  t.lap_best = v[14];

  t.lastRxMs = millis();

  pushHistory(t.thr, t.brk);
  updateFuelLapStats();
  return true;
}

static void drawTopBar() {
  const bool wifiOk = (WiFi.status() == WL_CONNECTED);
  const bool live = isLive();

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(wifiOk ? "WiFi " : "WiFi!");
  display.print(live ? " LIVE" : " WAIT");

  display.setCursor(86, 0);
  display.print("P");
  display.print((int)(uiPage + 1));
  display.print("/");
  display.print(NUM_PAGES);

  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);
}

static void drawNoData() {
  display.setTextSize(2);
  display.setCursor(0, 22);
  display.print("NO DATA");
  display.setTextSize(1);
  display.setCursor(0, 50);
  display.print("Start PC bridge");
}

// ---------- Pages ----------
static void drawPageGraphs() {
  // Page 1: THR/BRK graphs
  const int gx = 0, gy = 18, gw = 62, gh = 38;

  drawGraphBox(gx, gy, gw, gh, "THR");
  drawGraphLine(gx, gy, gw, gh, thrHist);

  drawGraphBox(66, gy, gw, gh, "BRK");
  drawGraphLine(66, gy, gw, gh, brkHist);

  display.setTextSize(1);
  display.setCursor(0, 58);
  display.print("T ");
  display.print((int)(constrain(t.thr, 0.0f, 1.0f) * 100));
  display.print("%");

  display.setCursor(66, 58);
  display.print("B ");
  display.print((int)(constrain(t.brk, 0.0f, 1.0f) * 100));
  display.print("%");
}

static void drawPageBarsGearFuel() {
  // Page 2: bars + gear + speed + fuel
  drawGearBox(0, 14, 44, 34, t.gear);

  display.setTextSize(2);
  display.setCursor(52, 16);
  display.print((int)t.speed_kmh);
  display.setTextSize(1);
  display.setCursor(52, 34);
  display.print("km/h");

  // Fuel (liters if available, else percent)
  display.setTextSize(1);
  display.setCursor(92, 16);
  display.print("FUEL");
  display.setCursor(92, 26);
  if (t.fuel_l > 0.01f) {
    display.print(t.fuel_l, 1);
    display.print("L");
  } else {
    display.print((int)(constrain(t.fuel_pct, 0.0f, 1.0f) * 100));
    display.print("%");
  }

  drawBarSimple(0, 52, 62, 10, t.thr, "THR");
  drawBarSimple(66, 52, 62, 10, t.brk, "BRK");
}

static void drawPageFuelStats() {
  // Page 3: Fuel stats
  display.setTextSize(1);
  display.setCursor(0, 14);
  display.print("Fuel now:");

  display.setTextSize(2);
  display.setCursor(0, 24);
  if (t.fuel_l > 0.01f) {
    display.print(t.fuel_l, 1);
    display.print(" L");
  } else {
    display.print((int)(constrain(t.fuel_pct, 0.0f, 1.0f) * 100));
    display.print(" %");
  }

  display.setTextSize(1);
  display.setCursor(0, 46);
  display.print("Last lap used: ");
  display.print(lastLapFuelUsed, 2);
  display.print("L");

  display.setCursor(0, 56);
  display.print("Avg / lap:     ");
  display.print(avgLapFuelUsed(), 2);
  display.print("L");
}

static void drawPageLapTimes() {
  // Page 4: lap timer
  char buf[16];

  display.setTextSize(1);
  display.setCursor(0, 14);
  display.print("Current:");

  fmtTime(buf, sizeof(buf), t.lap_cur);
  display.setTextSize(2);
  display.setCursor(0, 24);
  display.print(buf);

  display.setTextSize(1);
  display.setCursor(0, 46);
  display.print("Last: ");
  fmtTime(buf, sizeof(buf), t.lap_last);
  display.print(buf);

  display.setCursor(0, 56);
  display.print("Best: ");
  fmtTime(buf, sizeof(buf), t.lap_best);
  display.print(buf);
}

static void drawPagePositionLap() {
  // Page 5: position + lap
  display.setTextSize(2);
  display.setCursor(0, 16);
  display.print("P ");
  display.print(t.pos);

  display.setTextSize(1);
  display.setCursor(0, 38);
  display.print("Class P: ");
  display.print(t.class_pos);

  display.setCursor(0, 50);
  display.print("Lap: ");
  display.print(t.lap);
}

static void drawUI() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  drawTopBar();

  if (!isLive()) {
    drawNoData();
    display.display();
    return;
  }

  switch (uiPage) {
    case 0: drawPageGraphs(); break;
    case 1: drawPageBarsGearFuel(); break;
    case 2: drawPageFuelStats(); break;
    case 3: drawPageLapTimes(); break;
    case 4: drawPagePositionLap(); break;
  }

  display.display();
}

// ---------- Button handling ----------
static void handleButton() {
  int reading = digitalRead(BTN_PIN);
  uint32_t now = millis();

  if (reading != lastBtnRead) {
    lastDebounceMs = now;
    lastBtnRead = reading;
  }

  if ((now - lastDebounceMs) > DEBOUNCE_MS) {
    if (reading != stableBtn) {
      stableBtn = reading;
      if (stableBtn == LOW) {
        // pressed
        uiPage = (uiPage + 1) % NUM_PAGES;
      }
    }
  }
}

// ---------- Arduino entry points ----------
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(BTN_PIN, INPUT_PULLUP);

  Wire.begin(21, 22);

  if (!initDisplay()) {
    while (true) delay(1000);
  }

  showSplash("iRacing Telemetry", "ESP32 OLED (UDP)");
  delay(500);

  connectWiFi();
  udp.begin(UDP_PORT);

  showSplash("UDP listening...", "Waiting for data");
  delay(500);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    udp.stop();
    udp.begin(UDP_PORT);
  }

  handleButton();

  int packetSize = udp.parsePacket();
  if (packetSize > 0) {
    static char buf[256];
    int len = udp.read(buf, sizeof(buf) - 1);
    if (len > 0) {
      buf[len] = '\0';
      parseCsvPacket(buf);
    }
  }

  if (millis() - lastDrawMs > 100) {
    lastDrawMs = millis();
    drawUI();
  }
}
