// ============================================================
// long_chess.ino  —  Long Chess (8×16 board, two boards end-to-end)
//
//   Board A (White) = global rows  0–7   (rows 1–2 hold White pieces)
//   Board B (Black) = global rows 8–15   (rows 14–15 hold Black pieces)
//
//   The two boards sit end-to-end forming one 8×16 playing field.
//   Any piece can move anywhere on the full 16-row board.
//   Legal-move LEDs ripple across BOTH boards simultaneously.
//
//   When a piece crosses to the other physical board, the player
//   physically carries it over. The receiving board detects the
//   placement and confirms the move back.
//
//   Higher MAC address = WHITE (Board A).
//   Lower  MAC address = BLACK (Board B).
//   IO9 = reset / return to discovery at any time.
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
// Active board  → Passive board:  MSG_HIGHLIGHT, MSG_CLEAR, MSG_MOVE
// Passive board → Active board:   MSG_LIFT, MSG_PLACED, MSG_CANCEL
enum MsgType : uint8_t {
  MSG_HELLO      = 0,
  MSG_READY      = 1,
  MSG_SETUP_DONE = 2,
  MSG_LIFT       = 3,  // I detected active-color piece lift on MY board
                       //   d[0]=gRow, d[1]=col
  MSG_HIGHLIGHT  = 4,  // show legal-move hint on YOUR board
                       //   d[0]=gRow, d[1]=col, d[2]=type (0=empty,1=capture)
  MSG_CLEAR      = 5,  // clear all highlights on YOUR board
  MSG_PLACED     = 6,  // active piece placed on MY board (cross-board move)
                       //   d[0]=gRow, d[1]=col
  MSG_CANCEL     = 7,  // cross-board lift cancelled (piece returned to origin)
  MSG_MOVE       = 8,  // full move for state sync
                       //   d[0]=fromGRow, d[1]=fromCol, d[2]=toGRow, d[3]=toCol
  MSG_RESET      = 9
};

struct __attribute__((packed)) Message { MsgType type; uint8_t d[4]; };

uint8_t broadcastAddr[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
uint8_t peerMac[6];
bool    peerFound = false, iAmWhite = false;

volatile bool    msgPending = false;
volatile MsgType msgType;
volatile uint8_t msgData[4];
uint8_t          msgSenderMac[6];

// ── Global 16-row board ───────────────────────────────────────
// Row 0  = White back rank   (Board A local row 0)
// Row 1  = White pawns       (Board A local row 1)
// Rows 2–13 empty
// Row 14 = Black pawns       (Board B local row 6)
// Row 15 = Black back rank   (Board B local row 7)
char gBoard[16][8];
bool sensor[8][8], sensorPrev[8][8];

// Board A (White) physical rows = global rows 0–7  (offset 0)
// Board B (Black) physical rows = global rows 8–15 (offset 8)
int  myOff()       { return iAmWhite ? 0 : 8; }
int  toG(int lr)   { return lr + myOff(); }
int  toLoc(int gr) { return gr - myOff(); }
bool onMe(int gr)  { return gr >= myOff() && gr < myOff() + 8; }

void setGLED(int gr, int c, uint32_t color) {
  if (onMe(gr)) strip.setPixelColor(toLoc(gr) * 8 + c, color);
}

// Highlights cache on the passive board (so we can re-draw after illegal attempts)
struct HLSq { uint8_t gr, col, type; };
HLSq hlList[64];
int  hlCount = 0;

bool isHL(int gr, int c) {
  for (int i = 0; i < hlCount; i++)
    if (hlList[i].gr == (uint8_t)gr && hlList[i].col == (uint8_t)c) return true;
  return false;
}

void initGBoard() {
  for (int r = 0; r < 16; r++) for (int c = 0; c < 8; c++) gBoard[r][c] = ' ';
  const char wb[8] = {'R','N','B','Q','K','B','N','R'};
  const char bb[8] = {'r','n','b','q','k','b','n','r'};
  for (int c = 0; c < 8; c++) { gBoard[0][c]=wb[c]; gBoard[1][c]='P'; }
  for (int c = 0; c < 8; c++) { gBoard[15][c]=bb[c]; gBoard[14][c]='p'; }
}

// ── Hardware ──────────────────────────────────────────────────
void setColumn(int col) {
  digitalWrite(RCLK_PIN, LOW);
  for (int i = 7; i >= 0; i--) {
    digitalWrite(SRCLK_PIN, LOW);
    digitalWrite(SER_PIN, (i == col) ? HIGH : LOW);
    digitalWrite(SRCLK_PIN, HIGH);
  }
  digitalWrite(RCLK_PIN, HIGH);
}

void clearCols() {
  digitalWrite(RCLK_PIN, LOW);
  for (int i = 0; i < 8; i++) {
    digitalWrite(SRCLK_PIN, LOW);
    digitalWrite(SER_PIN, LOW);
    digitalWrite(SRCLK_PIN, HIGH);
  }
  digitalWrite(RCLK_PIN, HIGH);
}

void readSensors() {
  for (int c = 0; c < 8; c++) {
    setColumn(c);
    delayMicroseconds(10);
    for (int r = 0; r < 8; r++)
      sensor[r][c] = (digitalRead(ROW_PINS[r]) == LOW);
  }
  clearCols();
}

// ── Animations ────────────────────────────────────────────────
void searchAnim(int frame) {
  strip.clear();
  int d = frame % 15;
  for (int r = 0; r < 8; r++)
    for (int c = 0; c < 8; c++)
      if ((r+c) == d || (r+c) == d-1)
        strip.setPixelColor(r*8+c, strip.Color(0, 0, 120, 0));
  strip.show();
}

void fireworkAnim() {
  float cx = 3.5f, cy = 3.5f;
  for (float r = 0; r < 6.5f; r += 0.45f) {
    strip.clear();
    for (int row = 0; row < 8; row++)
      for (int col = 0; col < 8; col++) {
        float dx = col - cx, dy = row - cy;
        if (fabsf(sqrtf(dx*dx+dy*dy) - r) < 0.55f)
          strip.setPixelColor(row*8+col, iAmWhite ? strip.Color(0,0,0,255) : strip.Color(0,180,255,0));
      }
    strip.show(); delay(45);
  }
  for (int f = 0; f < 3; f++) {
    uint32_t c = iAmWhite ? strip.Color(0,0,0,255) : strip.Color(0,180,255,0);
    for (int i = 0; i < LED_COUNT; i++) strip.setPixelColor(i, c);
    strip.show(); delay(180); strip.clear(); strip.show(); delay(180);
  }
  strip.clear(); strip.show();
}

void illegalFlash(int gr, int c) {
  if (!onMe(gr)) return;
  for (int f = 0; f < 3; f++) {
    setGLED(gr, c, strip.Color(200,150,0,0)); strip.show(); delay(150);
    setGLED(gr, c, 0);                         strip.show(); delay(100);
  }
}

void captureAnim(int gr, int c) {
  if (!onMe(gr)) return;
  float cx = c, cy = (float)toLoc(gr);
  for (float r = 0; r < 6.0f; r += 0.8f) {
    strip.clear();
    for (int rr = 0; rr < 8; rr++)
      for (int cc = 0; cc < 8; cc++) {
        float dx = cc-cx, dy = rr-cy;
        if (fabsf(sqrtf(dx*dx+dy*dy) - r) < 0.7f)
          strip.setPixelColor(rr*8+cc, strip.Color(255,0,0,0));
      }
    strip.show(); delay(60);
  }
  strip.clear(); strip.show();
}

void promotionAnim(int gr, int c) {
  if (!onMe(gr)) return;
  int lc = toLoc(gr);
  for (int f = 0; f < 5; f++) {
    strip.setPixelColor(lc*8+c, strip.Color(255,200,0,100)); strip.show(); delay(120);
    strip.setPixelColor(lc*8+c, 0);                          strip.show(); delay(80);
  }
}

void checkFlash() {
  for (int f = 0; f < 3; f++) {
    for (int i = 0; i < LED_COUNT; i++) strip.setPixelColor(i, strip.Color(255,0,0,0));
    strip.show(); delay(150); strip.clear(); strip.show(); delay(120);
  }
}

void checkmateFlash() {
  for (int f = 0; f < 8; f++) {
    for (int i = 0; i < LED_COUNT; i++) strip.setPixelColor(i, strip.Color(255,0,0,0));
    strip.show(); delay(200); strip.clear(); strip.show(); delay(180);
  }
}

void gameOverAnim(bool iWon) {
  char myCol = iAmWhite ? 'w' : 'b';
  for (int f = 0; f < 10; f++) {
    for (int lr = 0; lr < 8; lr++)
      for (int c = 0; c < 8; c++) {
        int gr = toG(lr); char p = gBoard[gr][c];
        if (p == ' ') { strip.setPixelColor(lr*8+c, 0); continue; }
        bool mine = (myCol=='w') ? (p>='A'&&p<='Z') : (p>='a'&&p<='z');
        strip.setPixelColor(lr*8+c, (f%2==0) ? (mine ? strip.Color(0,255,0,0) : strip.Color(255,0,0,0)) : 0);
      }
    strip.show(); delay(350);
  }
  strip.clear(); strip.show();
}

// ── Chess logic — 16-row board ────────────────────────────────
bool isAttacked(int r, int c, char byColor) {
  int pd = (byColor=='w') ? 1 : -1;
  char eP = (byColor=='w') ? 'P' : 'p';
  int pdc[2] = {-1, 1};
  for (int i = 0; i < 2; i++) {
    int pr = r-pd, pc = c+pdc[i];
    if (pr>=0&&pr<16&&pc>=0&&pc<8&&gBoard[pr][pc]==eP) return true;
  }
  char eN=(byColor=='w')?'N':'n';
  int kd[8][2]={{2,1},{1,2},{-1,2},{-2,1},{-2,-1},{-1,-2},{1,-2},{2,-1}};
  for (int i=0;i<8;i++){
    int nr=r+kd[i][0], nc=c+kd[i][1];
    if (nr>=0&&nr<16&&nc>=0&&nc<8&&gBoard[nr][nc]==eN) return true;
  }
  char eK=(byColor=='w')?'K':'k';
  int kd2[8][2]={{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
  for (int i=0;i<8;i++){
    int nr=r+kd2[i][0], nc=c+kd2[i][1];
    if (nr>=0&&nr<16&&nc>=0&&nc<8&&gBoard[nr][nc]==eK) return true;
  }
  char eR=(byColor=='w')?'R':'r', eQ=(byColor=='w')?'Q':'q', eB=(byColor=='w')?'B':'b';
  // Straight rays for rook/queen (no reflection)
  int dirs[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
  for (int i=0;i<4;i++){
    for (int s=1;s<16;s++){int nr=r+s*dirs[i][0],nc=c+s*dirs[i][1];if(nr<0||nr>=16||nc<0||nc>=8)break;char t=gBoard[nr][nc];if(t!=' '){if(t==eR||t==eQ)return true;break;}}
  }
  // Reflecting diagonal rays for bishop/queen
  int diagDr[4]={1,1,-1,-1}, diagDc[4]={1,-1,1,-1};
  for (int i=0;i<4;i++){
    int nr=r, nc=c, dc=diagDc[i];
    for (int s=0;s<16;s++){
      nr+=diagDr[i]; nc+=dc;
      if(nc<0){nc=-nc;dc=-dc;}
      if(nc>7){nc=14-nc;dc=-dc;}
      if(nr<0||nr>=16)break;
      char t=gBoard[nr][nc];
      if(t!=' '){if(t==eB||t==eQ)return true;break;}
    }
  }
  return false;
}

bool inCheck(char color) {
  char king=(color=='w')?'K':'k', enemy=(color=='w')?'b':'w';
  for (int r=0;r<16;r++) for (int c=0;c<8;c++) if(gBoard[r][c]==king) return isAttacked(r,c,enemy);
  return false;
}

void getMoves(int row, int col, int &mc, int mv[][2]) {
  mc = 0;
  char piece = gBoard[row][col]; if (piece == ' ') return;
  char clr = (piece>='a'&&piece<='z') ? 'b' : 'w';
  char pt  = (piece>='a'&&piece<='z') ? piece-32 : piece;

  auto canLand = [&](int r, int c) -> bool {
    if (r<0||r>=16||c<0||c>=8) return false;
    char t = gBoard[r][c];
    return t==' ' || ((t>='a'&&t<='z') != (clr=='b'));
  };
  auto push = [&](int r, int c) {
    if (canLand(r,c)) { mv[mc][0]=r; mv[mc][1]=c; mc++; }
  };
  auto line = [&](int dr, int dc) {
    for (int s=1;s<16;s++){
      int nr=row+s*dr, nc=col+s*dc;
      if (nr<0||nr>=16||nc<0||nc>=8) break;
      char t=gBoard[nr][nc];
      if (t==' ')  { mv[mc][0]=nr; mv[mc][1]=nc; mc++; }
      else { if ((t>='a'&&t<='z')!=(clr=='b')){ mv[mc][0]=nr; mv[mc][1]=nc; mc++; } break; }
    }
  };

  // Diagonal line that reflects off the left (col 0) and right (col 7) walls.
  // The row direction stays constant; the column direction flips on each bounce.
  // The ray stops only when it hits the top/bottom boundary or a blocking piece.
  auto reflLine = [&](int dr, int initDc) {
    int nr=row, nc=col, dc=initDc;
    for (int s=0; s<16; s++) {
      nr += dr;
      nc += dc;
      if (nc < 0)  { nc = -nc;      dc = -dc; }  // bounce off left wall
      if (nc > 7)  { nc = 14 - nc;  dc = -dc; }  // bounce off right wall
      if (nr < 0 || nr >= 16) break;
      char t=gBoard[nr][nc];
      if (t==' ')  { mv[mc][0]=nr; mv[mc][1]=nc; mc++; }
      else { if ((t>='a'&&t<='z')!=(clr=='b')){ mv[mc][0]=nr; mv[mc][1]=nc; mc++; } break; }
    }
  };

  switch (pt) {
    case 'P': {
      // White pawns move toward row 15; Black toward row 0
      int dir=(clr=='w')?1:-1, start=(clr=='w')?1:14;
      if (row+dir>=0&&row+dir<16&&gBoard[row+dir][col]==' ') {
        mv[mc][0]=row+dir; mv[mc][1]=col; mc++;
        if (row==start&&gBoard[row+2*dir][col]==' ') { mv[mc][0]=row+2*dir; mv[mc][1]=col; mc++; }
      }
      int capDc[2]={-1,1};
      for (int i=0;i<2;i++){
        int nr=row+dir, nc=col+capDc[i];
        if (nr>=0&&nr<16&&nc>=0&&nc<8) {
          char t=gBoard[nr][nc];
          if (t!=' '&&(t>='a'&&t<='z')!=(clr=='b')) { mv[mc][0]=nr; mv[mc][1]=nc; mc++; }
        }
      }
      break;
    }
    case 'R': line(1,0);line(-1,0);line(0,1);line(0,-1); break;
    case 'B': reflLine(1,1);reflLine(1,-1);reflLine(-1,1);reflLine(-1,-1); break;
    case 'Q': line(1,0);line(-1,0);line(0,1);line(0,-1);
              reflLine(1,1);reflLine(1,-1);reflLine(-1,1);reflLine(-1,-1); break;
    case 'N': { int nd[8][2]={{2,1},{1,2},{-1,2},{-2,1},{-2,-1},{-1,-2},{1,-2},{2,-1}};for(int i=0;i<8;i++)push(row+nd[i][0],col+nd[i][1]); break; }
    case 'K': { int kd[8][2]={{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};for(int i=0;i<8;i++)push(row+kd[i][0],col+kd[i][1]); break; }
  }

  // Filter moves that leave own king in check
  int legal[64][2]; int lc = 0;
  for (int i = 0; i < mc; i++) {
    int tr=mv[i][0], tc=mv[i][1];
    char save=gBoard[tr][tc];
    gBoard[tr][tc]=piece; gBoard[row][col]=' ';
    if (!inCheck(clr)) { legal[lc][0]=tr; legal[lc][1]=tc; lc++; }
    gBoard[row][col]=piece; gBoard[tr][tc]=save;
  }
  mc = lc;
  for (int i=0;i<lc;i++) { mv[i][0]=legal[i][0]; mv[i][1]=legal[i][1]; }
}

bool hasLegal(char color) {
  for (int r=0;r<16;r++) for (int c=0;c<8;c++) {
    char p=gBoard[r][c]; if(p==' ')continue;
    bool mine=(color=='w')?(p>='A'&&p<='Z'):(p>='a'&&p<='z');
    if (!mine) continue;
    int mc=0; int mv[64][2];
    getMoves(r,c,mc,mv);
    if (mc>0) return true;
  }
  return false;
}

bool inLegal(int gr, int c, int mc, int mv[][2]) {
  for (int i=0;i<mc;i++) if(mv[i][0]==gr&&mv[i][1]==c) return true;
  return false;
}

void applyMoveToBoard(int fr, int fc, int tr, int tc) {
  if (gBoard[tr][tc]!=' ' && onMe(tr)) captureAnim(tr, tc);
  gBoard[tr][tc] = gBoard[fr][fc];
  gBoard[fr][fc] = ' ';
  if (gBoard[tr][tc]=='P' && tr==15) { promotionAnim(tr,tc); gBoard[tr][tc]='Q'; }
  if (gBoard[tr][tc]=='p' && tr==0)  { promotionAnim(tr,tc); gBoard[tr][tc]='q'; }
}

// ── ESP-NOW ───────────────────────────────────────────────────
void sendMsg(MsgType type,uint8_t d0=0,uint8_t d1=0,uint8_t d2=0,uint8_t d3=0) {
  Message msg; msg.type=type; msg.d[0]=d0; msg.d[1]=d1; msg.d[2]=d2; msg.d[3]=d3;
  esp_now_send(peerFound ? peerMac : broadcastAddr, (uint8_t*)&msg, sizeof(msg));
}

void onRecv(const esp_now_recv_info *ri, const uint8_t *data, int len) {
  if (len<1) return;
  const Message *m = (const Message*)data;
  msgType=m->type; msgData[0]=m->d[0]; msgData[1]=m->d[1]; msgData[2]=m->d[2]; msgData[3]=m->d[3];
  memcpy(msgSenderMac, ri->src_addr, 6);
  msgPending = true;
}
void onSent(const wifi_tx_info_t*, esp_now_send_status_t) {}

bool addPeer(uint8_t *mac) {
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t p = {}; memcpy(p.peer_addr,mac,6); p.channel=0; p.encrypt=false;
  return esp_now_add_peer(&p) == ESP_OK;
}
bool macGt(uint8_t *a, uint8_t *b) {
  for (int i=0;i<6;i++) { if(a[i]>b[i])return true; if(a[i]<b[i])return false; } return false;
}

// ── Discovery ─────────────────────────────────────────────────
bool discoveryLoop() {
  peerFound=false; msgPending=false;
  uint8_t myMac[6]; WiFi.macAddress(myMac);
  bool running=false; unsigned long lastB=0,lastR=0; int af=0; unsigned long at=0;

  while (!running) {
    if (digitalRead(RESET_PIN)==LOW){delay(50);if(digitalRead(RESET_PIN)==LOW)return false;}
    if (millis()-lastB>600)                          { sendMsg(MSG_HELLO); lastB=millis(); }
    if (peerFound&&!running&&millis()-lastR>700)     { sendMsg(MSG_READY); lastR=millis(); }
    if (millis()-at>90)                              { searchAnim(af++); at=millis(); }

    if (msgPending) {
      msgPending=false;
      if (msgType==MSG_HELLO&&!peerFound) {
        memcpy(peerMac,msgSenderMac,6); peerFound=true; addPeer(peerMac);
        iAmWhite=macGt(myMac,peerMac); sendMsg(MSG_READY); lastR=millis();
      } else if (msgType==MSG_HELLO&&peerFound) {
        sendMsg(MSG_READY); lastR=millis();
      } else if (msgType==MSG_READY) {
        if (!peerFound) {
          memcpy(peerMac,msgSenderMac,6); peerFound=true; addPeer(peerMac);
          iAmWhite=macGt(myMac,peerMac); sendMsg(MSG_READY); lastR=millis();
        }
        if (peerFound) running=true;
      }
    }
    delay(10);
  }
  strip.clear(); strip.show(); delay(300);
  fireworkAnim();
  return true;
}

// ── Setup — wait for home pieces, sync with peer ──────────────
bool setupPhase() {
  // White: needs pieces on local rows 0–1 (global 0–1)
  // Black: needs pieces on local rows 6–7 (global 14–15)
  while (true) {
    if (digitalRead(RESET_PIN)==LOW){delay(50);if(digitalRead(RESET_PIN)==LOW){strip.clear();strip.show();return false;}}
    readSensors();
    bool ok = true;
    for (int lr=0;lr<8;lr++) for (int c=0;c<8;c++) {
      int gr=toG(lr);
      bool need = (gBoard[gr][c]!=' ') && !sensor[lr][c];
      if (need) ok=false;
      strip.setPixelColor(lr*8+c, need ? strip.Color(0,0,0,80) : 0);
    }
    strip.show();
    if (ok) break;
    delay(200);
  }
  for (int f=0;f<2;f++) {
    for (int i=0;i<LED_COUNT;i++) strip.setPixelColor(i,strip.Color(0,150,0,0));
    strip.show(); delay(200); strip.clear(); strip.show(); delay(200);
  }

  bool peerOk=false; unsigned long lastS=0;
  while (!peerOk) {
    if (digitalRead(RESET_PIN)==LOW){delay(50);if(digitalRead(RESET_PIN)==LOW){strip.clear();strip.show();return false;}}
    if (millis()-lastS>500) { sendMsg(MSG_SETUP_DONE); lastS=millis(); }
    uint8_t pulse = (uint8_t)(80 + 80*sinf(millis()*0.005f));
    strip.clear();
    int cx[4]={3*8+3,3*8+4,4*8+3,4*8+4};
    for (int i=0;i<4;i++) strip.setPixelColor(cx[i], strip.Color(0,0,pulse,0));
    strip.show();
    if (msgPending) {
      msgPending=false;
      if (msgType==MSG_SETUP_DONE) peerOk=true;
      else if (msgType==MSG_RESET) { strip.clear(); strip.show(); return false; }
    }
    delay(20);
  }
  sendMsg(MSG_SETUP_DONE);
  for (int f=0;f<2;f++) {
    for (int i=0;i<LED_COUNT;i++) strip.setPixelColor(i,strip.Color(0,0,0,150));
    strip.show(); delay(200); strip.clear(); strip.show(); delay(200);
  }
  readSensors(); memcpy(sensorPrev,sensor,sizeof(sensor));
  return true;
}

// ── Active turn helpers ───────────────────────────────────────
void glowMyPieces() {
  char myCol = iAmWhite ? 'w' : 'b';
  strip.clear();
  for (int lr=0;lr<8;lr++) for (int c=0;c<8;c++) {
    int gr=toG(lr); char p=gBoard[gr][c]; if(p==' ')continue;
    bool mine=(myCol=='w')?(p>='A'&&p<='Z'):(p>='a'&&p<='z');
    if (mine) strip.setPixelColor(lr*8+c, strip.Color(0,0,0,40));
  }
  strip.show();
}

// Show legal-move hints with a distance-sorted ripple across both boards.
// Local squares light up with a 30 ms delay; remote ones are sent via
// MSG_HIGHLIGHT at the same cadence so the passive board mirrors the ripple.
void showHints(int liftGR, int liftGC, bool liftOnMe, int mc, int mv[][2]) {
  // Bubble-sort by global distance from the lifted square
  for (int i=0;i<mc-1;i++) for (int j=0;j<mc-i-1;j++) {
    int dr0=mv[j][0]-liftGR, dc0=mv[j][1]-liftGC;
    int dr1=mv[j+1][0]-liftGR, dc1=mv[j+1][1]-liftGC;
    if (dr0*dr0+dc0*dc0 > dr1*dr1+dc1*dc1) {
      int t0=mv[j][0],t1=mv[j][1];
      mv[j][0]=mv[j+1][0]; mv[j][1]=mv[j+1][1];
      mv[j+1][0]=t0; mv[j+1][1]=t1;
    }
  }
  strip.clear();
  if (liftOnMe) setGLED(liftGR, liftGC, strip.Color(0,0,0,255));

  for (int i = 0; i < mc; i++) {
    int gr2=mv[i][0], c2=mv[i][1];
    bool cap = (gBoard[gr2][c2] != ' ');
    uint32_t color = cap ? strip.Color(255,60,0,0) : strip.Color(0,0,0,180);
    if (onMe(gr2)) {
      setGLED(gr2, c2, color);
      strip.show();
      delay(30);
    } else {
      strip.show();
      sendMsg(MSG_HIGHLIGHT, (uint8_t)gr2, (uint8_t)c2, (uint8_t)(cap?1:0));
      delay(30); // same cadence keeps ripple in sync on passive board
    }
  }
  strip.show();
}

void restoreHintsLocal(int liftGR, int liftGC, bool liftOnMe, int mc, int mv[][2]) {
  strip.clear();
  if (liftOnMe) setGLED(liftGR, liftGC, strip.Color(0,0,0,255));
  for (int i=0;i<mc;i++) {
    int gr2=mv[i][0], c2=mv[i][1];
    if (onMe(gr2)) {
      bool cap=(gBoard[gr2][c2]!=' ');
      setGLED(gr2, c2, cap ? strip.Color(255,60,0,0) : strip.Color(0,0,0,180));
    }
  }
  strip.show();
}

// ═══════════════════════════════════════════════════════════════
// ACTIVE TURN — the controller for my color's move
//   Phase 1: wait for a lift of my piece (on my board or reported
//            via MSG_LIFT if the piece is on the other board).
//   Phase 2: compute legal moves, show hints on both boards.
//   Phase 3: wait for placement — local sensor OR MSG_PLACED.
// ═══════════════════════════════════════════════════════════════
bool handleActiveTurn() {
  char myCol = iAmWhite ? 'w' : 'b';
  glowMyPieces();

  int  liftGR=-1, liftGC=-1;
  bool liftOnMe=false;
  int  mc=0; int mv[64][2];
  bool hintsShown=false;

  while (true) {
    if (digitalRead(RESET_PIN)==LOW){delay(50);if(digitalRead(RESET_PIN)==LOW){sendMsg(MSG_RESET);strip.clear();strip.show();return false;}}
    readSensors();

    // ── Phase 1: look for a lift ───────────────────────────
    if (liftGR == -1) {
      for (int lr=0;lr<8;lr++) for (int c=0;c<8;c++) {
        if (!sensorPrev[lr][c] || sensor[lr][c]) continue;
        int gr=toG(lr); char p=gBoard[gr][c]; if(p==' ')continue;
        bool mine=(myCol=='w')?(p>='A'&&p<='Z'):(p>='a'&&p<='z');
        if (mine) { liftGR=gr; liftGC=c; liftOnMe=true; break; }
      }
      // Other board detected my piece (it had crossed there)
      if (!liftOnMe && msgPending) {
        msgPending=false;
        if      (msgType==MSG_LIFT)  { liftGR=(int)msgData[0]; liftGC=(int)msgData[1]; liftOnMe=false; }
        else if (msgType==MSG_RESET) { strip.clear(); strip.show(); return false; }
      }
    }

    // ── Phase 2: process lift once ──────────────────────────
    if (liftGR != -1 && !hintsShown) {
      getMoves(liftGR, liftGC, mc, mv);

      if (mc == 0) {
        // Pinned or stuck — flash and wait for return
        illegalFlash(liftGR, liftGC);
        if (liftOnMe) {
          while (!sensor[toLoc(liftGR)][liftGC]) {
            if (digitalRead(RESET_PIN)==LOW){delay(50);if(digitalRead(RESET_PIN)==LOW){sendMsg(MSG_RESET);strip.clear();strip.show();return false;}}
            readSensors(); memcpy(sensorPrev,sensor,sizeof(sensor)); delay(40);
          }
        } else {
          while (true) {
            if (digitalRead(RESET_PIN)==LOW){delay(50);if(digitalRead(RESET_PIN)==LOW){sendMsg(MSG_RESET);strip.clear();strip.show();return false;}}
            if (msgPending){ msgPending=false; if(msgType==MSG_CANCEL)break; if(msgType==MSG_RESET)return false; }
            delay(20);
          }
        }
        liftGR=-1; liftGC=-1; mc=0; liftOnMe=false;
        glowMyPieces();
        memcpy(sensorPrev,sensor,sizeof(sensor));
        continue;
      }

      showHints(liftGR, liftGC, liftOnMe, mc, mv);
      hintsShown=true;
    }

    // ── Phase 3: wait for placement ─────────────────────────
    if (liftGR != -1 && hintsShown) {

      // Piece returned to origin (local lift only)
      if (liftOnMe && sensor[toLoc(liftGR)][liftGC] && !sensorPrev[toLoc(liftGR)][liftGC]) {
        sendMsg(MSG_CLEAR); strip.clear(); strip.show();
        liftGR=-1; liftGC=-1; mc=0; hintsShown=false;
        glowMyPieces();
        memcpy(sensorPrev,sensor,sizeof(sensor));
        continue;
      }

      // Local placement
      for (int lr2=0;lr2<8;lr2++) for (int c2=0;c2<8;c2++) {
        if (!sensor[lr2][c2] || sensorPrev[lr2][c2]) continue;
        int gr2=toG(lr2);
        if (inLegal(gr2,c2,mc,mv)) {
          applyMoveToBoard(liftGR,liftGC,gr2,c2);
          sendMsg(MSG_CLEAR);
          strip.clear();
          if (onMe(liftGR)) setGLED(liftGR, liftGC, strip.Color(0,80,0,0));
          setGLED(gr2, c2, strip.Color(0,255,0,0));
          strip.show(); delay(500); strip.clear(); strip.show();
          sendMsg(MSG_MOVE,(uint8_t)liftGR,(uint8_t)liftGC,(uint8_t)gr2,(uint8_t)c2);
          readSensors(); memcpy(sensorPrev,sensor,sizeof(sensor));
          return true;
        } else {
          illegalFlash(gr2, c2);
          restoreHintsLocal(liftGR,liftGC,liftOnMe,mc,mv);
        }
      }

      // Cross-board placement reported by passive board
      if (msgPending) {
        msgPending=false;
        if (msgType==MSG_PLACED) {
          int gr2=(int)msgData[0], c2=(int)msgData[1];
          if (inLegal(gr2,c2,mc,mv)) {
            applyMoveToBoard(liftGR,liftGC,gr2,c2);
            strip.clear();
            if (onMe(liftGR)) { setGLED(liftGR,liftGC,strip.Color(0,255,0,0)); strip.show(); delay(400); strip.clear(); strip.show(); }
            sendMsg(MSG_MOVE,(uint8_t)liftGR,(uint8_t)liftGC,(uint8_t)gr2,(uint8_t)c2);
            readSensors(); memcpy(sensorPrev,sensor,sizeof(sensor));
            return true;
          }
        } else if (msgType==MSG_CANCEL && !liftOnMe) {
          // Remote piece returned to origin
          sendMsg(MSG_CLEAR); strip.clear(); strip.show();
          liftGR=-1; liftGC=-1; mc=0; hintsShown=false;
          glowMyPieces();
          memcpy(sensorPrev,sensor,sizeof(sensor));
          continue;
        } else if (msgType==MSG_RESET) {
          strip.clear(); strip.show(); return false;
        }
      }
    }

    memcpy(sensorPrev,sensor,sizeof(sensor));
    delay(40);
  }
}

// ═══════════════════════════════════════════════════════════════
// PASSIVE TURN — support the other player's move
//   • Show MSG_HIGHLIGHT squares as they arrive (building the ripple).
//   • If active player has a piece on MY physical rows, detect its
//     lift and report MSG_LIFT to the active board.
//   • If a piece lands on a highlighted square, report MSG_PLACED.
//   • End when MSG_MOVE arrives and is applied.
// ═══════════════════════════════════════════════════════════════
bool handlePassiveTurn() {
  char activeCol = iAmWhite ? 'b' : 'w'; // the other player's color
  hlCount=0; strip.clear(); strip.show();

  int liftGR=-1, liftGC=-1, liftLR=-1; // cross-board lift I detected

  while (true) {
    if (digitalRead(RESET_PIN)==LOW){delay(50);if(digitalRead(RESET_PIN)==LOW){sendMsg(MSG_RESET);strip.clear();strip.show();return false;}}
    readSensors();

    // ── Incoming messages ───────────────────────────────────
    if (msgPending) {
      msgPending=false;
      switch (msgType) {
        case MSG_HIGHLIGHT:
          if (onMe((int)msgData[0])) {
            bool cap=(msgData[2]==1);
            setGLED((int)msgData[0],(int)msgData[1], cap ? strip.Color(255,60,0,0) : strip.Color(0,0,0,180));
            strip.show();
          }
          if (hlCount<64) { hlList[hlCount].gr=msgData[0]; hlList[hlCount].col=msgData[1]; hlList[hlCount].type=msgData[2]; hlCount++; }
          break;

        case MSG_CLEAR:
          hlCount=0; liftGR=-1; liftGC=-1; liftLR=-1;
          strip.clear(); strip.show();
          break;

        case MSG_MOVE:
          applyMoveToBoard((int)msgData[0],(int)msgData[1],(int)msgData[2],(int)msgData[3]);
          hlCount=0; strip.clear(); strip.show();
          { char myCol2=iAmWhite?'w':'b'; if(inCheck(myCol2)) checkFlash(); }
          readSensors(); memcpy(sensorPrev,sensor,sizeof(sensor));
          return true;

        case MSG_RESET:
          strip.clear(); strip.show(); return false;

        default: break;
      }
    }

    // ── Detect active player's piece lift on MY board ───────
    if (liftGR == -1) {
      for (int lr=0;lr<8;lr++) for (int c=0;c<8;c++) {
        if (!sensorPrev[lr][c] || sensor[lr][c]) continue;
        int gr=toG(lr); char p=gBoard[gr][c]; if(p==' ')continue;
        bool isActive=(activeCol=='w')?(p>='A'&&p<='Z'):(p>='a'&&p<='z');
        if (isActive) { liftGR=gr; liftGC=c; liftLR=lr; sendMsg(MSG_LIFT,(uint8_t)gr,(uint8_t)c); break; }
      }
    }

    // ── Cross-board lift on MY board: watch for return to origin ──
    // (active player previously crossed a piece here and is now moving it again)
    if (liftGR != -1) {
      if (sensor[liftLR][liftGC] && !sensorPrev[liftLR][liftGC]) {
        sendMsg(MSG_CANCEL);
        hlCount=0; strip.clear(); strip.show();
        liftGR=-1; liftGC=-1; liftLR=-1;
        memcpy(sensorPrev,sensor,sizeof(sensor));
        continue;
      }
    }

    // ── Watch for placement on any highlighted square ───────────
    // Runs whenever highlights exist — covers both:
    //   (a) active player lifted from their own board and crosses here, and
    //   (b) active player lifted a piece already on my board.
    if (hlCount > 0) {
      for (int lr2=0;lr2<8;lr2++) for (int c2=0;c2<8;c2++) {
        if (!sensor[lr2][c2] || sensorPrev[lr2][c2]) continue;
        int gr2=toG(lr2);
        if (isHL(gr2,c2)) {
          setGLED(gr2, c2, strip.Color(0,255,0,0)); strip.show();
          sendMsg(MSG_PLACED,(uint8_t)gr2,(uint8_t)c2);
          // Wait for MSG_MOVE confirmation
          while (true) {
            if (digitalRead(RESET_PIN)==LOW){delay(50);if(digitalRead(RESET_PIN)==LOW){sendMsg(MSG_RESET);strip.clear();strip.show();return false;}}
            if (msgPending) {
              msgPending=false;
              if (msgType==MSG_MOVE) {
                applyMoveToBoard((int)msgData[0],(int)msgData[1],(int)msgData[2],(int)msgData[3]);
                hlCount=0; strip.clear(); strip.show();
                { char myCol2=iAmWhite?'w':'b'; if(inCheck(myCol2)) checkFlash(); }
                readSensors(); memcpy(sensorPrev,sensor,sizeof(sensor));
                return true;
              }
              if (msgType==MSG_RESET) { strip.clear(); strip.show(); return false; }
            }
            delay(20);
          }
        } else {
          // Illegal square — flash amber, restore highlights
          illegalFlash(gr2, c2);
          strip.clear();
          for (int i=0;i<hlCount;i++)
            if (onMe(hlList[i].gr))
              setGLED(hlList[i].gr, hlList[i].col, hlList[i].type==1 ? strip.Color(255,60,0,0) : strip.Color(0,0,0,180));
          strip.show();
        }
      }
    }

    memcpy(sensorPrev,sensor,sizeof(sensor));
    delay(20); // short delay to catch highlight messages during the ripple
  }
}

// ── Arduino setup ─────────────────────────────────────────────
void setup() {
  Serial.begin(115200); delay(1000);
  Serial.println("\n==============================");
  Serial.println("  Long Chess — 8×16 Board");
  Serial.println("==============================");

  pinMode(SER_PIN, OUTPUT); pinMode(RCLK_PIN, OUTPUT); pinMode(SRCLK_PIN, OUTPUT);
  for (int i=0;i<8;i++) pinMode(ROW_PINS[i], INPUT_PULLUP);
  pinMode(RESET_PIN, INPUT_PULLUP);

  strip.begin(); strip.setBrightness(BRIGHTNESS); strip.clear(); strip.show();

  WiFi.mode(WIFI_STA); WiFi.disconnect(); delay(100);

  if (esp_now_init() != ESP_OK) {
    while (true) {
      for (int i=0;i<LED_COUNT;i++) strip.setPixelColor(i,strip.Color(255,0,0,0));
      strip.show(); delay(500); strip.clear(); strip.show(); delay(500);
    }
  }
  esp_now_register_send_cb(onSent);
  esp_now_register_recv_cb(onRecv);
  esp_now_peer_info_t bcast = {};
  memcpy(bcast.peer_addr, broadcastAddr, 6); bcast.channel=0; bcast.encrypt=false;
  esp_now_add_peer(&bcast);
}

// ── Main loop ─────────────────────────────────────────────────
void loop() {
  initGBoard();

  if (!discoveryLoop()) { strip.clear(); strip.show(); return; }
  if (!setupPhase())    { strip.clear(); strip.show(); return; }

  bool whiteTurn = true;

  while (true) {
    if (digitalRead(RESET_PIN)==LOW){delay(50);if(digitalRead(RESET_PIN)==LOW){sendMsg(MSG_RESET);strip.clear();strip.show();return;}}

    bool isMyTurn = (whiteTurn == iAmWhite);
    bool ok = isMyTurn ? handleActiveTurn() : handlePassiveTurn();
    if (!ok) { strip.clear(); strip.show(); return; }

    whiteTurn = !whiteTurn;

    char nextCol = whiteTurn ? 'w' : 'b';
    if (!hasLegal(nextCol)) {
      bool check    = inCheck(nextCol);
      bool nextIsMe = (whiteTurn == iAmWhite);
      if (check) checkmateFlash();
      gameOverAnim(check ? !nextIsMe : false);
      delay(4000);
      return;
    }
    // Flash check warning on the board of the player who is now in check
    if (inCheck(nextCol) && (whiteTurn == iAmWhite)) checkFlash();
  }
}
