// ============================================================
// bt_chess.ino  —  Physical Two-Board Chess (ESP-NOW)
//
//   Both boards carry all 32 physical pieces.
//   Boards auto-pair over ESP-NOW — no router needed.
//   Higher MAC address = WHITE, lower = BLACK.
//
//   Flow:
//     1. Pair boards, then place all 32 pieces on each board.
//     2. On your turn: pick up one of your pieces, LEDs show
//        legal moves, place it on a valid square to commit.
//     3. Your move is sent wirelessly to the other board.
//     4. The other board lights orange on FROM and green on TO.
//        That player must physically move the piece to confirm.
//     5. Once physically confirmed, turns alternate.
//
//   IO9 = reset / return to discovery at any time
// ============================================================

#include <esp_now.h>
#include <WiFi.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>

// ── Hardware ─────────────────────────────────────────────────
#define LED_PIN    1
#define LED_COUNT  64
#define BRIGHTNESS 100

#define SER_PIN    45
#define RCLK_PIN   47
#define SRCLK_PIN  46
#define RESET_PIN  9

const int ROW_PINS[8] = {42, 41, 40, 39, 38, 37, 36, 35};

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_RGBW + NEO_KHZ800);

// ── ESP-NOW protocol ──────────────────────────────────────────
enum MsgType : uint8_t {
  MSG_HELLO      = 0,
  MSG_READY      = 1,
  MSG_SETUP_DONE = 2,
  MSG_MOVE       = 3,  // d = {fromRow, fromCol, toRow, toCol}
  MSG_MOVE_DONE  = 4,  // opponent physically completed the move
  MSG_RESET      = 5
};

struct __attribute__((packed)) Message {
  MsgType type;
  uint8_t d[4];
};

uint8_t broadcastAddr[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
uint8_t peerMac[6];
bool    peerFound = false;
bool    iAmWhite  = false;

volatile bool    msgPending = false;
volatile MsgType msgType;
volatile uint8_t msgData[4];
uint8_t          msgSenderMac[6];

// ── Board state ───────────────────────────────────────────────
bool sensorState[8][8];
bool sensorPrev[8][8];

const char INITIAL[8][8] = {
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

// ═══════════════════════════════════════════════════════════════
// HARDWARE
// ═══════════════════════════════════════════════════════════════

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
    for (int row = 0; row < 8; row++)
      sensorState[row][col] = (digitalRead(ROW_PINS[row]) == LOW);
  }
  clearAllColumns();
}

// ═══════════════════════════════════════════════════════════════
// ANIMATIONS
// ═══════════════════════════════════════════════════════════════

void searchingAnimation(int frame) {
  strip.clear();
  int diag = frame % 15;
  for (int r = 0; r < 8; r++)
    for (int c = 0; c < 8; c++)
      if ((r + c) == diag || (r + c) == diag - 1)
        strip.setPixelColor(r * 8 + c, strip.Color(0, 0, 120, 0));
  strip.show();
}

void fireworkAnimation() {
  float cx = 3.5f, cy = 3.5f;
  for (float r = 0; r < 6.5f; r += 0.45f) {
    strip.clear();
    for (int row = 0; row < 8; row++)
      for (int col = 0; col < 8; col++) {
        float dx = col - cx, dy = row - cy;
        if (fabsf(sqrtf(dx*dx + dy*dy) - r) < 0.55f)
          strip.setPixelColor(row*8+col, iAmWhite ? strip.Color(0,0,0,255) : strip.Color(0,180,255,0));
      }
    strip.show();
    delay(45);
  }
  for (int f = 0; f < 3; f++) {
    uint32_t c = iAmWhite ? strip.Color(0,0,0,255) : strip.Color(0,180,255,0);
    for (int i = 0; i < LED_COUNT; i++) strip.setPixelColor(i, c);
    strip.show(); delay(180);
    strip.clear(); strip.show(); delay(180);
  }
  strip.clear(); strip.show();
}

void illegalFlash(int row, int col) {
  for (int f = 0; f < 3; f++) {
    strip.setPixelColor(row*8+col, strip.Color(200, 150, 0, 0));
    strip.show(); delay(160);
    strip.setPixelColor(row*8+col, 0);
    strip.show(); delay(110);
  }
}

void checkFlash() {
  for (int f = 0; f < 3; f++) {
    for (int i = 0; i < LED_COUNT; i++) strip.setPixelColor(i, strip.Color(255, 0, 0, 0));
    strip.show(); delay(150);
    strip.clear(); strip.show(); delay(120);
  }
}

void checkmateFlash() {
  for (int f = 0; f < 8; f++) {
    for (int i = 0; i < LED_COUNT; i++) strip.setPixelColor(i, strip.Color(255, 0, 0, 0));
    strip.show(); delay(200);
    strip.clear(); strip.show(); delay(180);
  }
}

void gameOverAnimation(bool iWon) {
  char myColor = iAmWhite ? 'w' : 'b';
  for (int f = 0; f < 10; f++) {
    for (int r = 0; r < 8; r++)
      for (int c = 0; c < 8; c++) {
        char p = board[r][c];
        if (p == ' ') { strip.setPixelColor(r*8+c, 0); continue; }
        bool mine = (myColor == 'w') ? (p >= 'A' && p <= 'Z') : (p >= 'a' && p <= 'z');
        strip.setPixelColor(r*8+c, (f % 2 == 0) ? (mine ? strip.Color(0,255,0,0) : strip.Color(255,0,0,0)) : 0);
      }
    strip.show();
    delay(350);
  }
  strip.clear(); strip.show();
}

// Show a dim glow on all pieces belonging to myColor
void showMyPiecesGlow(char myColor) {
  strip.clear();
  for (int r = 0; r < 8; r++)
    for (int c = 0; c < 8; c++) {
      char p = board[r][c];
      if (p == ' ') continue;
      bool mine = (myColor == 'w') ? (p >= 'A' && p <= 'Z') : (p >= 'a' && p <= 'z');
      if (mine) strip.setPixelColor(r*8+c, strip.Color(0, 0, 0, 50));
    }
  strip.show();
}

// ═══════════════════════════════════════════════════════════════
// CHESS LOGIC
// ═══════════════════════════════════════════════════════════════

bool isSquareAttacked(int r, int c, char byColor) {
  int pDir = (byColor == 'w') ? 1 : -1;
  char ePawn = (byColor == 'w') ? 'P' : 'p';
  int pdc[2] = {-1, 1};
  for (int i = 0; i < 2; i++) {
    int pr = r - pDir, pc = c + pdc[i];
    if (pr>=0&&pr<8&&pc>=0&&pc<8&&board[pr][pc]==ePawn) return true;
  }
  char eN = (byColor=='w')?'N':'n';
  int kd[8][2] = {{2,1},{1,2},{-1,2},{-2,1},{-2,-1},{-1,-2},{1,-2},{2,-1}};
  for (int i = 0; i < 8; i++) {
    int nr=r+kd[i][0], nc=c+kd[i][1];
    if (nr>=0&&nr<8&&nc>=0&&nc<8&&board[nr][nc]==eN) return true;
  }
  char eK = (byColor=='w')?'K':'k';
  int kd2[8][2] = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
  for (int i = 0; i < 8; i++) {
    int nr=r+kd2[i][0], nc=c+kd2[i][1];
    if (nr>=0&&nr<8&&nc>=0&&nc<8&&board[nr][nc]==eK) return true;
  }
  char eR=(byColor=='w')?'R':'r', eQ=(byColor=='w')?'Q':'q', eB=(byColor=='w')?'B':'b';
  int d4[4][2]  = {{1,0},{-1,0},{0,1},{0,-1}};
  int d4d[4][2] = {{1,1},{1,-1},{-1,1},{-1,-1}};
  for (int i = 0; i < 4; i++) {
    for (int s = 1; s < 8; s++) {
      int nr=r+s*d4[i][0], nc=c+s*d4[i][1];
      if (nr<0||nr>=8||nc<0||nc>=8) break;
      char t=board[nr][nc];
      if (t != ' ') { if (t==eR||t==eQ) return true; break; }
    }
    for (int s = 1; s < 8; s++) {
      int nr=r+s*d4d[i][0], nc=c+s*d4d[i][1];
      if (nr<0||nr>=8||nc<0||nc>=8) break;
      char t=board[nr][nc];
      if (t != ' ') { if (t==eB||t==eQ) return true; break; }
    }
  }
  return false;
}

bool isInCheck(char color) {
  char king = (color=='w') ? 'K' : 'k';
  char enemy = (color=='w') ? 'b' : 'w';
  for (int r = 0; r < 8; r++)
    for (int c = 0; c < 8; c++)
      if (board[r][c] == king) return isSquareAttacked(r, c, enemy);
  return false;
}

void getPossibleMoves(int row, int col, int &moveCount, int moves[][2]) {
  moveCount = 0;
  char piece = board[row][col];
  if (piece == ' ') return;
  char clr = (piece >= 'a' && piece <= 'z') ? 'b' : 'w';
  char pt  = (piece >= 'a' && piece <= 'z') ? piece - 32 : piece;

  auto canLand = [&](int r, int c) -> bool {
    if (r<0||r>=8||c<0||c>=8) return false;
    char t = board[r][c];
    return t == ' ' || ((t >= 'a' && t <= 'z') != (clr == 'b'));
  };
  auto pushMove = [&](int r, int c) {
    if (canLand(r, c)) { moves[moveCount][0]=r; moves[moveCount][1]=c; moveCount++; }
  };
  auto addLine = [&](int dr, int dc) {
    for (int s = 1; s < 8; s++) {
      int nr=row+s*dr, nc=col+s*dc;
      if (nr<0||nr>=8||nc<0||nc>=8) break;
      char t = board[nr][nc];
      if (t == ' ') { moves[moveCount][0]=nr; moves[moveCount][1]=nc; moveCount++; }
      else {
        if ((t >= 'a' && t <= 'z') != (clr == 'b')) { moves[moveCount][0]=nr; moves[moveCount][1]=nc; moveCount++; }
        break;
      }
    }
  };

  switch (pt) {
    case 'P': {
      int dir = (clr=='w') ? 1 : -1;
      int start = (clr=='w') ? 1 : 6;
      if (row+dir >= 0 && row+dir < 8 && board[row+dir][col] == ' ') {
        moves[moveCount][0]=row+dir; moves[moveCount][1]=col; moveCount++;
        if (row == start && board[row+2*dir][col] == ' ') {
          moves[moveCount][0]=row+2*dir; moves[moveCount][1]=col; moveCount++;
        }
      }
      int capDc[2] = {-1, 1};
      for (int i = 0; i < 2; i++) {
        int nr=row+dir, nc=col+capDc[i];
        if (nr>=0&&nr<8&&nc>=0&&nc<8) {
          char t = board[nr][nc];
          if (t != ' ' && (t >= 'a' && t <= 'z') != (clr == 'b'))
            { moves[moveCount][0]=nr; moves[moveCount][1]=nc; moveCount++; }
        }
      }
      break;
    }
    case 'R': addLine(1,0);addLine(-1,0);addLine(0,1);addLine(0,-1); break;
    case 'B': addLine(1,1);addLine(1,-1);addLine(-1,1);addLine(-1,-1); break;
    case 'Q': addLine(1,0);addLine(-1,0);addLine(0,1);addLine(0,-1);
              addLine(1,1);addLine(1,-1);addLine(-1,1);addLine(-1,-1); break;
    case 'N': {
      int nd[8][2]={{2,1},{1,2},{-1,2},{-2,1},{-2,-1},{-1,-2},{1,-2},{2,-1}};
      for (int i = 0; i < 8; i++) pushMove(row+nd[i][0], col+nd[i][1]);
      break;
    }
    case 'K': {
      int kd[8][2]={{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
      for (int i = 0; i < 8; i++) pushMove(row+kd[i][0], col+kd[i][1]);
      break;
    }
  }

  // Filter moves that leave own king in check
  int legal[28][2]; int lc = 0;
  for (int i = 0; i < moveCount; i++) {
    int tr=moves[i][0], tc=moves[i][1];
    char save = board[tr][tc];
    board[tr][tc] = piece; board[row][col] = ' ';
    if (!isInCheck(clr)) { legal[lc][0]=tr; legal[lc][1]=tc; lc++; }
    board[row][col] = piece; board[tr][tc] = save;
  }
  moveCount = lc;
  for (int i = 0; i < lc; i++) { moves[i][0]=legal[i][0]; moves[i][1]=legal[i][1]; }
}

bool hasLegalMoves(char color) {
  for (int r = 0; r < 8; r++)
    for (int c = 0; c < 8; c++) {
      char p = board[r][c];
      if (p == ' ') continue;
      bool mine = (color == 'w') ? (p >= 'A' && p <= 'Z') : (p >= 'a' && p <= 'z');
      if (!mine) continue;
      int mc = 0; int mv[28][2];
      getPossibleMoves(r, c, mc, mv);
      if (mc > 0) return true;
    }
  return false;
}

void applyPromotion(int row, int col) {
  if (board[row][col] == 'P' && row == 7) board[row][col] = 'Q';
  if (board[row][col] == 'p' && row == 0) board[row][col] = 'q';
}

void initBoard() {
  for (int r = 0; r < 8; r++)
    for (int c = 0; c < 8; c++)
      board[r][c] = INITIAL[r][c];
}

// ═══════════════════════════════════════════════════════════════
// ESP-NOW
// ═══════════════════════════════════════════════════════════════

void sendMsg(MsgType type, uint8_t d0=0, uint8_t d1=0, uint8_t d2=0, uint8_t d3=0) {
  Message msg; msg.type = type;
  msg.d[0]=d0; msg.d[1]=d1; msg.d[2]=d2; msg.d[3]=d3;
  esp_now_send(peerFound ? peerMac : broadcastAddr, (uint8_t*)&msg, sizeof(msg));
}

void onDataRecv(const esp_now_recv_info *recv_info, const uint8_t *data, int len) {
  if (len < 1) return;
  const Message* msg = (const Message*)data;
  msgType = msg->type;
  msgData[0]=msg->d[0]; msgData[1]=msg->d[1];
  msgData[2]=msg->d[2]; msgData[3]=msg->d[3];
  memcpy(msgSenderMac, recv_info->src_addr, 6);
  msgPending = true;
}

void onDataSent(const wifi_tx_info_t*, esp_now_send_status_t) {}

bool addPeer(uint8_t* mac) {
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, mac, 6);
  p.channel = 0; p.encrypt = false;
  return esp_now_add_peer(&p) == ESP_OK;
}

bool macGreater(uint8_t* a, uint8_t* b) {
  for (int i = 0; i < 6; i++) {
    if (a[i] > b[i]) return true;
    if (a[i] < b[i]) return false;
  }
  return false;
}

// ═══════════════════════════════════════════════════════════════
// DISCOVERY — broadcast until paired. Returns false if reset pressed.
// ═══════════════════════════════════════════════════════════════

bool discoveryLoop() {
  peerFound = false; msgPending = false;
  bool gameRunning = false;
  uint8_t myMac[6]; WiFi.macAddress(myMac);
  unsigned long lastBcast=0, lastReady=0;
  int animFrame=0; unsigned long animTimer=0;

  while (!gameRunning) {
    if (digitalRead(RESET_PIN)==LOW) { delay(50); if (digitalRead(RESET_PIN)==LOW) return false; }
    if (millis()-lastBcast > 600)                       { sendMsg(MSG_HELLO); lastBcast=millis(); }
    if (peerFound && !gameRunning && millis()-lastReady > 700) { sendMsg(MSG_READY); lastReady=millis(); }
    if (millis()-animTimer > 90)                        { searchingAnimation(animFrame++); animTimer=millis(); }

    if (msgPending) {
      msgPending = false;
      if (msgType == MSG_HELLO && !peerFound) {
        memcpy(peerMac, msgSenderMac, 6);
        peerFound = true; addPeer(peerMac);
        iAmWhite = macGreater(myMac, peerMac);
        sendMsg(MSG_READY); lastReady = millis();
      } else if (msgType == MSG_HELLO && peerFound) {
        sendMsg(MSG_READY); lastReady = millis();
      } else if (msgType == MSG_READY) {
        if (!peerFound) {
          memcpy(peerMac, msgSenderMac, 6);
          peerFound = true; addPeer(peerMac);
          iAmWhite = macGreater(myMac, peerMac);
          sendMsg(MSG_READY); lastReady = millis();
        }
        if (peerFound) gameRunning = true;
      }
    }
    delay(10);
  }
  strip.clear(); strip.show();
  delay(300);
  fireworkAnimation();
  return true;
}

// ═══════════════════════════════════════════════════════════════
// SETUP — wait for all 32 pieces, sync with peer before starting.
// Dim white on each empty starting square guides placement.
// ═══════════════════════════════════════════════════════════════

bool setupPhase() {
  // Wait until all pieces are on starting squares
  while (true) {
    if (digitalRead(RESET_PIN)==LOW) { delay(50); if (digitalRead(RESET_PIN)==LOW) { strip.clear(); strip.show(); return false; } }
    readSensors();
    bool allPlaced = true;
    for (int r = 0; r < 8; r++)
      for (int c = 0; c < 8; c++) {
        bool needsPiece = (INITIAL[r][c] != ' ') && !sensorState[r][c];
        if (needsPiece) allPlaced = false;
        strip.setPixelColor(r*8+c, needsPiece ? strip.Color(0,0,0,80) : 0);
      }
    strip.show();
    if (allPlaced) break;
    delay(200);
  }

  // Brief green flash — pieces are set
  for (int f = 0; f < 2; f++) {
    for (int i = 0; i < LED_COUNT; i++) strip.setPixelColor(i, strip.Color(0, 150, 0, 0));
    strip.show(); delay(200); strip.clear(); strip.show(); delay(200);
  }

  // Send MSG_SETUP_DONE and wait for the same from peer
  bool peerReady = false;
  unsigned long lastSend = 0;
  while (!peerReady) {
    if (digitalRead(RESET_PIN)==LOW) { delay(50); if (digitalRead(RESET_PIN)==LOW) { strip.clear(); strip.show(); return false; } }
    if (millis()-lastSend > 500) { sendMsg(MSG_SETUP_DONE); lastSend = millis(); }

    // Blue pulse on center 4 squares while waiting for peer
    uint8_t pulse = (uint8_t)(80 + 80 * sinf(millis() * 0.005f));
    strip.clear();
    strip.setPixelColor(3*8+3, strip.Color(0, 0, pulse, 0));
    strip.setPixelColor(3*8+4, strip.Color(0, 0, pulse, 0));
    strip.setPixelColor(4*8+3, strip.Color(0, 0, pulse, 0));
    strip.setPixelColor(4*8+4, strip.Color(0, 0, pulse, 0));
    strip.show();

    if (msgPending) {
      msgPending = false;
      if (msgType == MSG_SETUP_DONE) peerReady = true;
      else if (msgType == MSG_RESET) { strip.clear(); strip.show(); return false; }
    }
    delay(20);
  }

  sendMsg(MSG_SETUP_DONE); // final send so peer can exit too

  // Brief white flash — both boards ready
  for (int f = 0; f < 2; f++) {
    for (int i = 0; i < LED_COUNT; i++) strip.setPixelColor(i, strip.Color(0, 0, 0, 150));
    strip.show(); delay(200); strip.clear(); strip.show(); delay(200);
  }

  readSensors();
  memcpy(sensorPrev, sensorState, sizeof(sensorState));
  return true;
}

// ═══════════════════════════════════════════════════════════════
// MY TURN — pick up a piece, place on a legal square.
// Returns true on successful move, false on reset.
// ═══════════════════════════════════════════════════════════════

bool handleMyTurn() {
  char myColor = iAmWhite ? 'w' : 'b';
  showMyPiecesGlow(myColor);

  while (true) {
    if (digitalRead(RESET_PIN)==LOW) { delay(50); if (digitalRead(RESET_PIN)==LOW) { sendMsg(MSG_RESET); strip.clear(); strip.show(); return false; } }
    readSensors();

    for (int row = 0; row < 8; row++) {
      for (int col = 0; col < 8; col++) {
        if (!sensorPrev[row][col] || sensorState[row][col]) continue;

        // Piece lifted
        char piece = board[row][col];
        if (piece == ' ') continue;
        bool mine = (myColor=='w') ? (piece>='A'&&piece<='Z') : (piece>='a'&&piece<='z');
        if (!mine) { illegalFlash(row, col); continue; }

        int moveCount = 0; int moves[28][2];
        getPossibleMoves(row, col, moveCount, moves);

        if (moveCount == 0) {
          // Piece is pinned — flash amber, wait for return
          illegalFlash(row, col);
          while (!sensorState[row][col]) {
            readSensors();
            memcpy(sensorPrev, sensorState, sizeof(sensorState));
            delay(40);
          }
          memcpy(sensorPrev, sensorState, sizeof(sensorState));
          showMyPiecesGlow(myColor);
          break;
        }

        // Highlight lifted square and legal move targets
        strip.clear();
        strip.setPixelColor(row*8+col, strip.Color(0, 0, 0, 255));
        for (int i = 0; i < moveCount; i++) {
          int r2=moves[i][0], c2=moves[i][1];
          bool cap = (board[r2][c2] != ' ');
          strip.setPixelColor(r2*8+c2, cap ? strip.Color(255,60,0,0) : strip.Color(0,0,0,180));
          strip.show(); delay(20);
        }
        strip.show();

        // Wait for placement
        int toRow = -1, toCol = -1;
        bool placed = false;
        while (!placed) {
          if (digitalRead(RESET_PIN)==LOW) { delay(50); if (digitalRead(RESET_PIN)==LOW) { sendMsg(MSG_RESET); strip.clear(); strip.show(); return false; } }
          readSensors();

          if (sensorState[row][col]) {
            // Piece returned to origin — cancel
            toRow = -1; toCol = -1;
            placed = true;
            showMyPiecesGlow(myColor);
            break;
          }

          for (int r2 = 0; r2 < 8 && !placed; r2++) {
            for (int c2 = 0; c2 < 8 && !placed; c2++) {
              if (r2 == row && c2 == col) continue;
              if (!sensorState[r2][c2] || sensorPrev[r2][c2]) continue;
              bool legal = false;
              for (int i = 0; i < moveCount; i++) if (moves[i][0]==r2 && moves[i][1]==c2) { legal=true; break; }
              if (legal) {
                toRow = r2; toCol = c2; placed = true;
              } else {
                // Illegal square — flash amber, restore display
                strip.setPixelColor(r2*8+c2, strip.Color(200, 150, 0, 0));
                strip.show(); delay(400);
                strip.clear();
                strip.setPixelColor(row*8+col, strip.Color(0,0,0,255));
                for (int i = 0; i < moveCount; i++) {
                  int mr=moves[i][0], mc=moves[i][1];
                  strip.setPixelColor(mr*8+mc, (board[mr][mc]!=' ') ? strip.Color(255,60,0,0) : strip.Color(0,0,0,180));
                }
                strip.show();
              }
            }
          }
          memcpy(sensorPrev, sensorState, sizeof(sensorState));
          delay(40);
        }

        if (toRow == -1) break; // cancelled

        // Apply move
        board[toRow][toCol] = piece;
        board[row][col] = ' ';
        applyPromotion(toRow, toCol);

        // Green confirmation
        strip.clear();
        strip.setPixelColor(row*8+col,   strip.Color(0, 80, 0, 0));
        strip.setPixelColor(toRow*8+toCol, strip.Color(0, 255, 0, 0));
        strip.show();

        // Send move to peer
        sendMsg(MSG_MOVE, (uint8_t)row, (uint8_t)col, (uint8_t)toRow, (uint8_t)toCol);

        // Wait for opponent to physically complete the move on their board.
        // Pulse the to-square green while waiting. Resend every 2s if no reply.
        bool moveDone = false;
        unsigned long lastResend = millis();
        while (!moveDone) {
          if (digitalRead(RESET_PIN)==LOW) { delay(50); if (digitalRead(RESET_PIN)==LOW) { sendMsg(MSG_RESET); strip.clear(); strip.show(); return false; } }

          if (millis()-lastResend > 2000) {
            sendMsg(MSG_MOVE, (uint8_t)row, (uint8_t)col, (uint8_t)toRow, (uint8_t)toCol);
            lastResend = millis();
          }

          uint8_t pulse = (uint8_t)(80 + 80 * sinf(millis() * 0.006f));
          strip.setPixelColor(toRow*8+toCol, strip.Color(0, pulse, 0, 0));
          strip.show();

          if (msgPending) {
            msgPending = false;
            if (msgType == MSG_MOVE_DONE) moveDone = true;
            else if (msgType == MSG_RESET) { strip.clear(); strip.show(); return false; }
          }
          delay(30);
        }

        strip.clear(); strip.show();
        readSensors();
        memcpy(sensorPrev, sensorState, sizeof(sensorState));
        return true;
      }
    }

    memcpy(sensorPrev, sensorState, sizeof(sensorState));
    delay(40);
  }
}

// ═══════════════════════════════════════════════════════════════
// OPPONENT'S TURN — receive move, guide player to physically do it.
// Orange = pick up this piece. Green = place it here.
// Returns true on completion, false on reset.
// ═══════════════════════════════════════════════════════════════

bool handleOpponentTurn() {
  // Gentle blue pulse across the whole board while waiting for opponent's move
  int fromRow=-1, fromCol=-1, toRow=-1, toCol=-1;

  while (fromRow == -1) {
    if (digitalRead(RESET_PIN)==LOW) { delay(50); if (digitalRead(RESET_PIN)==LOW) { sendMsg(MSG_RESET); strip.clear(); strip.show(); return false; } }

    uint8_t pulse = (uint8_t)(20 + 20 * sinf(millis() * 0.004f));
    for (int i = 0; i < LED_COUNT; i++) strip.setPixelColor(i, strip.Color(0, 0, pulse, 0));
    strip.show();

    // Warn if player tries to move a piece — it's not their turn
    readSensors();
    for (int r = 0; r < 8; r++)
      for (int c = 0; c < 8; c++)
        if (sensorPrev[r][c] && !sensorState[r][c]) illegalFlash(r, c);
    memcpy(sensorPrev, sensorState, sizeof(sensorState));

    if (msgPending) {
      msgPending = false;
      if (msgType == MSG_MOVE) {
        fromRow=(int)msgData[0]; fromCol=(int)msgData[1];
        toRow  =(int)msgData[2]; toCol  =(int)msgData[3];
      } else if (msgType == MSG_RESET) { strip.clear(); strip.show(); return false; }
    }
    delay(30);
  }

  // Step 1: If there's a piece on the TO square, it must be captured (removed first).
  //         Flash that square red until the player removes it.
  if (board[toRow][toCol] != ' ') {
    while (true) {
      if (digitalRead(RESET_PIN)==LOW) { delay(50); if (digitalRead(RESET_PIN)==LOW) { sendMsg(MSG_RESET); strip.clear(); strip.show(); return false; } }
      readSensors();
      if (!sensorState[toRow][toCol]) break;
      strip.clear();
      strip.setPixelColor(toRow*8+toCol, strip.Color(255, 0, 0, 0));
      strip.show(); delay(200);
      strip.clear(); strip.show(); delay(150);
    }
    memcpy(sensorPrev, sensorState, sizeof(sensorState));
  }

  // Step 2: Show FROM (orange) and TO (green). Wait for player to lift FROM.
  strip.clear();
  strip.setPixelColor(fromRow*8+fromCol, strip.Color(255, 100, 0, 0)); // orange: pick up
  strip.setPixelColor(toRow*8+toCol,    strip.Color(0,   200, 0, 0)); // green: place here
  strip.show();

  while (sensorState[fromRow][fromCol]) {
    if (digitalRead(RESET_PIN)==LOW) { delay(50); if (digitalRead(RESET_PIN)==LOW) { sendMsg(MSG_RESET); strip.clear(); strip.show(); return false; } }
    // Resend may arrive while we wait — if same move, ignore; piece not placed yet
    if (msgPending) { msgPending = false; }
    readSensors();
    memcpy(sensorPrev, sensorState, sizeof(sensorState));
    delay(40);
  }

  // Step 3: FROM is empty. Pulse TO green until piece is placed there.
  while (true) {
    if (digitalRead(RESET_PIN)==LOW) { delay(50); if (digitalRead(RESET_PIN)==LOW) { sendMsg(MSG_RESET); strip.clear(); strip.show(); return false; } }
    if (msgPending) { msgPending = false; } // absorb any resends
    readSensors();
    if (sensorState[toRow][toCol]) break;
    uint8_t pulse = (uint8_t)(100 + 155 * fabsf(sinf(millis() * 0.007f)));
    strip.clear();
    strip.setPixelColor(toRow*8+toCol, strip.Color(0, pulse, 0, 0));
    strip.show();
    memcpy(sensorPrev, sensorState, sizeof(sensorState));
    delay(40);
  }

  // Physical move confirmed — apply to board
  char piece = board[fromRow][fromCol];
  board[toRow][toCol] = piece;
  board[fromRow][fromCol] = ' ';
  applyPromotion(toRow, toCol);

  // Confirmation flash
  strip.clear();
  strip.setPixelColor(fromRow*8+fromCol, strip.Color(0, 100, 0, 0));
  strip.setPixelColor(toRow*8+toCol,    strip.Color(0, 255, 0, 0));
  strip.show(); delay(600);
  strip.clear(); strip.show();

  // Tell the other board the move was physically completed
  sendMsg(MSG_MOVE_DONE);

  // Check if this move puts my king in check
  char myColor = iAmWhite ? 'w' : 'b';
  if (isInCheck(myColor)) checkFlash();

  readSensors();
  memcpy(sensorPrev, sensorState, sizeof(sensorState));
  return true;
}

// ═══════════════════════════════════════════════════════════════
// SETUP
// ═══════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n========================================");
  Serial.println("  BT-Chess — Physical Two-Board Chess");
  Serial.println("========================================");

  pinMode(SER_PIN,   OUTPUT);
  pinMode(RCLK_PIN,  OUTPUT);
  pinMode(SRCLK_PIN, OUTPUT);
  for (int i = 0; i < 8; i++) pinMode(ROW_PINS[i], INPUT_PULLUP);
  pinMode(RESET_PIN, INPUT_PULLUP);

  strip.begin();
  strip.setBrightness(BRIGHTNESS);
  strip.clear(); strip.show();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  if (esp_now_init() != ESP_OK) {
    while (true) {
      for (int i = 0; i < LED_COUNT; i++) strip.setPixelColor(i, strip.Color(255,0,0,0));
      strip.show(); delay(500); strip.clear(); strip.show(); delay(500);
    }
  }

  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);

  esp_now_peer_info_t bcast = {};
  memcpy(bcast.peer_addr, broadcastAddr, 6);
  bcast.channel = 0; bcast.encrypt = false;
  esp_now_add_peer(&bcast);
}

// ═══════════════════════════════════════════════════════════════
// MAIN LOOP
// ═══════════════════════════════════════════════════════════════

void loop() {
  initBoard();

  if (!discoveryLoop())  { strip.clear(); strip.show(); return; }
  if (!setupPhase())     { strip.clear(); strip.show(); return; }

  bool whiteTurn = true;

  while (true) {
    if (digitalRead(RESET_PIN)==LOW) { delay(50); if (digitalRead(RESET_PIN)==LOW) { sendMsg(MSG_RESET); strip.clear(); strip.show(); return; } }

    bool isMyTurn = (whiteTurn == iAmWhite);
    bool ok = isMyTurn ? handleMyTurn() : handleOpponentTurn();
    if (!ok) { strip.clear(); strip.show(); return; }

    whiteTurn = !whiteTurn;

    char nextColor = whiteTurn ? 'w' : 'b';
    if (!hasLegalMoves(nextColor)) {
      bool inCheck   = isInCheck(nextColor);
      bool nextIsMe  = (whiteTurn == iAmWhite);
      if (inCheck) checkmateFlash();
      gameOverAnimation(inCheck ? !nextIsMe : false);
      delay(4000);
      return;
    }
  }
}
