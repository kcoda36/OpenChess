// ============================================================
// bot_chess.ino
// Physical chess board with two game modes:
//   IO9  = Bot mode  (vs. chess-api.com AI, no token needed)
//   IO10 = Two-player mode (white gets LED hints, black free movement)
// ============================================================

#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>

// ---------------------------
// WiFi credentials
// ---------------------------
const char* ssid     = "HooverMaxExtractModel60";
const char* password = "NewFilter";

// ---------------------------
// Difficulty → Stockfish depth mapping (levels 1–8)
// ---------------------------
const int DEPTH_FOR_LEVEL[8] = { 1, 2, 3, 5, 7, 9, 12, 15 };

// ---------------------------
// Pin Definitions
// ---------------------------
#define LED_PIN               1
#define LED_COUNT             64
#define BRIGHTNESS            100

#define SER_PIN               45
#define RCLK_PIN              47
#define SRCLK_PIN             46

#define BOT_BUTTON_PIN        9   // IO9  = bot mode
#define TWO_PLAYER_BUTTON_PIN 10  // IO10 = two-player mode

const int ROW_PINS[8] = {42, 41, 40, 39, 38, 37, 36, 35};

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_RGBW + NEO_KHZ800);

// ---------------------------
// Game State
// ---------------------------
enum GameMode { MODE_SELECT, MODE_TWO_PLAYER, MODE_BOT };
GameMode gameMode = MODE_SELECT;

bool sensorState[8][8];
bool sensorPrev[8][8];

char initialBoard[8][8] = {
  {'R','N','B','Q','K','B','N','R'},
  {'P','P','P','P','P','P','P','P'},
  {' ',' ',' ',' ',' ',' ',' ',' '},
  {' ',' ',' ',' ',' ',' ',' ',' '},
  {' ',' ',' ',' ',' ',' ',' ',' '},
  {' ',' ',' ',' ',' ',' ',' ',' '},
  {'p','p','p','p','p','p','p','p'},
  {'r','n','b','q','k','b','n','r'}
};

char board[8][8];
uint32_t PROMOTION_COLOR;

// Bot-specific state
int botDifficulty  = 1;
int fullmoveNumber = 1;  // increments after every black move

// Set before entering MODE_SELECT to skip the button-wait menu
GameMode pendingMode = MODE_SELECT;

// ---------------------------
// Function Prototypes
// ---------------------------
void      setColumn(int col);
void      clearAllColumns();
void      readSensors();
bool      checkInitialBoard();
void      updateSetupDisplay();
void      fireworkAnimation();
void      captureAnimation(int row, int col);
void      promotionAnimation(int col);
void      checkForPromotion(int targetRow, int targetCol, char piece);
void      getPossibleMoves(int row, int col, int &moveCount, int moves[][2]);
bool      isSquareAttacked(int row, int col, char attackerColor);
bool      isInCheck(char kingColor);
bool      hasLegalMoves(char color);
void      gameOverAnimation(bool whiteWon);
void      resetBoard();
void      syncSensorPrev();
void      showModeSelectDisplay();
GameMode  waitForModeSelect();
int       selectDifficulty();
bool      handleWhitePiecePickup(int fromRow, int fromCol, int &targetRow, int &targetCol);
void      handleBlackFreeMove(int fromRow, int fromCol);
void      executeBotMoveOnBoard(int fromRow, int fromCol, int toRow, int toCol, bool isCapture);
void      animateMoveHints(int fromRow, int fromCol, int moveCount, int moves[][2]);
void      startupAnimation();
bool      checkResetButtons();
String    boardToFEN(bool blackToMove);
String    getBotMove(int depth);
String    toUCI(int fromRow, int fromCol, int toRow, int toCol);
void      fromUCI(String uci, int &fromRow, int &fromCol, int &toRow, int &toCol);

// ---------------------------
// SETUP
// ---------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n========================================");
  Serial.println("   PrintChess - Bot + Two-Player Board");
  Serial.println("========================================");
  Serial.println("  IO9  = Bot mode   (vs. AI)");
  Serial.println("  IO10 = Two-player mode");
  Serial.println("========================================\n");

  pinMode(SER_PIN,   OUTPUT);
  pinMode(RCLK_PIN,  OUTPUT);
  pinMode(SRCLK_PIN, OUTPUT);
  for (int i = 0; i < 8; i++) pinMode(ROW_PINS[i], INPUT_PULLUP);
  pinMode(BOT_BUTTON_PIN,        INPUT_PULLUP);
  pinMode(TWO_PLAYER_BUTTON_PIN, INPUT_PULLUP);

  strip.begin();
  strip.setBrightness(BRIGHTNESS);
  strip.clear();
  strip.show();
  PROMOTION_COLOR = strip.Color(255, 215, 0, 255);
  startupAnimation();

  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi failed - bot mode will not work");
  }

  gameMode = MODE_SELECT;
}

// ---------------------------
// MAIN LOOP
// ---------------------------
void loop() {
  // ── Mode selection screen ──────────────────────────────────
  if (gameMode == MODE_SELECT) {
    if (pendingMode != MODE_SELECT) {
      // Button already determined the mode — skip the menu
      gameMode     = pendingMode;
      pendingMode  = MODE_SELECT;
    } else {
      showModeSelectDisplay();
      gameMode = waitForModeSelect();
    }
    strip.clear();
    strip.show();

    if (gameMode == MODE_BOT) {
      botDifficulty  = selectDifficulty();
      fullmoveNumber = 1;
      Serial.print("Difficulty level ");
      Serial.print(botDifficulty);
      Serial.print("  (depth ");
      Serial.print(DEPTH_FOR_LEVEL[botDifficulty - 1]);
      Serial.println(")");
    }

    resetBoard();
    Serial.println("Place all pieces to begin...");
    while (!checkInitialBoard()) {
      readSensors();
      updateSetupDisplay();
      delay(500);
    }
    fireworkAnimation();
    syncSensorPrev();

    Serial.println("Game started! White to move.\n");
    return;
  }

  // ── In-game reset ───────────────────────────────────────────
  //   IO9  → mode select menu (choose bot or two-player)
  //   IO10 → restart two-player immediately, no menu
  static bool lastBot = HIGH, lastTwo = HIGH;
  bool botBtn = digitalRead(BOT_BUTTON_PIN);
  bool twoBtn = digitalRead(TWO_PLAYER_BUTTON_PIN);

  if ((lastBot == HIGH && botBtn == LOW) || (lastTwo == HIGH && twoBtn == LOW)) {
    delay(50);
    bool io9  = (digitalRead(BOT_BUTTON_PIN)        == LOW);
    bool io10 = (digitalRead(TWO_PLAYER_BUTTON_PIN) == LOW);
    if (io9 || io10) {
      strip.setBrightness(BRIGHTNESS);
      strip.clear(); strip.show();
      for (int f = 0; f < 3; f++) {
        for (int i = 0; i < LED_COUNT; i++) strip.setPixelColor(i, strip.Color(255, 0, 0, 0));
        strip.show(); delay(150);
        strip.clear(); strip.show(); delay(150);
      }
      if (io10) {
        Serial.println("\nIO10 reset → two-player\n");
        pendingMode = MODE_TWO_PLAYER;  // skip menu, go straight to two-player
      } else {
        Serial.println("\nIO9 reset → mode select\n");
        pendingMode = MODE_SELECT;      // show the menu
      }
      gameMode = MODE_SELECT;
      return;
    }
  }
  lastBot = botBtn;
  lastTwo = twoBtn;

  readSensors();

  // ── Two-player mode ─────────────────────────────────────────
  // Both colors get full LED hints and legal-move validation.
  if (gameMode == MODE_TWO_PLAYER) {
    for (int row = 0; row < 8; row++) {
      for (int col = 0; col < 8; col++) {
        if (!sensorPrev[row][col] || sensorState[row][col]) continue;
        char piece = board[row][col];
        if (piece == ' ') continue;

        int targetRow, targetCol;
        bool moved = handleWhitePiecePickup(row, col, targetRow, targetCol);
        if (moved && !(targetRow == row && targetCol == col)) {
          bool isCapture = (board[targetRow][targetCol] != ' ');
          if (isCapture) captureAnimation(targetRow, targetCol);
          board[targetRow][targetCol] = piece;
          board[row][col] = ' ';
          checkForPromotion(targetRow, targetCol, piece);

          int newPx = targetRow * 8 + targetCol;
          for (int b = 0; b < 2; b++) {
            strip.setPixelColor(newPx, strip.Color(0, 255, 0, 255)); strip.show(); delay(200);
            strip.setPixelColor(newPx, 0);                           strip.show(); delay(200);
          }
        }
        strip.clear();
        strip.show();
      }
    }
  }

  // ── Bot mode ────────────────────────────────────────────────
  else if (gameMode == MODE_BOT) {
    for (int row = 0; row < 8; row++) {
      for (int col = 0; col < 8; col++) {
        if (!sensorPrev[row][col] || sensorState[row][col]) continue;
        char piece = board[row][col];
        if (piece == ' ') continue;
        if (piece < 'A' || piece > 'Z') continue;  // white pieces only

        int targetRow, targetCol;
        bool moved = handleWhitePiecePickup(row, col, targetRow, targetCol);

        if (!moved || (targetRow == row && targetCol == col)) {
          strip.clear(); strip.show();
          continue;
        }

        // Commit white's move
        bool isCapture = (board[targetRow][targetCol] != ' ');
        if (isCapture) captureAnimation(targetRow, targetCol);
        board[targetRow][targetCol] = piece;
        board[row][col] = ' ';
        checkForPromotion(targetRow, targetCol, piece);

        int newPx = targetRow * 8 + targetCol;
        for (int b = 0; b < 2; b++) {
          strip.setPixelColor(newPx, strip.Color(0, 255, 0, 255)); strip.show(); delay(200);
          strip.setPixelColor(newPx, 0);                           strip.show(); delay(200);
        }
        strip.clear(); strip.show();

        Serial.print("White: ");
        Serial.print(toUCI(row, col, targetRow, targetCol));
        Serial.println();

        // ── Fetch bot response ────────────────────────────────
        Serial.println("Bot thinking...");

        int depth = DEPTH_FOR_LEVEL[botDifficulty - 1];
        String botUCI = getBotMove(depth);  // black to move

        if (botUCI.length() >= 4) {
          int bfr, bfc, btr, btc;
          fromUCI(botUCI, bfr, bfc, btr, btc);
          bool botCapture = (board[btr][btc] != ' ');

          Serial.print("Bot:   "); Serial.println(botUCI);

          // Block until player physically executes the bot's move
          executeBotMoveOnBoard(bfr, bfc, btr, btc, botCapture);
          fullmoveNumber++;
        } else {
          // No bot move — check if the game is legitimately over
          if (!hasLegalMoves('b')) {
            if (isInCheck('b')) {
              Serial.println("CHECKMATE! White wins!");
              gameOverAnimation(true);
            } else {
              Serial.println("STALEMATE! Draw.");
              gameOverAnimation(false);
            }
          } else {
            Serial.println("Bot API error - skipping bot turn");
          }
        }

        syncSensorPrev();
        return;
      }
    }
  }

  syncSensorPrev();
  delay(100);
}

// ============================================================
// MODE SELECTION
// ============================================================

void showModeSelectDisplay() {
  // Board stays dark while waiting for button press
}

// Startup animation: diagonal fill from A1 corner to full board, then
// diagonal clear from A1 corner back to empty.
void startupAnimation() {
  // Phase 1: light up one diagonal at a time until the board is full
  for (int d = 0; d <= 14; d++) {
    for (int row = 0; row < 8; row++) {
      for (int col = 0; col < 8; col++) {
        if (row + col == d) {
          strip.setPixelColor(row * 8 + col, strip.Color(0, 0, 255, 0));
        }
      }
    }
    strip.show();
    delay(40);
  }

  delay(80);

  // Phase 2: turn off one diagonal at a time until the board is empty
  for (int d = 0; d <= 14; d++) {
    for (int row = 0; row < 8; row++) {
      for (int col = 0; col < 8; col++) {
        if (row + col == d) {
          strip.setPixelColor(row * 8 + col, 0);
        }
      }
    }
    strip.show();
    delay(40);
  }
}

GameMode waitForModeSelect() {
  Serial.println("Press IO9  (left/blue)   → BOT mode");
  Serial.println("Press IO10 (right/green) → TWO-PLAYER mode");

  while (true) {
    showModeSelectDisplay();

    if (digitalRead(BOT_BUTTON_PIN) == LOW) {
      delay(50);
      if (digitalRead(BOT_BUTTON_PIN) == LOW) {
        Serial.println("BOT mode selected\n");
        while (digitalRead(BOT_BUTTON_PIN) == LOW) delay(10);
        return MODE_BOT;
      }
    }
    if (digitalRead(TWO_PLAYER_BUTTON_PIN) == LOW) {
      delay(50);
      if (digitalRead(TWO_PLAYER_BUTTON_PIN) == LOW) {
        Serial.println("TWO-PLAYER mode selected\n");
        while (digitalRead(TWO_PLAYER_BUTTON_PIN) == LOW) delay(10);
        return MODE_TWO_PLAYER;
      }
    }
    delay(20);
  }
}

// ============================================================
// DIFFICULTY SELECTION
// ============================================================

int selectDifficulty() {
  Serial.println("Select difficulty:");
  Serial.println("  Place a pawn on rank 4 — A=Level 1 (easy) … H=Level 8 (hard)");

  // Green → red gradient across row 3 (rank 4)
  for (int col = 0; col < 8; col++) {
    int r = (col * 255) / 7;
    int g = 255 - r;
    strip.setPixelColor(3 * 8 + col, strip.Color(r, g, 0, 0));
  }
  strip.show();

  // Wait for any square on row 3 to be occupied
  int selectedCol = -1;
  while (selectedCol == -1) {
    readSensors();
    for (int col = 0; col < 8; col++) {
      if (sensorState[3][col]) { selectedCol = col; break; }
    }
    delay(50);
  }

  int selectedDiff = selectedCol + 1;
  Serial.print("Level "); Serial.print(selectedDiff);
  Serial.println(" locked - remove pawn to confirm");

  // Pulse the chosen square while waiting for pawn removal
  unsigned long lastPulse = 0;
  bool pulseHigh = true;
  while (sensorState[3][selectedCol]) {
    readSensors();
    if (millis() - lastPulse > 200) {
      pulseHigh = !pulseHigh;
      lastPulse = millis();
      int r = (selectedCol * 255) / 7, g = 255 - r;
      strip.setPixelColor(3 * 8 + selectedCol,
        pulseHigh ? strip.Color(r, g, 0, 255) : strip.Color(r / 4, g / 4, 0, 0));
      strip.show();
    }
    delay(30);
  }

  // Clear difficulty row
  for (int col = 0; col < 8; col++) strip.setPixelColor(3 * 8 + col, 0);
  strip.show();

  // Quick column flash to confirm
  for (int b = 0; b < 3; b++) {
    int r = (selectedCol * 255) / 7, g = 255 - r;
    for (int row = 0; row < 8; row++) strip.setPixelColor(row * 8 + selectedCol, strip.Color(r, g, 0, 0));
    strip.show(); delay(120);
    for (int row = 0; row < 8; row++) strip.setPixelColor(row * 8 + selectedCol, 0);
    strip.show(); delay(120);
  }

  return selectedDiff;
}

// ============================================================
// MOVE HANDLING HELPERS
// ============================================================

// Call inside any blocking loop. If either button is pressed, clears LEDs,
// resets brightness, triggers the red flash, sets gameMode = MODE_SELECT,
// and returns true so the caller can break out immediately.
bool checkResetButtons() {
  if (digitalRead(BOT_BUTTON_PIN) == HIGH && digitalRead(TWO_PLAYER_BUTTON_PIN) == HIGH) {
    return false;
  }
  delay(50);
  if (digitalRead(BOT_BUTTON_PIN) == HIGH && digitalRead(TWO_PLAYER_BUTTON_PIN) == HIGH) {
    return false;
  }
  // Button confirmed pressed — tear down and request reset
  strip.setBrightness(BRIGHTNESS);
  strip.clear();
  strip.show();
  for (int f = 0; f < 3; f++) {
    for (int i = 0; i < LED_COUNT; i++) strip.setPixelColor(i, strip.Color(255, 0, 0, 0));
    strip.show(); delay(150);
    strip.clear(); strip.show(); delay(150);
  }
  Serial.println("\nReset - returning to mode select\n");
  gameMode = MODE_SELECT;
  return true;
}

// Lights current square + legal moves, waits for placement.
// Returns true = legal move made, false = put back or illegal.
bool handleWhitePiecePickup(int fromRow, int fromCol, int &targetRow, int &targetCol) {
  Serial.print("Lifted: ");
  Serial.print((char)('A' + fromCol)); Serial.println(fromRow + 1);

  int moveCount = 0;
  int moves[28][2];
  getPossibleMoves(fromRow, fromCol, moveCount, moves);
  animateMoveHints(fromRow, fromCol, moveCount, moves);

  targetRow = -1; targetCol = -1;
  bool placed = false;

  while (!placed) {
    if (checkResetButtons()) { strip.clear(); strip.show(); return false; }
    readSensors();

    if (sensorState[fromRow][fromCol]) {
      targetRow = fromRow; targetCol = fromCol;
      placed = true; break;
    }

    for (int r2 = 0; r2 < 8 && !placed; r2++) {
      for (int c2 = 0; c2 < 8 && !placed; c2++) {
        if (r2 == fromRow && c2 == fromCol) continue;

        bool legal = false;
        for (int i = 0; i < moveCount; i++) {
          if (moves[i][0] == r2 && moves[i][1] == c2) { legal = true; break; }
        }
        if (!legal) continue;

        if (board[r2][c2] != ' ' && !sensorState[r2][c2] && sensorPrev[r2][c2]) {
          targetRow = r2; targetCol = c2;
          strip.setPixelColor(r2 * 8 + c2, strip.Color(255, 0, 0, 255));
          strip.show();
          while (!sensorState[r2][c2]) {
            if (checkResetButtons()) { strip.clear(); strip.show(); return false; }
            readSensors(); delay(50);
          }
          placed = true;
        } else if (board[r2][c2] == ' ' && sensorState[r2][c2] && !sensorPrev[r2][c2]) {
          targetRow = r2; targetCol = c2;
          placed = true;
        }
      }
    }
    delay(50);
  }

  strip.clear(); strip.show();
  if (targetRow == fromRow && targetCol == fromCol) return false;

  for (int i = 0; i < moveCount; i++) {
    if (moves[i][0] == targetRow && moves[i][1] == targetCol) return true;
  }

  // Illegal destination: blink red
  int px = targetRow * 8 + targetCol;
  for (int b = 0; b < 3; b++) {
    strip.setPixelColor(px, strip.Color(255, 0, 0, 0)); strip.show(); delay(150);
    strip.setPixelColor(px, 0);                         strip.show(); delay(150);
  }
  return false;
}

void handleBlackFreeMove(int fromRow, int fromCol) {
  char piece = board[fromRow][fromCol];
  int targetRow = -1, targetCol = -1;
  bool placed = false;

  while (!placed) {
    readSensors();
    if (sensorState[fromRow][fromCol]) { placed = true; break; }
    for (int r2 = 0; r2 < 8 && !placed; r2++) {
      for (int c2 = 0; c2 < 8 && !placed; c2++) {
        if (r2 == fromRow && c2 == fromCol) continue;
        if (sensorState[r2][c2] && !sensorPrev[r2][c2]) {
          targetRow = r2; targetCol = c2; placed = true;
        }
      }
    }
    delay(50);
  }

  if (targetRow != -1) {
    board[targetRow][targetCol] = piece;
    board[fromRow][fromCol] = ' ';
  }
}

// Show bot's required move on LEDs and wait for the player to execute it physically.
//
// FROM square: FLASHES blue      → "pick up this piece"
// TO   square: SOLID green/orange → "place it here"
//
// White cannot move until this function returns.
void executeBotMoveOnBoard(int fromRow, int fromCol, int toRow, int toCol, bool isCapture) {
  // Guard: reject any out-of-bounds coordinates before touching sensors or LEDs
  if (fromRow < 0 || fromRow > 7 || fromCol < 0 || fromCol > 7 ||
      toRow   < 0 || toRow   > 7 || toCol   < 0 || toCol   > 7) {
    Serial.println("executeBotMove: invalid coords, skipping");
    return;
  }

  Serial.println("---- Bot's turn ----");
  Serial.print("Move: ");
  Serial.print((char)('A' + fromCol)); Serial.print(fromRow + 1);
  Serial.print(" → ");
  Serial.print((char)('A' + toCol)); Serial.println(toRow + 1);
  if (isCapture) Serial.println("(capture)");

  int fromPx = fromRow * 8 + fromCol;
  int toPx   = toRow   * 8 + toCol;
  uint32_t toColor = isCapture ? strip.Color(255, 100, 0, 0)  // orange = capture square
                               : strip.Color(0, 255, 0, 0);   // green  = normal move

  strip.setBrightness(255);  // full brightness for bot move indicators

  // --- Step 1: Flash source, solid destination, wait for bot piece to be lifted ---
  unsigned long lastFlash = 0;
  bool flashOn = true;

  strip.setPixelColor(toPx, toColor);  // destination: always solid
  strip.show();

  while (sensorState[fromRow][fromCol]) {
    if (checkResetButtons()) return;
    readSensors();
    if (millis() - lastFlash > 350) {
      flashOn = !flashOn;
      lastFlash = millis();
      strip.setPixelColor(fromPx, flashOn ? strip.Color(0, 0, 255, 0) : 0);
      strip.setPixelColor(toPx, toColor);
      strip.show();
    }
    delay(30);
  }

  // Source lifted — turn it off, keep destination solid
  strip.setPixelColor(fromPx, 0);
  strip.setPixelColor(toPx, toColor);
  strip.show();

  // --- Step 2 (capture only): wait for the captured piece to be removed ---
  if (isCapture) {
    // Destination sensor is currently TRUE (captured piece still there).
    // Wait for it to go FALSE (player removes the captured piece).
    while (sensorState[toRow][toCol]) {
      if (checkResetButtons()) return;
      readSensors();
      delay(30);
    }
    // Keep destination solid while player places the bot piece there
    strip.setPixelColor(toPx, toColor);
    strip.show();
  }

  // --- Step 3: Destination stays solid, wait for bot piece to land there ---
  while (!sensorState[toRow][toCol]) {
    if (checkResetButtons()) return;
    readSensors();
    delay(50);
  }

  // Piece placed — now play capture animation
  if (isCapture) captureAnimation(toRow, toCol);

  // Commit to board state
  char piece = board[fromRow][fromCol];
  board[toRow][toCol] = piece;
  board[fromRow][fromCol] = ' ';
  checkForPromotion(toRow, toCol, piece);

  // Cyan confirmation blink
  for (int b = 0; b < 2; b++) {
    strip.setPixelColor(toPx, strip.Color(0, 200, 200, 0)); strip.show(); delay(200);
    strip.setPixelColor(toPx, 0);                           strip.show(); delay(200);
  }
  strip.setBrightness(BRIGHTNESS);  // restore normal brightness
  strip.clear(); strip.show();

  Serial.println("Bot move executed - your turn");
  Serial.println("--------------------\n");
}

void animateMoveHints(int fromRow, int fromCol, int moveCount, int moves[][2]) {
  strip.setPixelColor(fromRow * 8 + fromCol, strip.Color(0, 0, 0, 255));
  strip.show();

  float distances[28];
  for (int i = 0; i < moveCount; i++) {
    int dr = moves[i][0] - fromRow, dc = moves[i][1] - fromCol;
    distances[i] = sqrt((float)(dr * dr + dc * dc));
  }
  for (int i = 0; i < moveCount - 1; i++) {
    for (int j = 0; j < moveCount - i - 1; j++) {
      if (distances[j] > distances[j + 1]) {
        float td = distances[j]; distances[j] = distances[j + 1]; distances[j + 1] = td;
        int tr = moves[j][0], tc = moves[j][1];
        moves[j][0] = moves[j+1][0]; moves[j][1] = moves[j+1][1];
        moves[j+1][0] = tr; moves[j+1][1] = tc;
      }
    }
  }
  for (int i = 0; i < moveCount; i++) {
    int r = moves[i][0], c = moves[i][1];
    strip.setPixelColor(r * 8 + c,
      board[r][c] == ' ' ? strip.Color(0, 0, 0, 255) : strip.Color(255, 0, 0, 255));
    strip.show();
    delay(30);
  }
}

// ============================================================
// CHESS-API.COM  (no token required)
// ============================================================

// Converts board[8][8] to a FEN string.
// black to move = true after white's move, false otherwise.
String boardToFEN(bool blackToMove) {
  String fen = "";

  // Rank 8 (row 7) → Rank 1 (row 0)
  for (int row = 7; row >= 0; row--) {
    int empty = 0;
    for (int col = 0; col < 8; col++) {
      char p = board[row][col];
      if (p == ' ') {
        empty++;
      } else {
        if (empty > 0) { fen += String(empty); empty = 0; }
        fen += p;
      }
    }
    if (empty > 0) fen += String(empty);
    if (row > 0) fen += '/';
  }

  fen += blackToMove ? " b " : " w ";

  // Castling: infer from whether pieces are still on their starting squares
  String castling = "";
  if (board[0][4] == 'K') {
    if (board[0][7] == 'R') castling += 'K';
    if (board[0][0] == 'R') castling += 'Q';
  }
  if (board[7][4] == 'k') {
    if (board[7][7] == 'r') castling += 'k';
    if (board[7][0] == 'r') castling += 'q';
  }
  fen += (castling == "") ? "-" : castling;
  fen += " - 0 ";  // no en passant tracked, halfmove clock 0
  fen += String(fullmoveNumber);

  return fen;
}

// POST the current position to chess-api.com and return the bot's UCI move.
String getBotMove(int depth) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("No WiFi - cannot get bot move");
    return "";
  }

  String fen = boardToFEN(true);  // black to move
  Serial.print("FEN: "); Serial.println(fen);

  String body = "{\"fen\":\"" + fen + "\",\"depth\":" + String(depth) + "}";
  String move = "";

  // Retry up to 3 times in case of timeout or transient error
  for (int attempt = 1; attempt <= 3 && move == ""; attempt++) {
    if (attempt > 1) {
      Serial.print("Retrying (attempt "); Serial.print(attempt); Serial.println(")...");
      delay(1000);
    }

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    http.begin(client, "https://chess-api.com/v1");
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(25000);  // 25-second timeout (deep searches can be slow)

    int code = http.POST(body);

    if (code == 200) {
      String resp = http.getString();
      Serial.println("API: " + resp);

      StaticJsonDocument<512> doc;
      if (!deserializeJson(doc, resp)) {
        // Prefer "from"+"to" — always pure square names like "e7","e5"
        if (doc.containsKey("from") && doc.containsKey("to")) {
          String f = doc["from"].as<String>();
          String t = doc["to"].as<String>();
          if (f.length() == 2 && t.length() == 2) move = f + t;
        }
        // Fallback: "text" field if from/to absent
        if (move == "" && doc.containsKey("text")) {
          String txt = doc["text"].as<String>();
          if (txt.length() >= 4) move = txt.substring(0, 4);
        }
      } else {
        Serial.println("JSON parse error");
      }
    } else {
      Serial.print("chess-api.com error (attempt ");
      Serial.print(attempt); Serial.print("): ");
      Serial.println(code);
    }

    http.end();
  }

  // Validate: must be exactly 4 chars, each within a–h / 1–8
  if (move.length() == 4 &&
      move[0] >= 'a' && move[0] <= 'h' &&
      move[1] >= '1' && move[1] <= '8' &&
      move[2] >= 'a' && move[2] <= 'h' &&
      move[3] >= '1' && move[3] <= '8') {
    return move;
  }

  Serial.println("Invalid or missing UCI move: \"" + move + "\"");
  return "";
}

// ============================================================
// UCI HELPERS
// ============================================================

String toUCI(int fromRow, int fromCol, int toRow, int toCol) {
  String uci = "";
  uci += (char)('a' + fromCol);
  uci += (char)('1' + fromRow);
  uci += (char)('a' + toCol);
  uci += (char)('1' + toRow);
  return uci;
}

void fromUCI(String uci, int &fromRow, int &fromCol, int &toRow, int &toCol) {
  fromCol = uci[0] - 'a';
  fromRow = uci[1] - '1';
  toCol   = uci[2] - 'a';
  toRow   = uci[3] - '1';
}

// ============================================================
// SENSOR / HARDWARE
// ============================================================

void setColumn(int col) {
  digitalWrite(RCLK_PIN, LOW);
  for (int i = 7; i >= 0; i--) {
    digitalWrite(SRCLK_PIN, LOW);
    digitalWrite(SER_PIN, (i == col) ? HIGH : LOW);
    digitalWrite(SRCLK_PIN, HIGH);
  }
  digitalWrite(RCLK_PIN, HIGH);
}

void clearAllColumns() {
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
  clearAllColumns();
}

void syncSensorPrev() {
  readSensors();
  for (int row = 0; row < 8; row++)
    for (int col = 0; col < 8; col++)
      sensorPrev[row][col] = sensorState[row][col];
}

bool checkInitialBoard() {
  readSensors();
  for (int row = 0; row < 8; row++)
    for (int col = 0; col < 8; col++)
      if (initialBoard[row][col] != ' ' && !sensorState[row][col]) return false;
  return true;
}

void updateSetupDisplay() {
  for (int row = 0; row < 8; row++) {
    for (int col = 0; col < 8; col++) {
      int px = row * 8 + col;
      if (initialBoard[row][col] != ' ') {
        strip.setPixelColor(px, !sensorState[row][col] ? strip.Color(0, 0, 0, 255) : 0);
      } else {
        strip.setPixelColor(px, 0);
      }
    }
  }
  strip.show();
}

void resetBoard() {
  for (int row = 0; row < 8; row++)
    for (int col = 0; col < 8; col++)
      board[row][col] = initialBoard[row][col];
}

// ============================================================
// ANIMATIONS
// ============================================================

void fireworkAnimation() {
  float cx = 3.5f, cy = 3.5f;
  for (float r = 0; r < 6; r += 0.5f) {
    for (int row = 0; row < 8; row++)
      for (int col = 0; col < 8; col++) {
        float dx = col - cx, dy = row - cy, d = sqrt(dx * dx + dy * dy);
        strip.setPixelColor(row * 8 + col, fabsf(d - r) < 0.5f ? strip.Color(0, 0, 0, 255) : 0);
      }
    strip.show(); delay(50);
  }
  strip.clear(); strip.show();
}

void captureAnimation(int captureRow, int captureCol) {
  float cx = captureCol, cy = captureRow;
  for (float r = 0; r < 6; r += 0.8f) {
    for (int row = 0; row < 8; row++)
      for (int col = 0; col < 8; col++) {
        float dx = col - cx, dy = row - cy, d = sqrt(dx * dx + dy * dy);
        strip.setPixelColor(row * 8 + col, fabsf(d - r) < 0.7f ? strip.Color(255, 0, 0, 0) : 0);
      }
    strip.show(); delay(80);
  }
  strip.clear(); strip.show();
}

void promotionAnimation(int col) {
  for (int step = 0; step < 16; step++) {
    for (int row = 0; row < 8; row++) {
      strip.setPixelColor(row * 8 + col, (step + row) % 8 < 4 ? PROMOTION_COLOR : 0);
    }
    strip.show(); delay(100);
  }
  strip.clear(); strip.show();
}

void checkForPromotion(int targetRow, int targetCol, char piece) {
  if ((piece == 'P' && targetRow == 7) || (piece == 'p' && targetRow == 0)) {
    Serial.println("Pawn promoted to Queen!");
    promotionAnimation(targetCol);
    board[targetRow][targetCol] = (piece == 'P') ? 'Q' : 'q';
    int px = targetRow * 8 + targetCol;
    while (sensorState[targetRow][targetCol]) {
      strip.setPixelColor(px, PROMOTION_COLOR); strip.show(); delay(250);
      strip.setPixelColor(px, 0);              strip.show(); delay(250);
      readSensors();
    }
    while (!sensorState[targetRow][targetCol]) {
      strip.setPixelColor(px, PROMOTION_COLOR); strip.show(); delay(250);
      strip.setPixelColor(px, 0);              strip.show(); delay(250);
      readSensors();
    }
    Serial.println("Promotion complete!");
  }
}

// ============================================================
// CHESS LOGIC
// ============================================================

// Returns true if (row, col) is attacked by any piece of attackerColor
bool isSquareAttacked(int row, int col, char attackerColor) {
  // Pawn: white pawns on (row-1) attack up, black pawns on (row+1) attack down
  int pawnRow = (attackerColor == 'w') ? row - 1 : row + 1;
  char pawnPiece = (attackerColor == 'w') ? 'P' : 'p';
  if (pawnRow >= 0 && pawnRow < 8) {
    for (int dc = -1; dc <= 1; dc += 2) {
      if (col + dc >= 0 && col + dc < 8 && board[pawnRow][col + dc] == pawnPiece) return true;
    }
  }

  // Knight
  int km[8][2] = {{2,1},{1,2},{-1,2},{-2,1},{-2,-1},{-1,-2},{1,-2},{2,-1}};
  char knightPiece = (attackerColor == 'w') ? 'N' : 'n';
  for (int i = 0; i < 8; i++) {
    int nr = row + km[i][0], nc = col + km[i][1];
    if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8 && board[nr][nc] == knightPiece) return true;
  }

  // Rook + Queen (straight lines)
  char rookPiece  = (attackerColor == 'w') ? 'R' : 'r';
  char queenPiece = (attackerColor == 'w') ? 'Q' : 'q';
  int straight[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
  for (int d = 0; d < 4; d++) {
    for (int s = 1; s < 8; s++) {
      int nr = row + s * straight[d][0], nc = col + s * straight[d][1];
      if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) break;
      char t = board[nr][nc];
      if (t != ' ') { if (t == rookPiece || t == queenPiece) return true; break; }
    }
  }

  // Bishop + Queen (diagonals)
  char bishopPiece = (attackerColor == 'w') ? 'B' : 'b';
  int diag[4][2] = {{1,1},{1,-1},{-1,1},{-1,-1}};
  for (int d = 0; d < 4; d++) {
    for (int s = 1; s < 8; s++) {
      int nr = row + s * diag[d][0], nc = col + s * diag[d][1];
      if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) break;
      char t = board[nr][nc];
      if (t != ' ') { if (t == bishopPiece || t == queenPiece) return true; break; }
    }
  }

  // King
  char kingPiece = (attackerColor == 'w') ? 'K' : 'k';
  int kd[8][2] = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
  for (int i = 0; i < 8; i++) {
    int nr = row + kd[i][0], nc = col + kd[i][1];
    if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8 && board[nr][nc] == kingPiece) return true;
  }

  return false;
}

// Returns true if the king of kingColor is currently in check
bool isInCheck(char kingColor) {
  char kingPiece     = (kingColor == 'w') ? 'K' : 'k';
  char attackerColor = (kingColor == 'w') ? 'b' : 'w';
  for (int row = 0; row < 8; row++)
    for (int col = 0; col < 8; col++)
      if (board[row][col] == kingPiece)
        return isSquareAttacked(row, col, attackerColor);
  return false;
}

// Returns true if the given color has at least one fully legal move
bool hasLegalMoves(char color) {
  for (int row = 0; row < 8; row++) {
    for (int col = 0; col < 8; col++) {
      char p = board[row][col];
      if (p == ' ') continue;
      bool isBlack = (p >= 'a' && p <= 'z');
      if ((color == 'b') != isBlack) continue;
      int moveCount = 0;
      int moves[28][2];
      getPossibleMoves(row, col, moveCount, moves);
      if (moveCount > 0) return true;
    }
  }
  return false;
}

// Flash winning pieces green, losing pieces red, then hold until a button is pressed
void gameOverAnimation(bool whiteWon) {
  strip.setBrightness(255);

  for (int flash = 0; flash < 6; flash++) {
    for (int row = 0; row < 8; row++) {
      for (int col = 0; col < 8; col++) {
        char p = board[row][col];
        if (p == ' ') { strip.setPixelColor(row * 8 + col, 0); continue; }
        bool isWhite = (p >= 'A' && p <= 'Z');
        bool isWinner = (whiteWon == isWhite);
        strip.setPixelColor(row * 8 + col,
          flash % 2 == 0 ? (isWinner ? strip.Color(0, 255, 0, 0) : strip.Color(255, 0, 0, 0))
                         : 0);
      }
    }
    strip.show();
    delay(400);
  }

  // Hold: winners green, losers red
  for (int row = 0; row < 8; row++) {
    for (int col = 0; col < 8; col++) {
      char p = board[row][col];
      if (p == ' ') { strip.setPixelColor(row * 8 + col, 0); continue; }
      bool isWhite = (p >= 'A' && p <= 'Z');
      bool isWinner = (whiteWon == isWhite);
      strip.setPixelColor(row * 8 + col,
        isWinner ? strip.Color(0, 255, 0, 0) : strip.Color(255, 0, 0, 0));
    }
  }
  strip.show();

  Serial.println("Press any button to return to menu...");
  while (digitalRead(BOT_BUTTON_PIN) == HIGH && digitalRead(TWO_PLAYER_BUTTON_PIN) == HIGH) {
    delay(100);
  }

  strip.setBrightness(BRIGHTNESS);
  strip.clear();
  strip.show();
  gameMode = MODE_SELECT;
}

void getPossibleMoves(int row, int col, int &moveCount, int moves[][2]) {
  moveCount = 0;
  char piece = board[row][col];
  if (piece == ' ') return;

  char pieceColor = (piece >= 'a' && piece <= 'z') ? 'b' : 'w';
  piece = (piece >= 'a' && piece <= 'z') ? piece - 32 : piece;

  auto addMove = [&](int r, int c) {
    if (r < 0 || r >= 8 || c < 0 || c >= 8) return;
    char t = board[r][c];
    if (t == ' ' || ((t >= 'a' && t <= 'z') != (pieceColor == 'b'))) {
      moves[moveCount][0] = r;
      moves[moveCount][1] = c;
      moveCount++;
    }
  };

  auto addLine = [&](int dirs[][2], int count) {
    for (int d = 0; d < count; d++) {
      for (int step = 1; step < 8; step++) {
        int nr = row + step * dirs[d][0], nc = col + step * dirs[d][1];
        if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) break;
        char t = board[nr][nc];
        if (t == ' ') {
          moves[moveCount][0] = nr; moves[moveCount][1] = nc; moveCount++;
        } else {
          if ((t >= 'a' && t <= 'z') != (pieceColor == 'b')) {
            moves[moveCount][0] = nr; moves[moveCount][1] = nc; moveCount++;
          }
          break;
        }
      }
    }
  };

  switch (piece) {
    case 'P': {
      int dir = (pieceColor == 'w') ? 1 : -1;
      if (row + dir >= 0 && row + dir < 8 && board[row + dir][col] == ' ') {
        addMove(row + dir, col);
        if ((pieceColor == 'w' && row == 1) || (pieceColor == 'b' && row == 6)) {
          if (board[row + 2 * dir][col] == ' ') addMove(row + 2 * dir, col);
        }
      }
      for (int dc = -1; dc <= 1; dc += 2) {
        int nr = row + dir, nc = col + dc;
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
          char t = board[nr][nc];
          if (t != ' ' && (t >= 'a' && t <= 'z') != (pieceColor == 'b')) addMove(nr, nc);
        }
      }
      break;
    }
    case 'R': { int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}}; addLine(dirs, 4); break; }
    case 'N': {
      int km[8][2] = {{2,1},{1,2},{-1,2},{-2,1},{-2,-1},{-1,-2},{1,-2},{2,-1}};
      for (int i = 0; i < 8; i++) addMove(row + km[i][0], col + km[i][1]);
      break;
    }
    case 'B': { int dirs[4][2] = {{1,1},{1,-1},{-1,1},{-1,-1}}; addLine(dirs, 4); break; }
    case 'Q': { int dirs[8][2] = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}}; addLine(dirs, 8); break; }
    case 'K': {
      int km[8][2] = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
      for (int i = 0; i < 8; i++) addMove(row + km[i][0], col + km[i][1]);
      break;
    }
  }

  // Filter out any move that leaves own king in check
  char originalPiece = board[row][col];
  int legalCount = 0;
  int legalMoves[28][2];

  for (int i = 0; i < moveCount; i++) {
    int tr = moves[i][0], tc = moves[i][1];
    char savedTo = board[tr][tc];

    board[tr][tc]  = originalPiece;
    board[row][col] = ' ';

    if (!isInCheck(pieceColor)) {
      legalMoves[legalCount][0] = tr;
      legalMoves[legalCount][1] = tc;
      legalCount++;
    }

    board[row][col] = originalPiece;
    board[tr][tc]   = savedTo;
  }

  moveCount = legalCount;
  for (int i = 0; i < moveCount; i++) {
    moves[i][0] = legalMoves[i][0];
    moves[i][1] = legalMoves[i][1];
  }
}
