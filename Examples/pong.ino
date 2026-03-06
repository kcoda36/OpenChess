// ============================================================
// pong.ino  —  8×8 LED Pong
//
// Left  piece on column A (col 0) = left  paddle (green)
// Right piece on column H (col 7) = right paddle (red)
// Ball is white, bounces between the paddles.
// IO10 = start / reset
// Ball starts slow, speeds up continuously over time.
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
int  ballCol  = 3,  ballRow  = 3;
int  ballDCol = 1,  ballDRow = 1;   // ±1

int  leftPadRow  = 3;  // top row of left  paddle (covers rows [row, row+1])
int  rightPadRow = 3;  // top row of right paddle

int  leftScore  = 0;
int  rightScore = 0;

bool gameActive = false;

unsigned long lastMoveTime  = 0;
unsigned long gameStartTime = 0;
int           moveInterval  = 400;  // ms between ball steps, shrinks over time

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
// Paddle tracking
// ---------------------------

void updatePaddles() {
  readSensors();

  // Left paddle: first piece found on column 0
  for (int row = 0; row < 8; row++) {
    if (sensorState[row][0]) {
      leftPadRow = constrain(row, 0, 6);  // clamp so paddle stays on board
      break;
    }
  }

  // Right paddle: first piece found on column 7
  for (int row = 0; row < 8; row++) {
    if (sensorState[row][7]) {
      rightPadRow = constrain(row, 0, 6);
      break;
    }
  }
}

// ---------------------------
// Rendering
// ---------------------------

void drawFrame() {
  strip.clear();

  // Left paddle — green, 2 pixels tall
  for (int dr = 0; dr <= 1; dr++) {
    int r = leftPadRow + dr;
    if (r >= 0 && r < 8)
      strip.setPixelColor(r * 8 + 0, strip.Color(0, 255, 0, 0));
  }

  // Right paddle — red, 2 pixels tall
  for (int dr = 0; dr <= 1; dr++) {
    int r = rightPadRow + dr;
    if (r >= 0 && r < 8)
      strip.setPixelColor(r * 8 + 7, strip.Color(255, 0, 0, 0));
  }

  // Ball — bright white
  strip.setPixelColor(ballRow * 8 + ballCol, strip.Color(0, 0, 0, 255));

  strip.show();
}

// ---------------------------
// Animations
// ---------------------------

// Flash the scoring wall column
void scoreFlash(bool leftScored) {
  int scoreCol  = leftScored ? 0 : 7;
  uint32_t color = leftScored ? strip.Color(0, 255, 0, 0)
                              : strip.Color(255, 0, 0, 0);
  for (int f = 0; f < 6; f++) {
    for (int row = 0; row < 8; row++) {
      strip.setPixelColor(row * 8 + scoreCol, (f % 2 == 0) ? color : 0);
    }
    strip.show();
    delay(100);
  }
}

// Blue columns sweep outward from center
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

// Idle pulse — gentle blue throb in the center while waiting for IO10
void idlePulse() {
  static unsigned long last = 0;
  static int  brightness = 0;
  static int  dir        = 1;

  if (millis() - last < 15) return;
  last = millis();

  brightness += dir * 3;
  if (brightness >= 80)  { brightness = 80;  dir = -1; }
  if (brightness <= 0)   { brightness = 0;   dir =  1; }

  // Just light the 4 center squares
  strip.clear();
  for (int row = 3; row <= 4; row++)
    for (int col = 3; col <= 4; col++)
      strip.setPixelColor(row * 8 + col, strip.Color(0, 0, brightness, 0));
  strip.show();
}

// ---------------------------
// Ball logic
// ---------------------------

void launchBall() {
  ballCol  = 3 + random(0, 2);      // 3 or 4
  ballRow  = 2 + random(0, 4);      // 2–5
  ballDCol = (random(0, 2) == 0) ? 1 : -1;
  ballDRow = (random(0, 2) == 0) ? 1 : -1;
  drawFrame();
}

void moveBall() {
  int nextCol = ballCol + ballDCol;
  int nextRow = ballRow + ballDRow;

  // Bounce off top / bottom walls
  if (nextRow < 0) { nextRow = 0; ballDRow =  1; }
  if (nextRow > 7) { nextRow = 7; ballDRow = -1; }

  // ── Left wall (col 0) ──────────────────────────────────────
  if (nextCol < 0) {
    bool hit = (nextRow >= leftPadRow && nextRow <= leftPadRow + 1);
    if (hit) {
      ballDCol = 1;   // bounce right
      // Deflect angle based on where on paddle
      ballDRow = (nextRow == leftPadRow) ? -1 : 1;
      nextCol  = 1;   // push back into field
    } else {
      rightScore++;
      Serial.print("Right scores!  ");
      Serial.print(leftScore); Serial.print(" – "); Serial.println(rightScore);
      scoreFlash(false);
      launchBall();
      return;
    }
  }

  // ── Right wall (col 7) ─────────────────────────────────────
  if (nextCol > 7) {
    bool hit = (nextRow >= rightPadRow && nextRow <= rightPadRow + 1);
    if (hit) {
      ballDCol = -1;  // bounce left
      ballDRow = (nextRow == rightPadRow) ? -1 : 1;
      nextCol  = 6;
    } else {
      leftScore++;
      Serial.print("Left  scores!  ");
      Serial.print(leftScore); Serial.print(" – "); Serial.println(rightScore);
      scoreFlash(true);
      launchBall();
      return;
    }
  }

  ballCol = nextCol;
  ballRow = nextRow;
}

// Speed ramps from 400 ms/step → 70 ms/step over ~110 seconds
void updateSpeed() {
  unsigned long elapsed = (millis() - gameStartTime) / 1000UL;
  moveInterval = max(70, 400 - (int)(elapsed * 3));
}

// ---------------------------
// Start / Reset
// ---------------------------

void startGame() {
  leftScore   = 0;
  rightScore  = 0;
  leftPadRow  = 3;
  rightPadRow = 3;
  gameStartTime = millis();
  moveInterval  = 400;
  gameActive    = true;

  startAnimation();
  launchBall();

  Serial.println("──────────────────────────────");
  Serial.println("  PONG  –  Game started!");
  Serial.println("  Left  piece on column A");
  Serial.println("  Right piece on column H");
  Serial.println("──────────────────────────────");
}

// ---------------------------
// Setup
// ---------------------------

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("PONG  –  Press IO10 to start");

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
  // IO10 — start or reset
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

  // Read paddle positions every loop tick (fast)
  updatePaddles();

  // Move ball on the current interval
  unsigned long now = millis();
  if (now - lastMoveTime >= (unsigned long)moveInterval) {
    lastMoveTime = now;
    updateSpeed();
    moveBall();
    drawFrame();
  }
}
