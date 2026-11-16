/*
 IN-12-style Nixie NTP clock with sealed glass tips, corrected 2/5 cathodes and touchscreen setup
 ESP32-2432S028 / 320x240 / Arduino IDE

 Libraries (Arduino Library Manager):
   TFT_eSPI by Bodmer (configure User_Setup.h for CYD ILI9341, HSPI)
   XPT2046_Touchscreen by Paul Stoffregen
 Built-in: WiFi, SPI, Preferences, time

 TFT: SCLK14 MOSI13 MISO12 CS15 DC2 BL21
 Touch: SCLK25 MOSI32 MISO39 CS33 IRQ36 (separate VSPI bus)
 First boot: 4-point touch calibration, saved in Preferences.
 Tap any Nixie tube to open settings.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

#define BL_PIN 21
#define LDR_PIN 34

// Most CYD boards: covered LDR gives a higher ADC reading.
// Set false if your board reports higher readings in bright light.
const bool LDR_DARK_HIGH = true;
const int LDR_DARK_RAW = 500;  // tune using the live ADC value on settings screen
const int LDR_BRIGHT_RAW = 0;
const uint8_t AUTO_MIN_BRIGHTNESS = 30;
const uint8_t AUTO_MAX_BRIGHTNESS = 235;
#define TOUCH_CS 33
#define TOUCH_IRQ 36
#define TOUCH_CLK 25
#define TOUCH_MISO 39
#define TOUCH_MOSI 32

const int W = 320, H = 240;
const int TW = 44, TH = 128, TY = 51;
const int TUBE_X[6] = {6,58,110,162,214,266};

// Anode cage appearance: 0=off, 1=subtle, 2=normal, 3=strong.
// The CYD has only 44 pixels per tube, so the wire-mesh is deliberately
// simplified into a fine hexagonal lattice with a dotted, low-contrast finish.
const uint8_t MESH_LEVEL = 2;


TFT_eSPI tft;
TFT_eSprite tube(&tft);
SPIClass touchSPI(VSPI);  // CYD screen uses HSPI; touch uses VSPI
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);
Preferences prefs;

// Settings stored in flash; initial Wi-Fi credentials can also be entered via touchscreen
String ssid = "";
String pass = "";
uint8_t brightness = 210;
bool autoBrightness = false;  // preserved manual behavior on first upgrade
uint8_t actualBrightness = 210;
float filteredLdr = -1;
int lightRaw = 0;
unsigned long lastLightSample = 0;
unsigned long lastLightDisplay = 0;
bool use24 = false;
uint8_t zone = 0;
bool showUtc = false;  // true = temporary UTC display; home zone is unchanged
bool rotate180 = false;  // LCD rotation 1 (normal) or 3 (upside down)

struct Zone { const char* label; const char* posix; };
const Zone zones[] = {
  {"US Eastern",  "EST5EDT,M3.2.0,M11.1.0"},
  {"US Central",  "CST6CDT,M3.2.0,M11.1.0"},
  {"US Mountain", "MST7MDT,M3.2.0,M11.1.0"},
  {"US Pacific",  "PST8PDT,M3.2.0,M11.1.0"},
  {"Arizona",     "MST7"},
  {"UTC",         "UTC0"}
};
const int NZONES = sizeof(zones) / sizeof(zones[0]);

// Four-point calibration, with automatic handling of reversed/swapped axes
bool swapAxes = false;
int32_t rawX0=0, rawX1=0, rawY0=0, rawY1=0;
bool calibrated = false;

enum Page { CLOCK, SETTINGS, WIFI_EDIT, ZONE_SELECT };
Page page = CLOCK;
bool editingPassword = false;
bool upperCase = false;
String editBuffer;
int lastDigits[6] = {-2,-2,-2,-2,-2,-2};
bool timeValid = false;
String displayedDate = ""; // Track displayed date; redraw only when it changes
int lastWifi = -1;
unsigned long lastTick = 0, lastRetry = 0, lastStatus = 0;

uint16_t C_BLACK, C_GLASS, C_EDGE, C_METAL, C_GHOST;
uint16_t C_HALO, C_GLOW, C_ORANGE, C_CORE;
uint16_t C_BUTTON, C_SELECTED, C_TEXT;

void setBacklight(uint8_t level) {
  actualBrightness = level;
  // analogWrite supported by Arduino-ESP32 2.x / 3.x.
  analogWrite(BL_PIN, level);
}

void applyBrightness() {
  if (!autoBrightness) setBacklight(brightness);
}

void updateAutoBrightness(bool force = false) {
  unsigned long now = millis();
  if (!force && now - lastLightSample < 250) return;
  lastLightSample = now;
  long sum = 0;
  for (int i = 0; i < 8; i++) sum += analogRead(LDR_PIN);
  lightRaw = (int)(sum / 8);
  if (filteredLdr < 0) filteredLdr = lightRaw;
  else filteredLdr = filteredLdr * 0.90f + lightRaw * 0.10f;
  if (!autoBrightness) return;

  // The LDR_DARK_HIGH option handles reversed sensor divider polarity.
  // On many CYDs, higher readings mean a darker room.
  float darkFrac;
  if (LDR_DARK_HIGH) {
    darkFrac = (filteredLdr - LDR_BRIGHT_RAW) /
               float(LDR_DARK_RAW - LDR_BRIGHT_RAW);
  } else {
    darkFrac = (LDR_BRIGHT_RAW - filteredLdr) /
               float(LDR_BRIGHT_RAW - LDR_DARK_RAW);
  }
  darkFrac = constrain(darkFrac, 0.0f, 1.0f);
  int target = lroundf(AUTO_MAX_BRIGHTNESS -
                       darkFrac * (AUTO_MAX_BRIGHTNESS - AUTO_MIN_BRIGHTNESS));
  // Smooth actual PWM output and suppress tiny duty-cycle changes.
  int current = actualBrightness;
  int delta = target - current;
  if (abs(delta) >= 2) {
    int step = constrain(delta, -4, 4);
    setBacklight((uint8_t)constrain(current + step, 1, 255));
  }
}

void savePrefs() {
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.putUChar("bright", brightness);
  prefs.putBool("autoBL", autoBrightness);
  prefs.putBool("24h", use24);
  prefs.putUChar("zone", zone);
  prefs.putBool("utcView", showUtc);
  prefs.putBool("rotate180", rotate180);
}

void loadPrefs() {
  prefs.begin("nixieClock", false);
  ssid = prefs.getString("ssid", "");
  pass = prefs.getString("pass", "");
  brightness = prefs.getUChar("bright", 210);
  autoBrightness = prefs.getBool("autoBL", false);
  if (brightness < 20) brightness = 20;
  use24 = prefs.getBool("24h", false);
  zone = prefs.getUChar("zone", 0);
  if (zone >= NZONES) zone = 0;
  showUtc = prefs.getBool("utcView", false);
  rotate180 = prefs.getBool("rotate180", false);

  calibrated = prefs.getBool("calOK", false);
  swapAxes = prefs.getBool("swap", false);
  rawX0 = prefs.getInt("x0", 0);
  rawX1 = prefs.getInt("x1", 0);
  rawY0 = prefs.getInt("y0", 0);
  rawY1 = prefs.getInt("y1", 0);
  if (abs(rawX1-rawX0)<500 || abs(rawY1-rawY0)<500) calibrated = false;
}

const char* activeTimeZone() {
  return showUtc ? "UTC0" : zones[zone].posix;
}

// Switching modes changes only the time-zone interpretation of the
// existing system clock; it does not interrupt or restart NTP.
void applyDisplayTimeZone() {
  setenv("TZ", activeTimeZone(), 1);
  tzset();
}

void startNtp() {
  configTzTime(activeTimeZone(), "pool.ntp.org", "time.nist.gov", "time.google.com");
}

void startWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.disconnect();
  if (ssid.length()) WiFi.begin(ssid.c_str(), pass.c_str());
  lastRetry = millis();
}

bool rawTap(TS_Point &p) {
  if (!ts.touched()) return false;
  delay(25);                  // debounce
  if (!ts.touched()) return false;
  p = ts.getPoint();
  while (ts.touched()) delay(10); // one action per press
  delay(40);
  return true;
}

bool screenTap(int &x, int &y) {
  if (!calibrated) return false;
  TS_Point p;
  if (!rawTap(p)) return false;
  int32_t rx = swapAxes ? p.y : p.x;
  int32_t ry = swapAxes ? p.x : p.y;
  if (rawX1 == rawX0 || rawY1 == rawY0) return false;
  x = 30 + (rx - rawX0) * 260L / (rawX1 - rawX0);
  y = 30 + (ry - rawY0) * 180L / (rawY1 - rawY0);
  x = constrain(x, 0, W-1);
  y = constrain(y, 0, H-1);
  // Calibration values are defined in the NORMAL landscape orientation.
  // A 180-degree rotation reverses both touchscreen coordinates.
  if (rotate180) {
    x = W - 1 - x;
    y = H - 1 - y;
  }
  return true;
}

void title(const char *s) {
  tft.fillScreen(C_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(C_ORANGE, C_BLACK);
  tft.drawString(s, 160, 9);
  tft.drawFastHLine(12, 31, 296, C_EDGE);
}

void button(int x, int y, int w, int h, const String &label, bool selected=false) {
  tft.fillRoundRect(x, y, w, h, 6, selected ? C_SELECTED : C_BUTTON);
  tft.drawRoundRect(x, y, w, h, 6, C_EDGE);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_TEXT, selected ? C_SELECTED : C_BUTTON);
  tft.setTextFont(2);
  tft.drawString(label, x+w/2, y+h/2);
}

bool inside(int px,int py,int x,int y,int w,int h) {
  return px>=x && px<x+w && py>=y && py<y+h;
}

TS_Point waitRawTouch() {
  TS_Point p;
  while (!rawTap(p)) delay(10);
  return p;
}

void calibrateTouch() {
  // Always calibrate with the display in its original orientation,
  // so previously saved mapping stays valid in both orientations.
  tft.setRotation(1);
  const int xx[4] = {30,290,30,290};
  const int yy[4] = {30,30,210,210};
  TS_Point pt[4];
  for (int i=0; i<4; ++i) {
    title("TOUCH CALIBRATION");
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(C_TEXT, C_BLACK);
    tft.drawString("Tap center of crosshair",160,43);
    tft.drawString(String(i+1)+" of 4",160,66);
    tft.drawFastHLine(xx[i]-11, yy[i], 23, C_ORANGE);
    tft.drawFastVLine(xx[i], yy[i]-11, 23, C_ORANGE);
    delay(350);
    pt[i] = waitRawTouch();
  }

  // TL->TR movement identifies which controller axis represents screen X.
  int32_t dx0 = (int32_t)pt[1].x - pt[0].x;
  int32_t dy0 = (int32_t)pt[1].y - pt[0].y;
  swapAxes = abs(dy0) > abs(dx0);
  int32_t a[4], b[4];
  for (int i=0; i<4; ++i) {
    a[i] = swapAxes ? pt[i].y : pt[i].x;
    b[i] = swapAxes ? pt[i].x : pt[i].y;
  }
  rawX0 = (a[0]+a[2])/2;
  rawX1 = (a[1]+a[3])/2;
  rawY0 = (b[0]+b[1])/2;
  rawY1 = (b[2]+b[3])/2;
  calibrated = abs(rawX1-rawX0)>500 && abs(rawY1-rawY0)>500;
  if (calibrated) {
    prefs.putBool("calOK", true);
    prefs.putBool("swap", swapAxes);
    prefs.putInt("x0", rawX0);
    prefs.putInt("x1", rawX1);
    prefs.putInt("y0", rawY0);
    prefs.putInt("y1", rawY1);
    tft.setRotation(rotate180 ? 3 : 1);
  } else {
    title("CALIBRATION FAILED");
    tft.setTextDatum(TC_DATUM);
    tft.drawString("Try tapping more accurately",160,105);
    delay(1500);
    calibrateTouch();
  }
}

// ---------- Nixie vector drawing ----------
// Perspective displacement of each cathode layer (in screen pixels).
// The rear layers sit slightly higher and to the left of the front digit.
int cathodeShiftX = 0;
int cathodeShiftY = 0;
// User-selected digit 5 geometry: flip TOP/BOTTOM ONLY (Y axis values).
// Its X coordinates stay unchanged. This is intentionally NOT a 180-degree rotation.
bool flipFiveVertical = false;
int gx(int x) {
  return 8 + (x * 28) / 100 + cathodeShiftX;
}
int gy(int y) {
  int py = flipFiveVertical ? (100 - y) : y;
  return 25 + (py * 77) / 100 + cathodeShiftY;
}
int pathX, pathY, strokeRadius;
uint16_t strokeColor;

void strokePixel(int x,int y,int radius,uint16_t col) {
  if (radius == 0) tube.drawPixel(x,y,col);
  else tube.fillCircle(x,y,radius,col);
}
void strokeLine(int x1,int y1,int x2,int y2,int radius,uint16_t col) {
  int steps=max(abs(x2-x1),abs(y2-y1))*2;
  if (steps<1) steps=1;
  for(int i=0;i<=steps;i++) {
    float t=(float)i/steps;
    strokePixel(roundf(x1+(x2-x1)*t),roundf(y1+(y2-y1)*t),radius,col);
  }
}
void M(int x,int y) { pathX=x; pathY=y; }
void L(int x,int y) {
  strokeLine(gx(pathX),gy(pathY),gx(x),gy(y),strokeRadius,strokeColor);
  pathX=x; pathY=y;
}
void Q(int cx,int cy,int x,int y) {
  int sx=pathX,sy=pathY;
  int lastX=gx(sx),lastY=gy(sy);
  for(int i=1;i<=24;i++) {
    float t=i/24.0f,u=1-t;
    int nx=gx(roundf(u*u*sx+2*u*t*cx+t*t*x));
    int ny=gy(roundf(u*u*sy+2*u*t*cy+t*t*y));
    strokeLine(lastX,lastY,nx,ny,strokeRadius,strokeColor);
    lastX=nx;lastY=ny;
  }
  pathX=x;pathY=y;
}
// Compact IN-12-style wire numerals. Digit 5 is a vertically
// reflected version of digit 2, without left/right reflection.
void numeralPath(int n) {
  switch(n) {
    case 0:
      M(50,5); Q(16,3,12,43); Q(6,93,49,96);
      Q(91,98,88,53); Q(94,6,50,5);
      break;
    case 1:
      M(27,24); L(53,5); L(53,96);
      break;
    case 2:
    case 5:
      // The long lower-left sweep of the 2 becomes the 5's
      // pronounced DOWNWARD bend below its horizontal top bar.
      // Unlike the earlier version, 5 does NOT invert X.
      M(13,30);
      Q(11,5,48,5);       // rounded opening of 2
      Q(89,4,88,30);      // upper bowl
      Q(87,47,68,59);    // shoulder
      Q(43,67,19,72);    // start near-vertical sweep 8 units earlier
      Q(11,74,12,89);    // longer curved descent beside horizontal base
      Q(13,96,34,96);    // preserve rounded join to the base
      L(89,96);          // becomes the top bar of 5
      break;
    case 3:
      M(15,12); Q(47,-2,75,11); Q(98,25,76,43);
      Q(67,49,51,50); Q(83,49,89,68);
      Q(99,96,52,96); Q(27,96,14,87);
      break;
    case 4:
      M(73,5); L(13,64); L(91,64);
      M(73,5); L(73,96);
      break;
    case 6:
      M(79,11); Q(47,-4,26,29); Q(4,64,24,88);
      Q(48,109,75,89); Q(96,69,76,51);
      Q(53,35,25,57);
      break;
    case 7:
      M(11,5); L(89,5); L(40,96);
      break;
    case 8:
      M(49,48); Q(11,40,22,17); Q(49,-7,77,15);
      Q(94,37,49,48); Q(3,56,17,81);
      Q(49,109,81,83); Q(99,58,49,48);
      break;
    case 9:
      M(77,45); Q(53,60,29,49); Q(5,35,23,13);
      Q(49,-8,73,13); Q(96,38,78,71);
      Q(65,97,23,96);
      break;
  }
}
void drawNumeral(int n,int radius,uint16_t col) {
  if (n<0 || n>9) return;
  flipFiveVertical = (n == 5);
  strokeRadius=radius; strokeColor=col; numeralPath(n);
  flipFiveVertical = false;
}
void drawGlass() {
  tube.fillSprite(C_BLACK);
  tube.fillRoundRect(1,3,42,119,13,C_GLASS);
  tube.drawRoundRect(1,3,42,119,13,C_EDGE);
  tube.fillRoundRect(4,7,36,107,10,C_BLACK);
  tube.drawFastVLine(5,20,65,C_EDGE);
  tube.drawFastVLine(38,20,72,C_GLASS);
  tube.drawFastHLine(13,7,18,C_EDGE);
  tube.drawFastVLine(8,20,84,C_GLASS);
  tube.drawFastVLine(36,20,84,C_GLASS);
  tube.drawFastHLine(10,109,24,C_METAL);
  tube.fillRoundRect(3,116,38,10,3,C_METAL);
  tube.drawFastHLine(5,117,34,C_EDGE);
}
// ---------- Simulated hexagonal wire-mesh anode ----------
// The real IN-12 has a fine metal anode cage in front of its cathodes.
// Here a clipped, one-pixel honeycomb lattice is composited over the
// tube. Alternate pixels give the wires a translucent screen appearance.
// We leave the actively glowing cathode unobstructed at mesh crossings.
void meshPixel(int x, int y, uint16_t meshColor) {
  // Keep the mesh within the curved inner glass, not on the glass rim/base.
  if (x < 9 || x > 35 || y < 20 || y > 104) return;
  if ((x + y) % (MESH_LEVEL == 3 ? 2 : 3) != 0) return;

  uint16_t under = (uint16_t)tube.readPixel(x, y);
  if (under == C_ORANGE || under == C_CORE ||
      under == C_GLOW || under == C_HALO) return;
  tube.drawPixel(x, y, meshColor);
}

void meshLine(int x0, int y0, int x1, int y1, uint16_t color) {
  int dx = abs(x1-x0), sx = x0<x1 ? 1 : -1;
  int dy = -abs(y1-y0), sy = y0<y1 ? 1 : -1;
  int err = dx+dy;
  while (true) {
    meshPixel(x0, y0, color);
    if (x0==x1 && y0==y1) break;
    int e2=2*err;
    if (e2>=dy) { err+=dy; x0+=sx; }
    if (e2<=dx) { err+=dx; y0+=sy; }
  }
}

void drawAnodeMesh() {
  if (MESH_LEVEL == 0) return;
  uint16_t wire = MESH_LEVEL == 1 ? tft.color565(42, 34, 27) :
                  MESH_LEVEL == 2 ? tft.color565(70, 55, 37) :
                                    tft.color565(102, 79, 52);

  // Point-up hexagons: radius ~6px, staggered to interlock.
  // Adjacent tiles share edges, forming a continuous honeycomb.
  for (int row=0; row<11; ++row) {
    int cy = 22 + row*9;
    for (int col=-1; col<5; ++col) {
      int cx = 8 + col*10 + ((row & 1) ? 5 : 0);
      const int8_t vx[6] = {0,5,5,0,-5,-5};
      const int8_t vy[6] = {-6,-3,3,6,3,-3};
      for (int edge=0; edge<6; ++edge) {
        int next=(edge+1)%6;
        meshLine(cx+vx[edge], cy+vy[edge],
                 cx+vx[next], cy+vy[next], wire);
      }
    }
  }

  // Slightly brighter side contours suggest a curved wire cage.
  uint16_t rim = tft.color565(79, 61, 43);
  tube.drawFastVLine(8, 22, 80, rim);
  tube.drawFastVLine(36, 22, 80, rim);
}

// ---------- Small sealed glass exhaust tip ----------
// Drawn just above the sprite, without changing the numeral positions.
// The tip is part of the outer glass envelope, not an electrode.
const bool SHOW_GLASS_TIP = true;
const int GLASS_TIP_HEIGHT = 8;  // pixels extending above each tube

void drawGlassTip(int index) {
  if (!SHOW_GLASS_TIP) return;
  int cx = TUBE_X[index] + TW / 2;
  int y = TY - GLASS_TIP_HEIGHT;

  // Narrow glass neck, joining the rounded crown of the tube.
  tft.fillRoundRect(cx - 4, y, 9, GLASS_TIP_HEIGHT + 9, 4, C_GLASS);
  tft.drawRoundRect(cx - 4, y, 9, GLASS_TIP_HEIGHT + 9, 4, C_EDGE);

  // A sealed rounded pip at the top and a slender glass reflection.
  tft.fillCircle(cx, y + 2, 3, C_GLASS);
  tft.drawCircle(cx, y + 2, 3, C_EDGE);
  tft.drawFastVLine(cx - 2, y + 5, 6, C_EDGE);
  tft.drawPixel(cx - 1, y + 2, C_EDGE);
}

void drawTube(int index,int digit) {
  drawGlass();

  // 9 separate, dim cathodes behind the selected illuminated digit.
  // Shifts are deliberately small: ~1-3 pixels on this tiny screen.
  // Rear layers have less contrast, front layers look like copper wire.
  const int8_t depthX[10] = {-2,-1,-2,-1,0,-1,0,1,0,1};
  const int8_t depthY[10] = {-2,-2,-1,0,-2,1,-1,0,1,2};
  const uint16_t rearWire = tft.color565(38, 26, 19);
  const uint16_t middleWire = tft.color565(65, 43, 28);
  const uint16_t frontWire = tft.color565(94, 60, 36);

  for (int layer=0; layer<10; layer++) {
    if (layer == digit) continue;
    cathodeShiftX = depthX[layer];
    cathodeShiftY = depthY[layer];
    uint16_t wire = layer < 3 ? rearWire :
                    layer < 7 ? middleWire : frontWire;
    drawNumeral(layer, 0, wire);
  }

  // The lit cathode is at the apparent front of the tube.  An almost
  // black surround occludes some background wire crossings and creates
  // separation before the orange discharge/glow is drawn.
  cathodeShiftX = 0;
  cathodeShiftY = 0;
  if (digit>=0 && digit<=9) {
    drawNumeral(digit, 5, tft.color565(9, 5, 3));
    drawNumeral(digit, 4, C_HALO);
    drawNumeral(digit, 2, C_GLOW);
    drawNumeral(digit, 1, C_ORANGE);
    drawNumeral(digit, 0, C_CORE);
  }
  // Frontmost anode cage; never cover the lit cathode's orange core.
  drawAnodeMesh();

  tube.drawFastVLine(5,20,27,C_EDGE);
  tube.drawFastHLine(13,7,18,C_EDGE);
  tube.pushSprite(TUBE_X[index],TY);
  drawGlassTip(index);
}

void clockBackground() {
  tft.fillScreen(C_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(C_ORANGE,C_BLACK);
  // Date appears here once NTP has supplied valid time.
  // The UTC/LOCAL button at right is preserved.
  displayedDate = "";
  // Dedicated main-screen touch button; all other touches open settings.
  button(256, 5, 60, 29, showUtc ? "UTC" : "LOCAL", showUtc);
  tft.drawFastHLine(28,37,264,C_EDGE);
  tft.drawFastHLine(28,190,264,C_EDGE);
  for(int x : {106,210}) {
    tft.fillCircle(x,102,2,C_ORANGE);
    tft.fillCircle(x,132,2,C_ORANGE);
  }
}
void clockStatus() {
  tft.fillRect(0,200,320,40,C_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(C_ORANGE,C_BLACK);
  String line = timeValid
      ? String(showUtc ? "UTC" : zones[zone].label) + "  -  TAP CLOCK FOR SETUP"
      : "WAITING FOR NTP";
  tft.drawString(line,160,201);
  tft.setTextColor(C_EDGE,C_BLACK);
  tft.drawString(WiFi.status()==WL_CONNECTED?"WiFi CONNECTED":"WiFi DISCONNECTED",160,220);
}
void showClock() {
  page=CLOCK;
  clockBackground();
  for (int i=0;i<6;i++) lastDigits[i]=-2;
  timeValid=false;
  for(int i=0;i<6;i++) drawTube(i,-1);
  clockStatus();
}
// Show month, day and year in the upper header using the currently
// selected display time zone. Refresh automatically across midnight.
void updateClockDate(const struct tm &dateTime) {
  char buffer[32];
  if (strftime(buffer, sizeof(buffer), "%B %d, %Y", &dateTime) == 0) return;
  String nextDate(buffer);
  if (nextDate == displayedDate) return;
  displayedDate = nextDate;
  // Keep clear of the UTC/LOCAL touch button starting at X=250.
  tft.fillRect(0, 5, 248, 29, C_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(C_ORANGE, C_BLACK);
  tft.drawString(displayedDate, 125, 20);
}

void tickClock() {
  struct tm tmNow;
  bool valid=getLocalTime(&tmNow,1) && tmNow.tm_year+1900>=2024;
  if (!valid) {
    if (timeValid) {timeValid=false;clockStatus();}
    return;
  }
  updateClockDate(tmNow);
  int hour=tmNow.tm_hour;
  if (!use24) {hour%=12;if(hour==0)hour=12;}
  int dd[6]={hour/10,hour%10,tmNow.tm_min/10,tmNow.tm_min%10,tmNow.tm_sec/10,tmNow.tm_sec%10};
  for(int i=0;i<6;i++) if(dd[i]!=lastDigits[i]) {drawTube(i,dd[i]);lastDigits[i]=dd[i];}
  if(!timeValid) {timeValid=true;clockStatus();}
}

// ---------- Touchscreen pages ----------
void settingsLightStatus() {
  // Update the lower line without redrawing the buttons.
  tft.fillRect(0, 202, 320, 38, C_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(C_TEXT, C_BLACK);
  tft.drawString(String("LDR: ") + lightRaw +
                 "  Backlight: " + actualBrightness, 160, 204);
  tft.drawString(String("Zone: ") + zones[zone].label, 160, 221);
}
void showSettings() {
  page=SETTINGS;
  title("CLOCK SETTINGS");
  button(9,38,148,35,"WiFi");
  button(163,38,148,35,"Time Zone");
  button(9,80,148,35,use24?"24-hour":"12-hour");
  button(163,80,148,35,autoBrightness?"AUTO LIGHT":"MANUAL LIGHT",autoBrightness);
  button(9,122,148,35,"Dim -",false);
  button(163,122,148,35,"Bright +",false);
  button(9,164,98,35,rotate180?"Rotate 0":"Rotate 180",rotate180);
  button(111,164,98,35,"Calibrate");
  button(213,164,98,35,"DONE");
  settingsLightStatus();
}

void showZones() {
  page=ZONE_SELECT;
  title("SELECT TIME ZONE");
  for(int i=0;i<NZONES;i++) {
    int y=36+i*28;
    button(20,y,280,26,zones[i].label,zone==i);
  }
  button(70,209,180,28,"BACK");
}
void showKeyboard() {
  page=WIFI_EDIT;
  title(editingPassword?"WiFi PASSWORD":"WiFi NETWORK (SSID)");
  tft.fillRoundRect(6,36,308,28,4,C_BUTTON);
  tft.setTextDatum(ML_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(C_TEXT,C_BUTTON);
  String shown=editBuffer;
  if (editingPassword) {
    shown="";
    for (size_t i=0; i<editBuffer.length(); ++i) shown += '*';
  }
  if(shown.length()>29) shown="..."+shown.substring(shown.length()-26);
  tft.drawString(shown,12,50);
  const char *rows[4]={"1234567890","qwertyuiop","asdfghjkl-","zxcvbnm._@"};
  for(int r=0;r<4;r++) {
    String s=rows[r];
    for(int c=0;c<10;c++) {
      char ch=s[c];
      if(upperCase && ch>='a' && ch<='z') ch-=32;
      button(2+c*32,69+r*34,30,30,String(ch));
    }
  }
  button(2,208,75,29,"SPACE");
  button(79,208,62,29,"DEL");
  button(143,208,58,29,"Aa");
  button(203,208,70,29,editingPassword?"SAVE":"NEXT");
  button(275,208,43,29,"EXIT");
}
void beginWifiEdit() {
  editingPassword=false;
  editBuffer=ssid;
  upperCase=false;
  showKeyboard();
}
void handleKeyboard(int x,int y) {
  if (y>=69 && y<205) {
    int r=(y-69)/34,c=(x-2)/32;
    if (x<2 || c<0 || c>=10 || r<0 || r>=4) return;
    const char *rows[4]={"1234567890","qwertyuiop","asdfghjkl-","zxcvbnm._@"};
    char ch=rows[r][c];
    if (upperCase && ch>='a' && ch<='z') ch-=32;
    if(editBuffer.length()<(editingPassword?63:32)) editBuffer+=ch;
    showKeyboard();
  } else if(y>=208) {
    if(x<77) {
      if(editBuffer.length()<(editingPassword?63:32)) editBuffer+=' ';
    } else if(x<141) {
      if(editBuffer.length()) editBuffer.remove(editBuffer.length()-1);
    } else if(x<201) {
      upperCase=!upperCase;
    } else if(x<273) {
      if(!editingPassword) {
        ssid=editBuffer;
        editingPassword=true;
        editBuffer=pass;
        upperCase=false;
      } else {
        pass=editBuffer;
        savePrefs();
        startWifi();
        startNtp();
        showSettings();
        return;
      }
    } else {
      showSettings();
      return;
    }
    showKeyboard();
  }
}
void handleSettings(int x,int y) {
  if(inside(x,y,9,38,148,35)) {beginWifiEdit();return;}
  if(inside(x,y,163,38,148,35)) {showZones();return;}
  if(inside(x,y,9,80,148,35)) {use24=!use24;savePrefs();showSettings();return;}
  if(inside(x,y,163,80,148,35)) {
    autoBrightness=!autoBrightness;
    if (autoBrightness) {
      filteredLdr = -1;
      updateAutoBrightness(true);
    } else applyBrightness();
    savePrefs();showSettings();return;
  }
  if(inside(x,y,9,122,148,35)) {
    // +/- adjust manual brightness. Auto mode is left enabled only by toggle.
    if (autoBrightness) autoBrightness=false;
    brightness=(uint8_t)max(20,(int)brightness-25);
    applyBrightness();savePrefs();showSettings();return;
  }
  if(inside(x,y,163,122,148,35)) {
    if (autoBrightness) autoBrightness=false;
    brightness=(uint8_t)min(255,(int)brightness+25);
    applyBrightness();savePrefs();showSettings();return;
  }
  if(inside(x,y,9,164,98,35)) {
    rotate180 = !rotate180;
    savePrefs();
    tft.setRotation(rotate180 ? 3 : 1);
    showSettings();
    return;
  }
  if(inside(x,y,111,164,98,35)) {calibrateTouch();showSettings();return;}
  if(inside(x,y,213,164,98,35)) {showClock();return;}
}

void handleZones(int x,int y) {
  if(y>=36 && y<36+NZONES*28 && x>=20 && x<300) {
    int chosen=(y-36)/28;
    if(chosen>=0 && chosen<NZONES) {
      zone=(uint8_t)chosen;
      // Choosing a home zone also shows that zone immediately.
      showUtc = false;
      savePrefs();
      applyDisplayTimeZone();
      showSettings();
    }
  } else if(y>=209) showSettings();
}

void setup() {
  Serial.begin(115200);
  loadPrefs();

  pinMode(BL_PIN,OUTPUT);
  pinMode(LDR_PIN, INPUT);
  analogReadResolution(12);
  applyBrightness();
  updateAutoBrightness(true);
  tft.init();
  tft.setRotation(rotate180 ? 3 : 1);

  C_BLACK=TFT_BLACK;
 // C_GLASS=tft.color565(17,12,11);
 // C_EDGE=tft.color565(78,54,37);
  C_GLASS = tft.color565(35, 25, 18);
  C_EDGE  = tft.color565(255,111,8);
  C_METAL=tft.color565(55,45,37);
  C_GHOST=tft.color565(85,48,22);
  C_HALO=tft.color565(85,17,0);
  C_GLOW=tft.color565(174,45,0);
  C_ORANGE=tft.color565(255,111,8);
  C_CORE=tft.color565(255,218,96);
  C_BUTTON=tft.color565(40,31,24);
  C_SELECTED=tft.color565(95,49,14);
  C_TEXT=tft.color565(255,196,116);

  tube.setColorDepth(16);
  if(!tube.createSprite(TW,TH)) {
    Serial.println("Cannot allocate tube sprite");
    while(true)delay(1000);
  }

  // IMPORTANT: XPT2046 is on a separate SPI bus.
  // Use rotation 0 and determine mapping during calibration.
  // Screen rotation is handled after calibration by reversing X and Y.
  touchSPI.begin(TOUCH_CLK,TOUCH_MISO,TOUCH_MOSI,TOUCH_CS);
  ts.begin(touchSPI);
  ts.setRotation(0);

  if(!calibrated) calibrateTouch();
  startWifi();
  startNtp();
  showClock();
}

void loop() {
  int x,y;
  if(screenTap(x,y)) {
    switch(page) {
      case CLOCK:
        if (inside(x,y,250,0,70,39)) {
          showUtc = !showUtc;
          savePrefs();
          applyDisplayTimeZone();
          showClock();  // repaint all digits and the mode indicator immediately
        } else {
          showSettings();
        }
        break;
      case SETTINGS: handleSettings(x,y);break;
      case WIFI_EDIT: handleKeyboard(x,y);break;
      case ZONE_SELECT: handleZones(x,y);break;
    }
  }

  unsigned long now=millis();
  updateAutoBrightness();
  if(page==SETTINGS && now-lastLightDisplay>=800) {
    lastLightDisplay=now;
    settingsLightStatus();
  }
  if(page==CLOCK && now-lastTick>=200) {
    lastTick=now;
    tickClock();
  }
  if(WiFi.status()!=WL_CONNECTED && ssid.length() && now-lastRetry>=15000) {
    lastRetry=now;
    WiFi.reconnect();
  }
  if(page==CLOCK && now-lastStatus>=1000) {
    lastStatus=now;
    int s=(int)WiFi.status();
    if(s!=lastWifi) {lastWifi=s;clockStatus();}
  }
  delay(5);
}
