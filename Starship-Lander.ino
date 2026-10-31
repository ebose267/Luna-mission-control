#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Preferences.h>

#if __has_include(<esp_arduino_version.h>)
  #include <esp_arduino_version.h>
#endif
#ifndef ESP_ARDUINO_VERSION_MAJOR
  #define ESP_ARDUINO_VERSION_MAJOR 2
#endif

// ======================= PIN DEFINITIONS =======================
#define TFT_SCK     12
#define TFT_MOSI    11
#define TFT_RST      8
#define TFT_DC       9
#define TFT_CS      10
#define BL_PIN       7

#define JOY_X_PIN   15
#define JOY_Y_PIN    6
#define JOY_SW_PIN  17
#define PIEZO_PIN   39  // ESP32-S3: usable output. Original ESP32: GPIO39 is input-only.

// ======================= DISPLAY & GAME TUNING =======================
#define SPI_FREQ       40000000
#define INVERT_COLORS  true
#define W              240
#define H              320

#define JOY_CENTER_ADC 2048
#define JOY_DEADZONE   250

const float DEG2RAD   = 0.0174532925f;
const float MAX_ACC   = 45.0f;
const float BURN_RATE = 10.0f;
const float VY_MAX    = 30.0f;
const float VX_MAX    = 20.0f;
const float ANG_MAX   = 15.0f;

// ======================= COLOUR PALETTE =======================
#define RGB565(r, g, b) (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))

const uint16_t C_SPACE_BG  = RGB565(5, 5, 18);
const uint16_t C_WHITE     = 0xFFFF;
const uint16_t C_GREY      = RGB565(140, 150, 170);
const uint16_t C_STEEL     = RGB565(220, 225, 240);
const uint16_t C_HEATSHLD  = RGB565(40, 40, 50);
const uint16_t C_TERR_FILL = RGB565(25, 20, 45);
const uint16_t C_TERR_NEON = RGB565(180, 40, 255);
const uint16_t C_PAD_NEON  = RGB565(0, 255, 180);
const uint16_t C_RED       = RGB565(255, 40, 90);
const uint16_t C_GREEN     = RGB565(40, 255, 120);
const uint16_t C_YELLOW    = RGB565(255, 230, 30);
const uint16_t C_CYAN      = RGB565(0, 220, 255);
const uint16_t C_PINK      = RGB565(255, 60, 180);
const uint16_t C_HUD_BG    = RGB565(15, 15, 30);
const uint16_t C_DUST1     = RGB565(200, 180, 220);
const uint16_t C_DUST2     = RGB565(140, 100, 180);

// ======================= HARDWARE OBJECTS =======================
Adafruit_ST7789 tft(&SPI, TFT_CS, TFT_DC, TFT_RST);
GFXcanvas16 *canvas = nullptr;
Preferences prefs;

// ======================= SOUND ENGINE =======================
struct Note {
  uint16_t f;
  uint16_t ms;
};

const Note SND_START[] = {
  {523, 65}, {659, 65}, {784, 65}, {1047, 150}
};
const Note SND_LAND[] = {
  {523, 75}, {659, 75}, {784, 75}, {1047, 110}, {1318, 210}
};
const Note SND_CRASH[] = {
  {440, 65}, {330, 75}, {220, 90}, {130, 260}
};
const Note SND_WARN[] = {
  {880, 75}, {0, 90}, {880, 75}, {0, 90}
};
const Note SND_SELECT[] = {
  {880, 35}, {1175, 55}
};

#define ARRLEN(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))

const Note *seq = nullptr;
uint8_t seqLen = 0;
uint8_t seqIdx = 0;
uint32_t noteEnd = 0;
int lastTone = -1;
bool buzzerReady = false;
uint32_t lastEngineToneAt = 0;
uint32_t nextFuelWarningAt = 0;

#if ESP_ARDUINO_VERSION_MAJOR < 3
  static const uint8_t BUZZER_CHANNEL = 0;
#endif

void setTone(int frequency) {
  frequency = constrain(frequency, 0, 4000);
  if (!buzzerReady || frequency == lastTone) return;
  lastTone = frequency;

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWriteTone(PIEZO_PIN, (uint32_t)frequency);
#else
  ledcWriteTone(BUZZER_CHANNEL, (uint32_t)frequency);
#endif
}

void buzzerBegin() {
  pinMode(PIEZO_PIN, OUTPUT);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  buzzerReady = ledcAttach(PIEZO_PIN, 2000, 10);
#else
  ledcSetup(BUZZER_CHANNEL, 2000, 10);
  ledcAttachPin(PIEZO_PIN, BUZZER_CHANNEL);
  buzzerReady = true;
#endif
  lastTone = -1;
  setTone(0);
}

void playSeq(const Note *notes, uint8_t count) {
  seq = notes;
  seqLen = count;
  seqIdx = 0;
  noteEnd = 0;
}

void soundUpdate() {
  if (!seqLen) return;

  const uint32_t now = millis();
  if (now < noteEnd) return;

  if (seqIdx >= seqLen) {
    seqLen = 0;
    setTone(0);
    return;
  }

  setTone(seq[seqIdx].f);
  noteEnd = now + seq[seqIdx].ms;
  seqIdx++;
}

void updateEngineSound(float throttle) {
  if (seqLen) return;  // Music and alerts take priority over the engine.

  if (throttle < 1.0f) {
    setTone(0);
    return;
  }

  const uint32_t now = millis();
  if (now - lastEngineToneAt < 85) return;
  lastEngineToneAt = now;

  // A steady pitch that rises with throttle. No frame-by-frame random squealing.
  const int engineFrequency = 150 + (int)(throttle * 3.2f);
  if (abs(engineFrequency - lastTone) >= 7) setTone(engineFrequency);
}

// ======================= GAME STATE =======================
enum State {
  ST_TITLE,
  ST_PLAY,
  ST_LANDED,
  ST_CRASH
};

State state = ST_TITLE;
uint32_t stateT = 0;

struct Ship {
  float x, y, vx, vy, ang, thr, fuel;
};
Ship s;

struct Pad {
  int16_t x0, x1;
  uint8_t mult;
};
Pad pads[2];
int16_t terr[W];

int level = 1;
uint32_t score = 0;
uint32_t hiScore = 0;
uint32_t lastPts = 0;
const char *crashReason = "";
const char *landingLine = "THE PAD ACCEPTS YOU.";
const char *crashLine = "THE PLANET WINS THIS ROUND.";
float gravity = 11.5f;

// Mission Control's current line appears in the narrow banner below the HUD.
const char *missionLine = "MISSION CONTROL: KEEP IT TOGETHER.";
uint32_t nextCommentAt = 0;

// ======================= STARFIELD & PARTICLES =======================
struct Star {
  float x, y;
  uint8_t layer;
  uint16_t color;
};
const int NSTARS = 35;
Star stars[NSTARS];

struct DustParticle {
  float x, y, vx, vy, life, maxLife;
  uint16_t color;
};
const int NDUST = 45;
DustParticle dust[NDUST];

// ======================= MISSION CONTROL COMMENTARY =======================
const char *const AMBIENT_LINES[] = {
  "STEADY NOW. LET'S KEEP IT THAT WAY.",
  "THE GROUND IS NOT A CUSHION.",
  "FUEL GAUGE: WE'RE WATCHING.",
  "SMOOTH INPUTS. NOT SOUP.",
  "LOOKING GOOD. DO NOT TEST THAT.",
  "THE PAD IS THE GREEN GLOWING BIT.",
  "WE HAVE FAITH. THE METERS DON'T.",
  "CONTROL WOULD LOOK GOOD ON YOU."
};

const char *const LANDING_LINES[] = {
  "THE PAD ACCEPTS YOU.",
  "ALMOST PROFESSIONAL.",
  "THE LEGS SURVIVED.",
  "MISSION CONTROL APPLAUDS.",
  "YOU LANDED. TWICE-CHECKED.",
  "CLEAN LANDING. WHO TAUGHT YOU?"
};

const char *const CRASH_LINES[] = {
  "THE PLANET WINS THIS ROUND.",
  "THAT WAS AN EXPENSIVE ARRIVAL.",
  "WE'LL CALL IT GEOLOGY.",
  "MISSION CONTROL BLINKED.",
  "LANDING WAS A SUGGESTION.",
  "GOOD NEWS: YOU FOUND THE GROUND."
};

void setMissionLine(const char *line) {
  missionLine = line;
}

void updateCommentary(float distToGround) {
  const uint32_t now = millis();
  if (now < nextCommentAt) return;

  const char *line = nullptr;
  uint32_t delayMs = 4200;

  if (s.fuel <= 15.0f) {
    line = "FUEL LOW. SHOULD'VE PACKED MORE.";
    delayMs = 2400;
  } else if (fabsf(s.ang) > 42.0f) {
    line = "YOU'RE FLYING SIDEWAYS. VERY BOLD.";
    delayMs = 2200;
  } else if (fabsf(s.vx) > VX_MAX) {
    line = "THAT DRIFT HAS ITS OWN POSTCODE.";
    delayMs = 2200;
  } else if (s.vy > VY_MAX) {
    line = "BRAKE. THE PLANET IS COMING UP.";
    delayMs = 2200;
  } else if (distToGround < 35.0f) {
    line = "GROUND CONTACT SOON. MAKE IT COUNT.";
    delayMs = 2200;
  } else if (distToGround < 90.0f) {
    line = "EASY. THIS IS THE EXPENSIVE PART.";
    delayMs = 3000;
  } else {
    line = AMBIENT_LINES[random(ARRLEN(AMBIENT_LINES))];
    delayMs = random(4200, 7000);
  }

  setMissionLine(line);
  nextCommentAt = now + delayMs;
}

// ======================= DUST ENGINE =======================
void spawnGroundDust(float groundX, float groundY, float intensity) {
  if (intensity < 0.05f) return;

  for (int i = 0; i < NDUST; i++) {
    if (dust[i].life <= 0.0f) {
      dust[i].x = groundX + random(-12, 13);
      dust[i].y = groundY - random(0, 4);
      const float dir = (random(100) < 50) ? -1.0f : 1.0f;
      dust[i].vx = dir * random(20, 80) * intensity;
      dust[i].vy = -random(5, 30) * intensity;
      dust[i].maxLife = random(15, 45) / 100.0f;
      dust[i].life = dust[i].maxLife;
      dust[i].color = (random(100) < 50) ? C_DUST1 : C_DUST2;
      break;
    }
  }
}

void triggerTouchdownDustCloud(float groundX, float groundY) {
  for (int i = 0; i < 25; i++) {
    spawnGroundDust(groundX, groundY, 1.2f);
  }
}

void updateDust(float dt) {
  for (int i = 0; i < NDUST; i++) {
    if (dust[i].life <= 0.0f) continue;
    dust[i].x += dust[i].vx * dt;
    dust[i].y += dust[i].vy * dt;
    dust[i].vy += 15.0f * dt;
    dust[i].life -= dt;
  }
}

void drawDust() {
  for (int i = 0; i < NDUST; i++) {
    if (dust[i].life <= 0.0f) continue;
    const int sz = (dust[i].life / dust[i].maxLife > 0.5f) ? 2 : 1;
    canvas->fillRect((int)dust[i].x, (int)dust[i].y, sz, sz, dust[i].color);
  }
}

// ======================= TERRAIN & ENVIRONMENT =======================
int wrapIdx(float fx) {
  int ix = (int)floorf(fx) % W;
  if (ix < 0) ix += W;
  return ix;
}

void setPad(int i, int centre, int width, uint8_t mult) {
  pads[i].x0 = centre - width / 2;
  pads[i].x1 = centre + width / 2;
  pads[i].mult = mult;

  const int y = random(245, 292);
  for (int x = pads[i].x0; x <= pads[i].x1; x++) {
    if (x >= 0 && x < W) terr[x] = y;
  }
}

void genTerrain() {
  const int STEP = 20;
  int cp[14];
  for (int i = 0; i < 14; i++) cp[i] = random(220, 298);

  for (int x = 0; x < W; x++) {
    const int i = x / STEP;
    const float t = (x % STEP) / (float)STEP;
    const float h = cp[i] * (1.0f - t) + cp[i + 1] * t + random(-1, 2);
    terr[x] = constrain((int)h, 195, H - 8);
  }

  const int wideW = max(44, 68 - level * 4);
  const int narW  = max(28, 40 - level * 3);
  const bool narrowLeft = random(2);

  setPad(0, random(45, 95), narrowLeft ? narW : wideW, narrowLeft ? 5 : 2);
  setPad(1, random(145, 195), narrowLeft ? wideW : narW, narrowLeft ? 2 : 5);
}

void initStars() {
  for (int i = 0; i < NSTARS; i++) {
    stars[i].x = random(0, W);
    stars[i].y = random(0, 230);
    stars[i].layer = random(1, 4);
    stars[i].color = (stars[i].layer == 1)
      ? RGB565(80, 90, 140)
      : RGB565(180, 200, 240);
  }
}

// ======================= JOYSTICK INPUT =======================
void readJoystickInputs() {
  const int joyX = analogRead(JOY_X_PIN);
  if (joyX < (JOY_CENTER_ADC - JOY_DEADZONE)) {
    s.ang = map(joyX, JOY_CENTER_ADC - JOY_DEADZONE, 0, 0, -75);
  } else if (joyX > (JOY_CENTER_ADC + JOY_DEADZONE)) {
    s.ang = map(joyX, JOY_CENTER_ADC + JOY_DEADZONE, 4095, 0, 75);
  } else {
    s.ang = 0.0f;
  }
  s.ang = constrain(s.ang, -75.0f, 75.0f);

  const int joyY = analogRead(JOY_Y_PIN);
  if (joyY < (JOY_CENTER_ADC - JOY_DEADZONE)) {
    s.thr = map(joyY, JOY_CENTER_ADC - JOY_DEADZONE, 0, 0, 100);
  } else {
    s.thr = 0.0f;
  }
  s.thr = constrain(s.thr, 0.0f, 100.0f);
}

bool isBtnPressed() {
  static bool lastSt = HIGH;
  static uint32_t lastT = 0;
  const bool cur = digitalRead(JOY_SW_PIN);
  bool pressed = false;

  if (cur == LOW && lastSt == HIGH && (millis() - lastT > 50)) {
    pressed = true;
    lastT = millis();
  }
  lastSt = cur;
  return pressed;
}

// ======================= SHIP DRAWING =======================
void drawStarshipAt(float cx, float cy, float angDeg, float thrust01) {
  const float a = angDeg * DEG2RAD;
  const float c = cosf(a);
  const float sn = sinf(a);

  int16_t ax, ay, bx, by, cx1, cy1, dx, dy;
  auto P = [&](float px, float py, int16_t &ox, int16_t &oy) {
    ox = (int16_t)lroundf(cx + px * c - py * sn);
    oy = (int16_t)lroundf(cy + px * sn + py * c);
  };

  // Plasma exhaust.
  if (thrust01 > 0.01f) {
    const float len = 6 + thrust01 * 18 + random(0, 4);
    P(-3, 10, ax, ay); P(3, 10, bx, by); P(0, 10 + len, dx, dy);
    canvas->fillTriangle(ax, ay, bx, by, dx, dy, C_PINK);
    P(-1, 10, ax, ay); P(1, 10, bx, by); P(0, 10 + len * 0.65f, dx, dy);
    canvas->fillTriangle(ax, ay, bx, by, dx, dy, C_CYAN);
  }

  // Stainless-steel body.
  P(-4, -12, ax, ay); P(4, -12, bx, by); P(5, 10, cx1, cy1); P(-5, 10, dx, dy);
  canvas->fillTriangle(ax, ay, bx, by, cx1, cy1, C_STEEL);
  canvas->fillTriangle(ax, ay, cx1, cy1, dx, dy, C_STEEL);

  // Heat-shield nose.
  P(0, -20, ax, ay); P(-4, -12, bx, by); P(4, -12, dx, dy);
  canvas->fillTriangle(ax, ay, bx, by, dx, dy, C_HEATSHLD);

  // Steering flaps and landing legs.
  P(-4, -10, ax, ay); P(-8, -8, bx, by); canvas->drawLine(ax, ay, bx, by, C_CYAN);
  P(4, -10, ax, ay);  P(8, -8, bx, by);  canvas->drawLine(ax, ay, bx, by, C_CYAN);
  P(-5, 6, ax, ay);   P(-9, 10, bx, by); canvas->drawLine(ax, ay, bx, by, C_WHITE);
  P(5, 6, ax, ay);    P(9, 10, bx, by);  canvas->drawLine(ax, ay, bx, by, C_WHITE);
  P(-5, 8, ax, ay);   P(-9, 15, bx, by); canvas->drawLine(ax, ay, bx, by, C_GREY);
  P(5, 8, ax, ay);    P(9, 15, bx, by);  canvas->drawLine(ax, ay, bx, by, C_GREY);
}

void drawStarshipWrapped(float cx, float cy, float angDeg, float thrust01) {
  drawStarshipAt(cx, cy, angDeg, thrust01);
  if (cx < 20) drawStarshipAt(cx + W, cy, angDeg, thrust01);
  if (cx > W - 20) drawStarshipAt(cx - W, cy, angDeg, thrust01);
}

// ======================= HUD & SCREENS =======================
void drawHUD() {
  canvas->fillRect(0, 0, W, 50, C_HUD_BG);
  canvas->drawFastHLine(0, 50, W, C_TERR_NEON);

  canvas->setTextSize(1);
  canvas->setTextColor(C_WHITE);
  canvas->setCursor(4, 4);
  canvas->printf("SCORE %06lu", (unsigned long)score);
  canvas->setCursor(104, 4);
  canvas->printf("LVL %02d", level);
  canvas->setCursor(164, 4);
  canvas->printf("HI %06lu", (unsigned long)hiScore);

  const int fuelWidth = constrain((int)(90.0f * (s.fuel / 100.0f)), 0, 90);
  const int thrustWidth = constrain((int)(90.0f * (s.thr / 100.0f)), 0, 90);
  canvas->fillRect(4, 18, 90, 6, RGB565(35, 45, 55));
  canvas->fillRect(4, 18, fuelWidth, 6, (s.fuel <= 15.0f) ? C_RED : C_GREEN);
  canvas->fillRect(4, 28, 90, 6, RGB565(35, 45, 55));
  canvas->fillRect(4, 28, thrustWidth, 6, C_CYAN);

  canvas->setCursor(120, 18);
  canvas->setTextColor(C_PINK);
  canvas->printf("ANG: %+.0f deg", s.ang);

  canvas->setCursor(4, 38);
  canvas->setTextColor(fabsf(s.vx) <= VX_MAX ? C_GREEN : C_RED);
  canvas->printf("VX %+.0f", s.vx);
  canvas->setCursor(80, 38);
  canvas->setTextColor(s.vy <= VY_MAX ? C_GREEN : C_RED);
  canvas->printf("VY %+.0f", s.vy);
}

void drawMissionBanner() {
  canvas->fillRect(0, 53, W, 16, C_HUD_BG);
  canvas->drawFastHLine(0, 53, W, RGB565(55, 65, 95));
  canvas->setTextSize(1);
  canvas->setTextColor(C_CYAN);
  canvas->setCursor(4, 58);
  canvas->print(missionLine);
}

void drawTitleScreen() {
  canvas->setTextSize(3);
  canvas->setTextColor(C_CYAN);
  canvas->setCursor(20, 110);
  canvas->print("STARSHIP");
  canvas->setTextColor(C_PINK);
  canvas->setCursor(40, 145);
  canvas->print("LANDER");

  canvas->setTextSize(1);
  canvas->setTextColor(C_GREEN);
  canvas->setCursor(24, 204);
  canvas->print("MISSION: LAND IN ONE PIECE.");
  canvas->setTextColor(C_WHITE);
  canvas->setCursor(18, 220);
  canvas->print("Tilt: LEFT / RIGHT");
  canvas->setCursor(18, 233);
  canvas->print("Thrust: PUSH JOYSTICK UP");
  canvas->setTextColor(C_YELLOW);
  canvas->setCursor(18, 260);
  canvas->print("MISSION CONTROL: CONFIDENT.");
  canvas->setCursor(18, 273);
  canvas->print("ABOUT THE PILOT: LESS SO.");
  canvas->setTextColor(C_CYAN);
  canvas->setCursor(26, 296);
  canvas->print("PRESS STICK TO LAUNCH");
}

void drawLandedScreen() {
  canvas->fillRect(16, 108, 208, 105, C_HUD_BG);
  canvas->drawRect(16, 108, 208, 105, C_PAD_NEON);

  canvas->setTextSize(2);
  canvas->setTextColor(C_GREEN);
  canvas->setCursor(42, 120);
  canvas->print("TOUCHDOWN!");

  canvas->setTextSize(1);
  canvas->setTextColor(C_WHITE);
  canvas->setCursor(22, 153);
  canvas->print(landingLine);

  canvas->setTextColor(C_YELLOW);
  canvas->setCursor(22, 171);
  canvas->printf("BONUS: +%lu", (unsigned long)lastPts);
  canvas->setCursor(22, 184);
  canvas->printf("TOTAL: %lu", (unsigned long)score);

  canvas->setTextColor(C_CYAN);
  canvas->setCursor(28, 225);
  canvas->print("PRESS TO FLY THE NEXT LEVEL");
}

void drawCrashScreen() {
  canvas->fillRect(16, 108, 208, 105, C_HUD_BG);
  canvas->drawRect(16, 108, 208, 105, C_RED);

  canvas->setTextSize(2);
  canvas->setTextColor(C_RED);
  canvas->setCursor(55, 120);
  canvas->print("FLIGHT OVER");

  canvas->setTextSize(1);
  canvas->setTextColor(C_YELLOW);
  canvas->setCursor(22, 151);
  canvas->print(crashReason);
  canvas->setTextColor(C_WHITE);
  canvas->setCursor(22, 170);
  canvas->print(crashLine);
  canvas->setTextColor(C_CYAN);
  canvas->setCursor(30, 225);
  canvas->print("PRESS TO RETURN TO BASE");
}

void render() {
  canvas->fillScreen(C_SPACE_BG);

  for (int i = 0; i < NSTARS; i++) {
    canvas->drawPixel((int)stars[i].x, (int)stars[i].y, stars[i].color);
  }

  for (int x = 0; x < W; x++) {
    canvas->drawFastVLine(x, terr[x], H - terr[x], C_TERR_FILL);
    canvas->drawPixel(x, terr[x], C_TERR_NEON);
  }

  for (int i = 0; i < 2; i++) {
    const int w = pads[i].x1 - pads[i].x0 + 1;
    canvas->fillRect(pads[i].x0, terr[constrain(pads[i].x0, 0, W - 1)], w, 4, C_PAD_NEON);
    canvas->setTextSize(1);
    canvas->setTextColor(C_YELLOW);
    canvas->setCursor(pads[i].x0 + 2, terr[constrain(pads[i].x0, 0, W - 1)] - 10);
    canvas->printf("x%u", pads[i].mult);
  }

  drawDust();

  if (state == ST_PLAY) {
    drawStarshipWrapped(s.x, s.y, s.ang, (s.fuel > 0.0f) ? s.thr / 100.0f : 0.0f);
  } else if (state == ST_LANDED) {
    drawStarshipWrapped(s.x, s.y, s.ang, 0.0f);
  }

  drawHUD();
  drawMissionBanner();

  if (state == ST_TITLE) {
    drawTitleScreen();
  } else if (state == ST_LANDED) {
    drawLandedScreen();
  } else if (state == ST_CRASH) {
    drawCrashScreen();
  }

  tft.startWrite();
  tft.setAddrWindow(0, 0, W, H);
  tft.writePixels(canvas->getBuffer(), (uint32_t)W * H, true);
  tft.endWrite();
}

// ======================= GAME FLOW =======================
void startLevel() {
  gravity = min(22.0f, 10.5f + level * 1.4f);
  genTerrain();

  s.x = random(50, 190);
  s.y = 55;
  s.vx = random(-18, 19);
  s.vy = 0.0f;
  s.ang = 0.0f;
  s.thr = 0.0f;
  s.fuel = max(55.0f, 100.0f - (level - 1) * 7.5f);

  for (int i = 0; i < NDUST; i++) dust[i].life = 0.0f;
  nextFuelWarningAt = 0;
  nextCommentAt = millis() + 2500;
  setMissionLine((level == 1)
    ? "MISSION CONTROL: DON'T IMPRESS US." 
    : "NEW LEVEL. SAME QUESTIONABLE PILOT.");
}

void newGame() {
  level = 1;
  score = 0;
  lastPts = 0;
  startLevel();
  state = ST_PLAY;
  stateT = millis();
  setMissionLine("ENGINES HOT. INSURANCE NOT INCLUDED.");
  playSeq(SND_START, ARRLEN(SND_START));
}

void updatePlay(float dt) {
  readJoystickInputs();

  const float thrNow = (s.fuel > 0.0f) ? s.thr : 0.0f;
  const float T = MAX_ACC * thrNow / 100.0f;
  const float rad = s.ang * DEG2RAD;

  s.vx += (T * sinf(rad)) * dt;
  s.vy += (gravity - T * cosf(rad)) * dt;
  s.x  += s.vx * dt;
  s.y  += s.vy * dt;

  s.fuel -= BURN_RATE * (thrNow / 100.0f) * dt;
  if (s.fuel < 0.0f) s.fuel = 0.0f;

  if (s.x < 0.0f) s.x += W;
  if (s.x >= W) s.x -= W;
  if (s.y < -30.0f) {
    s.y = -30.0f;
    if (s.vy < 0.0f) s.vy = 0.0f;
  }

  const int groundY = terr[wrapIdx(s.x)];
  const float distToGround = groundY - (s.y + 15.0f);

  if (distToGround <= 45.0f && thrNow > 0.0f) {
    const float intensity = (1.0f - (distToGround / 45.0f)) * (thrNow / 100.0f);
    spawnGroundDust(s.x, groundY, intensity);
  }

  updateDust(dt);
  updateCommentary(distToGround);
  updateEngineSound(thrNow);

  // Low-fuel alert. It is deliberately intermittent, not a constant scream.
  if (s.fuel <= 15.0f && millis() >= nextFuelWarningAt && !seqLen) {
    playSeq(SND_WARN, ARRLEN(SND_WARN));
    nextFuelWarningAt = millis() + 2600;
  }

  static const int8_t CP[5][2] = {
    {0, -18}, {-8, 8}, {8, 8}, {-9, 15}, {9, 15}
  };
  const float ca = cosf(rad);
  const float sa = sinf(rad);
  float maxPen = -1e9f;
  int ixFoot[2] = {0, 0};

  for (int k = 0; k < 5; k++) {
    const float rx = CP[k][0] * ca - CP[k][1] * sa;
    const float ry = CP[k][0] * sa + CP[k][1] * ca;
    const int ix = wrapIdx(s.x + rx);
    const float pen = (s.y + ry) - terr[ix];
    if (pen > maxPen) maxPen = pen;
    if (k == 3) ixFoot[0] = ix;
    if (k == 4) ixFoot[1] = ix;
  }

  if (maxPen >= 0.0f) {
    s.y -= maxPen;
    setTone(0);

    Pad *onPad = nullptr;
    for (int i = 0; i < 2; i++) {
      if (ixFoot[0] >= pads[i].x0 && ixFoot[0] <= pads[i].x1 &&
          ixFoot[1] >= pads[i].x0 && ixFoot[1] <= pads[i].x1) {
        onPad = &pads[i];
      }
    }

    const bool vyOK = s.vy <= VY_MAX;
    const bool vxOK = fabsf(s.vx) <= VX_MAX;
    const bool angOK = fabsf(s.ang) <= ANG_MAX;

    stateT = millis();
    if (onPad && vyOK && vxOK && angOK) {
      lastPts = onPad->mult * 75 + (uint32_t)s.fuel * 2;
      score += lastPts;
      if (score > hiScore) {
        hiScore = score;
        prefs.putUInt("hi", hiScore);
      }

      s.vx = 0.0f;
      s.vy = 0.0f;
      s.thr = 0.0f;
      state = ST_LANDED;
      landingLine = LANDING_LINES[random(ARRLEN(LANDING_LINES))];
      setMissionLine("TOUCHDOWN CONFIRMED. ACT CASUAL.");
      triggerTouchdownDustCloud(s.x, terr[wrapIdx(s.x)]);
      playSeq(SND_LAND, ARRLEN(SND_LAND));
    } else {
      if (!vyOK) crashReason = "CRITICAL: DESCENT SPEED";
      else if (!vxOK) crashReason = "CRITICAL: LATERAL DRIFT";
      else if (!angOK) crashReason = "CRITICAL: ROCKET TILT";
      else crashReason = "MISSED PAD LANDING ZONE";

      crashLine = CRASH_LINES[random(ARRLEN(CRASH_LINES))];
      state = ST_CRASH;
      setMissionLine("FLIGHT DATA RECEIVED. DEBRIEF SOON.");
      playSeq(SND_CRASH, ARRLEN(SND_CRASH));
    }
  }
}

// ======================= SETUP & MAIN LOOP =======================
void setup() {
  Serial.begin(115200);

  pinMode(BL_PIN, OUTPUT);
  digitalWrite(BL_PIN, HIGH);
  pinMode(JOY_SW_PIN, INPUT_PULLUP);

  buzzerBegin();
  if (!buzzerReady) Serial.println("WARNING: buzzer LEDC attach failed; check PIEZO_PIN and board type.");

  SPI.begin(TFT_SCK, -1, TFT_MOSI, TFT_CS);
  tft.init(240, 320);
  tft.setSPISpeed(SPI_FREQ);
  tft.setRotation(0);
  tft.invertDisplay(INVERT_COLORS);

  canvas = new GFXcanvas16(W, H);
  if (!canvas) {
    Serial.println("FATAL: Could not allocate display canvas.");
    while (true) delay(100);
  }

  randomSeed(esp_random());
  initStars();

  prefs.begin("lander", false);
  hiScore = prefs.getUInt("hi", 0);

  genTerrain();
  s.x = W / 2.0f;
  s.y = 55.0f;
  s.vx = s.vy = s.ang = s.thr = 0.0f;
  s.fuel = 100.0f;
  setMissionLine("MISSION CONTROL: READY WHEN YOU ARE.");
}

void loop() {
  static uint32_t lastU = micros();
  const uint32_t nowU = micros();
  float dt = (nowU - lastU) / 1000000.0f;
  lastU = nowU;
  if (dt > 0.05f) dt = 0.05f;

  const bool btn = isBtnPressed();

  switch (state) {
    case ST_TITLE:
      if (btn) newGame();
      break;

    case ST_PLAY:
      updatePlay(dt);
      break;

    case ST_LANDED:
      updateDust(dt);
      if (btn && millis() - stateT > 600) {
        level++;
        startLevel();
        state = ST_PLAY;
        stateT = millis();
        setMissionLine("NEXT LANDING. MAKE IT LOOK EASY.");
        playSeq(SND_START, ARRLEN(SND_START));
      }
      break;

    case ST_CRASH:
      updateDust(dt);
      if (btn && millis() - stateT > 900) {
        state = ST_TITLE;
        setMissionLine("WELCOME BACK. SHIP IS MOSTLY HERE.");
        playSeq(SND_SELECT, ARRLEN(SND_SELECT));
      }
      break;
  }

  soundUpdate();
  render();
}
