// NING NING - by Claude - inspired by Mark
// Simple Meshtastic BLE client for the 2.8" CYD (ESP32-2432S028R)
//  - splash screen
//  - 4-corner touch calibration (first boot, or touch screen during splash)
//  - BLE scan / pair (PIN pad) / auto-reconnect to saved node
//  - two channel views: PRIMARY (ch 0) and SECONDARY (ch 1)
//  - big on-screen QWERTY keyboard, send + receive text

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <vector>

// ---------------------------------------------------------------- pins
#define TOUCH_CS   33
#define TOUCH_CLK  25
#define TOUCH_MOSI 32
#define TOUCH_MISO 39

// ---------------------------------------------------------------- Meshtastic BLE
static const char* UUID_SVC      = "6ba1b218-15a8-461f-9fa8-5dcae273eafd";
static const char* UUID_TORADIO  = "f75c76d2-129e-4dad-a1dd-7866124401e7";
static const char* UUID_FROMRADIO= "2c55e69e-4993-11ed-b878-0242ac120002";
static const char* UUID_FROMNUM  = "ed9da18c-a800-4f66-a670-aa7547e34453";

// ---------------------------------------------------------------- layout (320x240 landscape)
#define SCR_W   320
#define SCR_H   240
#define BAR_H   22
#define MSG_Y   22
#define MSG_H   78
#define INPUT_Y 100
#define INPUT_H 20
#define KB_Y    120
#define KEY_H   30
#define MAX_TEXT 200

#define COL_BG     TFT_BLACK
#define COL_KEY    0x39E7
#define COL_KEYSP  0x2945
#define COL_INPUT  0x2104

TFT_eSPI tft;
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(TOUCH_CS);
Preferences prefs;

// ---------------------------------------------------------------- state
enum Screen { SCR_CHAT, SCR_SCAN };
Screen screen = SCR_SCAN;
int curCh = 0;
bool unread[2] = {false, false};
String inputText;
int kbLayer = 0;                 // 0 lower, 1 upper, 2 symbols
bool connectedBLE = false;
bool syncing = false;
uint32_t syncStart = 0;
volatile bool dataAvail = false;
bool msgDirty = false;
uint32_t myNum = 0;

String savedAddr, savedName;
uint8_t savedType = 0;

// ---------------------------------------------------------------- touch calibration
struct Cal { float a0, a1, b0, b1; bool swap; bool valid; } cal = {0, 0, 0, 0, false, false};

bool readRaw(int &rx, int &ry) {
  if (!ts.touched()) return false;
  TS_Point p = ts.getPoint();
  rx = p.x; ry = p.y;
  return true;
}

void mapTouch(int rx, int ry, int &sx, int &sy) {
  float A = cal.swap ? ry : rx;
  float B = cal.swap ? rx : ry;
  float fx = 20.0f + (A - cal.a0) * 280.0f / (cal.a1 - cal.a0);
  float fy = 20.0f + (B - cal.b0) * 200.0f / (cal.b1 - cal.b0);
  sx = constrain((int)fx, 0, SCR_W - 1);
  sy = constrain((int)fy, 0, SCR_H - 1);
}

// returns true once per touch-down, with screen coordinates
bool getTap(int &x, int &y) {
  static bool down = false;
  static uint32_t lastTouch = 0;
  int rx, ry;
  bool t = readRaw(rx, ry);
  uint32_t now = millis();
  if (t) {
    lastTouch = now;
    if (!down) {
      down = true;
      mapTouch(rx, ry, x, y);
      return true;
    }
  } else if (down && now - lastTouch > 40) {
    down = false;
  }
  return false;
}

void saveCal() {
  prefs.putFloat("a0", cal.a0); prefs.putFloat("a1", cal.a1);
  prefs.putFloat("b0", cal.b0); prefs.putFloat("b1", cal.b1);
  prefs.putBool("swap", cal.swap); prefs.putBool("calok", true);
}

void loadCal() {
  cal.valid = prefs.getBool("calok", false);
  cal.a0 = prefs.getFloat("a0", 0); cal.a1 = prefs.getFloat("a1", 0);
  cal.b0 = prefs.getFloat("b0", 0); cal.b1 = prefs.getFloat("b1", 0);
  cal.swap = prefs.getBool("swap", false);
}

void runCalibration() {
  const int tx[4] = {20, 300, 300, 20};
  const int ty[4] = {20, 20, 220, 220};
  const char* names[4] = {"TOP-LEFT", "TOP-RIGHT", "BOTTOM-RIGHT", "BOTTOM-LEFT"};
  float rawX[4], rawY[4];
  bool ok = false;
  while (!ok) {
    for (int i = 0; i < 4; i++) {
      tft.fillScreen(COL_BG);
      tft.setTextDatum(MC_DATUM);
      tft.setTextFont(2);
      tft.setTextColor(TFT_WHITE, COL_BG);
      tft.drawString("TOUCH CALIBRATION", 160, 100);
      String s = String("Tap the cross: ") + names[i];
      tft.drawString(s, 160, 125);
      tft.drawLine(tx[i] - 12, ty[i], tx[i] + 12, ty[i], TFT_RED);
      tft.drawLine(tx[i], ty[i] - 12, tx[i], ty[i] + 12, TFT_RED);
      tft.drawCircle(tx[i], ty[i], 6, TFT_RED);
      int rx, ry;
      while (readRaw(rx, ry)) delay(10);      // wait for release
      delay(250);
      while (!readRaw(rx, ry)) delay(10);     // wait for press
      long sx = 0, sy = 0; int n = 0;
      uint32_t t0 = millis();
      while (millis() - t0 < 150) {
        if (readRaw(rx, ry)) { sx += rx; sy += ry; n++; }
        delay(5);
      }
      if (n == 0) n = 1;
      rawX[i] = (float)sx / n; rawY[i] = (float)sy / n;
    }
    float leftX = (rawX[0] + rawX[3]) / 2, rightX = (rawX[1] + rawX[2]) / 2;
    float leftY = (rawY[0] + rawY[3]) / 2, rightY = (rawY[1] + rawY[2]) / 2;
    float topX = (rawX[0] + rawX[1]) / 2, botX = (rawX[2] + rawX[3]) / 2;
    float topY = (rawY[0] + rawY[1]) / 2, botY = (rawY[2] + rawY[3]) / 2;
    cal.swap = fabsf(rightY - leftY) > fabsf(rightX - leftX);
    if (!cal.swap) { cal.a0 = leftX; cal.a1 = rightX; cal.b0 = topY; cal.b1 = botY; }
    else           { cal.a0 = leftY; cal.a1 = rightY; cal.b0 = topX; cal.b1 = botX; }
    ok = fabsf(cal.a1 - cal.a0) > 500 && fabsf(cal.b1 - cal.b0) > 500;
    if (!ok) {
      tft.fillScreen(COL_BG);
      tft.setTextColor(TFT_RED, COL_BG);
      tft.drawString("Bad readings - try again", 160, 120);
      delay(1500);
    }
  }
  cal.valid = true;
  saveCal();
  tft.fillScreen(COL_BG);
  tft.setTextColor(TFT_GREEN, COL_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(4);
  tft.drawString("CALIBRATED", 160, 120);
  delay(800);
}

// ---------------------------------------------------------------- tiny protobuf helpers
struct PB {
  uint8_t b[320]; size_t n = 0;
  void varint(uint64_t v) { while (v > 0x7F) { b[n++] = (v & 0x7F) | 0x80; v >>= 7; } b[n++] = (uint8_t)v; }
  void tag(int f, int wt) { varint(((uint64_t)f << 3) | wt); }
  void u32(int f, uint32_t v) { tag(f, 0); varint(v); }
  void fx32(int f, uint32_t v) { tag(f, 5); for (int i = 0; i < 4; i++) b[n++] = (v >> (8 * i)) & 0xFF; }
  void bytes(int f, const uint8_t* p, size_t l) { tag(f, 2); varint(l); memcpy(b + n, p, l); n += l; }
};

struct Rd {
  const uint8_t* p; size_t n; size_t i;
  Rd(const uint8_t* p_, size_t n_) : p(p_), n(n_), i(0) {}
  bool varint(uint64_t &v) {
    v = 0; int s = 0;
    while (i < n) {
      uint8_t c = p[i++];
      v |= (uint64_t)(c & 0x7F) << s;
      if (!(c & 0x80)) return true;
      s += 7; if (s > 63) return false;
    }
    return false;
  }
  bool next(uint32_t &f, int &wt, uint64_t &v, const uint8_t* &d, size_t &dl) {
    uint64_t t;
    if (!varint(t)) return false;
    f = (uint32_t)(t >> 3); wt = (int)(t & 7); v = 0; d = nullptr; dl = 0;
    switch (wt) {
      case 0: return varint(v);
      case 1: if (n - i < 8) return false; for (int k = 0; k < 8; k++) v |= (uint64_t)p[i + k] << (8 * k); i += 8; return true;
      case 2: { uint64_t l; if (!varint(l) || l > n - i) return false; d = p + i; dl = (size_t)l; i += (size_t)l; return true; }
      case 5: if (n - i < 4) return false; for (int k = 0; k < 4; k++) v |= (uint64_t)p[i + k] << (8 * k); i += 4; return true;
      default: return false;
    }
  }
};

// ---------------------------------------------------------------- chat storage
#define MAXMSG 30
struct Msg { char text[240]; uint16_t col; };
Msg chat[2][MAXMSG];
int chatCount[2] = {0, 0};

void addMsg(int ch, const String &s, uint16_t col) {
  if (ch < 0 || ch > 1) ch = 1;
  if (chatCount[ch] == MAXMSG) {
    memmove(&chat[ch][0], &chat[ch][1], sizeof(Msg) * (MAXMSG - 1));
    chatCount[ch]--;
  }
  Msg &m = chat[ch][chatCount[ch]++];
  strncpy(m.text, s.c_str(), sizeof(m.text) - 1);
  m.text[sizeof(m.text) - 1] = 0;
  m.col = col;
}

// node short names
struct NodeName { uint32_t num; char name[8]; };
NodeName nodes[80];
int nodeCount = 0;

String sanitize(const uint8_t* d, size_t l) {
  String s; bool lastQ = false;
  for (size_t i = 0; i < l; i++) {
    uint8_t c = d[i];
    if (c >= 32 && c < 127) { s += (char)c; lastQ = false; }
    else if (c == '\n' || c == '\r') { s += ' '; lastQ = false; }
    else if (!lastQ) { s += '?'; lastQ = true; }
  }
  return s;
}

void setNodeName(uint32_t num, const String &nm) {
  for (int i = 0; i < nodeCount; i++)
    if (nodes[i].num == num) { strncpy(nodes[i].name, nm.c_str(), 7); nodes[i].name[7] = 0; return; }
  if (nodeCount < 80) {
    nodes[nodeCount].num = num;
    strncpy(nodes[nodeCount].name, nm.c_str(), 7); nodes[nodeCount].name[7] = 0;
    nodeCount++;
  }
}

String nameOf(uint32_t num) {
  for (int i = 0; i < nodeCount; i++) if (nodes[i].num == num && nodes[i].name[0]) return String(nodes[i].name);
  return String(num & 0xFFFF, HEX);
}

// ---------------------------------------------------------------- FromRadio parsing
void onText(uint32_t from, uint32_t to, uint32_t ch, const uint8_t* d, size_t dl) {
  String t = sanitize(d, dl);
  String line = nameOf(from) + ": " + t;
  if (to != 0xFFFFFFFF) line = "(DM) " + line;
  int idx = (ch == 0) ? 0 : 1;
  if (ch > 1) line = "[c" + String(ch) + "] " + line;
  addMsg(idx, line, TFT_WHITE);
  if (!(screen == SCR_CHAT && curCh == idx)) unread[idx] = true;
  msgDirty = true;
}

void parseData(uint32_t from, uint32_t to, uint32_t ch, const uint8_t* d, size_t dl) {
  Rd r(d, dl);
  uint32_t f; int wt; uint64_t v; const uint8_t* dd; size_t ddl;
  uint32_t port = 0; const uint8_t* pl = nullptr; size_t pll = 0;
  while (r.next(f, wt, v, dd, ddl)) {
    if (f == 1 && wt == 0) port = (uint32_t)v;
    else if (f == 2 && wt == 2) { pl = dd; pll = ddl; }
  }
  if (port == 1 && pl) onText(from, to, ch, pl, pll);   // TEXT_MESSAGE_APP
}

void parsePacket(const uint8_t* d, size_t dl) {
  Rd r(d, dl);
  uint32_t f; int wt; uint64_t v; const uint8_t* dd; size_t ddl;
  uint32_t from = 0, to = 0xFFFFFFFF, ch = 0; const uint8_t* dec = nullptr; size_t decl = 0;
  while (r.next(f, wt, v, dd, ddl)) {
    if (f == 1 && wt == 5) from = (uint32_t)v;
    else if (f == 2 && wt == 5) to = (uint32_t)v;
    else if (f == 3 && wt == 0) ch = (uint32_t)v;
    else if (f == 4 && wt == 2) { dec = dd; decl = ddl; }
  }
  if (dec) parseData(from, to, ch, dec, decl);
}

void parseNodeInfo(const uint8_t* d, size_t dl) {
  Rd r(d, dl);
  uint32_t f; int wt; uint64_t v; const uint8_t* dd; size_t ddl;
  uint32_t num = 0; String shortName;
  while (r.next(f, wt, v, dd, ddl)) {
    if (f == 1 && wt == 0) num = (uint32_t)v;
    else if (f == 2 && wt == 2) {
      Rd u(dd, ddl);
      uint32_t f2; int wt2; uint64_t v2; const uint8_t* d2; size_t l2;
      while (u.next(f2, wt2, v2, d2, l2)) if (f2 == 3 && wt2 == 2) shortName = sanitize(d2, l2);
    }
  }
  if (num && shortName.length()) setNodeName(num, shortName);
}

void handleFromRadio(const uint8_t* d, size_t dl) {
  Rd r(d, dl);
  uint32_t f; int wt; uint64_t v; const uint8_t* dd; size_t ddl;
  while (r.next(f, wt, v, dd, ddl)) {
    if (f == 2 && wt == 2) parsePacket(dd, ddl);
    else if (f == 3 && wt == 2) {                    // my_info
      Rd m(dd, ddl); uint32_t f2; int wt2; uint64_t v2; const uint8_t* d2; size_t l2;
      while (m.next(f2, wt2, v2, d2, l2)) if (f2 == 1 && wt2 == 0) myNum = (uint32_t)v2;
    }
    else if (f == 4 && wt == 2) parseNodeInfo(dd, ddl);
    else if (f == 7 && wt == 0) syncing = false;     // config_complete_id
  }
}

// ---------------------------------------------------------------- BLE
NimBLEClient* client = nullptr;
NimBLERemoteCharacteristic *chTo = nullptr, *chFrom = nullptr, *chNum = nullptr;

uint32_t promptPin();

class ClientCB : public NimBLEClientCallbacks {
  uint32_t onPassKeyRequest() override { return promptPin(); }
  void onDisconnect(NimBLEClient* c) override { connectedBLE = false; }
};
ClientCB ccb;

void notifyCB(NimBLERemoteCharacteristic*, uint8_t*, size_t, bool) { dataAvail = true; }

void showStatus(const char* msg, uint16_t col = TFT_YELLOW) {
  tft.fillRect(0, 100, SCR_W, 40, COL_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(col, COL_BG);
  tft.drawString(msg, 160, 120);
}

bool connectTo(const String &addr, uint8_t type) {
  tft.fillScreen(COL_BG);
  showStatus("Connecting...");
  if (!client) {
    client = NimBLEDevice::createClient();
    client->setClientCallbacks(&ccb, false);
    client->setConnectTimeout(10);
  }
  if (client->isConnected()) client->disconnect();
  if (!client->connect(NimBLEAddress(std::string(addr.c_str()), type), true)) {
    showStatus("Connect failed", TFT_RED); delay(1500); return false;
  }
  showStatus("Pairing / securing...");
  client->secureConnection();      // triggers PIN prompt on first pairing
  NimBLERemoteService* svc = client->getService(UUID_SVC);
  if (!svc) { showStatus("Not a Meshtastic node", TFT_RED); client->disconnect(); delay(1500); return false; }
  chTo   = svc->getCharacteristic(UUID_TORADIO);
  chFrom = svc->getCharacteristic(UUID_FROMRADIO);
  chNum  = svc->getCharacteristic(UUID_FROMNUM);
  if (!chTo || !chFrom) { showStatus("Characteristics missing", TFT_RED); client->disconnect(); delay(1500); return false; }
  if (chNum && chNum->canNotify()) chNum->subscribe(true, notifyCB);

  // ask the node for its config / node list, then we keep reading FromRadio
  PB tr; tr.u32(3, esp_random() | 1);      // ToRadio.want_config_id
  chTo->writeValue(tr.b, tr.n, true);
  syncing = true; syncStart = millis(); dataAvail = true;
  connectedBLE = true;
  return true;
}

bool sendText(int ch, const String &txt) {
  if (!connectedBLE || !chTo) return false;
  PB d;  d.u32(1, 1); d.bytes(2, (const uint8_t*)txt.c_str(), txt.length());   // Data{portnum=TEXT, payload}
  PB mp; mp.fx32(2, 0xFFFFFFFF); mp.u32(3, ch); mp.bytes(4, d.b, d.n);          // MeshPacket{to,channel,decoded}
  mp.fx32(6, esp_random()); mp.u32(9, 3);                                         // id, hop_limit
  PB tr; tr.bytes(1, mp.b, mp.n);                                                 // ToRadio{packet}
  return chTo->writeValue(tr.b, tr.n, true);
}

void pollRadio() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if (syncing && now - syncStart > 20000) syncing = false;
  if (!(dataAvail || syncing || now - last > 2000)) return;
  last = now; dataAvail = false;
  int k;
  for (k = 0; k < 8; k++) {
    std::string v = chFrom->readValue();
    if (v.empty()) break;
    handleFromRadio((const uint8_t*)v.data(), v.size());
  }
  if (k == 8) dataAvail = true;
}

// ---------------------------------------------------------------- PIN pad
uint32_t promptPin() {
  String pin;
  const char* lab[12] = {"1","2","3","4","5","6","7","8","9","DEL","0","OK"};
  auto draw = [&]() {
    tft.fillScreen(COL_BG);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(2); tft.setTextColor(TFT_YELLOW, COL_BG);
    tft.drawString("Enter PIN shown on the node", 160, 10);
    tft.setTextFont(4); tft.setTextColor(TFT_WHITE, COL_BG);
    tft.drawString(pin.length() ? pin : String("------"), 160, 38);
    for (int i = 0; i < 12; i++) {
      int x = 10 + (i % 3) * 100, y = 62 + (i / 3) * 44;
      tft.fillRoundRect(x, y, 96, 40, 6, i == 11 ? TFT_DARKGREEN : COL_KEY);
      tft.setTextFont(4); tft.setTextColor(TFT_WHITE);
      tft.drawString(lab[i], x + 48, y + 20);
    }
  };
  draw();
  uint32_t t0 = millis();
  while (millis() - t0 < 60000) {
    int x, y;
    if (getTap(x, y) && y >= 62 && x >= 10 && x < 310) {
      int col = (x - 10) / 100, row = (y - 62) / 44;
      if (col > 2 || row > 3) continue;
      int i = row * 3 + col;
      if (i == 9) { if (pin.length()) pin.remove(pin.length() - 1); }
      else if (i == 11) { if (pin.length()) return (uint32_t)pin.toInt(); }
      else if (pin.length() < 6) pin += lab[i];
      draw();
    }
    delay(10);
  }
  return 0;
}

// ---------------------------------------------------------------- chat UI
struct Key { int16_t x, y, w, h; char c; uint8_t code; };
enum { K_CHAR = 0, K_SHIFT, K_BKSP, K_SYM, K_SEND, K_SPACE };
Key keys[40]; int nkeys = 0;

void addKey(int x, int row, int w, char c, uint8_t code) {
  keys[nkeys++] = {(int16_t)x, (int16_t)(KB_Y + row * KEY_H), (int16_t)w, (int16_t)KEY_H, c, code};
}

void buildKeys() {
  nkeys = 0;
  const char *r1, *r2, *r3;
  if (kbLayer == 2) { r1 = "1234567890"; r2 = "@#&*-+=()"; r3 = "!?'\":;/"; }
  else              { r1 = "qwertyuiop"; r2 = "asdfghjkl"; r3 = "zxcvbnm"; }
  auto fix = [&](char c) { return (kbLayer == 1 && c >= 'a' && c <= 'z') ? (char)(c - 32) : c; };
  for (int i = 0; i < 10; i++) addKey(i * 32, 0, 32, fix(r1[i]), K_CHAR);
  for (int i = 0; i < 9; i++)  addKey(16 + i * 32, 1, 32, fix(r2[i]), K_CHAR);
  addKey(0, 2, 48, 0, K_SHIFT);
  for (int i = 0; i < 7; i++)  addKey(48 + i * 32, 2, 32, fix(r3[i]), K_CHAR);
  addKey(272, 2, 48, 0, K_BKSP);
  addKey(0, 3, 48, 0, K_SYM);
  addKey(48, 3, 32, ',', K_CHAR);
  addKey(80, 3, 128, ' ', K_SPACE);
  addKey(208, 3, 32, '.', K_CHAR);
  addKey(240, 3, 80, 0, K_SEND);
}

void drawKey(const Key &k, bool pressed) {
  uint16_t bg = (k.code == K_CHAR || k.code == K_SPACE) ? COL_KEY : COL_KEYSP;
  if (k.code == K_SEND) bg = TFT_DARKGREEN;
  if (k.code == K_SHIFT && kbLayer == 1) bg = TFT_BLUE;
  uint16_t fg = TFT_WHITE;
  if (pressed) { bg = TFT_WHITE; fg = TFT_BLACK; }
  tft.fillRoundRect(k.x + 1, k.y + 1, k.w - 2, k.h - 2, 4, bg);
  tft.setTextFont(2); tft.setTextDatum(MC_DATUM); tft.setTextColor(fg, bg);
  String lab;
  switch (k.code) {
    case K_SHIFT: lab = "Shift"; break;
    case K_BKSP:  lab = "DEL"; break;
    case K_SYM:   lab = (kbLayer == 2) ? "ABC" : "123"; break;
    case K_SEND:  lab = "SEND"; break;
    case K_SPACE: lab = "space"; break;
    default:      lab = String(k.c);
  }
  tft.drawString(lab, k.x + k.w / 2, k.y + k.h / 2);
}

void drawKeyboard() { buildKeys(); for (int i = 0; i < nkeys; i++) drawKey(keys[i], false); }

void drawInput() {
  tft.fillRect(0, INPUT_Y, SCR_W, INPUT_H, COL_INPUT);
  tft.setTextFont(2); tft.setTextDatum(ML_DATUM); tft.setTextColor(TFT_WHITE, COL_INPUT);
  String s = inputText + "_";
  while (tft.textWidth(s) > 312) s.remove(0, 1);
  tft.drawString(s, 4, INPUT_Y + INPUT_H / 2);
}

void drawBar() {
  const char* n[2] = {"PRIMARY", "SECONDARY"};
  for (int i = 0; i < 2; i++) {
    uint16_t bg = (curCh == i) ? TFT_BLUE : COL_KEYSP;
    tft.fillRect(i * 105, 0, 105, BAR_H, bg);
    tft.setTextFont(2); tft.setTextDatum(MC_DATUM); tft.setTextColor(TFT_WHITE, bg);
    String s = String(n[i]) + (unread[i] ? " *" : "");
    tft.drawString(s, i * 105 + 52, BAR_H / 2);
  }
  uint16_t bg = connectedBLE ? TFT_DARKGREEN : TFT_MAROON;
  tft.fillRect(210, 0, 110, BAR_H, bg);
  tft.setTextFont(2); tft.setTextDatum(MC_DATUM); tft.setTextColor(TFT_WHITE, bg);
  tft.drawString(connectedBLE ? (syncing ? "BT: sync" : "BT: OK") : "BT: OFF", 265, BAR_H / 2);
}

void drawMessages() {
  tft.fillRect(0, MSG_Y, SCR_W, MSG_H, COL_BG);
  struct L { String s; uint16_t c; };
  std::vector<L> lines;
  for (int i = 0; i < chatCount[curCh]; i++) {
    String t = chat[curCh][i].text; uint16_t c = chat[curCh][i].col;
    bool first = true;
    while (t.length()) {
      int cap = first ? 52 : 50;
      String part;
      if ((int)t.length() <= cap) { part = t; t = ""; }
      else {
        int cut = t.lastIndexOf(' ', cap);
        if (cut < cap / 2) cut = cap;
        part = t.substring(0, cut);
        t = t.substring(cut); t.trim();
      }
      lines.push_back({first ? part : String("  ") + part, c});
      first = false;
    }
  }
  int start = (int)lines.size() - 8; if (start < 0) start = 0;
  tft.setTextFont(1); tft.setTextSize(1); tft.setTextDatum(TL_DATUM);
  for (int i = start; i < (int)lines.size(); i++) {
    tft.setTextColor(lines[i].c, COL_BG);
    tft.drawString(lines[i].s, 2, MSG_Y + 3 + (i - start) * 9);
  }
}

void drawChat() {
  tft.fillScreen(COL_BG);
  drawBar(); drawMessages(); drawInput(); drawKeyboard();
}

void tapChat(int x, int y) {
  if (y < BAR_H) {
    if (x < 105) { curCh = 0; unread[0] = false; drawBar(); drawMessages(); }
    else if (x < 210) { curCh = 1; unread[1] = false; drawBar(); drawMessages(); }
    else { screen = SCR_SCAN; /* scan screen drawn by caller */ }
    return;
  }
  if (y < KB_Y) return;
  for (int i = 0; i < nkeys; i++) {
    Key &k = keys[i];
    if (x >= k.x && x < k.x + k.w && y >= k.y && y < k.y + k.h) {
      drawKey(k, true); delay(40);
      switch (k.code) {
        case K_CHAR:
        case K_SPACE:
          if (inputText.length() < MAX_TEXT) inputText += k.c;
          if (kbLayer == 1) kbLayer = 0;
          drawKeyboard(); drawInput(); break;
        case K_BKSP: if (inputText.length()) inputText.remove(inputText.length() - 1); drawKey(k, false); drawInput(); break;
        case K_SHIFT: if (kbLayer != 2) kbLayer = kbLayer ? 0 : 1; drawKeyboard(); break;
        case K_SYM: kbLayer = (kbLayer == 2) ? 0 : 2; drawKeyboard(); break;
        case K_SEND:
          if (inputText.length()) {
            if (sendText(curCh, inputText)) { addMsg(curCh, "Me: " + inputText, TFT_CYAN); inputText = ""; drawMessages(); }
            else { addMsg(curCh, "! not sent - not connected", TFT_RED); drawMessages(); }
            drawInput();
          }
          drawKey(k, false); break;
      }
      return;
    }
  }
}

// ---------------------------------------------------------------- scan screen
struct Dev { String name, addr; uint8_t type; };
std::vector<Dev> devs;

void drawScan() {
  tft.fillScreen(COL_BG);
  tft.setTextFont(2); tft.setTextDatum(MC_DATUM); tft.setTextColor(TFT_YELLOW, COL_BG);
  tft.drawString("SELECT MESHTASTIC NODE", 160, 10);
  for (int i = 0; i < (int)devs.size() && i < 5; i++) {
    int y = 24 + i * 34;
    tft.fillRoundRect(4, y, 312, 31, 5, COL_KEY);
    tft.setTextDatum(ML_DATUM); tft.setTextColor(TFT_WHITE, COL_KEY);
    tft.drawString(devs[i].name + "  " + devs[i].addr.substring(9), 12, y + 15);
  }
  if (devs.empty()) { tft.setTextDatum(MC_DATUM); tft.setTextColor(TFT_LIGHTGREY, COL_BG); tft.drawString("No nodes found - RESCAN", 160, 100); }
  const char* b[4] = {"RESCAN", "CALIB", "FORGET", "BACK"};
  for (int i = 0; i < 4; i++) {
    tft.fillRoundRect(i * 80 + 2, 200, 76, 36, 6, i == 3 && !connectedBLE ? COL_BG : COL_KEYSP);
    if (i == 3 && !connectedBLE) continue;
    tft.setTextDatum(MC_DATUM); tft.setTextColor(TFT_WHITE, COL_KEYSP);
    tft.drawString(b[i], i * 80 + 40, 218);
  }
}

void doScan() {
  tft.fillScreen(COL_BG);
  showStatus("Scanning (6s)...");
  devs.clear();
  if (savedAddr.length()) devs.push_back({savedName.length() ? savedName : String("Saved node"), savedAddr, savedType});
  NimBLEScan* sc = NimBLEDevice::getScan();
  sc->setActiveScan(true); sc->setInterval(100); sc->setWindow(99);
  NimBLEScanResults res = sc->start(6, false);
  for (int i = 0; i < res.getCount() && devs.size() < 5; i++) {
    NimBLEAdvertisedDevice d = res.getDevice(i);
    String nm = String(d.getName().c_str());
    if (!(d.isAdvertisingService(NimBLEUUID(UUID_SVC)) || nm.startsWith("Meshtastic"))) continue;
    String a = String(d.getAddress().toString().c_str());
    bool dup = false; for (auto &e : devs) if (e.addr.equalsIgnoreCase(a)) dup = true;
    if (!dup) devs.push_back({nm.length() ? nm : String("Meshtastic"), a, d.getAddress().getType()});
  }
  sc->clearResults();
  drawScan();
}

void enterChat() {
  screen = SCR_CHAT;
  drawChat();
}

void tapScan(int x, int y) {
  if (y >= 200) {
    int b = x / 80;
    if (b == 0) doScan();
    else if (b == 1) { runCalibration(); drawScan(); }
    else if (b == 2) {
      NimBLEDevice::deleteAllBonds();
      prefs.remove("addr"); savedAddr = ""; savedName = "";
      showStatus("Bonds + saved node cleared", TFT_GREEN); delay(1200); doScan();
    }
    else if (b == 3 && connectedBLE) enterChat();
    return;
  }
  int i = (y - 24) / 34;
  if (y >= 24 && i >= 0 && i < (int)devs.size()) {
    Dev d = devs[i];
    if (connectTo(d.addr, d.type)) {
      savedAddr = d.addr; savedName = d.name; savedType = d.type;
      prefs.putString("addr", savedAddr); prefs.putString("name", savedName); prefs.putUChar("atype", savedType);
      enterChat();
    } else drawScan();
  }
}

// ---------------------------------------------------------------- setup / loop
void splash() {
  tft.fillScreen(COL_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(4); tft.setTextSize(2);
  tft.setTextColor(TFT_CYAN, COL_BG);
  tft.drawString("NING NING", 160, 70);
  tft.setTextSize(1);
  tft.setTextColor(tft.color565(217, 119, 87), COL_BG);
  tft.drawString("BY CLAUDE", 160, 130);
  tft.setTextFont(2);
  tft.setTextColor(TFT_LIGHTGREY, COL_BG);
  tft.drawString("INSPIRED BY MARK", 160, 165);
  tft.setTextFont(1);
  tft.setTextColor(TFT_DARKGREY, COL_BG);
  tft.drawString("touch screen now to recalibrate", 160, 225);
}

void setup() {
  Serial.begin(115200);
  pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, HIGH);
  tft.init(); tft.setRotation(1); tft.fillScreen(COL_BG);
  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  ts.begin(touchSPI);
  prefs.begin("ningning", false);
  loadCal();
  savedAddr = prefs.getString("addr", ""); savedName = prefs.getString("name", ""); savedType = prefs.getUChar("atype", 0);

  splash();
  bool forceCal = false;
  uint32_t t0 = millis();
  while (millis() - t0 < 2500) { int rx, ry; if (readRaw(rx, ry)) forceCal = true; delay(10); }
  if (!cal.valid || forceCal) runCalibration();

  NimBLEDevice::init("NingNing");
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);
  NimBLEDevice::setMTU(512);

  if (savedAddr.length() && connectTo(savedAddr, savedType)) enterChat();
  else { screen = SCR_SCAN; doScan(); }
}

void loop() {
  int x, y;
  if (getTap(x, y)) {
    if (screen == SCR_CHAT) { tapChat(x, y); if (screen == SCR_SCAN) doScan(); }
    else tapScan(x, y);
  }
  if (screen == SCR_CHAT) {
    static bool shown = false;
    if (connectedBLE != shown) { shown = connectedBLE; drawBar(); }
    if (connectedBLE) {
      bool wasSync = syncing;
      pollRadio();
      if (wasSync && !syncing) drawBar();
    }
    if (msgDirty) { msgDirty = false; drawMessages(); drawBar(); }
  }
  delay(5);
}
