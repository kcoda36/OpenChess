// ============================================================
// chess.ino  —  Two-Player Chess (single board, no WiFi)
//
//   Power on → place all 32 pieces → game starts automatically.
//   Both players get full legal-move LED hints on their turn.
//
//   LEDs:
//     Dim white glow  — your pieces (it's your turn)
//     Bright white    — lifted piece
//     White           — legal empty square
//     Orange-red      — legal capture
//     Green           — move confirmed
//     Amber flash     — illegal move / pinned piece
//     Red flash       — check
//     Gold            — pawn promotion (auto-queens)
//
//   IO9 = reset to setup screen at any time
// ============================================================

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
bool whiteTurn = true;

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

void startupAnimation() {
  for (int d = 0; d < 15; d++) {
    strip.clear();
    for (int r = 0; r < 8; r++)
      for (int c = 0; c < 8; c++)
        if (r + c == d || r + c == d - 1)
          strip.setPixelColor(r*8+c, strip.Color(0, 0, 200, 0));
    strip.show();
    delay(60);
  }
  strip.clear(); strip.show();
}

void captureAnimation(int row, int col) {
  float cx = col, cy = row;
  for (float r = 0; r < 6.0f; r += 0.8f) {
    strip.clear();
    for (int rr = 0; rr < 8; rr++)
      for (int cc = 0; cc < 8; cc++) {
        float dx = cc - cx, dy = rr - cy;
        if (fabsf(sqrtf(dx*dx + dy*dy) - r) < 0.7f)
          strip.setPixelColor(rr*8+cc, strip.Color(255, 0, 0, 0));
      }
    strip.show(); delay(60);
  }
  strip.clear(); strip.show();
}

void promotionAnimation(int col) {
  for (int f = 0; f < 5; f++) {
    for (int r = 0; r < 8; r++)
      strip.setPixelColor(r*8+col, strip.Color(255, 200, 0, 100));
    strip.show(); delay(120);
    strip.clear(); strip.show(); delay(80);
  }
}

void checkFlash() {
  for (int f = 0; f < 3; f++) {
    for (int i = 0; i < LED_COUNT; i++) strip.setPixelColor(i, strip.Color(255, 0, 0, 0));
    strip.show(); delay(150);
    strip.clear(); strip.show(); delay(120);
  }
}

void gameOverAnimation(bool whiteWon) {
  for (int f = 0; f < 10; f++) {
    for (int r = 0; r < 8; r++)
      for (int c = 0; c < 8; c++) {
        char p = board[r][c];
        if (p == ' ') { strip.setPixelColor(r*8+c, 0); continue; }
        bool isWhite = (p >= 'A' && p <= 'Z');
        bool winner  = (isWhite == whiteWon);
        strip.setPixelColor(r*8+c, (f % 2 == 0) ? (winner ? strip.Color(0,255,0,0) : strip.Color(255,0,0,0)) : 0);
      }
    strip.show(); delay(350);
  }
  strip.clear(); strip.show();
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
  char eN=(byColor=='w')?'N':'n';
  int kd[8][2]={{2,1},{1,2},{-1,2},{-2,1},{-2,-1},{-1,-2},{1,-2},{2,-1}};
  for (int i=0;i<8;i++){int nr=r+kd[i][0],nc=c+kd[i][1];if(nr>=0&&nr<8&&nc>=0&&nc<8&&board[nr][nc]==eN)return true;}
  char eK=(byColor=='w')?'K':'k';
  int kd2[8][2]={{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
  for (int i=0;i<8;i++){int nr=r+kd2[i][0],nc=c+kd2[i][1];if(nr>=0&&nr<8&&nc>=0&&nc<8&&board[nr][nc]==eK)return true;}
  char eR=(byColor=='w')?'R':'r', eQ=(byColor=='w')?'Q':'q', eB=(byColor=='w')?'B':'b';
  int d4[4][2]={{1,0},{-1,0},{0,1},{0,-1}}, d4d[4][2]={{1,1},{1,-1},{-1,1},{-1,-1}};
  for (int i=0;i<4;i++){
    for(int s=1;s<8;s++){int nr=r+s*d4[i][0],nc=c+s*d4[i][1];if(nr<0||nr>=8||nc<0||nc>=8)break;char t=board[nr][nc];if(t!=' '){if(t==eR||t==eQ)return true;break;}}
    for(int s=1;s<8;s++){int nr=r+s*d4d[i][0],nc=c+s*d4d[i][1];if(nr<0||nr>=8||nc<0||nc>=8)break;char t=board[nr][nc];if(t!=' '){if(t==eB||t==eQ)return true;break;}}
  }
  return false;
}

bool isInCheck(char color) {
  char king = (color=='w') ? 'K' : 'k';
  char enemy = (color=='w') ? 'b' : 'w';
  for (int r=0;r<8;r++) for (int c=0;c<8;c++) if(board[r][c]==king) return isSquareAttacked(r,c,enemy);
  return false;
}

void getPossibleMoves(int row, int col, int &moveCount, int moves[][2]) {
  moveCount = 0;
  char piece = board[row][col];
  if (piece == ' ') return;
  char clr = (piece>='a'&&piece<='z') ? 'b' : 'w';
  char pt  = (piece>='a'&&piece<='z') ? piece - 32 : piece;

  auto canLand = [&](int r, int c) -> bool {
    if (r<0||r>=8||c<0||c>=8) return false;
    char t = board[r][c];
    return t == ' ' || ((t>='a'&&t<='z') != (clr=='b'));
  };
  auto pushMove = [&](int r, int c) {
    if (canLand(r,c)) { moves[moveCount][0]=r; moves[moveCount][1]=c; moveCount++; }
  };
  auto addLine = [&](int dr, int dc) {
    for (int s=1;s<8;s++) {
      int nr=row+s*dr, nc=col+s*dc;
      if (nr<0||nr>=8||nc<0||nc>=8) break;
      char t=board[nr][nc];
      if (t==' ') { moves[moveCount][0]=nr; moves[moveCount][1]=nc; moveCount++; }
      else { if((t>='a'&&t<='z')!=(clr=='b')){moves[moveCount][0]=nr;moves[moveCount][1]=nc;moveCount++;} break; }
    }
  };

  switch (pt) {
    case 'P': {
      int dir=(clr=='w')?1:-1, start=(clr=='w')?1:6;
      if (row+dir>=0&&row+dir<8&&board[row+dir][col]==' ') {
        moves[moveCount][0]=row+dir; moves[moveCount][1]=col; moveCount++;
        if (row==start&&board[row+2*dir][col]==' ') { moves[moveCount][0]=row+2*dir; moves[moveCount][1]=col; moveCount++; }
      }
      int capDc[2]={-1,1};
      for (int i=0;i<2;i++){int nr=row+dir,nc=col+capDc[i];if(nr>=0&&nr<8&&nc>=0&&nc<8){char t=board[nr][nc];if(t!=' '&&(t>='a'&&t<='z')!=(clr=='b')){moves[moveCount][0]=nr;moves[moveCount][1]=nc;moveCount++;}}}
      break;
    }
    case 'R': addLine(1,0);addLine(-1,0);addLine(0,1);addLine(0,-1); break;
    case 'B': addLine(1,1);addLine(1,-1);addLine(-1,1);addLine(-1,-1); break;
    case 'Q': addLine(1,0);addLine(-1,0);addLine(0,1);addLine(0,-1);
              addLine(1,1);addLine(1,-1);addLine(-1,1);addLine(-1,-1); break;
    case 'N': { int nd[8][2]={{2,1},{1,2},{-1,2},{-2,1},{-2,-1},{-1,-2},{1,-2},{2,-1}};for(int i=0;i<8;i++)pushMove(row+nd[i][0],col+nd[i][1]);break; }
    case 'K': { int kd[8][2]={{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};for(int i=0;i<8;i++)pushMove(row+kd[i][0],col+kd[i][1]);break; }
  }

  int legal[28][2]; int lc=0;
  for (int i=0;i<moveCount;i++) {
    int tr=moves[i][0], tc=moves[i][1];
    char save=board[tr][tc];
    board[tr][tc]=piece; board[row][col]=' ';
    if (!isInCheck(clr)) { legal[lc][0]=tr; legal[lc][1]=tc; lc++; }
    board[row][col]=piece; board[tr][tc]=save;
  }
  moveCount=lc;
  for (int i=0;i<lc;i++) { moves[i][0]=legal[i][0]; moves[i][1]=legal[i][1]; }
}

bool hasLegalMoves(char color) {
  for (int r=0;r<8;r++) for (int c=0;c<8;c++) {
    char p=board[r][c]; if(p==' ')continue;
    bool mine=(color=='w')?(p>='A'&&p<='Z'):(p>='a'&&p<='z');
    if (!mine) continue;
    int mc=0; int mv[28][2];
    getPossibleMoves(r,c,mc,mv);
    if (mc>0) return true;
  }
  return false;
}

void applyPromotion(int row, int col) {
  if (board[row][col]=='P' && row==7) { promotionAnimation(col); board[row][col]='Q'; }
  if (board[row][col]=='p' && row==0) { promotionAnimation(col); board[row][col]='q'; }
}

void initBoard() {
  for (int r=0;r<8;r++) for (int c=0;c<8;c++) board[r][c]=INITIAL[r][c];
}

// ═══════════════════════════════════════════════════════════════
// SETUP SCREEN — wait until all 32 pieces are placed
// ═══════════════════════════════════════════════════════════════

void waitForSetup() {
  while (true) {
    readSensors();
    bool allPlaced = true;
    for (int r=0;r<8;r++) for (int c=0;c<8;c++) {
      bool needed = (INITIAL[r][c] != ' ') && !sensorState[r][c];
      if (needed) allPlaced = false;
      strip.setPixelColor(r*8+c, needed ? strip.Color(0,0,0,80) : 0);
    }
    strip.show();
    if (allPlaced) break;
    delay(200);
  }

  // Brief green flash — ready to play
  for (int f=0;f<3;f++) {
    for (int i=0;i<LED_COUNT;i++) strip.setPixelColor(i, strip.Color(0,200,0,0));
    strip.show(); delay(150); strip.clear(); strip.show(); delay(100);
  }
  startupAnimation();
}

// ═══════════════════════════════════════════════════════════════
// GAME LOOP — one full turn for the current player
// Returns false if reset pressed
// ═══════════════════════════════════════════════════════════════

bool playTurn() {
  char myColor = whiteTurn ? 'w' : 'b';

  // Dim glow on active player's pieces
  strip.clear();
  for (int r=0;r<8;r++) for (int c=0;c<8;c++) {
    char p=board[r][c]; if(p==' ')continue;
    bool mine=(myColor=='w')?(p>='A'&&p<='Z'):(p>='a'&&p<='z');
    if (mine) strip.setPixelColor(r*8+c, strip.Color(0,0,0,40));
  }
  strip.show();

  while (true) {
    if (digitalRead(RESET_PIN)==LOW) { delay(50); if (digitalRead(RESET_PIN)==LOW) return false; }
    readSensors();

    for (int row=0; row<8; row++) {
      for (int col=0; col<8; col++) {
        if (!sensorPrev[row][col] || sensorState[row][col]) continue;

        // Piece lifted at (row, col)
        char piece = board[row][col];
        if (piece == ' ') continue;
        bool mine = (myColor=='w') ? (piece>='A'&&piece<='Z') : (piece>='a'&&piece<='z');

        if (!mine) {
          // Wrong colour — amber flash
          for (int f=0;f<3;f++) {
            strip.setPixelColor(row*8+col, strip.Color(200,150,0,0));
            strip.show(); delay(150);
            strip.setPixelColor(row*8+col, 0); strip.show(); delay(100);
          }
          continue;
        }

        int moveCount=0; int moves[28][2];
        getPossibleMoves(row, col, moveCount, moves);

        if (moveCount == 0) {
          // Pinned or no moves — flash amber, wait for return
          for (int f=0;f<3;f++) {
            strip.setPixelColor(row*8+col, strip.Color(200,150,0,0));
            strip.show(); delay(150);
            strip.setPixelColor(row*8+col, 0); strip.show(); delay(100);
          }
          while (!sensorState[row][col]) {
            readSensors();
            memcpy(sensorPrev, sensorState, sizeof(sensorState));
            delay(40);
          }
          memcpy(sensorPrev, sensorState, sizeof(sensorState));
          // Restore glow
          strip.clear();
          for (int r=0;r<8;r++) for (int c=0;c<8;c++) {
            char p=board[r][c]; if(p==' ')continue;
            bool m=(myColor=='w')?(p>='A'&&p<='Z'):(p>='a'&&p<='z');
            if (m) strip.setPixelColor(r*8+c, strip.Color(0,0,0,40));
          }
          strip.show();
          break;
        }

        // Highlight lifted square + legal targets with ripple
        strip.clear();
        strip.setPixelColor(row*8+col, strip.Color(0,0,0,255));
        // Sort by distance for ripple effect
        for (int i=0;i<moveCount-1;i++) for (int j=0;j<moveCount-i-1;j++) {
          int dr0=moves[j][0]-row, dc0=moves[j][1]-col;
          int dr1=moves[j+1][0]-row, dc1=moves[j+1][1]-col;
          if (dr0*dr0+dc0*dc0 > dr1*dr1+dc1*dc1) {
            int tmp0=moves[j][0], tmp1=moves[j][1];
            moves[j][0]=moves[j+1][0]; moves[j][1]=moves[j+1][1];
            moves[j+1][0]=tmp0; moves[j+1][1]=tmp1;
          }
        }
        for (int i=0;i<moveCount;i++) {
          int r2=moves[i][0], c2=moves[i][1];
          strip.setPixelColor(r2*8+c2, (board[r2][c2]!=' ') ? strip.Color(255,60,0,0) : strip.Color(0,0,0,180));
          strip.show(); delay(25);
        }
        strip.show();

        // Wait for placement
        int toRow=-1, toCol=-1;
        bool placed=false;
        while (!placed) {
          if (digitalRead(RESET_PIN)==LOW) { delay(50); if (digitalRead(RESET_PIN)==LOW) return false; }
          readSensors();

          if (sensorState[row][col]) {
            // Returned to origin — cancel
            toRow=-1; toCol=-1; placed=true;
            strip.clear();
            for (int r=0;r<8;r++) for (int c=0;c<8;c++) {
              char p=board[r][c]; if(p==' ')continue;
              bool m=(myColor=='w')?(p>='A'&&p<='Z'):(p>='a'&&p<='z');
              if (m) strip.setPixelColor(r*8+c, strip.Color(0,0,0,40));
            }
            strip.show();
            break;
          }

          for (int r2=0;r2<8&&!placed;r2++) for (int c2=0;c2<8&&!placed;c2++) {
            if (r2==row && c2==col) continue;
            if (!sensorState[r2][c2] || sensorPrev[r2][c2]) continue;
            bool legal=false;
            for (int i=0;i<moveCount;i++) if(moves[i][0]==r2&&moves[i][1]==c2){legal=true;break;}
            if (legal) {
              toRow=r2; toCol=c2; placed=true;
            } else {
              // Illegal square — amber flash, restore display
              strip.setPixelColor(r2*8+c2, strip.Color(200,150,0,0));
              strip.show(); delay(400);
              strip.clear();
              strip.setPixelColor(row*8+col, strip.Color(0,0,0,255));
              for (int i=0;i<moveCount;i++) {
                int mr=moves[i][0], mc=moves[i][1];
                strip.setPixelColor(mr*8+mc, (board[mr][mc]!=' ') ? strip.Color(255,60,0,0) : strip.Color(0,0,0,180));
              }
              strip.show();
            }
          }
          memcpy(sensorPrev, sensorState, sizeof(sensorState));
          delay(40);
        }

        if (toRow == -1) break; // cancelled — wait for next pickup

        // Commit the move
        bool isCapture = (board[toRow][toCol] != ' ');
        if (isCapture) captureAnimation(toRow, toCol);

        board[toRow][toCol] = piece;
        board[row][col] = ' ';
        applyPromotion(toRow, toCol);

        // Green confirmation blink
        strip.clear();
        strip.setPixelColor(row*8+col,   strip.Color(0, 80, 0, 0));
        strip.setPixelColor(toRow*8+toCol, strip.Color(0, 255, 0, 0));
        strip.show(); delay(500);
        strip.clear(); strip.show();

        readSensors();
        memcpy(sensorPrev, sensorState, sizeof(sensorState));
        return true; // move made, flip turn
      }
    }

    memcpy(sensorPrev, sensorState, sizeof(sensorState));
    delay(40);
  }
}

// ═══════════════════════════════════════════════════════════════
// SETUP
// ═══════════════════════════════════════════════════════════════

void setup() {
  pinMode(SER_PIN,   OUTPUT);
  pinMode(RCLK_PIN,  OUTPUT);
  pinMode(SRCLK_PIN, OUTPUT);
  for (int i=0;i<8;i++) pinMode(ROW_PINS[i], INPUT_PULLUP);
  pinMode(RESET_PIN, INPUT_PULLUP);

  strip.begin();
  strip.setBrightness(BRIGHTNESS);
  strip.clear(); strip.show();
}

// ═══════════════════════════════════════════════════════════════
// MAIN LOOP
// ═══════════════════════════════════════════════════════════════

void loop() {
  initBoard();
  whiteTurn = true;

  waitForSetup();

  readSensors();
  memcpy(sensorPrev, sensorState, sizeof(sensorState));

  while (true) {
    // Reset button
    if (digitalRead(RESET_PIN)==LOW) { delay(50); if (digitalRead(RESET_PIN)==LOW) { strip.clear(); strip.show(); return; } }

    bool ok = playTurn();
    if (!ok) { strip.clear(); strip.show(); return; }

    whiteTurn = !whiteTurn;

    // Check for checkmate or stalemate
    char nextColor = whiteTurn ? 'w' : 'b';
    if (!hasLegalMoves(nextColor)) {
      bool inCheck = isInCheck(nextColor);
      if (inCheck) {
        // Checkmate — loser's king flashes, then game-over animation
        for (int f=0;f<8;f++) {
          for (int i=0;i<LED_COUNT;i++) strip.setPixelColor(i, strip.Color(255,0,0,0));
          strip.show(); delay(200); strip.clear(); strip.show(); delay(180);
        }
        gameOverAnimation(whiteTurn); // current whiteTurn is the winner
      } else {
        // Stalemate — full board amber pulse
        for (int f=0;f<6;f++) {
          for (int i=0;i<LED_COUNT;i++) strip.setPixelColor(i, strip.Color(200,150,0,0));
          strip.show(); delay(300); strip.clear(); strip.show(); delay(200);
        }
      }
      delay(3000);
      return; // restart
    }

    // Flash check warning on the player whose turn it now is
    if (isInCheck(nextColor)) checkFlash();
  }
}
