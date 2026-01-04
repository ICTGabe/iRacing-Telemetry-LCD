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
static const int BTN_PIN = 27;           // your GPIO27 (other side to GND)
static const uint32_t DEBOUNCE_MS = 35;
static int lastBtnRead = HIGH;
static int stableBtn = HIGH;
static uint32_t lastDebounceMs = 0;

// ---------- Pages ----------
static const uint8_t NUM_PAGES = 6;
static uint8_t uiPage = 0;

// ---------- Telemetry state ----------
struct Telemetry {
  uint32_t seq = 0;
  float rpm = 0;
  int gear = 0;
  float thr = 0;       // 0..1
  float brk = 0;       // 0..1

  float session_remain_s = 0; // seconds (from PC)
  float fuel_l = 0;           // liters
  int incidents = 0;

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
  if (t.fuel_l <= 0.001f) return;
  if (t.lap <= 0) return;

  if (lastLapSeen < 0) {
    lastLapSeen = t.lap;
    fuelAtLapStart = t.fuel_l;
    return;
  }

  if (t.lap != lastLapSeen) {
    float used = fuelAtLapStart - t.fuel_l;

    // Filter out refuel / nonsense
    if (used > 0.0f && used < 10.0f) {
      lastLapFuelUsed = used;
      totalFuelUsed += used;
      fuelLapsCounted++;
    } else {
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

static float estLapTime() {
  if (t.lap_last > 0.1f) return t.lap_last;
  if (t.lap_best > 0.1f) return t.lap_best;
  return 0.0f;
}

static float fuelNeededToFinish() {
  float avg = avgLapFuelUsed();
  float lapT = estLapTime();
  if (avg <= 0.0001f) return -1.0f;
  if (lapT <= 0.1f) return -1.0f;
  if (t.session_remain_s <= 1.0f) return -1.0f;

  float lapsRemain = t.session_remain_s / lapT;

  // Small margin so it doesn't under-estimate
  lapsRemain += 0.5f;

  float need = lapsRemain * avg;
  if (need < 0.0f) need = 0.0f;
  return need;
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
  // 15-field payload (NEW ORDER):
  // seq,rpm,gear,thr,brk,session_remain_s,fuel_l,incidents,speed_kmh,lap,pos,class_pos,lap_cur,lap_last,lap_best

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

  t.session_remain_s = v[5];
  t.fuel_l = v[6];
  t.incidents = (int)v[7];

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

// ---------- Page 6: RPM + shift lights + gear ----------
static const float RPM_MAX   = 9000.0f;
static const float RPM_SHIFT = 8500.0f;
static const int   RPM_LIGHTS = 10;

static void gearToStr(char* out, size_t n, int g) {
  if (g < 0) snprintf(out, n, "R");
  else if (g == 0) snprintf(out, n, "N");
  else snprintf(out, n, "%d", g);
}

static void drawShiftLights(float rpm) {
  const int w = 10;
  const int h = 8;
  const int gap = 2;
  const int total = RPM_LIGHTS * w + (RPM_LIGHTS - 1) * gap;
  const int x0 = (128 - total) / 2;
  const int y0 = 14;

  float pct = rpm / RPM_MAX;
  pct = constrain(pct, 0.0f, 1.0f);

  int lit = (int)(pct * RPM_LIGHTS + 0.0001f);
  if (lit > RPM_LIGHTS) lit = RPM_LIGHTS;

  bool blink = (rpm >= RPM_SHIFT);
  bool blinkOn = ((millis() / 150) % 2) == 0;

  for (int i = 0; i < RPM_LIGHTS; i++) {
    int x = x0 + i * (w + gap);
    display.drawRect(x, y0, w, h, SSD1306_WHITE);

    bool shouldFill = (i < lit);
    if (blink && i >= (RPM_LIGHTS - 3)) {
      shouldFill = shouldFill && blinkOn;
    }
    if (shouldFill) {
      display.fillRect(x + 1, y0 + 1, w - 2, h - 2, SSD1306_WHITE);
    }
  }
}

static void drawRpmBar(float rpm) {
  const int x = 0, y = 26, w = 128, h = 8;
  display.drawRect(x, y, w, h, SSD1306_WHITE);

  float pct = rpm / RPM_MAX;
  pct = constrain(pct, 0.0f, 1.0f);

  int fillW = (int)((w - 2) * pct);
  display.fillRect(x + 1, y + 1, fillW, h - 2, SSD1306_WHITE);
}

static void drawPageRpmShift() {
  drawShiftLights(t.rpm);
  drawRpmBar(t.rpm);

  display.setTextSize(1);
  display.setCursor(0, 38);
  display.print("RPM");

  display.setTextSize(2);
  display.setCursor(0, 46);
  display.print((int)t.rpm);

  char gbuf[4];
  gearToStr(gbuf, sizeof(gbuf), t.gear);

  display.setTextSize(1);
  display.setCursor(94, 38);
  display.print("GEAR");

  display.setTextSize(3);
  display.setCursor(96, 44);
  display.print(gbuf);
}

// ---------- Pages (LIVE) ----------
static void drawPageGraphs() {
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
  drawGearBox(0, 14, 44, 34, t.gear);

  display.setTextSize(2);
  display.setCursor(52, 16);
  display.print((int)t.speed_kmh);
  display.setTextSize(1);
  display.setCursor(52, 34);
  display.print("km/h");

  // Fuel always as liters (as requested)
  display.setTextSize(1);
  display.setCursor(92, 16);
  display.print("FUEL");
  display.setCursor(92, 26);
  if (t.fuel_l > 0.01f) {
    display.print(t.fuel_l, 1);
    display.print("L");
  } else {
    display.print("--.-L");
  }

  drawBarSimple(0, 52, 62, 10, t.thr, "THR");
  drawBarSimple(66, 52, 62, 10, t.brk, "BRK");
}

static void drawPageFuelStats() {
  // Page 3: show fuel needed to finish (instead of "Fuel now %")
  float need = fuelNeededToFinish();

  display.setTextSize(1);
  display.setCursor(0, 14);
  display.print("Need fuel:");

  display.setTextSize(2);
  display.setCursor(0, 24);
  if (need >= 0.0f) {
    display.print(need, 1);
    display.print(" L");
  } else {
    display.print("--.- L");
  }

  display.setTextSize(1);
  display.setCursor(0, 46);
  display.print("Fuel now: ");
  if (t.fuel_l > 0.01f) {
    display.print(t.fuel_l, 1);
    display.print("L");
  } else {
    display.print("--.-L");
  }

  display.setCursor(0, 56);
  display.print("Avg/lap:  ");
  display.print(avgLapFuelUsed(), 2);
  display.print("L");
}

static void drawPageLapTimes() {
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
  // Page 5: add incidents
  display.setTextSize(2);
  display.setCursor(0, 14);
  display.print("P ");
  display.print(t.pos);

  display.setTextSize(1);
  display.setCursor(0, 36);
  display.print("Class P: ");
  display.print(t.class_pos);

  display.setCursor(0, 48);
  display.print("Lap: ");
  display.print(t.lap);

  display.setCursor(70, 48);
  display.print("Inc: ");
  display.print(t.incidents);
}

// ---------- Demo mode (when not live, for pages 2..6 only) ----------
static void demoValues(float &rpm, int &gear, float &thr, float &brk, float &speed, float &fuel, int &pos, int &cls, int &lap, int &inc) {
  uint32_t ms = millis();

  // simple wavey demo
  float phase = (ms % 6000) / 6000.0f;           // 0..1
  float p2 = (ms % 2000) / 2000.0f;              // 0..1

  rpm = 1500.0f + 7000.0f * phase;               // 1500..8500
  if (rpm > 9000.0f) rpm = 9000.0f;

  gear = 1 + (int)(phase * 6.0f);                // 1..7-ish
  if (gear > 6) gear = 6;

  thr = 0.2f + 0.8f * (phase < 0.8f ? phase / 0.8f : 1.0f);
  thr = constrain(thr, 0.0f, 1.0f);

  brk = (p2 > 0.8f) ? (p2 - 0.8f) / 0.2f : 0.0f; // occasional braking
  brk = constrain(brk, 0.0f, 1.0f);

  speed = 40.0f + 240.0f * phase;                // 40..280
  fuel = 42.0f - 10.0f * phase;                  // 42..32
  if (fuel < 0) fuel = 0;

  pos = 7;
  cls = 3;
  lap = 12;
  inc = 2;
}

static void drawDemoPage(uint8_t page) {
  float rpm, thr, brk, speed, fuel;
  int gear, pos, cls, lap, inc;
  demoValues(rpm, gear, thr, brk, speed, fuel, pos, cls, lap, inc);

  switch (page) {
    case 1: { // Bars+Gear+Fuel demo
      drawGearBox(0, 14, 44, 34, gear);

      display.setTextSize(2);
      display.setCursor(52, 16);
      display.print((int)speed);
      display.setTextSize(1);
      display.setCursor(52, 34);
      display.print("km/h");

      display.setTextSize(1);
      display.setCursor(92, 16);
      display.print("FUEL");
      display.setCursor(92, 26);
      display.print(fuel, 1);
      display.print("L");

      drawBarSimple(0, 52, 62, 10, thr, "THR");
      drawBarSimple(66, 52, 62, 10, brk, "BRK");
    } break;

    case 2: { // Fuel stats demo
      display.setTextSize(1);
      display.setCursor(0, 14);
      display.print("Need fuel:");

      display.setTextSize(2);
      display.setCursor(0, 24);
      display.print("18.6 L");

      display.setTextSize(1);
      display.setCursor(0, 46);
      display.print("Fuel now: ");
      display.print(fuel, 1);
      display.print("L");

      display.setCursor(0, 56);
      display.print("Avg/lap:  3.10L");
    } break;

    case 3: { // Lap times demo
      display.setTextSize(1);
      display.setCursor(0, 14);
      display.print("Current:");

      display.setTextSize(2);
      display.setCursor(0, 24);
      display.print("1:58.432");

      display.setTextSize(1);
      display.setCursor(0, 46);
      display.print("Last: 1:57.981");

      display.setCursor(0, 56);
      display.print("Best: 1:57.640");
    } break;

    case 4: { // Position/Lap/Inc demo
      display.setTextSize(2);
      display.setCursor(0, 14);
      display.print("P ");
      display.print(pos);

      display.setTextSize(1);
      display.setCursor(0, 36);
      display.print("Class P: ");
      display.print(cls);

      display.setCursor(0, 48);
      display.print("Lap: ");
      display.print(lap);

      display.setCursor(70, 48);
      display.print("Inc: ");
      display.print(inc);
    } break;

    case 5: { // RPM demo
      // temporarily set to demo values
      drawShiftLights(rpm);
      drawRpmBar(rpm);

      display.setTextSize(1);
      display.setCursor(0, 38);
      display.print("RPM");

      display.setTextSize(2);
      display.setCursor(0, 46);
      display.print((int)rpm);

      char gbuf[4];
      gearToStr(gbuf, sizeof(gbuf), gear);

      display.setTextSize(1);
      display.setCursor(94, 38);
      display.print("GEAR");

      display.setTextSize(3);
      display.setCursor(96, 44);
      display.print(gbuf);
    } break;

    default:
      break;
  }
}

// ---------- UI ----------
static void drawUI() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  drawTopBar();

  bool live = isLive();

  // Only page 1 (uiPage==0) shows "NO DATA". Other pages show demo until live.
  if (!live) {
    if (uiPage == 0) {
      drawNoData();
    } else {
      drawDemoPage(uiPage);
    }
    display.display();
    return;
  }

  // Live rendering
  switch (uiPage) {
    case 0: drawPageGraphs(); break;
    case 1: drawPageBarsGearFuel(); break;
    case 2: drawPageFuelStats(); break;
    case 3: drawPageLapTimes(); break;
    case 4: drawPagePositionLap(); break;
    case 5: drawPageRpmShift(); break;
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
