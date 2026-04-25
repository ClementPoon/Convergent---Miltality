#include <Arduino.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SD.h>
#include "HAPPY_FACE.h"
#include "MID_FACE.h"
#include "SAD_FACE.h"

// --- WiFi ---
#define WIFI_SSID       "utexas-iot"
#define WIFI_PASSWORD   "41263109312255198747"

// --- Firebase ---
// DB Secret: Firebase Console -> Project Settings -> Service accounts -> Database secrets
#define FIREBASE_DB_SECRET  "M25bytWDBNyFY1B2A69u3WfuWVSeSXjJU2q5ZnMV"
#define FIREBASE_HOST       "https://convergent-3fd2a-default-rtdb.firebaseio.com"
#define FIREBASE_DB_PATH    "/plants/GP-A1B2C3"

// --- Sensor Pin ---
// Must be an ADC1 pin (32-39) — ADC2 pins (0,2,4,12-15,25-27) conflict with WiFi
#define MOISTURE_PIN  34

// --- Moisture Thresholds (0-4095) ---
// Capacitive sensor: low = wet, high = dry
// These also control which face is shown on the display.
#define THRESHOLD_HAPPY  4000   // below this -> Happy face (wet)
#define THRESHOLD_SAD    4050   // above this -> Sad face (dry)
                                // between the two  -> Middle face

// --- Display (ST7735 SPI TFT) ---
// Hardware SPI pins (fixed by ESP32 VSPI): MOSI=23, SCLK=18
// For 128x128 displays change initR(INITR_BLACKTAB) -> initR(INITR_144GREENTAB) in setup()
#define TFT_CS   5
#define TFT_DC   2
#define TFT_RST  4
#define TFT_WIDTH   160
#define TFT_HEIGHT  128

// --- SD Card (shares VSPI bus with TFT; MISO=19 required) ---
// Change SD_CS to match your module's wiring if it differs from GPIO 15
#define SD_CS   15
#define FACE_W  160
#define FACE_H  128

// Brand palette (RGB565)
#define COLOR_SAGE       0x75B1  // #71B48D — sage green   (Middle background)
#define COLOR_YELLOW     0xFECF  // #FFD97D — warm yellow  (face fill)
#define COLOR_OFFWHITE   0xF7BE  // #F2F7F2 — off-white   (eye shine)
#define COLOR_BROWN      0x4184  // #443125 — dark brown   (Sad background + features)
#define COLOR_GREEN      0x0C2A  // #0A8754 — forest green (Happy background)
#define COLOR_PINK       0xBF3F  // #B9E6FE — light blue-pink (unused, available)
#define COLOR_WATER      0x14DF  // cornflower blue — water drops / pump animation
#define COLOR_APP_BG     0x9772  // #90EE90 light green — face background when app is online

// --- DRV8833 Motor Driver ---
// nSLEEP: active-LOW sleep — pull LOW to sleep (saves power), HIGH to enable
// IN1/IN2: HIGH/LOW = forward, LOW/LOW = coast (off)
// Pump runs in one direction only, so IN2 is held LOW.
#define DRV_IN1            27
#define DRV_IN2            26
#define DRV_SLEEP          13    // nSLEEP — pulled LOW when pump is idle
#define MOTOR_POLL_MS      2000  // how often to read motorState from Firebase
#define MOTOR_PULSE_MS     400   // pump on-time per trigger (ms)
#define APP_POLL_MS        2000  // how often to read isOnline (app presence) from Firebase

// --- Sampling ---
#define SAMPLE_INTERVAL_MS  1000  // one read every 5 s (plants change over hours)
#define MIN_SAMPLES         3     // don't send until we have at least this many
#define MAX_SAMPLES         5     // stop collecting once we hit this; wait for Firebase

// --- Display power ---
#define DISPLAY_SLEEP_MS  300000UL  // sleep panel after 5 min with no activity

// --- Online border chase ---
#define BORDER_TICK_MS   18         // ms between animation steps
#define BORDER_SEG       120         // lit pixels per chasing segment
#define BORDER_TOTAL     572        // perimeter pixel count: 160+127+159+126
#define BORDER_STEP      286        // spacing between segments (BORDER_TOTAL/2)
#define BORDER_ON        0x07E0     // bright green
#define BORDER_OFF       0x0180     // dim green (background glow)

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_RST);

unsigned long lastSample = 0;
long sampleSum = 0;
int sampleCount = 0;
int lastMoisture = -1;
String lastState = "";      // last state pushed to Firebase
String displayState = "";   // currently shown face (updated every sample)
bool lastMotorOn = false;
unsigned long lastMotorPoll = 0;
bool motorPulsing = false;
unsigned long motorPulseStart = 0;
unsigned long wateringDisplayEnd = 0;
int lastWifiBars = -1;
unsigned long lastWifiCheck = 0;
#define WIFI_CHECK_MS 5000
bool displayAsleep = false;
unsigned long lastActivityMs = 0;
bool lastAppOnline = false;
unsigned long lastAppPoll = 0;
bool sdOk = false;
int           borderOffset   = 0;
unsigned long borderLastTick = 0;
const char* currentFacePath = nullptr;
const uint16_t* currentFaceBitmap = nullptr;

// --- Artificial Watering Detection ---
bool trackArtificialWater = false;
unsigned long lastTrackPoll = 0;
#define TRACK_POLL_MS        10000
bool artificialWaterPending = false;
unsigned long sadToHappyMs  = 0;
#define ARTIFICIAL_WATER_CONFIRM_MS 5000

// Mounts the SD card and verifies all three face files are present.
void initSD() {
  SPI.begin(18, 19, 23, SD_CS);  // SCK, MISO, MOSI, SS — ensures MISO is configured
  if (!SD.begin(SD_CS, SPI, 4000000)) { Serial.println("SD: mount failed — check wiring/SD_CS"); return; }
  sdOk = true;
  Serial.println("SD: mounted");
  const char* needed[] = { "/HAPPY_FACE", "/MID_FACE", "/SAD_FACE" };
  for (const char* p : needed) {
    if (!SD.exists(p)) { Serial.print("SD: missing "); Serial.println(p); sdOk = false; }
  }
  if (sdOk) Serial.println("SD: all face files found");
}

// Wakes the display panel if it was put to sleep by the idle timeout.
void wakeDisplay() {
  if (displayAsleep) {
    tft.enableSleep(false);
    delay(5);
    displayAsleep = false;
  }
  lastActivityMs = millis();
}

// Draws numArcs upper-semicircle WiFi arcs building inward→outward (1=inner only, 3=all).
// cx/cy is the dot origin. Radii outer→inner: 26, 18, 10.
void drawWifiArcs(int cx, int cy, int numArcs, uint16_t color, uint16_t bg) {
  const int radii[3] = {26, 18, 10};
  int skip = 3 - (numArcs < 3 ? numArcs : 3);
  for (int i = skip; i < 3; i++) {
    int r = radii[i];
    tft.drawCircle(cx, cy, r, color);
    tft.fillRect(cx - r - 1, cy, 2 * r + 3, r + 2, bg);
  }
  tft.fillCircle(cx, cy, 3, color);
}

// Maps current RSSI to 0–2 bars for the small corner icon.
int wifiIconBars() {
  int rssi = WiFi.RSSI();
  if (rssi >= -60) return 2;
  if (rssi >= -75) return 1;
  return 0;
}

// Small corner WiFi icon (r=7,4). numArcs 0–2 reflects signal strength.
void drawWifiIcon(int cx, int cy, int numArcs, uint16_t color, uint16_t bg) {
  const int radii[2] = {7, 4};
  int skip = 2 - (numArcs < 2 ? numArcs : 2);
  for (int i = skip; i < 2; i++) {
    int r = radii[i];
    tft.drawCircle(cx, cy, r, color);
    tft.fillRect(cx - r - 1, cy, 2 * r + 3, r + 2, bg);
  }
  tft.fillCircle(cx, cy, 2, color);
}

// Full-screen connecting page. frame 0–3 animates arcs building outward.
void drawConnecting(int frame) {
  wakeDisplay();
  tft.fillScreen(COLOR_SAGE);
  tft.setTextColor(0xFFFF);
  tft.setTextSize(1);
  tft.setCursor(22, 28);
  tft.print("Connecting to WiFi");
  drawWifiArcs(80, 80, frame, 0xFFFF, COLOR_SAGE);
}

// Progress bar drawn on the connected screen while waiting for Firebase auth.
// pct 0–100; called repeatedly to animate the fill.
void drawFirebaseBar(int pct) {
  const int bx = 10, by = 104, bw = TFT_WIDTH - 20, bh = 8;
  tft.setTextColor(0xFFFF);
  tft.setTextSize(1);
  tft.setCursor(bx, by - 12);
  tft.print("Connecting Firebase...");
  tft.drawRect(bx, by, bw, bh, 0xFFFF);
  int fill = (bw - 2) * pct / 100;
  if (fill > 0) tft.fillRect(bx + 1, by + 1, fill, bh - 2, 0xFFFF);
}

// Brief full-screen confirmation after WiFi connects.
void drawConnected() {
  wakeDisplay();
  tft.fillScreen(COLOR_GREEN);
  tft.setTextColor(0xFFFF);
  tft.setTextSize(1);

  // WiFi arcs near top
  drawWifiArcs(80, 32, 3, 0xFFFF, COLOR_GREEN);

  // "Connected!" centered
  tft.setCursor(50, 48);
  tft.print("Connected!");

  // Network name centered
  String ssid = WiFi.SSID();
  tft.setCursor((TFT_WIDTH - (int)ssid.length() * 6) / 2, 68);
  tft.print(ssid);

  // Signal strength centered
  String sig = "Signal: " + String(WiFi.RSSI()) + " dBm";
  tft.setCursor((TFT_WIDTH - (int)sig.length() * 6) / 2, 84);
  tft.print(sig);
}

// Teardrop shape — tip points up, circle at bottom.
void drawWaterDrop(int x, int y, int r, uint16_t color) {
  tft.fillCircle(x, y + r * 2, r, color);
  tft.fillTriangle(x - r + 1, y + r * 2, x + r - 1, y + r * 2, x, y, color);
}

// Maps perimeter index (0–571, clockwise from top-left) to screen (x, y).
static void borderPos(int i, int16_t &x, int16_t &y) {
  if      (i < 160) { x = i;       y = 0;       }  // top  →
  else if (i < 287) { x = 159;     y = i - 159; }  // right ↓
  else if (i < 446) { x = 445 - i; y = 127;     }  // bottom ←
  else              { x = 0;       y = 572 - i; }  // left  ↑
}

// Full border repaint — used after a face blit so the chase reappears immediately.
// When offline, paints the perimeter black so no face pixels bleed through.
static void drawBorderFrame() {
  int16_t x, y;
  for (int i = 0; i < BORDER_TOTAL; i++) {
    borderPos(i, x, y);
    if (!lastAppOnline) {
      tft.drawPixel(x, y, 0x0000);
    } else {
      int rel = (i - borderOffset + BORDER_TOTAL) % BORDER_TOTAL;
      tft.drawPixel(x, y, (rel % BORDER_STEP) < BORDER_SEG ? BORDER_ON : BORDER_OFF);
    }
  }
}

// Incremental chase tick — catches up for any missed ticks due to blocking calls.
void tickBorder() {
  if (!lastAppOnline) return;
  unsigned long now = millis();
  unsigned long elapsed = now - borderLastTick;
  if (elapsed < BORDER_TICK_MS) return;
  borderLastTick = now;

  // Cap catch-up at 12 steps (~40 drawPixel calls) to avoid a new stall.
  int steps = min((int)(elapsed / BORDER_TICK_MS), 12);
  int16_t x, y;
  for (int s = 0; s < steps; s++) {
    for (int k = 0; k < 2; k++) {
      borderPos((borderOffset + k * BORDER_STEP) % BORDER_TOTAL, x, y);
      tft.drawPixel(x, y, BORDER_OFF);
    }
    borderOffset = (borderOffset + 1) % BORDER_TOTAL;
    for (int k = 0; k < 2; k++) {
      borderPos((borderOffset + BORDER_SEG - 1 + k * BORDER_STEP) % BORDER_TOTAL, x, y);
      tft.drawPixel(x, y, BORDER_ON);
    }
  }
}

// Redraws the WiFi icon on top of whatever is on screen.
// Call after any bitmap blit to ensure the icon is never left stale.
void redrawOverlays() {
  lastWifiBars = wifiIconBars();
  drawWifiIcon(148, 12, lastWifiBars, COLOR_BROWN, 0xFFFF);
  drawBorderFrame();
}

// Byte-swap helper: converts LSB-first RGB565 to the big-endian format the ST7735 expects.
static inline uint16_t swapBytes(uint16_t v) { return (v >> 8) | (v << 8); }

// Reads [nRows] rows from the SD face file (preferred) or PROGMEM fallback.
void blitRows(int rowStart, int nRows) {
  static uint16_t rowBuf[FACE_W];
  if (sdOk && currentFacePath) {
    File f = SD.open(currentFacePath);
    if (!f) { Serial.print("SD: open failed: "); Serial.println(currentFacePath); return; }
    f.seek((uint32_t)rowStart * FACE_W * 2);
    for (int r = 0; r < nRows; r++) {
      f.read((uint8_t*)rowBuf, FACE_W * 2);
      for (int i = 0; i < FACE_W; i++) rowBuf[i] = swapBytes(rowBuf[i]);
      tft.drawRGBBitmap(0, rowStart + r, rowBuf, FACE_W, 1);
    }
    f.close();
  } else if (currentFaceBitmap) {
    const uint16_t* src = currentFaceBitmap + (uint32_t)rowStart * FACE_W;
    for (int r = 0; r < nRows; r++) {
      for (int i = 0; i < FACE_W; i++) rowBuf[i] = swapBytes(pgm_read_word(&src[r * FACE_W + i]));
      tft.drawRGBBitmap(0, rowStart + r, rowBuf, FACE_W, 1);
    }
  }
}

// Restores the top strip after a pump animation, then redraws all overlays.
void restoreTopStrip() {
  wakeDisplay();
  blitRows(0, 22);
  redrawOverlays();
}

// Watering overlay drawn over the top strip during a pump pulse.
void drawPumpAnimation() {
  wakeDisplay();
  tft.fillRect(0, 0, TFT_WIDTH, 22, 0xFFFF);
  tft.setTextColor(COLOR_BROWN);
  tft.setTextSize(1);
  tft.setCursor(52, 7);
  tft.print("Watering!");
  drawWaterDrop(34,  6, 4, COLOR_WATER);
  drawWaterDrop(122, 6, 4, COLOR_WATER);
}

void flashRed() {
  wakeDisplay();
  for (int f = 0; f < 3; f++) {
    tft.fillScreen(0xF800);
    delay(200);
    if (f < 2) { tft.fillScreen(0x0000); delay(100); }
  }
}

void drawFace(const String& state) {
  wakeDisplay();
  if (state == "Happy") {
    currentFacePath   = "/HAPPY_FACE";
    currentFaceBitmap = HAPPY_FACE;
  } else if (state == "Middle") {
    currentFacePath   = "/MID_FACE";
    currentFaceBitmap = MID_FACE;
  } else {
    currentFacePath   = "/SAD_FACE";
    currentFaceBitmap = SAD_FACE;
  }
  blitRows(0, FACE_H);
  redrawOverlays();
}

void setup() {
  setCpuFrequencyMhz(80);  // 240→80 MHz: ~60% less active CPU power
  Serial.begin(115200);

  tft.initR(INITR_BLACKTAB);  // 128x160; landscape via rotation
  tft.setRotation(3);
  drawConnecting(0);

  Serial.print("MAC Address: ");
  Serial.println(WiFi.macAddress());

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  int wifiFrame = 0;
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    wifiFrame = (wifiFrame % 3) + 1;
    drawConnecting(wifiFrame);
    delay(500);
  }
  Serial.println();
  Serial.print("Connected! IP: ");
  Serial.println(WiFi.localIP());
  drawConnected();
  WiFi.setSleep(true);  // modem idles between TX/RX bursts

  config.database_url = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_DB_SECRET;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // Animate loading bar while Firebase authenticates (up to ~8 s)
  drawFirebaseBar(0);
  for (int t = 0; t < 40; t++) {
    if (Firebase.ready()) break;
    delay(200);
    drawFirebaseBar((t + 1) * 95 / 40);
  }
  drawFirebaseBar(100);
  delay(300);

  pinMode(DRV_IN1,    OUTPUT);
  pinMode(DRV_IN2,    OUTPUT);
  pinMode(DRV_SLEEP,  OUTPUT);
  digitalWrite(DRV_IN1,   LOW);
  digitalWrite(DRV_IN2,   LOW);
  digitalWrite(DRV_SLEEP, LOW);  // start in sleep mode

  Firebase.RTDB.setString(&fbdo, "esp32/device1/status", "online");

  initSD();

  // Take an initial reading and show the correct face immediately.
  {
    int raw = analogRead(MOISTURE_PIN);
    displayState = (raw < THRESHOLD_HAPPY) ? "Happy" : (raw > THRESHOLD_SAD) ? "Sad" : "Middle";
    drawFace(displayState);
  }
}

void sendAverage() {
  int moisture = sampleSum / sampleCount;
  sampleSum = 0;
  int sent = sampleCount;
  sampleCount = 0;

  String state;
  if (moisture < THRESHOLD_HAPPY) {
    state = "Happy";
  } else if (moisture > THRESHOLD_SAD) {
    state = "Sad";
  } else {
    state = "Middle";
  }

  bool moistureChanged = (moisture != lastMoisture);
  bool stateChanged = (state != lastState);

  if (!moistureChanged && !stateChanged) {
    Serial.println("No change, skipping Firebase write.");
    return;
  }

  Serial.print("Sending avg of ");
  Serial.print(sent);
  Serial.print(" samples: ");
  Serial.print(moisture);
  Serial.print(" -> ");
  Serial.println(state);

  if (moistureChanged) {
    Firebase.RTDB.setInt(&fbdo, FIREBASE_DB_PATH "/soilMoisture", moisture);
    lastMoisture = moisture;
  }
  if (stateChanged) {
    Firebase.RTDB.setString(&fbdo, FIREBASE_DB_PATH "/screenState", state.c_str());
    lastState = state;
  }
}

void loop() {
  // Clear watering animation after 2 seconds
  if (wateringDisplayEnd > 0 && millis() >= wateringDisplayEnd) {
    wateringDisplayEnd = 0;
    if (displayState.length() > 0) restoreTopStrip();
  }

  // End pump pulse after MOTOR_PULSE_MS
  if (motorPulsing && millis() - motorPulseStart >= MOTOR_PULSE_MS) {
    motorPulsing = false;
    digitalWrite(DRV_IN1, LOW);   // stop pump, driver stays awake until Firebase says off
    Serial.println("Motor: PULSE END");
  }

  // Always collect up to MAX_SAMPLES, one per interval
  if (sampleCount < MAX_SAMPLES && millis() - lastSample >= SAMPLE_INTERVAL_MS) {
    lastSample = millis();
    sampleSum += analogRead(MOISTURE_PIN);
    sampleCount++;
    int avg = sampleSum / sampleCount;
    Serial.print("Sample ");
    Serial.print(sampleCount);
    Serial.print("/");
    Serial.print(MAX_SAMPLES);
    Serial.print(": raw=");
    Serial.println(avg);

    // Update face immediately; Firebase batching handled separately in sendAverage()
    String instState = (avg < THRESHOLD_HAPPY) ? "Happy" : (avg > THRESHOLD_SAD) ? "Sad" : "Middle";
    if (instState != displayState && wateringDisplayEnd == 0) {
      // Sad -> Happy jump: start artificial water confirmation window
      if (trackArtificialWater && displayState == "Sad" && instState == "Happy") {
        artificialWaterPending = true;
        sadToHappyMs = millis();
      }
      // Left Happy before confirmation; cancel
      if (artificialWaterPending && instState != "Happy") {
        artificialWaterPending = false;
      }
      drawFace(instState);
      displayState = instState;
    }
  }

  // Send as soon as Firebase is ready and we meet the minimum sample count
  if (Firebase.ready() && sampleCount >= MIN_SAMPLES) {
    sendAverage();
  }

  // Refresh WiFi icon only when signal strength changes and no transient overlay is active
  if (displayState.length() > 0 && wateringDisplayEnd == 0 &&
      millis() - lastWifiCheck >= WIFI_CHECK_MS) {
    lastWifiCheck = millis();
    int bars = wifiIconBars();
    if (bars != lastWifiBars) {
      blitRows(0, 22);   // restore bitmap pixels before redrawing overlays
      redrawOverlays();
    }
  }

  // Sleep the display panel after DISPLAY_SLEEP_MS of no activity
  if (!displayAsleep && lastActivityMs > 0 &&
      millis() - lastActivityMs >= DISPLAY_SLEEP_MS) {
    tft.enableSleep(true);
    displayAsleep = true;
    Serial.println("Display: SLEEP");
  }

  // Poll Firebase for motorState and drive the motor pin
  if (Firebase.ready() && millis() - lastMotorPoll >= MOTOR_POLL_MS) {
    lastMotorPoll = millis();
    if (Firebase.RTDB.getString(&fbdo, FIREBASE_DB_PATH "/motorState")) {
      bool motorOn = (fbdo.stringData() == "on");
      if (motorOn != lastMotorOn) {
        if (motorOn) {
          digitalWrite(DRV_SLEEP,  HIGH);  // wake driver
          digitalWrite(DRV_IN1,    HIGH);  // run pump
          motorPulsing       = true;
          motorPulseStart    = millis();
          wateringDisplayEnd = millis() + 2000;
          drawPumpAnimation();
          Serial.println("Motor: PULSE START");
        } else {
          motorPulsing = false;
          digitalWrite(DRV_IN1, LOW);                      // coast (stop)
          if (!lastAppOnline) digitalWrite(DRV_SLEEP, LOW); // sleep only if app offline
          Serial.println("Motor: OFF");
        }
        lastMotorOn = motorOn;
      }
    }
  }

  // Poll Firebase for app presence; adjust CPU speed and driver accordingly
  if (Firebase.ready() && millis() - lastAppPoll >= APP_POLL_MS) {
    lastAppPoll = millis();
    if (Firebase.RTDB.getBool(&fbdo, FIREBASE_DB_PATH "/isOnline")) {
      bool appOnline = fbdo.boolData();
      if (appOnline != lastAppOnline) {
        lastAppOnline = appOnline;
        if (appOnline) {
          setCpuFrequencyMhz(240);
          digitalWrite(DRV_SLEEP, HIGH);  // pre-arm driver
          Serial.println("App: ONLINE — 240 MHz, driver armed");
        } else {
          setCpuFrequencyMhz(80);
          motorPulsing = false;
          digitalWrite(DRV_IN1,   LOW);
          digitalWrite(DRV_SLEEP, LOW);   // sleep driver
          Serial.println("App: OFFLINE — 80 MHz, driver slept");
        }
        // Redraw face so border appears (online) or disappears (offline) immediately.
        if (displayState.length() > 0) drawFace(displayState);
      }
    }
  }

  // Poll trackArtificialWater flag from Firebase
  if (Firebase.ready() && millis() - lastTrackPoll >= TRACK_POLL_MS) {
    lastTrackPoll = millis();
    if (Firebase.RTDB.getBool(&fbdo, FIREBASE_DB_PATH "/trackArtificialWater")) {
      trackArtificialWater = fbdo.boolData();
    }
  }

  // Confirm artificial watering: Happy held for 5 s after Sad->Happy jump
  if (artificialWaterPending) {
    if (displayState != "Happy") {
      artificialWaterPending = false;
    } else if (millis() - sadToHappyMs >= ARTIFICIAL_WATER_CONFIRM_MS) {
      artificialWaterPending = false;
      flashRed();
      drawFace(displayState);
      // Red banner overlay
      tft.fillRect(0, 48, TFT_WIDTH, 32, 0xF800);
      tft.setTextColor(0xFFFF);
      tft.setTextSize(1);
      tft.setCursor(14, 54);
      tft.print("Artificially Watered!");
      tft.setCursor(16, 66);
      tft.print("Please do your tasks");
      delay(1000);
      if (Firebase.ready()) {
        int count = 0;
        if (Firebase.RTDB.getInt(&fbdo, FIREBASE_DB_PATH "/artificialWaters")) {
          count = fbdo.intData();
        }
        Firebase.RTDB.setInt(&fbdo, FIREBASE_DB_PATH "/artificialWaters", count + 1);
        Serial.println("Artificial watering detected — incremented artificialWaters");
      }
    }
  }

  tickBorder();

  delay(10);  // yield CPU rather than tight-spinning
}
