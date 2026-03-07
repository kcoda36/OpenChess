// ============================================================
// gameoflife.ino  -  8x8 LED Conway's Game of Life
//
// At game start (IO10 press), the 8x8 sensor board is scanned.
// Occupied sensor cells become alive; empty cells become dead.
// The board then evolves with classic Conway rules.
// ============================================================

#include <Adafruit_NeoPixel.h>

// ---------------------------
// Hardware
// ---------------------------
#define LED_PIN    1
#define LED_COUNT  64
#define BRIGHTNESS 180

#define SER_PIN    45
#define RCLK_PIN   47
#define SRCLK_PIN  46

const int ROW_PINS[8] = {42, 41, 40, 39, 38, 37, 36, 35};

#define BTN_PIN 10   // IO10 = start / reset

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_RGBW + NEO_KHZ800);

bool sensorState[8][8];

// ---------------------------
// Game state
// ---------------------------
bool lifeGrid[8][8];
bool nextLifeGrid[8][8];

bool gameActive = false;

unsigned long lastStepTime = 0;
unsigned long gameStartTime = 0;
unsigned long lastSensorPollTime = 0;
int generationInterval = 300;  // ms between generations

const unsigned long sensorInjectDelayMs =10000;   // start live injection after 5s
const unsigned long sensorPollIntervalMs = 200;   // continuous sensor polling cadence

// ---------------------------
// Hardware helpers
// ---------------------------

void setColumn(int col) {
  digitalWrite(RCLK_PIN, LOW);
  for (int i = 7; i >= 0; i--) {
    digitalWrite(SRCLK_PIN, LOW);
    digitalWrite(SER_PIN, (i == col) ? HIGH : LOW);
    digitalWrite(SRCLK_PIN, HIGH);
  }
  digitalWrite(RCLK_PIN, HIGH);
}

void clearColumns() {
  digitalWrite(RCLK_PIN, LOW);
  for (int i = 0; i < 8; i++) {
    digitalWrite(SRCLK_PIN, LOW);
    digitalWrite(SER_PIN, LOW);
    digitalWrite(SRCLK_PIN, HIGH);
  }
  digitalWrite(RCLK_PIN, HIGH);
}

void readSensors() {
  for (int col = 0; col < 8; col++) {
    setColumn(col);
    delayMicroseconds(10);
    for (int row = 0; row < 8; row++) {
      sensorState[row][col] = (digitalRead(ROW_PINS[row]) == LOW);
    }
  }
  clearColumns();
}

// ---------------------------
// Rendering
// ---------------------------

void drawFrame() {
  strip.clear();

  // Alive cells are white. Dead cells stay off.
  for (int row = 0; row < 8; row++) {
    for (int col = 0; col < 8; col++) {
      if (lifeGrid[row][col]) {
        strip.setPixelColor(row * 8 + col, strip.Color(0, 0, 0, 255));
      }
    }
  }

  strip.show();
}

// ---------------------------
// Animations
// ---------------------------

// Blue columns sweep outward from center on game start.
void startAnimation() {
  for (int step = 0; step < 4; step++) {
    strip.clear();
    for (int row = 0; row < 8; row++) {
      int c1 = 3 - step, c2 = 4 + step;
      if (c1 >= 0) strip.setPixelColor(row * 8 + c1, strip.Color(0, 0, 200, 0));
      if (c2 <  8) strip.setPixelColor(row * 8 + c2, strip.Color(0, 0, 200, 0));
    }
    strip.show();
    delay(120);
  }
  strip.clear();
  strip.show();
  delay(150);
}

// Idle pulse - gentle blue throb in the center while waiting for IO10.
void idlePulse() {
  static unsigned long last = 0;
  static int  brightness = 0;
  static int  dir        = 1;

  if (millis() - last < 15) return;
  last = millis();

  brightness += dir * 3;
  if (brightness >= 80)  { brightness = 80;  dir = -1; }
  if (brightness <= 0)   { brightness = 0;   dir =  1; }

  // Just light the 4 center squares.
  strip.clear();
  for (int row = 3; row <= 4; row++)
    for (int col = 3; col <= 4; col++)
      strip.setPixelColor(row * 8 + col, strip.Color(0, 0, brightness, 0));
  strip.show();
}

// ---------------------------
// Game of Life logic
// ---------------------------

int countLiveNeighbors(int row, int col) {
  int liveCount = 0;

  for (int dr = -1; dr <= 1; dr++) {
    for (int dc = -1; dc <= 1; dc++) {
      if (dr == 0 && dc == 0) continue;

      int nr = row + dr;
      int nc = col + dc;

      if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8 && lifeGrid[nr][nc]) {
        liveCount++;
      }
    }
  }

  return liveCount;
}

void stepGameOfLife() {
  for (int row = 0; row < 8; row++) {
    for (int col = 0; col < 8; col++) {
      int neighbors = countLiveNeighbors(row, col);
      bool isAlive = lifeGrid[row][col];

      if (isAlive) {
        nextLifeGrid[row][col] = (neighbors == 2 || neighbors == 3);
      } else {
        nextLifeGrid[row][col] = (neighbors == 3);
      }
    }
  }

  for (int row = 0; row < 8; row++) {
    for (int col = 0; col < 8; col++) {
      lifeGrid[row][col] = nextLifeGrid[row][col];
    }
  }
}

void applySensorOverrides() {
  readSensors();

  // While a sensor is active, that cell is forced alive.
  for (int row = 0; row < 8; row++) {
    for (int col = 0; col < 8; col++) {
      if (sensorState[row][col]) {
        lifeGrid[row][col] = true;
      }
    }
  }
}

// ---------------------------
// Start / Reset
// ---------------------------

void startGame() {
  readSensors();

  // Sensor presence directly seeds the first generation.
  for (int row = 0; row < 8; row++) {
    for (int col = 0; col < 8; col++) {
      lifeGrid[row][col] = sensorState[row][col];
      nextLifeGrid[row][col] = false;
    }
  }

  gameActive = true;
  gameStartTime = millis();
  lastStepTime = millis();
  lastSensorPollTime = 0;

  startAnimation();
  drawFrame();

  Serial.println("------------------------------");
  Serial.println("  GAME OF LIFE - Started");
  Serial.println("  Seed captured from sensors");
  Serial.println("  Live sensor inject after 5s");
  Serial.println("------------------------------");
}

// ---------------------------
// Setup
// ---------------------------

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("GAME OF LIFE - Press IO10 to seed/start");

  pinMode(SER_PIN,   OUTPUT);
  pinMode(RCLK_PIN,  OUTPUT);
  pinMode(SRCLK_PIN, OUTPUT);
  for (int i = 0; i < 8; i++) pinMode(ROW_PINS[i], INPUT_PULLUP);
  pinMode(BTN_PIN, INPUT_PULLUP);

  randomSeed(analogRead(0));

  strip.begin();
  strip.setBrightness(BRIGHTNESS);
  strip.clear();
  strip.show();
}

// ---------------------------
// Loop
// ---------------------------

void loop() {
  // IO10 - start or reseed from sensors.
  static bool lastBtn = HIGH;
  bool btnNow = digitalRead(BTN_PIN);
  if (lastBtn == HIGH && btnNow == LOW) {
    delay(50);
    if (digitalRead(BTN_PIN) == LOW) {
      startGame();
    }
  }
  lastBtn = btnNow;

  if (!gameActive) {
    idlePulse();
    return;
  }

  unsigned long now = millis();

  // After the initial delay, keep polling sensors and force active cells alive.
  if (now - gameStartTime >= sensorInjectDelayMs) {
    if (lastSensorPollTime == 0 || now - lastSensorPollTime >= sensorPollIntervalMs) {
      lastSensorPollTime = now;
      applySensorOverrides();
    }
  }

  // Step generations on the configured interval.
  if (now - lastStepTime >= (unsigned long)generationInterval) {
    lastStepTime = now;
    stepGameOfLife();

    // Re-apply overrides so currently active sensors stay alive in this frame.
    if (now - gameStartTime >= sensorInjectDelayMs) {
      applySensorOverrides();
      lastSensorPollTime = now;
    }

    drawFrame();
  }
}
