// ============================================================
// bt_chess.ino  —  Ghost Chess (ESP-NOW two-board)
//
//   Each board only holds its OWN physical pieces.
//   The opponent's pieces are shown as permanent purple ghost LEDs.
//   No physical mirroring of opponent moves required.
//
//   - Boards auto-discover each other via broadcast (no router)
//   - Higher MAC address = WHITE, lower = BLACK
//   - Firework animation plays when both boards connect
//   - Opponent's ghost pieces light up instantly when they move
//   - If the opponent captures one of YOUR pieces, that square
//     flashes red until you physically remove your piece
//   - IO9 = reset / return to discovery at any time
// ============================================================

#include <esp_now.h>
#include <WiFi.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>

// ── Hardware ─────────────────────────────────────────────────
#define LED_PIN    1
#define LED_COUNT  64
#define BRIGHTNESS 100

#define SER_PIN   45
#define RCLK_PIN  47
#define SRCLK_PIN 46

const int ROW_PINS[8] = {42, 41, 40, 39, 38, 37, 36, 35};

#define RESET_PIN 9   // IO9 → reset / return to discovery

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_RGBW + NEO_KHZ800);

// ── ESP-NOW messaging ─────────────────────────────────────────
enum MsgType : uint8_t { MSG_HELLO=0, MSG_READY, MSG_MOVE, MSG_RESET, MSG_PING, MSG_ACK };

struct __attribute__((packed)) Message {
  MsgType type;
  uint8_t d[4]; // MOVE: {fromRow, fromCol, toRow, toCol}
};

uint8_t broadcastAddr[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
uint8_t peerMac[6];
bool    peerFound   = false;
bool    gameRunning = false;
bool    iAmWhite    = false;

volatile bool          msgPending    = false;
volatile MsgType       msgType;
volatile uint8_t       msgData[4];
uint8_t                msgSenderMac[6];

volatile unsigned long lastPeerMsgMs = 0; // millis() of last message received from peer
volatile bool          pongPending   = false; // got MSG_PING, need to reply
volatile bool          moveAckd      = false; // got MSG_ACK confirming our last move

// ── Chess state ───────────────────────────────────────────────
bool sensorState[8][8];
bool sensorPrev[8][8];

const char initialBoard[8][8] = {
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

bool checkInitialBoard() {
  readSensors();
  for (int r = 0; r < 8; r++)
    for (int c = 0; c < 8; c++)
      if (initialBoard[r][c] != ' ' && !sensorState[r][c])
        return false;
  return true;
}

void showSetupProgress() {
  // Ghost Chess: only show missing pieces for YOUR rows
  readSensors();
  int r0 = iAmWhite ? 0 : 6;
  int r1 = iAmWhite ? 1 : 7;
  for (int r = 0; r < 8; r++)
    for (int c = 0; c < 8; c++) {
      int px = r * 8 + c;
      if (r >= r0 && r <= r1 && initialBoard[r][c] != ' ')
        strip.setPixelColor(px, sensorState[r][c] ? 0 : strip.Color(0, 0, 0, 200));
      else
        strip.setPixelColor(px, 0);
    }
  strip.show();
}

// Only check that THIS board's own pieces are in position (Ghost Chess)
bool checkMyPiecesPlaced() {
  readSensors();
  int r0 = iAmWhite ? 0 : 6;
  int r1 = iAmWhite ? 1 : 7;
  for (int r = r0; r <= r1; r++)
    for (int c = 0; c < 8; c++)
      if (initialBoard[r][c] != ' ' && !sensorState[r][c])
        return false;
  return true;
}

// Permanently display opponent's pieces as dim purple ghost LEDs
void showGhosts() {
  strip.clear();
  char oppColor = iAmWhite ? 'b' : 'w';
  for (int r = 0; r < 8; r++)
    for (int c = 0; c < 8; c++) {
      char p = board[r][c];
      if (p == ' ') continue;
      bool isOpp = (oppColor=='b') ? (p>='a'&&p<='z') : (p>='A'&&p<='Z');
      if (isOpp)
        strip.setPixelColor(r*8+c, strip.Color(80, 0, 200, 0));
    }
  strip.show();
}

// ═══════════════════════════════════════════════════════════════
// LED ANIMATIONS
// ═══════════════════════════════════════════════════════════════

void fireworkAnimation() {
  // Expanding ring burst
  float cx = 3.5f, cy = 3.5f;
  for (float r = 0; r < 6.5f; r += 0.45f) {
    for (int row = 0; row < 8; row++)
      for (int col = 0; col < 8; col++) {
        float dx = col - cx, dy = row - cy;
        float dist = sqrtf(dx*dx + dy*dy);
        uint32_t color = 0;
        if (fabsf(dist - r) < 0.55f)
          color = iAmWhite ? strip.Color(0,0,0,255) : strip.Color(0,180,255,0);
        strip.setPixelColor(row * 8 + col, color);
      }
    strip.show();
    delay(45);
  }
  // Three full-board flashes in player's color
  for (int f = 0; f < 3; f++) {
    uint32_t c = iAmWhite ? strip.Color(0,0,0,255) : strip.Color(0,180,255,0);
    for (int i = 0; i < LED_COUNT; i++) strip.setPixelColor(i, c);
    strip.show(); delay(180);
    strip.clear(); strip.show(); delay(180);
  }
  strip.clear();
  strip.show();
}

void captureAnimation(int captureRow, int captureCol) {
  float cx = captureCol, cy = captureRow;
  for (float r = 0; r < 6.0f; r += 0.8f) {
    for (int row = 0; row < 8; row++)
      for (int col = 0; col < 8; col++) {
        float dx = col - cx, dy = row - cy;
        float dist = sqrtf(dx*dx + dy*dy);
        strip.setPixelColor(row*8+col, fabsf(dist-r) < 0.7f ? strip.Color(255,0,0,0) : 0);
      }
    strip.show();
    delay(70);
  }
  strip.clear();
  strip.show();
}

void gameOverAnimation(bool iWon) {
  char myColor = iAmWhite ? 'w' : 'b';
  for (int f = 0; f < 10; f++) {
    for (int r = 0; r < 8; r++)
      for (int c = 0; c < 8; c++) {
        char p = board[r][c];
        if (p == ' ') { strip.setPixelColor(r*8+c, 0); continue; }
        bool mine = (myColor=='w') ? (p>='A'&&p<='Z') : (p>='a'&&p<='z');
        uint32_t col32 = 0;
        if (f % 2 == 0) col32 = mine ? strip.Color(0,255,0,0) : strip.Color(255,0,0,0);
        strip.setPixelColor(r*8+c, col32);
      }
    strip.show();
    delay(350);
  }
  strip.clear();
  strip.show();
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

void illegalMoveFlash(int row, int col) {
  for (int f = 0; f < 3; f++) {
    strip.setPixelColor(row*8+col, strip.Color(200, 150, 0, 0));
    strip.show(); delay(160);
    strip.setPixelColor(row*8+col, 0);
    strip.show(); delay(110);
  }
}

void searchingAnimation(int frame) {
  strip.clear();
  // A diagonal sweep to indicate "scanning"
  int diag = frame % 15;
  for (int r = 0; r < 8; r++)
    for (int c = 0; c < 8; c++) {
      int d = r + c;
      if (d == diag || d == diag - 1)
        strip.setPixelColor(r*8+c, strip.Color(0, 0, 120, 0));
    }
  strip.show();
}

// ═══════════════════════════════════════════════════════════════
// CHESS LOGIC
// ═══════════════════════════════════════════════════════════════

bool isSquareAttacked(int r, int c, char byColor) {
  // Pawns
  int pDir = (byColor == 'w') ? 1 : -1;
  char ePawn = (byColor == 'w') ? 'P' : 'p';
  int pawnDc[2] = {-1, 1};
  for (int i = 0; i < 2; i++) {
    int pr = r - pDir, pc = c + pawnDc[i];
    if (pr>=0&&pr<8&&pc>=0&&pc<8&&board[pr][pc]==ePawn) return true;
  }
  // Knights
  char eKnight = (byColor=='w') ? 'N' : 'n';
  int kd[8][2] = {{2,1},{1,2},{-1,2},{-2,1},{-2,-1},{-1,-2},{1,-2},{2,-1}};
  for (int i = 0; i < 8; i++) {
    int nr = r+kd[i][0], nc = c+kd[i][1];
    if (nr>=0&&nr<8&&nc>=0&&nc<8&&board[nr][nc]==eKnight) return true;
  }
  // King
  char eKing = (byColor=='w') ? 'K' : 'k';
  int kkd[8][2] = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
  for (int i = 0; i < 8; i++) {
    int nr = r+kkd[i][0], nc = c+kkd[i][1];
    if (nr>=0&&nr<8&&nc>=0&&nc<8&&board[nr][nc]==eKing) return true;
  }
  // Rook / Queen (straight)
  char eR = (byColor=='w')?'R':'r', eQ = (byColor=='w')?'Q':'q';
  int d4[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
  for (int i = 0; i < 4; i++) {
    for (int s = 1; s < 8; s++) {
      int nr = r+s*d4[i][0], nc = c+s*d4[i][1];
      if (nr<0||nr>=8||nc<0||nc>=8) break;
      char t = board[nr][nc];
      if (t != ' ') { if (t==eR||t==eQ) return true; break; }
    }
  }
  // Bishop / Queen (diagonal)
  char eB = (byColor=='w')?'B':'b';
  int d4d[4][2] = {{1,1},{1,-1},{-1,1},{-1,-1}};
  for (int i = 0; i < 4; i++) {
    for (int s = 1; s < 8; s++) {
      int nr = r+s*d4d[i][0], nc = c+s*d4d[i][1];
      if (nr<0||nr>=8||nc<0||nc>=8) break;
      char t = board[nr][nc];
      if (t != ' ') { if (t==eB||t==eQ) return true; break; }
    }
  }
  return false;
}

bool isInCheck(char color) {
  char king  = (color=='w') ? 'K' : 'k';
  char enemy = (color=='w') ? 'b' : 'w';
  for (int r = 0; r < 8; r++)
    for (int c = 0; c < 8; c++)
      if (board[r][c] == king)
        return isSquareAttacked(r, c, enemy);
  return false;
}

void getPossibleMoves(int row, int col, int &moveCount, int moves[][2]) {
  moveCount = 0;
  char piece = board[row][col];
  if (piece == ' ') return;

  char clr = (piece>='a'&&piece<='z') ? 'b' : 'w';
  char pt  = (piece>='a'&&piece<='z') ? piece-32 : piece;

  auto canLand = [&](int r, int c) -> bool {
    if (r<0||r>=8||c<0||c>=8) return false;
    char t = board[r][c];
    return t==' ' || ((t>='a'&&t<='z') != (clr=='b'));
  };
  auto pushMove = [&](int r, int c) {
    if (canLand(r,c)) { moves[moveCount][0]=r; moves[moveCount][1]=c; moveCount++; }
  };
  auto addLine = [&](int dr, int dc) {
    for (int s = 1; s < 8; s++) {
      int nr = row+s*dr, nc = col+s*dc;
      if (nr<0||nr>=8||nc<0||nc>=8) break;
      char t = board[nr][nc];
      if (t == ' ') { moves[moveCount][0]=nr; moves[moveCount][1]=nc; moveCount++; }
      else {
        if ((t>='a'&&t<='z') != (clr=='b')) { moves[moveCount][0]=nr; moves[moveCount][1]=nc; moveCount++; }
        break;
      }
    }
  };

  switch (pt) {
    case 'P': {
      int dir = (clr=='w') ? 1 : -1;
      int start = (clr=='w') ? 1 : 6;
      if (row+dir>=0&&row+dir<8&&board[row+dir][col]==' ') {
        moves[moveCount][0]=row+dir; moves[moveCount][1]=col; moveCount++;
        if (row==start&&board[row+2*dir][col]==' ') {
          moves[moveCount][0]=row+2*dir; moves[moveCount][1]=col; moveCount++;
        }
      }
      int capDc[2] = {-1, 1};
      for (int i = 0; i < 2; i++) {
        int nr = row+dir, nc = col+capDc[i];
        if (nr>=0&&nr<8&&nc>=0&&nc<8) {
          char t = board[nr][nc];
          if (t!=' ' && (t>='a'&&t<='z')!=(clr=='b'))
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
      for (int i=0;i<8;i++) pushMove(row+nd[i][0],col+nd[i][1]);
      break;
    }
    case 'K': {
      int kd[8][2]={{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
      for (int i=0;i<8;i++) pushMove(row+kd[i][0],col+kd[i][1]);
      break;
    }
  }

  // Filter moves that leave own king in check
  int legal[28][2]; int lc = 0;
  for (int i = 0; i < moveCount; i++) {
    int tr = moves[i][0], tc = moves[i][1];
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
      bool mine = (color=='w') ? (p>='A'&&p<='Z') : (p>='a'&&p<='z');
      if (!mine) continue;
      int mc = 0; int mv[28][2];
      getPossibleMoves(r, c, mc, mv);
      if (mc > 0) return true;
    }
  return false;
}

void applyPromotion(int row, int col) {
  char p = board[row][col];
  if (p=='P' && row==7) board[row][col]='Q';
  if (p=='p' && row==0) board[row][col]='q';
}

void initBoard() {
  for (int r = 0; r < 8; r++)
    for (int c = 0; c < 8; c++)
      board[r][c] = initialBoard[r][c];
}

// ═══════════════════════════════════════════════════════════════
// ESP-NOW
// ═══════════════════════════════════════════════════════════════

void sendMsg(MsgType type, uint8_t d0=0, uint8_t d1=0, uint8_t d2=0, uint8_t d3=0) {
  Message msg;
  msg.type = type;
  msg.d[0]=d0; msg.d[1]=d1; msg.d[2]=d2; msg.d[3]=d3;
  uint8_t* target = peerFound ? peerMac : broadcastAddr;
  esp_now_send(target, (uint8_t*)&msg, sizeof(msg));
}

// Callback — keep it minimal (no heavy work in ISR context)
void onDataRecv(const esp_now_recv_info *recv_info, const uint8_t *data, int len) {
  if (len < 1) return;
  const Message* msg = (const Message*)data;
  lastPeerMsgMs = millis();
  if (msg->type == MSG_ACK)  { moveAckd    = true; return; }
  if (msg->type == MSG_PING) { pongPending = true; return; }
  msgType   = msg->type;
  msgData[0]=msg->d[0]; msgData[1]=msg->d[1];
  msgData[2]=msg->d[2]; msgData[3]=msg->d[3];
  memcpy(msgSenderMac, recv_info->src_addr, 6);
  msgPending = true;
}

void onDataSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {}

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
// DISCOVERY PHASE
// Returns true when connected, false if reset pressed
// ═══════════════════════════════════════════════════════════════
bool discoveryLoop() {
  Serial.println("\n[BT-Chess] Searching for peer board...");
  peerFound   = false;
  gameRunning = false;
  msgPending  = false;

  uint8_t myMac[6];
  WiFi.macAddress(myMac);
  Serial.printf("My MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
    myMac[0],myMac[1],myMac[2],myMac[3],myMac[4],myMac[5]);

  unsigned long lastBroadcast = 0;
  unsigned long lastReady     = 0;
  int animFrame = 0;
  unsigned long animTimer = 0;

  while (!gameRunning) {
    if (digitalRead(RESET_PIN) == LOW) { delay(50); if (digitalRead(RESET_PIN)==LOW) return false; }

    // Broadcast hello every 600 ms
    if (millis() - lastBroadcast > 600) {
      sendMsg(MSG_HELLO);
      lastBroadcast = millis();
    }

    // If peer found but not yet confirmed ready, keep sending READY
    if (peerFound && !gameRunning && millis() - lastReady > 700) {
      sendMsg(MSG_READY);
      lastReady = millis();
    }

    // Searching animation
    if (millis() - animTimer > 90) {
      searchingAnimation(animFrame++);
      animTimer = millis();
    }

    if (msgPending) {
      msgPending = false;

      if (msgType == MSG_HELLO && !peerFound) {
        Serial.println("\n[BT-Chess] Peer found! Pairing...");
        memcpy(peerMac, msgSenderMac, 6);
        peerFound = true;
        addPeer(peerMac);
        iAmWhite = macGreater(myMac, peerMac);
        Serial.print("[BT-Chess] Role: "); Serial.println(iAmWhite ? "WHITE" : "BLACK");
        sendMsg(MSG_READY);
        lastReady = millis();
      }
      else if (msgType == MSG_HELLO && peerFound) {
        // Re-send ready in case they missed it
        sendMsg(MSG_READY);
        lastReady = millis();
      }
      else if (msgType == MSG_READY) {
        if (!peerFound) {
          // Peer sent READY before we saw their HELLO — accept it
          memcpy(peerMac, msgSenderMac, 6);
          peerFound = true;
          addPeer(peerMac);
          iAmWhite = macGreater(myMac, peerMac);
          Serial.print("[BT-Chess] Role (from READY): "); Serial.println(iAmWhite ? "WHITE" : "BLACK");
          sendMsg(MSG_READY);
          lastReady = millis();
        }
        if (peerFound) {
          Serial.println("[BT-Chess] Connection confirmed!");
          gameRunning   = true;
          lastPeerMsgMs = millis();
        }
      }
    }

    delay(10);
  }

  // Short pause then firework
  strip.clear(); strip.show();
  delay(300);
  fireworkAnimation();
  return true;
}

// ═══════════════════════════════════════════════════════════════
// RECONNECT  — re-pair without resetting board state or roles.
// Called mid-game when peer goes silent. Returns true on success.
// ═══════════════════════════════════════════════════════════════
bool reconnectMidGame() {
  Serial.println("[Ghost-Chess] Peer silent — attempting reconnect...");
  for (int f = 0; f < 3; f++) {
    for (int i = 0; i < LED_COUNT; i++) strip.setPixelColor(i, strip.Color(200, 80, 0, 0));
    strip.show(); delay(200); strip.clear(); strip.show(); delay(200);
  }

  peerFound  = false;
  msgPending = false;
  unsigned long lastBcast = 0, lastRdy = 0;
  unsigned long deadline  = millis() + 30000UL;

  while (millis() < deadline) {
    if (digitalRead(RESET_PIN) == LOW) { delay(50); if (digitalRead(RESET_PIN) == LOW) return false; }
    if (millis() - lastBcast > 500)               { sendMsg(MSG_HELLO); lastBcast = millis(); }
    if (peerFound && millis() - lastRdy > 600)    { sendMsg(MSG_READY); lastRdy   = millis(); }

    if (msgPending) {
      msgPending = false;
      if ((msgType == MSG_HELLO || msgType == MSG_READY) && !peerFound) {
        memcpy(peerMac, msgSenderMac, 6);
        peerFound = true;
        addPeer(peerMac);
        sendMsg(MSG_READY); lastRdy = millis();
      } else if (msgType == MSG_HELLO && peerFound) {
        sendMsg(MSG_READY); lastRdy = millis();
      } else if (msgType == MSG_READY && peerFound) {
        // Re-paired successfully
        lastPeerMsgMs = millis();
        Serial.println("[Ghost-Chess] Reconnected!");
        for (int f = 0; f < 2; f++) {
          for (int i = 0; i < LED_COUNT; i++) strip.setPixelColor(i, strip.Color(0, 200, 0, 0));
          strip.show(); delay(180); strip.clear(); strip.show(); delay(180);
        }
        return true;
      }
    }
    delay(10);
  }
  Serial.println("[Ghost-Chess] Reconnect timed out.");
  return false;
}

// Send MSG_MOVE and wait for MSG_ACK; retries up to 5 times.
// Responds to incoming pings while waiting. Returns true if acked.
bool sendMoveWithRetry(uint8_t fr, uint8_t fc, uint8_t tr, uint8_t tc) {
  for (int attempt = 0; attempt < 5; attempt++) {
    moveAckd = false;
    sendMsg(MSG_MOVE, fr, fc, tr, tc);
    Serial.printf("[Ghost-Chess] Move sent (try %d): %c%d->%c%d\n",
                  attempt+1, 'A'+fc, fr+1, 'A'+tc, tr+1);
    unsigned long t = millis();
    while (!moveAckd && millis() - t < 1500) {
      if (pongPending) { pongPending = false; sendMsg(MSG_ACK); }
      delay(20);
    }
    if (moveAckd) return true;
    delay(300 * (attempt + 1));
  }
  return false;
}

// ═══════════════════════════════════════════════════════════════
// MY TURN  — local player picks up and places their piece
// Returns true on successful move, false on reset
// ═══════════════════════════════════════════════════════════════
bool handleMyTurn() {
  char myColor = iAmWhite ? 'w' : 'b';
  Serial.println(iAmWhite ? "[BT-Chess] YOUR TURN (WHITE)" : "[BT-Chess] YOUR TURN (BLACK)");

  // Show opponent ghosts while waiting for the player to pick up a piece
  showGhosts();

  while (true) {
    if (digitalRead(RESET_PIN)==LOW) { delay(50); if (digitalRead(RESET_PIN)==LOW) { sendMsg(MSG_RESET); return false; } }
    if (pongPending) { pongPending = false; sendMsg(MSG_ACK); }

    readSensors();

    for (int row = 0; row < 8; row++) {
      for (int col = 0; col < 8; col++) {
        if (!sensorPrev[row][col] || sensorState[row][col]) continue;
        // Piece lifted
        char piece = board[row][col];
        if (piece == ' ') continue;
        bool mine = (myColor=='w') ? (piece>='A'&&piece<='Z') : (piece>='a'&&piece<='z');
        if (!mine) {
          // Opponent's piece — not allowed to move it
          illegalMoveFlash(row, col);
          showGhosts();
          continue;
        }

        // Compute legal moves
        int moveCount = 0; int moves[28][2];
        getPossibleMoves(row, col, moveCount, moves);

        if (moveCount == 0) {
          // Piece is legally stuck (pinned or all moves leave king in check)
          illegalMoveFlash(row, col);
          showGhosts();
          // Wait for piece to be returned to its square
          while (true) {
            if (digitalRead(RESET_PIN)==LOW) { delay(50); if (digitalRead(RESET_PIN)==LOW) { sendMsg(MSG_RESET); strip.clear(); strip.show(); return false; } }
            readSensors();
            if (sensorState[row][col]) break;
            memcpy(sensorPrev, sensorState, sizeof(sensorState));
            delay(40);
          }
          memcpy(sensorPrev, sensorState, sizeof(sensorState));
          break;
        }

        // Sort by distance for ripple effect
        for (int i = 0; i < moveCount-1; i++)
          for (int j = 0; j < moveCount-i-1; j++) {
            int dr0=moves[j][0]-row,   dc0=moves[j][1]-col;
            int dr1=moves[j+1][0]-row, dc1=moves[j+1][1]-col;
            if (dr0*dr0+dc0*dc0 > dr1*dr1+dc1*dc1) {
              int tr=moves[j][0],tc=moves[j][1];
              moves[j][0]=moves[j+1][0]; moves[j][1]=moves[j+1][1];
              moves[j+1][0]=tr; moves[j+1][1]=tc;
            }
          }

        // Show ghosts as base layer, overlay lifted square + legal moves
        showGhosts();
        strip.setPixelColor(row*8+col, strip.Color(0,0,0,200));
        for (int i = 0; i < moveCount; i++) {
          int r2=moves[i][0], c2=moves[i][1];
          bool cap = (board[r2][c2] != ' ');
          // Captures glow orange-red over the ghost; empty squares are white
          strip.setPixelColor(r2*8+c2, cap ? strip.Color(255,60,0,0) : strip.Color(0,0,0,180));
          strip.show();
          delay(25);
        }
        strip.show();

        // Wait for placement
        int toRow=-1, toCol=-1;
        bool placed = false;

        while (!placed) {
          if (digitalRead(RESET_PIN)==LOW) { delay(50); if (digitalRead(RESET_PIN)==LOW) {
            sendMsg(MSG_RESET); strip.clear(); strip.show(); return false;
          }}
          if (pongPending) { pongPending = false; sendMsg(MSG_ACK); }
          readSensors();

          // Returned to origin — cancel move
          if (sensorState[row][col]) { showGhosts(); placed=true; break; }

          for (int r2 = 0; r2 < 8 && !placed; r2++) {
            for (int c2 = 0; c2 < 8; c2++) {
              if (r2==row && c2==col) continue;
              if (!sensorState[r2][c2] || sensorPrev[r2][c2]) continue; // no new placement

              bool legal = false;
              for (int i=0;i<moveCount;i++) if(moves[i][0]==r2&&moves[i][1]==c2){legal=true;break;}

              if (legal) {
                toRow=r2; toCol=c2; placed=true;
              } else {
                // Illegal square — flash yellow then restore legal-move display
                strip.setPixelColor(r2*8+c2, strip.Color(200,150,0,0));
                strip.show(); delay(400);
                showGhosts();
                strip.setPixelColor(row*8+col, strip.Color(0,0,0,200));
                for (int i=0;i<moveCount;i++) {
                  int mr=moves[i][0], mc=moves[i][1];
                  bool cap=(board[mr][mc]!=' ');
                  strip.setPixelColor(mr*8+mc, cap ? strip.Color(255,60,0,0) : strip.Color(0,0,0,180));
                }
                strip.show();
              }
              break; // only one new event per scan cycle
            }
          }
          memcpy(sensorPrev, sensorState, sizeof(sensorState));
          delay(40);
        }

        // Returned to origin — cancel (ghosts already restored above)
        if (toRow==-1 || (toRow==row && toCol==col)) break;

        // Apply move
        bool isCapture = (board[toRow][toCol] != ' ');
        if (isCapture) captureAnimation(toRow, toCol);

        board[toRow][toCol] = piece;
        board[row][col] = ' ';
        applyPromotion(toRow, toCol);

        // Send move to peer (with ACK retry + reconnect fallback)
        bool ackd = sendMoveWithRetry((uint8_t)row,(uint8_t)col,(uint8_t)toRow,(uint8_t)toCol);
        if (!ackd) {
          if (reconnectMidGame())
            ackd = sendMoveWithRetry((uint8_t)row,(uint8_t)col,(uint8_t)toRow,(uint8_t)toCol);
        }
        if (!ackd) { strip.clear(); strip.show(); return false; }

        // Green confirmation blink over ghost display
        for (int f = 0; f < 2; f++) {
          showGhosts();
          strip.setPixelColor(toRow*8+toCol, strip.Color(0,255,0,0));
          strip.show(); delay(200);
          showGhosts(); delay(200);
        }
        showGhosts(); // restore ghost display (board state now updated)

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
// OPPONENT'S TURN — Ghost Chess edition
// Receive move via ESP-NOW, update ghost display instantly.
// No physical mirroring needed — opponent pieces are LEDs only.
// If the opponent captures one of YOUR physical pieces, flash
// that square red until the player removes it from the board.
// Returns true on success, false on reset
// ═══════════════════════════════════════════════════════════════
bool handleOpponentTurn() {
  Serial.println(iAmWhite ? "[Ghost-Chess] Waiting for BLACK's move..." : "[Ghost-Chess] Waiting for WHITE's move...");

  // Show ghosts while waiting
  showGhosts();

  int fromRow=-1, fromCol=-1, toRow=-1, toCol=-1;

  // Wait for MSG_MOVE
  while (fromRow == -1) {
    if (digitalRead(RESET_PIN)==LOW) { delay(50); if (digitalRead(RESET_PIN)==LOW) { strip.clear(); strip.show(); return false; } }

    if (msgPending) {
      msgPending = false;
      if (msgType == MSG_MOVE) {
        fromRow=(int)msgData[0]; fromCol=(int)msgData[1];
        toRow  =(int)msgData[2]; toCol  =(int)msgData[3];
        Serial.printf("[Ghost-Chess] Received: %c%d -> %c%d\n",'A'+fromCol,fromRow+1,'A'+toCol,toRow+1);
      } else if (msgType == MSG_RESET) {
        strip.clear(); strip.show(); return false;
      }
    }

    // Warn if the player tries to pick up any piece — it's not their turn
    readSensors();
    for (int r = 0; r < 8; r++) {
      for (int c = 0; c < 8; c++) {
        if (sensorPrev[r][c] && !sensorState[r][c]) {
          illegalMoveFlash(r, c);
          showGhosts();
        }
      }
    }
    memcpy(sensorPrev, sensorState, sizeof(sensorState));

    delay(20);
  }

  // Determine if opponent captured one of my physical pieces
  char myColor = iAmWhite ? 'w' : 'b';
  char capturedPiece = board[toRow][toCol];
  bool capturedMine = (capturedPiece != ' ') &&
    ((myColor=='w') ? (capturedPiece>='A'&&capturedPiece<='Z')
                    : (capturedPiece>='a'&&capturedPiece<='z'));

  // Apply move to internal board (ghost moves instantly)
  char piece = board[fromRow][fromCol];
  board[toRow][toCol] = piece;
  board[fromRow][fromCol] = ' ';
  applyPromotion(toRow, toCol);

  // If a physical piece of mine was captured, wait for player to remove it
  if (capturedMine) {
    Serial.printf("[Ghost-Chess] Your piece at %c%d was captured — please remove it.\n",
                  'A'+toCol, toRow+1);
    readSensors();
    while (sensorState[toRow][toCol]) {
      if (digitalRead(RESET_PIN)==LOW) { delay(50); if (digitalRead(RESET_PIN)==LOW) { sendMsg(MSG_RESET); strip.clear(); strip.show(); return false; } }
      // Flash red: "remove this piece"
      strip.setPixelColor(toRow*8+toCol, strip.Color(255,0,0,0));
      strip.show(); delay(220);
      strip.setPixelColor(toRow*8+toCol, 0);
      strip.show(); delay(220);
      readSensors();
    }
    memcpy(sensorPrev, sensorState, sizeof(sensorState));
  }

  // Brief flash to show where the ghost landed, then restore ghost display
  for (int f = 0; f < 3; f++) {
    showGhosts();
    strip.setPixelColor(toRow*8+toCol, strip.Color(0, 255, 120, 0));
    strip.show(); delay(150);
    showGhosts(); delay(100);
  }

  readSensors();
  memcpy(sensorPrev, sensorState, sizeof(sensorState));

  // Flash red if the opponent's move put my king in check
  if (isInCheck(myColor)) {
    Serial.println("[Ghost-Chess] Your king is in CHECK!");
    checkFlash();
  }

  showGhosts();
  return true;
}

// ═══════════════════════════════════════════════════════════════
// SETUP
// ═══════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n========================================");
  Serial.println("  BT-Chess — ESP-NOW Two-Board Chess");
  Serial.println("========================================");

  // Hardware init
  pinMode(SER_PIN,   OUTPUT);
  pinMode(RCLK_PIN,  OUTPUT);
  pinMode(SRCLK_PIN, OUTPUT);
  for (int i = 0; i < 8; i++) pinMode(ROW_PINS[i], INPUT_PULLUP);
  pinMode(RESET_PIN, INPUT_PULLUP);

  strip.begin();
  strip.setBrightness(BRIGHTNESS);
  strip.clear();
  strip.show();

  // ESP-NOW requires WiFi in STA mode (no connection needed)
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ERROR: ESP-NOW init failed!");
    // Flash red forever
    while (true) {
      for (int i=0;i<LED_COUNT;i++) strip.setPixelColor(i,strip.Color(255,0,0,0));
      strip.show(); delay(500); strip.clear(); strip.show(); delay(500);
    }
  }

  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);

  // Register broadcast peer for discovery
  esp_now_peer_info_t bcast = {};
  memcpy(bcast.peer_addr, broadcastAddr, 6);
  bcast.channel = 0; bcast.encrypt = false;
  esp_now_add_peer(&bcast);

  Serial.println("ESP-NOW ready. Waiting to discover peer...");
}

// ═══════════════════════════════════════════════════════════════
// MAIN LOOP
// ═══════════════════════════════════════════════════════════════
void loop() {
  initBoard();

  // ── Phase 1: Discovery ─────────────────────────────────────
  if (!discoveryLoop()) {
    // Reset pressed during discovery — restart
    strip.clear(); strip.show();
    return;
  }

  // ── Phase 2: Place YOUR pieces only (Ghost Chess) ──────────
  Serial.print("[Ghost-Chess] Place your ");
  Serial.println(iAmWhite ? "WHITE pieces (rows 1–2)." : "BLACK pieces (rows 7–8).");
  while (!checkMyPiecesPlaced()) {
    if (digitalRead(RESET_PIN)==LOW) { delay(50); if (digitalRead(RESET_PIN)==LOW) { strip.clear(); strip.show(); return; } }
    showSetupProgress();
    delay(300);
  }
  strip.clear(); strip.show();

  readSensors();
  memcpy(sensorPrev, sensorState, sizeof(sensorState));
  Serial.print("[Ghost-Chess] Game started! Playing as: ");
  Serial.println(iAmWhite ? "WHITE" : "BLACK");

  // Show opponent ghosts immediately so players can see the full board state
  showGhosts();

  // ── Phase 3: Game loop ─────────────────────────────────────
  bool whiteTurn = true;

  while (true) {
    // Reset button check at top of every turn
    if (digitalRead(RESET_PIN)==LOW) {
      delay(50);
      if (digitalRead(RESET_PIN)==LOW) {
        sendMsg(MSG_RESET);
        for (int f=0;f<3;f++) {
          for (int i=0;i<LED_COUNT;i++) strip.setPixelColor(i,strip.Color(255,0,0,0));
          strip.show(); delay(150); strip.clear(); strip.show(); delay(150);
        }
        return;
      }
    }

    bool isMyTurn = (whiteTurn == iAmWhite);
    bool ok = isMyTurn ? handleMyTurn() : handleOpponentTurn();

    if (!ok) {
      // Reset requested mid-game
      for (int f=0;f<3;f++) {
        for (int i=0;i<LED_COUNT;i++) strip.setPixelColor(i,strip.Color(255,0,0,0));
        strip.show(); delay(150); strip.clear(); strip.show(); delay(150);
      }
      return;
    }

    // Flip turn
    whiteTurn = !whiteTurn;

    // Game-over check for the next player
    char nextColor = whiteTurn ? 'w' : 'b';
    if (!hasLegalMoves(nextColor)) {
      bool inCheck = isInCheck(nextColor);
      bool nextIsMe = (whiteTurn == iAmWhite);
      Serial.println(inCheck ? "CHECKMATE!" : "STALEMATE!");

      if (inCheck) checkmateFlash();

      bool iWon = inCheck ? !nextIsMe : false;
      gameOverAnimation(iWon);
      delay(4000);
      return; // back to discovery
    }
  }
}
