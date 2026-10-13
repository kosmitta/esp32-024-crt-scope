#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <Preferences.h>

// ===== OSC-32 — finalny, wszystko na HSPI (jak CheshirCa/ESP32-024) =====
// TZT L1435 2.4" / ESP32-024: ST7789 + XPT2046 na HSPI 12/13/14,
// CS lcd=15, DC=2, BL=27, touch CS=33, IRQ=36, SD_CS=5 (odznaczony).
// Kolory panelu zanegowane -> INV() w software. Fosfor 4bpp: wlasny bufor.

#define PROBE_PIN 34
#define DISP_CS   15
#define DISP_DC   2
#define TOUCH_CS  33
#define TOUCH_IRQ 36
#define SD_CS     5
#define BL_PIN    27
#define BTN_BOOT  0

#define W       320
#define TOP_H   26
#define WAVE_H  190
#define BOT_H   24
#define NSAMP   640

SPIClass displaySpi(HSPI);
Adafruit_ST7789 tft(&displaySpi, DISP_CS, DISP_DC, -1);
Preferences prefs;

#define RGB565(r, g, b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
// Adafruit_ST7789 sam inicjalizuje inwersje panelu — software'owe negowanie zbędne
#define INV(c) ((uint16_t)(c))

// paleta fosforowa (wartosci logiczne; przy push nastepuje INV)
static const uint16_t PAL[16] = {
  RGB565(0, 0, 0), RGB565(0, 30, 6), RGB565(0, 70, 15), RGB565(0, 140, 30),
  RGB565(0, 225, 45), RGB565(185, 255, 185), 0, 0,
  RGB565(0, 52, 0), RGB565(0, 95, 0), 0, 0, 0, 0, 0, 0
};
static const uint8_t DECAY[16] = {0, 0, 1, 2, 3, 4, 0, 0, 8, 8, 0, 0, 0, 0, 0, 0};
enum { I_BG = 0, I_P1, I_P2, I_P3, I_GLOW, I_CORE, I_SP6, I_SP7, I_GRID, I_GRIDC };

// wlasny bufor 4bpp (2 piksele/bajt)
static uint8_t phos[(W * WAVE_H) / 2];
static uint16_t line16[W];

#define TFT_BLACK 0x0000
#define TFT_GREEN 0x07E0
const uint16_t SCR_BG    = INV(TFT_BLACK);
const uint16_t SCR_TXT   = INV(TFT_GREEN);
const uint16_t SCR_TXTD  = INV(RGB565(0, 150, 0));
const uint16_t SCR_AMBER = INV(RGB565(255, 170, 0));
const uint16_t SCR_DIM   = INV(RGB565(0, 120, 0));
const uint16_t SCR_SEL   = INV(RGB565(0, 45, 0));

static const uint32_t TB_DELAY[4] = {0, 50, 200, 800};
static const char *TB_NAME[4] = {"1x", "2x", "5x", "10x"};
static const uint8_t PERS_FRM[4] = {1, 1, 2, 4};
static const char *PERS_NAME[4] = {"OFF", "KROTKI", "SREDNI", "DLUGI"};
enum Wf { WF_AUTO = 0, WF_SINE, WF_SQR, WF_TRI, WF_SAW, WF_NOISE, WF_COUNT };
static const char *WF_NAME[] = {"AUTO", "SINUS", "KWADRAT", "TROJKAT", "PILA", "SZUM"};

static int8_t setTbase = 0, setPersist = 3, setWave = WF_AUTO;
static bool setGrid = true;

static uint16_t samples[NSAMP];
static bool probeMode = false, trigd = false;
static float measFreq = 0, measVpp = 0, g_dtUs = 50;
static float demoPhase = 0;
static Wf demoWf = WF_SINE;
static uint32_t wfChangeMs = 0;

// ================= DOTYK (sekwencja CheshirCa, sprawdzona!) =================
static uint16_t tCmd(uint8_t cmd) {
  displaySpi.transfer(cmd);
  return displaySpi.transfer16(0) >> 3;
}

// zwraca true + srednie X/Y jesli dotyk; p= cisnienie
static bool touchRead(int16_t *px, int16_t *py, int16_t *pp) {
  bool irq = digitalRead(TOUCH_IRQ) == LOW;
  digitalWrite(DISP_CS, HIGH);
  digitalWrite(SD_CS, HIGH);
  displaySpi.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(TOUCH_CS, LOW);
  displaySpi.transfer(0xB1);
  int16_t z1 = displaySpi.transfer16(0xC1) >> 3;
  int16_t pressure = z1 + 4095;
  int16_t z2 = displaySpi.transfer16(0x91) >> 3;
  pressure -= z2;
  int16_t x = 0, y = 0;
  if (pressure >= 400 || irq) {
    displaySpi.transfer16(0x91);
    int16_t y1 = displaySpi.transfer16(0xD1) >> 3;
    int16_t x1 = displaySpi.transfer16(0x91) >> 3;
    int16_t y2 = displaySpi.transfer16(0xD1) >> 3;
    int16_t x2 = displaySpi.transfer16(0x91) >> 3;
    x = (x1 + x2) / 2;
    y = (y1 + y2) / 2;
  }
  digitalWrite(TOUCH_CS, HIGH);
  displaySpi.endTransaction();
  if (pp) *pp = pressure;
  if (pressure >= 400 || irq) {
    if (px) *px = x;
    if (py) *py = y;
    return true;
  }
  return false;
}

// ================= KALIBRACJA 3-PUNKT =================
static bool calValid = false, axisSwap = false;
static int16_t tlX, tlY, trX, trY, blX, blY;

static void drawCross(int x, int y) {
  tft.drawFastHLine(x - 12, y, 25, SCR_AMBER);
  tft.drawFastVLine(x, y - 12, 25, SCR_AMBER);
  tft.drawCircle(x, y, 6, SCR_AMBER);
  tft.fillCircle(x, y, 2, SCR_AMBER);
}

static void waitCal(int16_t *x, int16_t *y) {
  while (!touchRead(NULL, NULL, NULL)) delay(10);
  delay(60);
  int32_t sx = 0, sy = 0;
  for (int i = 0; i < 5; i++) {
    int16_t a, b;
    touchRead(&a, &b, NULL);
    sx += a; sy += b;
    delay(10);
  }
  *x = sx / 5; *y = sy / 5;
  while (touchRead(NULL, NULL, NULL)) delay(10);
  delay(80);
}

static bool calibrateUI() {
  for (int att = 0; att < 2; att++) {
    tft.fillScreen(SCR_BG);
    tft.setTextColor(SCR_TXT, SCR_BG); tft.setTextSize(2);
    tft.setCursor(46, 8);  tft.print("KALIBRACJA 1/3");
    tft.setTextColor(SCR_AMBER, SCR_BG); tft.setTextSize(1);
    tft.setCursor(70, 28); tft.print("dotknij celownik i pusc");
    drawCross(40, 60);
    waitCal(&tlX, &tlY);
    tft.fillScreen(SCR_BG);
    tft.setTextColor(SCR_TXT, SCR_BG); tft.setTextSize(2);
    tft.setCursor(46, 8);  tft.print("KALIBRACJA 2/3");
    tft.setTextColor(SCR_AMBER, SCR_BG); tft.setTextSize(1);
    tft.setCursor(70, 28); tft.print("dotknij celownik i pusc");
    drawCross(280, 60);
    waitCal(&trX, &trY);
    tft.fillScreen(SCR_BG);
    tft.setTextColor(SCR_TXT, SCR_BG); tft.setTextSize(2);
    tft.setCursor(46, 8);  tft.print("KALIBRACJA 3/3");
    tft.setTextColor(SCR_AMBER, SCR_BG); tft.setTextSize(1);
    tft.setCursor(70, 28); tft.print("dotknij celownik i pusc");
    drawCross(40, 180);
    waitCal(&blX, &blY);
    Serial.printf("[TOUCH] kal TL=(%d,%d) TR=(%d,%d) BL=(%d,%d)\n", tlX, tlY, trX, trY, blX, blY);

    long dX_TR = abs((long)trX - tlX), dY_TR = abs((long)trY - tlY);
    axisSwap = (labs(dX_TR) < labs(dY_TR));      // dominujaca os w poziomie = X
    long dx = axisSwap ? labs((long)trY - tlY) : labs((long)trX - tlX);
    long dy = axisSwap ? labs((long)blX - tlX) : labs((long)blY - tlY);
    Serial.printf("[TOUCH] swap=%d dx=%ld dy=%ld\n", axisSwap, dx, dy);
    if (dx > 250 && dy > 250) {
      prefs.putShort("tlx", tlX); prefs.putShort("tly", tlY);
      prefs.putShort("trx", trX); prefs.putShort("try_", trY);
      prefs.putShort("blx", blX); prefs.putShort("bly", blY);
      prefs.putBool("swp", axisSwap);
      return true;
    }
    Serial.println("[TOUCH] zla kalibracja, powtorka");
  }
  return false;
}

// mapowanie surowych -> ekran
static bool touchMap(int *sx, int *sy) {
  if (!calValid) return false;
  int16_t rx, ry;
  if (!touchRead(&rx, &ry, NULL)) return false;
  int16_t x1 = axisSwap ? tlY : tlX;
  int16_t x2 = axisSwap ? trY : trX;
  int16_t y1 = axisSwap ? tlX : tlY;
  int16_t y2 = axisSwap ? blX : blY;
  int16_t r_x = axisSwap ? ry : rx;
  int16_t r_y = axisSwap ? rx : ry;
  if (x2 == x1 || y2 == y1) return false;
  float fx = 40.0f + (float)(r_x - x1) * (280.0f - 40.0f) / (float)(x2 - x1);
  float fy = 60.0f + (float)(r_y - y1) * (180.0f - 60.0f) / (float)(y2 - y1);
  *sx = constrain((int)fx, 0, W - 1);
  *sy = constrain((int)fy, 0, 239);
  return true;
}

// ================= BOOT =================
static uint8_t bootEvent() {
  static uint32_t tDown = 0;
  static bool down = false;
  bool pressed = (digitalRead(BTN_BOOT) == LOW);
  uint32_t now = millis();
  if (pressed && !down) { down = true; tDown = now; }
  if (!pressed && down) {
    down = false;
    if (now - tDown > 1200) return 2;
    if (now - tDown > 30) return 1;
  }
  return 0;
}

// ================= FOSFOR 4bpp =================
static inline void phPut(int x, int y, uint8_t idx) {
  if (x < 0 || x >= W || y < 0 || y >= WAVE_H) return;
  int i = (y * W + x) >> 1;
  if ((y * W + x) & 1) phos[i] = (phos[i] & 0xF0) | (idx & 0x0F);
  else                 phos[i] = (phos[i] & 0x0F) | ((idx & 0x0F) << 4);
}
static inline uint8_t phGet(int x, int y) {
  int i = (y * W + x) >> 1;
  return ((y * W + x) & 1) ? (phos[i] & 0x0F) : (phos[i] >> 4);
}
static void phVLine(int x, int y0, int y1, uint8_t idx) {
  if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
  for (int y = y0; y <= y1; y++) phPut(x, y, idx);
}

static void phDecay() {
  for (size_t i = 0; i < sizeof(phos); i++) {
    uint8_t b = phos[i];
    phos[i] = (uint8_t)((DECAY[b >> 4] << 4) | DECAY[b & 0x0F]);
  }
}

static void drawGraticule() {
  for (int x = 0; x <= W; x += W / 10)
    phVLine(x, 0, WAVE_H - 1, (x == W / 2) ? I_GRIDC : I_GRID);
  for (int y = 0; y < WAVE_H; y += WAVE_H / 8) {
    int idx = (y == WAVE_H / 2) ? I_GRIDC : I_GRID;
    for (int x = 0; x < W; x++) phPut(x, y, idx);
  }
  for (int x = W / 10; x < W; x += W / 10)
    for (int k = -3; k <= 3; k++) {
      phPut(x, WAVE_H / 2 + k, I_GRIDC);
      phPut(W / 2 + k, x, I_GRIDC);
    }
}

static void drawTrace() {
  int prevY = -1;
  for (int x = 0; x < W; x++) {
    uint32_t v = samples[x];
    if (v > 4095) v = 4095;
    int y = WAVE_H - 1 - (int)((v * (WAVE_H - 4)) >> 12) - 2;
    if (y < 0) y = 0;
    if (y >= WAVE_H) y = WAVE_H - 1;
    if (prevY >= 0 && abs(y - prevY) > 1)
      phVLine(x, y, prevY, I_GLOW);
    else
      phVLine(x, y - 1, y + 1, I_GLOW);
    phPut(x, y, I_CORE);
    prevY = y;
  }
}

static void phPush() {                    // konwersja 4bpp -> RGB565 (z INV) linia po linii
  tft.startWrite();
  for (int y = 0; y < WAVE_H; y++) {
    const uint8_t *row = &phos[(y * W) >> 1];
    for (int x = 0; x < W; x++)
      line16[x] = INV(PAL[(x & 1) ? (row[x >> 1] & 0x0F) : (row[x >> 1] >> 4)]);
    tft.setAddrWindow(0, TOP_H + y, W, 1);
    tft.writePixels(line16, W);
  }
  tft.endWrite();
}

// ================= PRZEBIEG =================
static void sampleSweep() {
  uint32_t delayPer = TB_DELAY[setTbase];
  uint32_t t0 = micros();
  for (int i = 0; i < NSAMP; i++) {
    samples[i] = analogRead(PROBE_PIN);
    if (delayPer) delayMicroseconds(delayPer);
  }
  g_dtUs = (micros() - t0) / (float)NSAMP;

  uint16_t mn = 4095, mx = 0;
  for (int i = 0; i < NSAMP; i++) {
    if (samples[i] < mn) mn = samples[i];
    if (samples[i] > mx) mx = samples[i];
  }
  probeMode = (mx - mn) > 150;
  float dtS = g_dtUs * 1e-6f;
  int mid = (mn + mx) / 2;
  int trig = -1;

  if (probeMode) {
    int cross = 0;
    for (int i = 1; i < NSAMP; i++)
      if (samples[i - 1] < mid && samples[i] >= mid) cross++;
    measFreq = cross / (NSAMP * dtS);
    measVpp = (mx - mn) * 3300.0f / 4095.0f;
    for (int i = 1; i < NSAMP - W; i++)
      if (samples[i - 1] < mid && samples[i] >= mid) { trig = i; break; }
  } else {
    uint32_t now = millis();
    if (setWave == WF_AUTO) {
      if (now - wfChangeMs > 10000) { wfChangeMs = now; demoWf = (Wf)((demoWf + 1) % (WF_COUNT - 1)); }
    } else {
      demoWf = (Wf)(setWave - 1);
    }
    Wf wf = (Wf)(demoWf + 1);
    float t = now * 0.001f;
    float f = 34.0f * (1.0f + 0.18f * sinf(t * 0.7f));
    float A = 1550.0f * (0.82f + 0.18f * sinf(t * 0.31f));
    measFreq = f;
    measVpp = 2.0f * A * 3300.0f / 4095.0f;
    demoPhase += 2.0f * PI * f * NSAMP * dtS;
    if (demoPhase > 1e7f) demoPhase = 0;
    float ph = demoPhase, noise = 0;
    for (int i = 0; i < NSAMP; i++) {
      float fr = ph / (2.0f * PI); fr -= floorf(fr);
      float v;
      switch (wf) {
        case WF_SQR:  v = (sinf(ph) >= 0) ? A : -A; break;
        case WF_TRI:  v = A * (4.0f * fabsf(fr - 0.5f) - 1.0f); break;
        case WF_SAW:  v = A * (2.0f * fr - 1.0f); break;
        case WF_NOISE: noise += random(-300, 300) * 0.02f; noise *= 0.97f; v = noise * 4.0f; break;
        default:      v = A * sinf(ph); break;
      }
      ph += 2.0f * PI * f * dtS;
      int vv = 2048 + (int)v + ((wf != WF_NOISE) ? random(-14, 14) : 0);
      samples[i] = constrain(vv, 0, 4095);
    }
    for (int i = 1; i < NSAMP - W; i++)
      if (samples[i - 1] < 2048 && samples[i] >= 2048) { trig = i; break; }
  }
  trigd = (trig > 0);
  if (trig > 0)
    for (int x = 0; x < W; x++) samples[x] = samples[trig + x];
}

// ================= BELKI =================
static void drawBars(uint32_t frame) {
  char b[48];
  tft.fillRect(0, 0, W, TOP_H, SCR_BG);
  tft.drawFastHLine(0, TOP_H - 1, W, SCR_DIM);
  tft.setTextSize(2);
  tft.setTextColor(SCR_AMBER, SCR_BG);
  tft.setCursor(6, 6);   tft.print("OSC-32");
  tft.setTextSize(1);
  tft.setTextColor(SCR_TXTD, SCR_BG);
  tft.setCursor(78, 10); tft.print("CRT");
  tft.setTextColor(SCR_TXT, SCR_BG);
  snprintf(b, sizeof(b), "0.4V/DIV %.1fms/DIV", g_dtUs * 32.0f / 1000.0f);
  tft.setCursor(106, 10); tft.print(b);

  int by = TOP_H + WAVE_H;
  tft.fillRect(0, by, W, BOT_H, SCR_BG);
  tft.drawFastHLine(0, by, W, SCR_DIM);
  tft.setTextColor(SCR_TXT, SCR_BG);
  if (probeMode) snprintf(b, sizeof(b), "WE: GPIO34");
  else           snprintf(b, sizeof(b), "DEMO:%s", WF_NAME[demoWf + 1]);
  tft.setCursor(6, by + 9); tft.print(b);
  tft.setTextColor(SCR_AMBER, SCR_BG);
  tft.setCursor(215, by + 9); tft.print("dotyk=menu");
  snprintf(b, sizeof(b), "%.0fHz %.2fVpp", measFreq, measVpp / 1000.0f);
  tft.setTextColor(SCR_TXT, SCR_BG);
  tft.setCursor(132, by + 9); tft.print(b);
  tft.setTextColor(((frame / 15) % 2) ? SCR_AMBER : SCR_DIM, SCR_BG);
  tft.setTextSize(2);
  tft.setCursor(276, by + 4);
  tft.print(trigd ? "*" : "-");
  tft.setTextSize(1);
}

// ================= MENU =================
static uint8_t selRow = 0;

static void drawSettings() {
  tft.fillScreen(SCR_BG);
  tft.drawRect(18, 22, W - 36, 196, SCR_DIM);
  tft.drawRect(20, 24, W - 40, 192, SCR_AMBER);
  tft.setTextSize(2);
  tft.setTextColor(SCR_AMBER, SCR_BG);
  int tw = strlen("U S T A W I E N I A") * 12;
  tft.setCursor((W - tw) / 2, 32); tft.print("U S T A W I E N I A");

  static const char *LBL[4] = {"Podstawa czasu", "Fosfor", "Przebieg demo", "Siatka"};
  const char *val[4];
  val[0] = TB_NAME[setTbase];
  val[1] = PERS_NAME[setPersist];
  val[2] = WF_NAME[setWave];
  val[3] = setGrid ? "ON" : "OFF";

  for (int r = 0; r < 4; r++) {
    int y = 62 + r * 30;
    tft.drawFastHLine(28, y + 24, W - 56, SCR_DIM);
    if (r == selRow) tft.fillRect(24, y, W - 48, 26, SCR_SEL);
    tft.setTextColor((r == selRow) ? SCR_AMBER : SCR_TXT, SCR_BG);
    tft.setCursor(30, y + 6); tft.print(LBL[r]);
    int vw = strlen(val[r]) * 12;
    tft.setTextColor(SCR_AMBER, SCR_BG);
    tft.setCursor(W - 34 - vw, y + 6); tft.print(val[r]);
  }

  bool selClose = (selRow == 4);
  if (selClose) tft.fillRect(108, 184, 104, 28, SCR_SEL);
  tft.drawRect(110, 186, 100, 24, selClose ? SCR_TXT : SCR_AMBER);
  tft.setTextColor(selClose ? SCR_AMBER : SCR_TXT, SCR_BG);
  int zw = strlen("ZAMKNIJ") * 12;
  tft.setCursor(160 - zw / 2, 192); tft.print("ZAMKNIJ");
  tft.setTextSize(1);
  tft.setTextColor(SCR_TXTD, SCR_BG);
  tft.setCursor(30, 222); tft.print("dotknij wiersza = zmiana | BOOT: krotko=next, dlugo=zmiana");
}

static void settingsChange(uint8_t row) {
  switch (row) {
    case 0: setTbase = (setTbase + 1) % 4; break;
    case 1: setPersist = (setPersist + 1) % 4; break;
    case 2: setWave = (setWave + 1) % WF_COUNT; break;
    case 3: setGrid = !setGrid; break;
  }
  prefs.putChar("tb", setTbase); prefs.putChar("pe", setPersist);
  prefs.putChar("wf", setWave); prefs.putBool("gr", setGrid);
}

static void settingsLoop() {
  drawSettings();
  while (true) {
    int sx, sy;
    if (touchMap(&sx, &sy)) {
      delay(30);
      int lx = sx, ly = sy;
      while (touchMap(&sx, &sy)) { lx = sx; ly = sy; delay(10); }
      delay(60);
      Serial.printf("[TOUCH] tap (%d,%d)\n", lx, ly);
      if (ly >= 186 && lx >= 110 && lx <= 210) return;
      int row = (ly - 56) / 30;
      if (ly >= 56 && row <= 3) {
        selRow = row;
        settingsChange(row);
        Serial.printf("[SET] row=%d\n", row);
        drawSettings();
      }
    }
    uint8_t ev = bootEvent();
    if (ev == 1) { selRow = (selRow + 1) % 5; drawSettings(); }
    else if (ev == 2) {
      if (selRow == 4) return;
      settingsChange(selRow);
      drawSettings();
    }
    delay(10);
  }
}

// ================= START =================
static void loadSettings() {
  prefs.begin("osc32", false);
  setTbase   = prefs.getChar("tb", 0);
  setPersist = prefs.getChar("pe", 3);
  setWave    = prefs.getChar("wf", WF_AUTO);
  setGrid    = prefs.getBool("gr", true);
  setTbase = constrain(setTbase, 0, 3);
  setPersist = constrain(setPersist, 0, 3);
  setWave = constrain(setWave, 0, WF_COUNT - 1);
  tlX = prefs.getShort("tlx", 0); tlY = prefs.getShort("tly", 0);
  trX = prefs.getShort("trx", 0); trY = prefs.getShort("try_", 0);
  blX = prefs.getShort("blx", 0); blY = prefs.getShort("bly", 0);
  axisSwap = prefs.getBool("swp", false);
  calValid = (tlX | tlY | trX | trY | blX | blY) != 0;
  // stara kalibracja z martwego ukladu? wymaz
  if (calValid && (tlX > 5000 || trX > 5000)) calValid = false;
}

void setup() {
  Serial.begin(115200);
  pinMode(BL_PIN, OUTPUT);
  digitalWrite(BL_PIN, HIGH);
  pinMode(TOUCH_CS, OUTPUT);
  digitalWrite(TOUCH_CS, HIGH);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  pinMode(TOUCH_IRQ, INPUT);
  pinMode(BTN_BOOT, INPUT_PULLUP);

  displaySpi.begin(14, 12, 13, DISP_CS);
  tft.init(240, 320, SPI_MODE0);
  tft.setSPISpeed(40000000);
  tft.invertDisplay(false);
  tft.setRotation(1);
  tft.fillScreen(SCR_BG);

  analogSetAttenuation(ADC_11db);
  randomSeed(esp_random());
  loadSettings();
  memset(phos, 0, sizeof(phos));

  Serial.printf("[SCOPE] start HSPI, dotyk: %s\n",
                calValid ? "skalibrowany" : "wymaga kalibracji (dotknij ekran)");
}

void loop() {
  static uint32_t frame = 0;

  int16_t trawX, trawY, trawP;
  if (touchRead(&trawX, &trawY, &trawP)) {          // surowa brama — dziala bez kalibracji
    Serial.printf("[TOUCH] raw (%d,%d) p=%d\n", trawX, trawY, trawP);
    delay(40);
    while (touchRead(NULL, NULL, NULL)) delay(10);
    if (!calValid) calValid = calibrateUI();
    else {
      settingsLoop();
      tft.fillScreen(SCR_BG);
      memset(phos, 0, sizeof(phos));
      Serial.println("[SCOPE] wrocono do oscyloskopu");
    }
  }
  if (bootEvent() == 1) {
    settingsLoop();
    tft.fillScreen(SCR_BG);
    memset(phos, 0, sizeof(phos));
  }

  sampleSweep();

  if (setPersist == 0) {
    memset(phos, 0, sizeof(phos));
  } else if ((frame % PERS_FRM[setPersist]) == 0) {
    phDecay();
  }
  if (setGrid) drawGraticule();
  drawTrace();
  phPush();

  drawBars(frame);
  frame++;

  if (frame % 100 == 0)
    Serial.printf("[SCOPE] %s f=%.0fHz Vpp=%.0fmV trig=%d\n",
                  probeMode ? "PROBE" : "DEMO", measFreq, measVpp, trigd);
  delay(5);
}
