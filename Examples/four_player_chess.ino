// ============================================================
// four_player_chess.ino  —  4-Player Chess (5 ESP-NOW boards)
//
//   Cross (+) formation:
//             [P1]  top    — White pieces
//   [P2]  [CENTER]  [P3]
//   left             right  — Green / Blue
//             [P4]  bottom — Red pieces
//
//   Global coordinate space  (24 × 24, + shaped):
//     P1 arm  : rows  0– 7, cols  8–15
//     P2 arm  : rows  8–15, cols  0– 7
//     CENTER  : rows  8–15, cols  8–15
//     P3 arm  : rows  8–15, cols 16–23
//     P4 arm  : rows 16–23, cols  8–15
//
//   Role assignment — automatic by MAC sort (ascending):
//     index 0 = P1  index 1 = P2  index 2 = P3
//     index 3 = P4  index 4 = CENTER
//
//   All player boards: pieces always start on local rows 0–1
//   (back rank row 0, pawns row 1). Local row 7 faces center.
//   Center board shows colored edges throughout the game:
//     top = P1 (White)  left = P2 (Green)
//     right = P3 (Blue) bottom = P4 (Red)
//
//   No castling / no en passant. Pawns auto-promote to queen.
//   Player eliminated when their king is captured.
//   IO9 = reset.  Any board reboot triggers auto-reconnect.
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

// ── Piece encoding ────────────────────────────────────────────
// uint8_t cell = (player << 4) | type,  0 = empty
// player: 1=P1 2=P2 3=P3 4=P4   type: 1-6 below
#define PAWN   1
#define KNIGHT 2
#define BISHOP 3
#define ROOK   4
#define QUEEN  5
#define KING   6

inline uint8_t mkPiece(int pl, int ty)  { return (uint8_t)((pl<<4)|ty); }
inline int     plOf(uint8_t p)          { return p >> 4; }
inline int     tyOf(uint8_t p)          { return p & 0xF; }
inline bool    isEm(uint8_t p)          { return p == 0; }

// ── Roles ─────────────────────────────────────────────────────
// 1=P1(top/W)  2=P2(left/G)  3=P3(right/B)  4=P4(bot/R)  5=CENTER
uint8_t myRole = 0;

// ── Coordinate mapping (supports board rotation per role) ─────
//
// Every player board uses the same physical orientation:
//   local row 0 = back rank (far from center)
//   local row 7 = near-center edge
//   local col 0-7 = left-to-right from the player's perspective
//
// Global ← local:
//   P1: gr=lr,      gc=lc+8
//   P2: gr=lc+8,    gc=lr          (board rotated 90° CW in global space)
//   P3: gr=lc+8,    gc=23-lr       (board rotated 90° CCW)
//   P4: gr=23-lr,   gc=lc+8        (board flipped, near-center = global row 16)
//   CENTER: gr=lr+8, gc=lc+8
int gRow(int lr, int lc) {
  switch(myRole) {
    case 1: return lr;
    case 2: return lc + 8;
    case 3: return lc + 8;
    case 4: return 23 - lr;
    case 5: return lr + 8;
  }
  return -1;
}
int gCol(int lr, int lc) {
  switch(myRole) {
    case 1: return lc + 8;
    case 2: return lr;
    case 3: return 23 - lr;
    case 4: return lc + 8;
    case 5: return lc + 8;
  }
  return -1;
}
// Local ← global (inverse of above)
int locR(int gr, int gc) {
  switch(myRole) {
    case 1: return gr;
    case 2: return gc;
    case 3: return 23 - gc;
    case 4: return 23 - gr;
    case 5: return gr - 8;
  }
  return -1;
}
int locC(int gr, int gc) {
  switch(myRole) {
    case 1: return gc - 8;
    case 2: return gr - 8;
    case 3: return gr - 8;
    case 4: return gc - 8;
    case 5: return gc - 8;
  }
  return -1;
}
bool onMe(int gr, int gc) {
  int lr=locR(gr,gc), lc=locC(gr,gc);
  return lr>=0&&lr<8&&lc>=0&&lc<8;
}

bool validSq(int r, int c) {
  if (r<0||r>=24||c<0||c>=24) return false;
  return (r>=8&&r<=15)||(c>=8&&c<=15);
}

// Which board owns this global square? (0 = invalid)
int squareRole(int gr, int gc) {
  if (!validSq(gr,gc)) return 0;
  if (gr <= 7)  return 1;   // P1 arm
  if (gc <= 7)  return 2;   // P2 arm
  if (gc >= 16) return 3;   // P3 arm
  if (gr >= 16) return 4;   // P4 arm
  return 5;                  // CENTER
}

// Pawn movement direction (dr, dc) in GLOBAL coords, indexed by player 1-4
const int PDR[5] = {0,  1,  0,  0, -1};
const int PDC[5] = {0,  0,  1, -1,  0};

// ── Global game state ─────────────────────────────────────────
uint8_t gBoard[24][24];
bool    sensor[8][8], sensorPrev[8][8];
bool    eliminated[5] = {};

void setGLED(int gr, int gc, uint32_t c) {
  if (onMe(gr,gc)) strip.setPixelColor(locR(gr,gc)*8+locC(gr,gc), c);
}

uint32_t pColor(int pl) {
  switch(pl) {
    case 1: return strip.Color(0,0,0,200);  // White
    case 2: return strip.Color(0,200,0,0);  // Green
    case 3: return strip.Color(0,0,200,0);  // Blue
    case 4: return strip.Color(200,0,0,0);  // Red
  }
  return 0;
}
uint32_t dimColor(int pl) {
  switch(pl) {
    case 1: return strip.Color(0,0,0,50);
    case 2: return strip.Color(0,50,0,0);
    case 3: return strip.Color(0,0,50,0);
    case 4: return strip.Color(50,0,0,0);
  }
  return 0;
}

// ── ESP-NOW message protocol ──────────────────────────────────
enum MsgType : uint8_t {
  MSG_HELLO=0,
  MSG_ROLE_READY,   // I know my role and am ready
  MSG_SETUP_DONE,   // my pieces are placed / CENTER is ready
  MSG_LIFT,         // d[0]=gr, d[1]=gc
  MSG_HIGHLIGHT,    // d[0]=gr, d[1]=gc, d[2]=type(0=empty,1=capture)
  MSG_CLEAR,        // clear highlights
  MSG_PLACED,       // d[0]=gr, d[1]=gc
  MSG_CANCEL,       // cross-board lift cancelled
  MSG_MOVE,         // d[0]=fromGR, d[1]=fromGC, d[2]=toGR, d[3]=toGC
  MSG_ELIM,         // d[0]=eliminated player #
  MSG_RESET
};

struct __attribute__((packed)) Message {
  MsgType type; uint8_t srcRole; uint8_t dstRole; uint8_t d[4];
};

uint8_t broadcastAddr[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
uint8_t roleMac[6][6];
bool    roleKnown[6] = {};
uint8_t myMac[6];

// Single-slot message queue for game-play messages
volatile bool    msgPending = false;
volatile MsgType msgType;
volatile uint8_t msgSrcRole, msgDstRole, msgData[4];
uint8_t          msgSenderMac[6];

// Sync counters updated directly in onRecv — never lost even during blocking ops
volatile uint8_t gReadySet = 0;   // bit i = role i sent ROLE_READY
volatile uint8_t gSetupSet = 0;   // bit i = role i sent SETUP_DONE

// Set by onRecv when a known peer sends MSG_HELLO during gameplay (they rebooted)
volatile bool reconnectNeeded = false;

// Highlights received on passive board
struct HLSq { uint8_t gr, gc, type; };
HLSq hlList[64]; int hlCount = 0;
bool isHL(int gr, int gc) {
  for (int i=0; i<hlCount; i++) if(hlList[i].gr==gr&&hlList[i].gc==gc) return true;
  return false;
}

// ── Hardware functions ────────────────────────────────────────
void setColumn(int col) {
  digitalWrite(RCLK_PIN, LOW);
  for (int i=7;i>=0;i--) {
    digitalWrite(SRCLK_PIN, LOW);
    digitalWrite(SER_PIN, (i==col)?HIGH:LOW);
    digitalWrite(SRCLK_PIN, HIGH);
  }
  digitalWrite(RCLK_PIN, HIGH);
}
void clearCols() {
  digitalWrite(RCLK_PIN, LOW);
  for (int i=0;i<8;i++) { digitalWrite(SRCLK_PIN,LOW); digitalWrite(SER_PIN,LOW); digitalWrite(SRCLK_PIN,HIGH); }
  digitalWrite(RCLK_PIN, HIGH);
}
void readSensors() {
  for (int c=0;c<8;c++) { setColumn(c); delayMicroseconds(10); for(int r=0;r<8;r++) sensor[r][c]=(digitalRead(ROW_PINS[r])==LOW); }
  clearCols();
}

// ── Animations ────────────────────────────────────────────────
void searchAnim(int frame) {
  strip.clear(); int d=frame%15;
  for(int r=0;r<8;r++) for(int c=0;c<8;c++) if((r+c)==d||(r+c)==d-1) strip.setPixelColor(r*8+c,strip.Color(0,0,120,0));
  strip.show();
}

void centerEdges() {
  for(int c=0;c<8;c++) strip.setPixelColor(0*8+c, pColor(1));
  for(int c=0;c<8;c++) strip.setPixelColor(7*8+c, pColor(4));
  for(int r=0;r<8;r++) strip.setPixelColor(r*8+0, pColor(2));
  for(int r=0;r<8;r++) strip.setPixelColor(r*8+7, pColor(3));
  strip.show();
}

// Non-blocking role flash: processes 10 ms ticks so onRecv still runs
void roleFlash(int role) {
  for (int f=0; f<4; f++) {
    strip.clear();
    if (role==5) { centerEdges(); }
    else { uint32_t c=pColor(role); if(f%2==0) for(int i=0;i<LED_COUNT;i++) strip.setPixelColor(i,c); strip.show(); }
    for (int t=0; t<30; t++) {       // 30 × 10 ms = 300 ms per flash
      if (msgPending) msgPending=false; // drain slot; gReadySet already updated
      if (digitalRead(RESET_PIN)==LOW) { delay(50); if(digitalRead(RESET_PIN)==LOW) return; }
      delay(10);
    }
  }
  strip.clear(); strip.show();
}

void illegalFlash(int gr, int gc) {
  if (!onMe(gr,gc)) return;
  for(int f=0;f<3;f++) { setGLED(gr,gc,strip.Color(200,150,0,0)); strip.show(); delay(150); setGLED(gr,gc,0); strip.show(); delay(100); }
}
void captureAnim(int gr, int gc) {
  if (!onMe(gr,gc)) return;
  float cx=locC(gr,gc), cy=locR(gr,gc);
  for (float r=0;r<6.0f;r+=0.8f) {
    strip.clear();
    for(int rr=0;rr<8;rr++) for(int cc=0;cc<8;cc++) { float dx=cc-cx,dy=rr-cy; if(fabsf(sqrtf(dx*dx+dy*dy)-r)<0.7f) strip.setPixelColor(rr*8+cc,strip.Color(255,0,0,0)); }
    strip.show(); delay(60);
  }
  strip.clear(); strip.show();
}
void promotionAnim(int gr, int gc) {
  if (!onMe(gr,gc)) return;
  int px=locR(gr,gc)*8+locC(gr,gc);
  for(int f=0;f<5;f++) { strip.setPixelColor(px,strip.Color(255,200,0,100)); strip.show(); delay(120); strip.setPixelColor(px,0); strip.show(); delay(80); }
}
void checkFlash() {
  for(int f=0;f<3;f++) { for(int i=0;i<LED_COUNT;i++) strip.setPixelColor(i,strip.Color(255,0,0,0)); strip.show(); delay(150); strip.clear(); strip.show(); delay(120); }
}
void elimAnim(int pl) {
  uint32_t c=pColor(pl);
  for(int f=0;f<6;f++) { for(int i=0;i<LED_COUNT;i++) strip.setPixelColor(i,(f%2==0)?c:0); strip.show(); delay(200); }
  strip.clear(); strip.show();
}
void restorePassiveDisplay() {
  if (myRole==5) centerEdges();
  else { strip.clear(); strip.show(); }
}

// ── Chess: board initialisation ───────────────────────────────
void initGBoard() {
  memset(gBoard, 0, sizeof(gBoard));
  const int bk[8] = {ROOK,KNIGHT,BISHOP,QUEEN,KING,BISHOP,KNIGHT,ROOK};
  // P1: back rank global row 0, pawns row 1, cols 8-15
  for(int i=0;i<8;i++) { gBoard[0][8+i]=mkPiece(1,bk[i]); gBoard[1][8+i]=mkPiece(1,PAWN); }
  // P2: back rank global col 0, pawns col 1, rows 8-15
  for(int i=0;i<8;i++) { gBoard[8+i][0]=mkPiece(2,bk[i]); gBoard[8+i][1]=mkPiece(2,PAWN); }
  // P3: back rank global col 23, pawns col 22, rows 8-15  (reversed piece order)
  for(int i=0;i<8;i++) { gBoard[8+i][23]=mkPiece(3,bk[7-i]); gBoard[8+i][22]=mkPiece(3,PAWN); }
  // P4: back rank global row 23, pawns row 22, cols 8-15  (reversed piece order)
  for(int i=0;i<8;i++) { gBoard[23][8+i]=mkPiece(4,bk[7-i]); gBoard[22][8+i]=mkPiece(4,PAWN); }
}

// ── Chess: move generation ────────────────────────────────────
bool onPawnStart(int pl, int gr, int gc) {
  if(pl==1) return gr==1&&gc>=8&&gc<=15;
  if(pl==2) return gc==1&&gr>=8&&gr<=15;
  if(pl==3) return gc==22&&gr>=8&&gr<=15;
  if(pl==4) return gr==22&&gc>=8&&gc<=15;
  return false;
}
bool onPromotion(int pl, int gr, int gc) {
  if(pl==1) return gr>=16;
  if(pl==2) return gc>=16;
  if(pl==3) return gc<=7;
  if(pl==4) return gr<=7;
  return false;
}

void getMovesRaw(int gr, int gc, int &mc, int mv[][2]) {
  mc = 0;
  uint8_t piece = gBoard[gr][gc]; if(isEm(piece)) return;
  int pl=plOf(piece), pt=tyOf(piece);
  auto canLand = [&](int r,int c)->bool {
    if(!validSq(r,c)) return false;
    uint8_t t=gBoard[r][c]; return isEm(t)||plOf(t)!=pl;
  };
  auto push  = [&](int r,int c) { if(canLand(r,c)){mv[mc][0]=r;mv[mc][1]=c;mc++;} };
  auto slide = [&](int dr,int dc) {
    for(int s=1;s<24;s++) { int nr=gr+s*dr,nc=gc+s*dc; if(!validSq(nr,nc)) break;
      uint8_t t=gBoard[nr][nc]; if(isEm(t)){mv[mc][0]=nr;mv[mc][1]=nc;mc++;}
      else{if(plOf(t)!=pl){mv[mc][0]=nr;mv[mc][1]=nc;mc++;} break;} }
  };
  switch(pt) {
    case PAWN: {
      int dr=PDR[pl], dc=PDC[pl], nr=gr+dr, nc=gc+dc;
      if(validSq(nr,nc)&&isEm(gBoard[nr][nc])) {
        mv[mc][0]=nr; mv[mc][1]=nc; mc++;
        if(onPawnStart(pl,gr,gc)) { int r2=nr+dr,c2=nc+dc; if(validSq(r2,c2)&&isEm(gBoard[r2][c2])){mv[mc][0]=r2;mv[mc][1]=c2;mc++;} }
      }
      int cr1r,cr1c,cr2r,cr2c;
      if(dr!=0){cr1r=gr+dr;cr1c=gc-1;cr2r=gr+dr;cr2c=gc+1;}
      else     {cr1r=gr-1; cr1c=gc+dc;cr2r=gr+1; cr2c=gc+dc;}
      for(int i=0;i<2;i++){int cr=(i==0)?cr1r:cr2r,cc=(i==0)?cr1c:cr2c;
        if(validSq(cr,cc)&&!isEm(gBoard[cr][cc])&&plOf(gBoard[cr][cc])!=pl){mv[mc][0]=cr;mv[mc][1]=cc;mc++;}}
      break;
    }
    case KNIGHT: { int nd[8][2]={{2,1},{1,2},{-1,2},{-2,1},{-2,-1},{-1,-2},{1,-2},{2,-1}}; for(int i=0;i<8;i++) push(gr+nd[i][0],gc+nd[i][1]); break; }
    case BISHOP: slide(1,1);slide(1,-1);slide(-1,1);slide(-1,-1); break;
    case ROOK:   slide(1,0);slide(-1,0);slide(0,1);slide(0,-1);   break;
    case QUEEN:  slide(1,0);slide(-1,0);slide(0,1);slide(0,-1);slide(1,1);slide(1,-1);slide(-1,1);slide(-1,-1); break;
    case KING:   { int kd[8][2]={{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}}; for(int i=0;i<8;i++) push(gr+kd[i][0],gc+kd[i][1]); break; }
  }
}

bool isKingInCheck(int player) {
  int kr=-1, kc=-1;
  for(int r=0;r<24&&kr==-1;r++) for(int c=0;c<24&&kr==-1;c++)
    if(validSq(r,c)&&plOf(gBoard[r][c])==player&&tyOf(gBoard[r][c])==KING){kr=r;kc=c;}
  if(kr==-1) return false;
  for(int ep=1;ep<=4;ep++) {
    if(ep==player) continue;
    int dr=PDR[ep], dc=PDC[ep];
    if(dr!=0) { for(int d=-1;d<=1;d+=2){int pr=kr-dr,pc=kc+d;if(validSq(pr,pc)&&plOf(gBoard[pr][pc])==ep&&tyOf(gBoard[pr][pc])==PAWN)return true;} }
    else      { for(int d=-1;d<=1;d+=2){int pr=kr+d,pc=kc-dc;if(validSq(pr,pc)&&plOf(gBoard[pr][pc])==ep&&tyOf(gBoard[pr][pc])==PAWN)return true;} }
  }
  int nd[8][2]={{2,1},{1,2},{-1,2},{-2,1},{-2,-1},{-1,-2},{1,-2},{2,-1}};
  for(int i=0;i<8;i++){int nr=kr+nd[i][0],nc=kc+nd[i][1];if(validSq(nr,nc)&&!isEm(gBoard[nr][nc])&&plOf(gBoard[nr][nc])!=player&&tyOf(gBoard[nr][nc])==KNIGHT)return true;}
  int kd[8][2]={{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
  for(int i=0;i<8;i++){int nr=kr+kd[i][0],nc=kc+kd[i][1];if(validSq(nr,nc)&&!isEm(gBoard[nr][nc])&&plOf(gBoard[nr][nc])!=player&&tyOf(gBoard[nr][nc])==KING)return true;}
  int drs[4]={1,-1,0,0},dcs[4]={0,0,1,-1},drd[4]={1,1,-1,-1},dcd[4]={1,-1,1,-1};
  for(int i=0;i<4;i++){
    for(int s=1;s<24;s++){int nr=kr+s*drs[i],nc=kc+s*dcs[i];if(!validSq(nr,nc))break;uint8_t t=gBoard[nr][nc];if(!isEm(t)){if(plOf(t)!=player&&(tyOf(t)==ROOK||tyOf(t)==QUEEN))return true;break;}}
    for(int s=1;s<24;s++){int nr=kr+s*drd[i],nc=kc+s*dcd[i];if(!validSq(nr,nc))break;uint8_t t=gBoard[nr][nc];if(!isEm(t)){if(plOf(t)!=player&&(tyOf(t)==BISHOP||tyOf(t)==QUEEN))return true;break;}}
  }
  return false;
}

void getMoves(int gr, int gc, int &mc, int mv[][2]) {
  getMovesRaw(gr, gc, mc, mv);
  uint8_t piece=gBoard[gr][gc]; int pl=plOf(piece);
  int legal[64][2]; int lc2=0;
  for(int i=0;i<mc;i++) {
    int tr=mv[i][0], tc=mv[i][1]; uint8_t save=gBoard[tr][tc];
    gBoard[tr][tc]=piece; gBoard[gr][gc]=0;
    if(!isKingInCheck(pl)){legal[lc2][0]=tr;legal[lc2][1]=tc;lc2++;}
    gBoard[gr][gc]=piece; gBoard[tr][tc]=save;
  }
  mc=lc2; for(int i=0;i<lc2;i++){mv[i][0]=legal[i][0];mv[i][1]=legal[i][1];}
}

bool kingPresent(int player) {
  for(int r=0;r<24;r++) for(int c=0;c<24;c++)
    if(validSq(r,c)&&plOf(gBoard[r][c])==player&&tyOf(gBoard[r][c])==KING) return true;
  return false;
}
bool inLegal(int gr, int gc, int mc, int mv[][2]) {
  for(int i=0;i<mc;i++) if(mv[i][0]==gr&&mv[i][1]==gc) return true;
  return false;
}

void applyMoveToBoard(int fr, int fc, int tr, int tc) {
  if(!isEm(gBoard[tr][tc]) && onMe(tr,tc)) captureAnim(tr,tc);
  gBoard[tr][tc]=gBoard[fr][fc]; gBoard[fr][fc]=0;
  uint8_t p=gBoard[tr][tc];
  if(tyOf(p)==PAWN && onPromotion(plOf(p),tr,tc)) { promotionAnim(tr,tc); gBoard[tr][tc]=mkPiece(plOf(p),QUEEN); }
}

void checkEliminations() {
  for(int pl=1;pl<=4;pl++) {
    if(!eliminated[pl] && !kingPresent(pl)) { eliminated[pl]=true; elimAnim(pl); }
  }
}

// ── ESP-NOW messaging ─────────────────────────────────────────
void sendMsg(MsgType type, uint8_t dst, uint8_t d0=0,uint8_t d1=0,uint8_t d2=0,uint8_t d3=0) {
  Message msg; msg.type=type; msg.srcRole=myRole; msg.dstRole=dst;
  msg.d[0]=d0; msg.d[1]=d1; msg.d[2]=d2; msg.d[3]=d3;
  if(dst>0&&dst<=5&&roleKnown[dst]) esp_now_send(roleMac[dst],(uint8_t*)&msg,sizeof(msg));
  else                               esp_now_send(broadcastAddr,(uint8_t*)&msg,sizeof(msg));
}
void sendAll(MsgType type, uint8_t d0=0,uint8_t d1=0,uint8_t d2=0,uint8_t d3=0) {
  for(int r=1;r<=5;r++) if(r!=myRole&&roleKnown[r]) sendMsg(type,(uint8_t)r,d0,d1,d2,d3);
}

bool addPeer(uint8_t *mac) {
  if(esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t p={}; memcpy(p.peer_addr,mac,6); p.channel=0; p.encrypt=false;
  return esp_now_add_peer(&p)==ESP_OK;
}
int macCmp(uint8_t *a, uint8_t *b) {
  for(int i=0;i<6;i++) { if(a[i]<b[i]) return -1; if(a[i]>b[i]) return 1; }
  return 0;
}

void onRecv(const esp_now_recv_info *ri, const uint8_t *data, int len) {
  if(len < (int)sizeof(Message)) return;
  const Message *m = (const Message*)data;
  if(m->dstRole!=0 && m->dstRole!=myRole) return;

  // Sync counters updated immediately — never lost even during blocking animations
  if(m->type==MSG_ROLE_READY && m->srcRole>=1 && m->srcRole<=5)
    gReadySet |= (uint8_t)(1 << m->srcRole);
  if(m->type==MSG_SETUP_DONE && m->srcRole>=1 && m->srcRole<=5)
    gSetupSet |= (uint8_t)(1 << m->srcRole);

  // Auto-reconnect: a known peer has rebooted and is searching again
  if(m->type==MSG_HELLO && myRole!=0) {
    for(int r=1; r<=5; r++) {
      if(roleKnown[r] && macCmp((uint8_t*)ri->src_addr, roleMac[r])==0) {
        reconnectNeeded=true; break;
      }
    }
  }

  msgType=m->type; msgSrcRole=m->srcRole; msgDstRole=m->dstRole;
  msgData[0]=m->d[0]; msgData[1]=m->d[1]; msgData[2]=m->d[2]; msgData[3]=m->d[3];
  memcpy(msgSenderMac, ri->src_addr, 6);
  msgPending=true;
}
void onSent(const wifi_tx_info_t*, esp_now_send_status_t) {}

// ── Discovery — collect 5 MACs, assign roles, sync ────────────
bool discoveryLoop() {
  reconnectNeeded=false;
  WiFi.macAddress(myMac);
  uint8_t allMacs[5][6]; memcpy(allMacs[0],myMac,6); int numMacs=1;
  memset(roleKnown,0,sizeof(roleKnown));
  msgPending=false;
  gReadySet=0; gSetupSet=0;
  unsigned long lastB=0; int af=0; unsigned long at=0;

  // ── Phase 1: collect 5 unique MACs via MSG_HELLO ──────────
  while(numMacs < 5) {
    if(digitalRead(RESET_PIN)==LOW){delay(50);if(digitalRead(RESET_PIN)==LOW)return false;}
    if(millis()-lastB > 600) { sendMsg(MSG_HELLO,0); lastB=millis(); }
    if(millis()-at > 90)     { searchAnim(af++); at=millis(); }
    if(msgPending) {
      msgPending=false;
      if(msgType==MSG_HELLO) {
        bool found=false;
        for(int i=0;i<numMacs;i++) if(macCmp(allMacs[i],msgSenderMac)==0){found=true;break;}
        if(!found&&numMacs<5) memcpy(allMacs[numMacs++],msgSenderMac,6);
      }
    }
    delay(10);
  }

  // Sort ascending → deterministic role assignment on every board
  for(int i=0;i<4;i++) for(int j=0;j<4-i;j++) {
    if(macCmp(allMacs[j],allMacs[j+1])>0) {
      uint8_t tmp[6]; memcpy(tmp,allMacs[j],6); memcpy(allMacs[j],allMacs[j+1],6); memcpy(allMacs[j+1],tmp,6);
    }
  }
  for(int i=0;i<5;i++) if(macCmp(allMacs[i],myMac)==0) myRole=(uint8_t)(i+1);

  for(int i=0;i<5;i++) {
    if(macCmp(allMacs[i],myMac)==0) continue;
    uint8_t role=(uint8_t)(i+1);
    memcpy(roleMac[role],allMacs[i],6);
    roleKnown[role]=true;
    addPeer(roleMac[role]);
  }
  roleKnown[myRole]=true;

  // Mark own bit before flash so late boards joining Phase 2 already see it
  gReadySet |= (uint8_t)(1 << myRole);

  // Non-blocking role flash (gReadySet updated in onRecv during each 10 ms tick)
  roleFlash(myRole);

  // ── Phase 2: wait until all 5 boards have acknowledged ────
  // gReadySet is written by onRecv in the ESP-NOW task — no messages missed.
  unsigned long lastR=0;
  while(gReadySet != 0x3E) {   // 0x3E = bits 1-5 all set
    if(digitalRead(RESET_PIN)==LOW){delay(50);if(digitalRead(RESET_PIN)==LOW)return false;}
    if(millis()-lastR > 400) { sendMsg(MSG_ROLE_READY,0); lastR=millis(); }
    uint8_t pulse=(uint8_t)(60+60*sinf(millis()*0.005f));
    strip.clear();
    if(myRole==5) {
      for(int c=0;c<8;c++) strip.setPixelColor(c,     strip.Color(0,0,0,pulse));
      for(int c=0;c<8;c++) strip.setPixelColor(56+c,  strip.Color(pulse,0,0,0));
      for(int r=0;r<8;r++) strip.setPixelColor(r*8,   strip.Color(0,pulse,0,0));
      for(int r=0;r<8;r++) strip.setPixelColor(r*8+7, strip.Color(0,0,pulse,0));
    } else {
      strip.setPixelColor(27,pColor(myRole)); strip.setPixelColor(28,pColor(myRole));
      strip.setPixelColor(35,pColor(myRole)); strip.setPixelColor(36,pColor(myRole));
    }
    strip.show();
    if(msgPending){msgPending=false; if(msgType==MSG_RESET)return false;}
    delay(20);
  }

  // Grace period: keep broadcasting for 2 s so any board still in Phase 1
  // has time to finish and enter Phase 2 before everyone moves on.
  unsigned long graceEnd=millis()+2000;
  while(millis()<graceEnd) {
    if(digitalRead(RESET_PIN)==LOW){delay(50);if(digitalRead(RESET_PIN)==LOW)return false;}
    if(millis()-lastR > 400) { sendMsg(MSG_ROLE_READY,0); lastR=millis(); }
    if(msgPending){msgPending=false; if(msgType==MSG_RESET)return false;}
    delay(20);
  }

  strip.clear(); strip.show(); delay(200);
  return true;
}

// ── Setup — place pieces, sync all boards ─────────────────────
bool setupPhase() {
  gSetupSet |= (uint8_t)(1 << myRole); // mark myself ready to start counting

  // All players have their back rank at local row 0 and pawns at local row 1.
  // Local rows 6-7 face the center — glow them in the player's color as a
  // visual orientation guide. The CENTER board just shows its colored edges.
  bool piecesOk = (myRole == 5);
  while(!piecesOk) {
    if(digitalRead(RESET_PIN)==LOW){delay(50);if(digitalRead(RESET_PIN)==LOW){strip.clear();strip.show();return false;}}
    readSensors();
    piecesOk=true;
    strip.clear();
    for(int lr=0;lr<8;lr++) for(int lc=0;lc<8;lc++) {
      int gr=gRow(lr,lc), gc=gCol(lr,lc);
      bool nearCenter = (lr >= 6);          // same for every player role
      bool hasPiece   = sensor[lr][lc];
      uint8_t expected = gBoard[gr][gc];
      if(nearCenter) {
        strip.setPixelColor(lr*8+lc, pColor(myRole));   // glow near-center rows
      } else if(!isEm(expected) && plOf(expected)==myRole && !hasPiece) {
        piecesOk=false;
        strip.setPixelColor(lr*8+lc, dimColor(myRole)); // missing piece guide
      }
    }
    strip.show();
    if(piecesOk) break;
    delay(200);
  }
  // Green "all pieces placed" flash
  for(int f=0;f<2;f++) { for(int i=0;i<LED_COUNT;i++) strip.setPixelColor(i,strip.Color(0,150,0,0)); strip.show(); delay(200); strip.clear(); strip.show(); delay(200); }

  // Wait for all boards to confirm, then hold briefly so late boards catch up
  unsigned long lastS=0;
  while(gSetupSet != 0x3E) {
    if(digitalRead(RESET_PIN)==LOW){delay(50);if(digitalRead(RESET_PIN)==LOW){strip.clear();strip.show();return false;}}
    if(millis()-lastS > 500) { sendMsg(MSG_SETUP_DONE,0); lastS=millis(); }
    if(myRole==5) centerEdges();
    if(msgPending){msgPending=false; if(msgType==MSG_RESET){strip.clear();strip.show();return false;}}
    delay(20);
  }
  unsigned long graceEnd=millis()+1000;
  while(millis()<graceEnd) {
    if(millis()-lastS > 500) { sendMsg(MSG_SETUP_DONE,0); lastS=millis(); }
    if(msgPending){msgPending=false; if(msgType==MSG_RESET){strip.clear();strip.show();return false;}}
    delay(20);
  }
  for(int f=0;f<2;f++) { for(int i=0;i<LED_COUNT;i++) strip.setPixelColor(i,pColor(myRole==5?1:myRole)); strip.show(); delay(200); strip.clear(); strip.show(); delay(200); }
  readSensors(); memcpy(sensorPrev,sensor,sizeof(sensor));
  return true;
}

// ── Turn helpers ──────────────────────────────────────────────
void glowMyPieces(int myPlayer) {
  strip.clear();
  for(int lr=0;lr<8;lr++) for(int lc=0;lc<8;lc++) {
    int gr=gRow(lr,lc), gc=gCol(lr,lc); uint8_t p=gBoard[gr][gc];
    if(!isEm(p)&&plOf(p)==myPlayer) strip.setPixelColor(lr*8+lc,dimColor(myPlayer));
  }
  strip.show();
}

void showHints(int liftGR, int liftGC, bool liftOnMe, int mc, int mv[][2]) {
  for(int i=0;i<mc-1;i++) for(int j=0;j<mc-i-1;j++) {
    int dr0=mv[j][0]-liftGR,dc0=mv[j][1]-liftGC;
    int dr1=mv[j+1][0]-liftGR,dc1=mv[j+1][1]-liftGC;
    if(dr0*dr0+dc0*dc0 > dr1*dr1+dc1*dc1) {
      int t0=mv[j][0],t1=mv[j][1]; mv[j][0]=mv[j+1][0]; mv[j][1]=mv[j+1][1]; mv[j+1][0]=t0; mv[j+1][1]=t1;
    }
  }
  strip.clear();
  if(liftOnMe) setGLED(liftGR,liftGC,strip.Color(0,0,0,255));
  for(int i=0;i<mc;i++) {
    int gr2=mv[i][0], gc2=mv[i][1];
    bool cap=(!isEm(gBoard[gr2][gc2]));
    uint32_t col=cap?strip.Color(255,60,0,0):strip.Color(0,0,0,180);
    if(onMe(gr2,gc2)) { setGLED(gr2,gc2,col); strip.show(); delay(30); }
    else { strip.show(); sendMsg(MSG_HIGHLIGHT,(uint8_t)squareRole(gr2,gc2),(uint8_t)gr2,(uint8_t)gc2,(uint8_t)(cap?1:0)); delay(30); }
  }
  strip.show();
}

void restoreHintsLocal(int liftGR, int liftGC, bool liftOnMe, int mc, int mv[][2]) {
  strip.clear();
  if(liftOnMe) setGLED(liftGR,liftGC,strip.Color(0,0,0,255));
  for(int i=0;i<mc;i++) if(onMe(mv[i][0],mv[i][1])) {
    bool cap=(!isEm(gBoard[mv[i][0]][mv[i][1]]));
    setGLED(mv[i][0],mv[i][1],cap?strip.Color(255,60,0,0):strip.Color(0,0,0,180));
  }
  strip.show();
}

// ═══════════════════════════════════════════════════════════════
// ACTIVE TURN
// ═══════════════════════════════════════════════════════════════
bool handleActiveTurn(int myPlayer) {
  glowMyPieces(myPlayer);
  int  liftGR=-1, liftGC=-1;
  bool liftOnMe=false;
  int  mc=0; int mv[64][2];
  bool hintsShown=false;

  while(true) {
    if(reconnectNeeded)                       return false;
    if(digitalRead(RESET_PIN)==LOW){delay(50);if(digitalRead(RESET_PIN)==LOW){sendAll(MSG_RESET);strip.clear();strip.show();return false;}}
    readSensors();

    // Phase 1: look for a lift
    if(liftGR == -1) {
      for(int lr=0;lr<8;lr++) for(int lc=0;lc<8;lc++) {
        if(!sensorPrev[lr][lc]||sensor[lr][lc]) continue;
        int gr=gRow(lr,lc),gc=gCol(lr,lc); uint8_t p=gBoard[gr][gc]; if(isEm(p)) continue;
        if(plOf(p)==myPlayer){liftGR=gr;liftGC=gc;liftOnMe=true;break;}
      }
      if(!liftOnMe&&msgPending) {
        msgPending=false;
        if(msgType==MSG_LIFT)  { liftGR=(int)msgData[0]; liftGC=(int)msgData[1]; liftOnMe=false; }
        else if(msgType==MSG_ELIM)  { eliminated[msgData[0]]=true; }
        else if(msgType==MSG_RESET) { strip.clear(); strip.show(); return false; }
      }
    }

    // Phase 2: compute and display legal moves
    if(liftGR != -1 && !hintsShown) {
      getMoves(liftGR,liftGC,mc,mv);
      if(mc == 0) {
        illegalFlash(liftGR,liftGC);
        if(liftOnMe) {
          while(!sensor[locR(liftGR,liftGC)][locC(liftGR,liftGC)]) {
            if(reconnectNeeded){strip.clear();strip.show();return false;}
            if(digitalRead(RESET_PIN)==LOW){delay(50);if(digitalRead(RESET_PIN)==LOW){sendAll(MSG_RESET);strip.clear();strip.show();return false;}}
            readSensors(); memcpy(sensorPrev,sensor,sizeof(sensor)); delay(40);
          }
        } else {
          while(true) {
            if(reconnectNeeded){strip.clear();strip.show();return false;}
            if(digitalRead(RESET_PIN)==LOW){delay(50);if(digitalRead(RESET_PIN)==LOW){sendAll(MSG_RESET);strip.clear();strip.show();return false;}}
            if(msgPending){msgPending=false;if(msgType==MSG_CANCEL)break;if(msgType==MSG_RESET)return false;}
            delay(20);
          }
        }
        liftGR=-1; liftGC=-1; mc=0; liftOnMe=false;
        glowMyPieces(myPlayer);
        memcpy(sensorPrev,sensor,sizeof(sensor)); continue;
      }
      showHints(liftGR,liftGC,liftOnMe,mc,mv);
      hintsShown=true;
    }

    // Phase 3: wait for placement
    if(liftGR != -1 && hintsShown) {
      // Piece returned to origin (local lift)
      if(liftOnMe && sensor[locR(liftGR,liftGC)][locC(liftGR,liftGC)] && !sensorPrev[locR(liftGR,liftGC)][locC(liftGR,liftGC)]) {
        sendAll(MSG_CLEAR);
        strip.clear(); strip.show();
        liftGR=-1; liftGC=-1; mc=0; hintsShown=false;
        glowMyPieces(myPlayer);
        memcpy(sensorPrev,sensor,sizeof(sensor)); continue;
      }
      // Local placement
      for(int lr=0;lr<8;lr++) for(int lc=0;lc<8;lc++) {
        if(!sensor[lr][lc]||sensorPrev[lr][lc]) continue;
        int gr=gRow(lr,lc), gc=gCol(lr,lc);
        if(inLegal(gr,gc,mc,mv)) {
          applyMoveToBoard(liftGR,liftGC,gr,gc);
          sendAll(MSG_MOVE,(uint8_t)liftGR,(uint8_t)liftGC,(uint8_t)gr,(uint8_t)gc);
          checkEliminations();
          sendAll(MSG_CLEAR);
          strip.clear();
          if(onMe(liftGR,liftGC)) setGLED(liftGR,liftGC,strip.Color(0,80,0,0));
          setGLED(gr,gc,strip.Color(0,255,0,0));
          strip.show(); delay(500); strip.clear(); strip.show();
          readSensors(); memcpy(sensorPrev,sensor,sizeof(sensor));
          return true;
        } else {
          illegalFlash(gr,gc);
          restoreHintsLocal(liftGR,liftGC,liftOnMe,mc,mv);
        }
      }
      // Cross-board placement
      if(msgPending) {
        msgPending=false;
        if(msgType==MSG_PLACED) {
          int gr=(int)msgData[0], gc=(int)msgData[1];
          if(inLegal(gr,gc,mc,mv)) {
            applyMoveToBoard(liftGR,liftGC,gr,gc);
            sendAll(MSG_MOVE,(uint8_t)liftGR,(uint8_t)liftGC,(uint8_t)gr,(uint8_t)gc);
            checkEliminations();
            strip.clear();
            if(onMe(liftGR,liftGC)){setGLED(liftGR,liftGC,strip.Color(0,255,0,0));strip.show();delay(400);strip.clear();strip.show();}
            readSensors(); memcpy(sensorPrev,sensor,sizeof(sensor));
            return true;
          }
        } else if(msgType==MSG_CANCEL && !liftOnMe) {
          sendAll(MSG_CLEAR);
          strip.clear(); strip.show();
          liftGR=-1; liftGC=-1; mc=0; hintsShown=false;
          glowMyPieces(myPlayer);
          memcpy(sensorPrev,sensor,sizeof(sensor)); continue;
        } else if(msgType==MSG_ELIM)  { eliminated[msgData[0]]=true; }
        else if(msgType==MSG_RESET)   { strip.clear(); strip.show(); return false; }
      }
    }
    memcpy(sensorPrev,sensor,sizeof(sensor));
    delay(40);
  }
}

// ═══════════════════════════════════════════════════════════════
// PASSIVE TURN  (non-active players + CENTER, always)
// ═══════════════════════════════════════════════════════════════
bool handlePassiveTurn(int activePl) {
  hlCount=0;
  strip.clear(); restorePassiveDisplay();
  int liftGR=-1, liftGC=-1, liftLR=-1, liftLC=-1;

  while(true) {
    if(reconnectNeeded)                       return false;
    if(digitalRead(RESET_PIN)==LOW){delay(50);if(digitalRead(RESET_PIN)==LOW){sendAll(MSG_RESET);strip.clear();strip.show();return false;}}
    readSensors();

    if(msgPending) {
      msgPending=false;
      switch(msgType) {
        case MSG_HIGHLIGHT:
          if(onMe((int)msgData[0],(int)msgData[1])) {
            bool cap=(msgData[2]==1);
            setGLED((int)msgData[0],(int)msgData[1], cap?strip.Color(255,60,0,0):strip.Color(0,0,0,180));
            strip.show();
          }
          if(hlCount<64){hlList[hlCount].gr=msgData[0];hlList[hlCount].gc=msgData[1];hlList[hlCount].type=msgData[2];hlCount++;}
          break;

        case MSG_CLEAR:
          hlCount=0; liftGR=-1; liftGC=-1; liftLR=-1; liftLC=-1;
          strip.clear(); restorePassiveDisplay();
          break;

        case MSG_MOVE:
          applyMoveToBoard((int)msgData[0],(int)msgData[1],(int)msgData[2],(int)msgData[3]);
          checkEliminations();
          hlCount=0; strip.clear(); restorePassiveDisplay();
          readSensors(); memcpy(sensorPrev,sensor,sizeof(sensor));
          return true;

        case MSG_ELIM: eliminated[msgData[0]]=true; break;
        case MSG_RESET: strip.clear(); strip.show(); return false;
        default: break;
      }
    }

    // Detect active player's piece lifted on MY board (cross-board move start)
    if(liftGR == -1) {
      for(int lr=0;lr<8;lr++) for(int lc=0;lc<8;lc++) {
        if(!sensorPrev[lr][lc]||sensor[lr][lc]) continue;
        int gr=gRow(lr,lc),gc=gCol(lr,lc); uint8_t p=gBoard[gr][gc]; if(isEm(p)) continue;
        if(plOf(p)==activePl) {
          liftGR=gr; liftGC=gc; liftLR=lr; liftLC=lc;
          sendMsg(MSG_LIFT,(uint8_t)activePl,(uint8_t)gr,(uint8_t)gc);
          break;
        }
      }
    }

    // Piece returned to its square (cancel cross-board move)
    if(liftGR != -1) {
      if(sensor[liftLR][liftLC] && !sensorPrev[liftLR][liftLC]) {
        sendMsg(MSG_CANCEL,(uint8_t)activePl);
        hlCount=0; strip.clear(); restorePassiveDisplay();
        liftGR=-1; liftGC=-1; liftLR=-1; liftLC=-1;
        memcpy(sensorPrev,sensor,sizeof(sensor)); continue;
      }
    }

    // Placement on any highlighted square
    if(hlCount > 0) {
      for(int lr=0;lr<8;lr++) for(int lc=0;lc<8;lc++) {
        if(!sensor[lr][lc]||sensorPrev[lr][lc]) continue;
        int gr=gRow(lr,lc), gc=gCol(lr,lc);
        if(isHL(gr,gc)) {
          setGLED(gr,gc,strip.Color(0,255,0,0)); strip.show();
          sendMsg(MSG_PLACED,(uint8_t)activePl,(uint8_t)gr,(uint8_t)gc);
          while(true) {
            if(reconnectNeeded){strip.clear();strip.show();return false;}
            if(digitalRead(RESET_PIN)==LOW){delay(50);if(digitalRead(RESET_PIN)==LOW){sendAll(MSG_RESET);strip.clear();strip.show();return false;}}
            if(msgPending) {
              msgPending=false;
              if(msgType==MSG_MOVE) {
                applyMoveToBoard((int)msgData[0],(int)msgData[1],(int)msgData[2],(int)msgData[3]);
                checkEliminations();
                hlCount=0; strip.clear(); restorePassiveDisplay();
                readSensors(); memcpy(sensorPrev,sensor,sizeof(sensor));
                return true;
              }
              if(msgType==MSG_RESET){ strip.clear(); strip.show(); return false; }
            }
            delay(20);
          }
        } else {
          illegalFlash(gr,gc);
          strip.clear();
          for(int i=0;i<hlCount;i++)
            if(onMe(hlList[i].gr,hlList[i].gc))
              setGLED(hlList[i].gr,hlList[i].gc, hlList[i].type==1?strip.Color(255,60,0,0):strip.Color(0,0,0,180));
          restorePassiveDisplay();
          strip.show();
        }
      }
    }

    memcpy(sensorPrev,sensor,sizeof(sensor));
    delay(20);
  }
}

// ── Arduino setup ─────────────────────────────────────────────
void setup() {
  Serial.begin(115200); delay(1000);
  Serial.println("\n============================");
  Serial.println("  4-Player Chess — 5 Boards");
  Serial.println("============================");

  pinMode(SER_PIN,OUTPUT); pinMode(RCLK_PIN,OUTPUT); pinMode(SRCLK_PIN,OUTPUT);
  for(int i=0;i<8;i++) pinMode(ROW_PINS[i],INPUT_PULLUP);
  pinMode(RESET_PIN,INPUT_PULLUP);

  strip.begin(); strip.setBrightness(BRIGHTNESS); strip.clear(); strip.show();

  WiFi.mode(WIFI_STA); WiFi.disconnect(); delay(100);
  if(esp_now_init() != ESP_OK) {
    while(true){ for(int i=0;i<LED_COUNT;i++) strip.setPixelColor(i,strip.Color(255,0,0,0)); strip.show(); delay(500); strip.clear(); strip.show(); delay(500); }
  }
  esp_now_register_send_cb(onSent);
  esp_now_register_recv_cb(onRecv);
  esp_now_peer_info_t bcast={}; memcpy(bcast.peer_addr,broadcastAddr,6); bcast.channel=0; bcast.encrypt=false;
  esp_now_add_peer(&bcast);
}

// ── Main game loop ────────────────────────────────────────────
int nextActiveTurn(int cur) {
  for(int i=1;i<=4;i++) { int n=(cur%4)+1; cur=n; if(!eliminated[n]) return n; }
  return -1;
}

void loop() {
  initGBoard();
  memset(eliminated, 0, sizeof(eliminated));

  if(!discoveryLoop()) { strip.clear(); strip.show(); return; }
  if(!setupPhase())    { strip.clear(); strip.show(); return; }

  int activeTurn = 1;

  while(true) {
    // A known peer has rebooted — all boards restart discovery together
    if(reconnectNeeded) {
      sendAll(MSG_RESET);
      strip.clear(); strip.show();
      delay(500);
      return; // loop() restarts from the top
    }
    if(digitalRead(RESET_PIN)==LOW){delay(50);if(digitalRead(RESET_PIN)==LOW){sendAll(MSG_RESET);strip.clear();strip.show();return;}}

    bool ok;
    if(myRole!=5 && myRole==activeTurn) ok=handleActiveTurn(activeTurn);
    else                                 ok=handlePassiveTurn(activeTurn);
    if(!ok){ strip.clear(); strip.show(); return; }

    activeTurn = nextActiveTurn(activeTurn);

    int remaining=0, lastSurvivor=0;
    for(int pl=1;pl<=4;pl++) if(!eliminated[pl]){ remaining++; lastSurvivor=pl; }
    if(remaining<=1) {
      if(remaining==1 && myRole==lastSurvivor) {
        for(int f=0;f<8;f++){ for(int i=0;i<LED_COUNT;i++) strip.setPixelColor(i,(f%2==0)?pColor(lastSurvivor):0); strip.show(); delay(250); }
      }
      delay(5000); return;
    }
    if(activeTurn < 0){ delay(3000); return; }

    if(myRole==activeTurn && isKingInCheck(activeTurn)) checkFlash();
  }
}
