/*
  Smart Mirror - Arduino UNO R4 WiFi  (Blynk library, stall diagnostics)
  OLED 1.3" SH1106 128x64 (I2C 0x3C)  -> time, date, day, weather, forecast, schedule
  LCD 16x2 + I2C backpack (0x27)      -> scrolling long text

  Libraries (Library Manager):
    U8g2, ArduinoJson (v7), LiquidCrystal I2C (Frank de Brabander), Blynk (Volodymyr Shymanskyy)
  Wiring: SDA -> SDA, SCL -> SCL, VCC -> 5V, GND -> GND (both devices in parallel)
  Debug:  Serial Monitor at 115200 baud
*/

// ---- Turn network tasks on/off to find what blocks the LCD (1 = on, 0 = off) ----
#define USE_BLYNK    1
#define USE_NTP      1
#define USE_WEATHER  1

// ---- Blynk credentials (must come BEFORE the Blynk include) ----
#define BLYNK_TEMPLATE_ID "YOUR_TEMPLATE_ID"
#define BLYNK_TEMPLATE_NAME "YOUR_TEMPLATE_NAME"
#define BLYNK_AUTH_TOKEN "YOUR_AUTH_TOKEN"

#include <SPI.h>
#include <WiFiS3.h>
#if USE_BLYNK
#include <BlynkSimpleWifi.h>
#endif
#include <Wire.h>
#include <U8g2lib.h>
#include <ArduinoJson.h>
#include <LiquidCrystal_I2C.h>
#include <math.h>

// ======================= USER SETTINGS =======================
const char WIFI_SSID[]   = "YOUR_WIFI_NAME";
const char WIFI_PASS[]   = "YOUR_WIFI_PASSWORD";

const float LATITUDE     = 0;
const float LONGITUDE    = 0;
const long  TZ_OFFSET_S  = 0;                          // IN SECONDS

const unsigned long WEATHER_INTERVAL_MS = 1800000UL;   // 30 min
const unsigned long TIME_SYNC_MS        = 3600000UL;   // 1 h (after first success)
const unsigned long RETRY_MIN_MS        = 10000UL;     // first retry after a failure
const unsigned long RETRY_MAX_MS        = 120000UL;    // backoff ceiling (2 min)
const unsigned long PAGE_TIME_MS        = 4000UL;      // OLED page duration
const unsigned long LCD_SCROLL_MS       = 275UL;       // one LCD ROW updates per tick
const int           LCD_STEP_CHARS      = 1;           // characters moved per update
// =============================================================

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);   // OLED at 0x3C
LiquidCrystal_I2C lcd(0x27, 16, 2);                                // LCD at 0x27

// ---------- Data ----------
String schedule = "No schedule yet";
struct Weather {
  bool  ok = false;
  float temp = 0;
  int   hum = 0, code = 0;
  int   hi[3] = {0}, lo[3] = {0}, rain[3] = {0}, dcode[3] = {0};
} wx;

// ---------- Time keeping ----------
uint32_t epochBase = 0;
unsigned long syncMs = 0;
bool timeValid = false;

const char* DAY_FULL[]  = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
const char* DAY_SHORT[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
const char* MON_SHORT[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};

struct DT { int y, mo, d, h, mi, s, dow; };

uint32_t localEpoch() {
  return epochBase + (millis() - syncMs) / 1000UL + TZ_OFFSET_S;
}

DT breakTime(uint32_t t) {
  DT r;
  r.s = t % 60; r.mi = (t / 60) % 60; r.h = (t / 3600) % 24;
  uint32_t days = t / 86400UL;
  r.dow = (days + 4) % 7;
  long z = (long)days + 719468L;
  long era = z / 146097L;
  unsigned long doe = z - era * 146097L;
  unsigned long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long y = (long)yoe + era * 400;
  unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned long mp = (5 * doy + 2) / 153;
  r.d = doy - (153 * mp + 2) / 5 + 1;
  r.mo = mp < 10 ? mp + 3 : mp - 9;
  r.y = y + (r.mo <= 2);
  return r;
}

// tries > 1 waits between attempts (use in setup); tries == 1 never waits (use in loop)
bool syncTime(int tries) {
  for (int i = 0; i < tries; i++) {
    unsigned long t = WiFi.getTime();
    if (t > 1700000000UL) {
      epochBase = t; syncMs = millis(); timeValid = true;
      return true;
    }
    if (tries > 1) delay(500);
  }
  return false;
}

// ---------- Helpers ----------
const char* wmoText(int c) {
  if (c == 0) return "Clear sky";
  if (c == 1) return "Mainly clear";
  if (c == 2) return "Partly cloudy";
  if (c == 3) return "Overcast";
  if (c == 45 || c == 48) return "Fog";
  if (c >= 51 && c <= 57) return "Drizzle";
  if (c >= 61 && c <= 67) return "Rain";
  if (c >= 71 && c <= 77) return "Snow";
  if (c >= 80 && c <= 82) return "Rain showers";
  if (c == 85 || c == 86) return "Snow showers";
  if (c >= 95) return "Thunderstorm";
  return "Unknown";
}

String cleanText(String s) {
  String o;
  for (unsigned int i = 0; i < s.length() && o.length() < 240; i++) {
    char c = s[i];
    if (c == '\r' || c == '\n') c = ' ';
    if (c < 32 || c > 126) c = ' ';
    o += c;
  }
  o.trim();
  return o;
}

void logTask(const __FlashStringHelper* name, bool ok, unsigned long t0) {
  Serial.print(name); Serial.print(ok ? F(": ok ") : F(": FAILED "));
  Serial.print(millis() - t0); Serial.println(F(" ms"));
}

// ---------- Weather fetch (Open-Meteo, no API key) ----------
bool fetchWeather() {
  WiFiSSLClient c;
  c.setTimeout(3000);
  if (!c.connect("api.open-meteo.com", 443)) return false;
  c.print(F("GET /v1/forecast?latitude="));
  c.print(LATITUDE, 4);
  c.print(F("&longitude="));
  c.print(LONGITUDE, 4);
  c.print(F("&current=temperature_2m,relative_humidity_2m,weather_code"
            "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
            "&timezone=auto&forecast_days=3 HTTP/1.0\r\n"
            "Host: api.open-meteo.com\r\nConnection: close\r\n\r\n"));

  String status = c.readStringUntil('\n');
  if (status.indexOf(" 200") < 0) { c.stop(); return false; }
  while (c.connected() || c.available()) {
    String l = c.readStringUntil('\n');
    if (l.length() == 0 || l == "\r") break;
  }

  JsonDocument filter;
  filter["current"] = true;
  filter["daily"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, c, DeserializationOption::Filter(filter));
  c.stop();
  if (err) return false;

  JsonObject cur = doc["current"];
  wx.temp = cur["temperature_2m"] | 0.0f;
  wx.hum  = cur["relative_humidity_2m"] | 0;
  wx.code = cur["weather_code"] | 0;
  JsonObject d = doc["daily"];
  for (int i = 0; i < 3; i++) {
    wx.hi[i]    = lround((float)(d["temperature_2m_max"][i] | 0.0f));
    wx.lo[i]    = lround((float)(d["temperature_2m_min"][i] | 0.0f));
    wx.rain[i]  = d["precipitation_probability_max"][i] | 0;
    wx.dcode[i] = d["weather_code"][i] | 0;
  }
  wx.ok = true;
  return true;
}

// ---------- OLED pages ----------
const int NUM_PAGES = 4;
int page = 0;

void drawClockPage() {
  DT t = breakTime(localEpoch());
  char buf[32];

  u8g2.setFont(u8g2_font_6x10_tf);
  snprintf(buf, sizeof(buf), "%s, %d %s %d", DAY_FULL[t.dow], t.d, MON_SHORT[t.mo - 1], t.y);
  u8g2.drawStr(0, 10, buf);

  u8g2.setFont(u8g2_font_logisoso28_tn);
  snprintf(buf, sizeof(buf), "%02d:%02d", t.h, t.mi);
  u8g2.drawStr(0, 46, buf);

  u8g2.setFont(u8g2_font_6x10_tf);
  snprintf(buf, sizeof(buf), ":%02d", t.s);
  u8g2.drawStr(104, 46, buf);

  if (wx.ok) snprintf(buf, sizeof(buf), "%dC %s", (int)lround(wx.temp), wmoText(wx.code));
  else       snprintf(buf, sizeof(buf), "Weather loading...");
  buf[21] = 0;
  u8g2.drawStr(0, 62, buf);
}

void drawWeatherPage() {
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, "Weather now");
  if (!wx.ok) { u8g2.drawStr(0, 36, "No data yet"); return; }

  char num[8];
  snprintf(num, sizeof(num), "%d", (int)lround(wx.temp));
  u8g2.setFont(u8g2_font_logisoso28_tn);
  int w = u8g2.getStrWidth(num);
  u8g2.drawStr(0, 44, num);
  u8g2.drawCircle(w + 6, 20, 3);
  u8g2.setFont(u8g2_font_helvB14_tr);
  u8g2.drawStr(w + 12, 44, "C");

  u8g2.setFont(u8g2_font_6x10_tf);
  char buf[32];
  snprintf(buf, sizeof(buf), "%s", wmoText(wx.code));
  u8g2.drawStr(0, 55, buf);
  snprintf(buf, sizeof(buf), "Humidity %d%%", wx.hum);
  u8g2.drawStr(0, 64, buf);
}

void drawForecastPage() {
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, "Forecast");
  if (!wx.ok) { u8g2.drawStr(0, 36, "No data yet"); return; }
  DT t = breakTime(localEpoch());
  char buf[32];
  for (int i = 0; i < 3; i++) {
    snprintf(buf, sizeof(buf), "%s %d-%dC rain %d%%",
             i == 0 ? "Tod" : DAY_SHORT[(t.dow + i) % 7], wx.lo[i], wx.hi[i], wx.rain[i]);
    u8g2.drawStr(0, 26 + i * 14, buf);
  }
}

void drawSchedulePage() {
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, "Schedule");
  const int COLS = 21, MAXLINES = 4;
  char line[COLS + 1];
  int col = 0, row = 0;
  for (unsigned int i = 0; i <= schedule.length() && row < MAXLINES; i++) {
    char ch = (i < schedule.length()) ? schedule[i] : ';';
    if (ch != ';') {
      if (col == 0 && ch == ' ') continue;
      line[col++] = ch;
    }
    if (ch == ';' || col == COLS) {
      line[col] = 0;
      u8g2.drawStr(0, 24 + row * 12, line);
      row++; col = 0;
    }
  }
}

void drawOLED() {
  u8g2.clearBuffer();
  if (!timeValid && page == 0) {
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 30, "Syncing time...");
  } else {
    switch (page) {
      case 0: drawClockPage();    break;
      case 1: drawWeatherPage();  break;
      case 2: drawForecastPage(); break;
      case 3: drawSchedulePage(); break;
    }
  }
  u8g2.sendBuffer();
}

// ---------- LCD marquee ----------
struct Marquee {
  String text;
  unsigned int pos = 0;
  String last;
  void set(const String& s) { if (s != text) { text = s; pos = 0; last = ""; } }
  void show(int row) {
    char out[17];
    unsigned int n = text.length();
    if (n <= 16) {
      for (int i = 0; i < 16; i++) out[i] = (i < (int)n) ? text[i] : ' ';
    } else {
      unsigned int total = n + 4;                       // 4-space gap before looping
      for (int i = 0; i < 16; i++) {
        unsigned int k = (pos + i) % total;
        out[i] = (k < n) ? text[k] : ' ';
      }
      pos = (pos + LCD_STEP_CHARS) % total;
    }
    out[16] = 0;
    if (last != out) { lcd.setCursor(0, row); lcd.print(out); last = out; }
  }
} mqTop, mqBottom;

void rebuildLcdText() {
  String s = schedule;
  s.replace(";", " | ");
  mqTop.set("Plan: " + s);

  if (wx.ok) {
    String w = "Now " + String((int)lround(wx.temp)) + "C " + wmoText(wx.code) +
               ", humidity " + String(wx.hum) + "%. Today " + String(wx.lo[0]) + "-" +
               String(wx.hi[0]) + "C rain " + String(wx.rain[0]) + "%. Tomorrow " +
               wmoText(wx.dcode[1]) + " " + String(wx.lo[1]) + "-" + String(wx.hi[1]) +
               "C rain " + String(wx.rain[1]) + "%.";
    mqBottom.set(w);
  } else {
    mqBottom.set("Weather loading...");
  }
}

// ---------- Blynk handlers ----------
#if USE_BLYNK
BLYNK_WRITE(V0) {                    // runs whenever V0 changes in Blynk
  schedule = cleanText(param.asString());
  if (schedule.length() == 0) schedule = "No events";
  rebuildLcdText();
}

BLYNK_CONNECTED() {                  // runs each time the board (re)connects
  Blynk.syncVirtual(V0);
  Serial.println(F("Blynk: connected"));
}
#endif

// ---------- Display refresh (fast, never touches the network) ----------
unsigned long lastPage = 0, lastLcd = 0;
uint32_t lastSecDrawn = 0;

void updateDisplays() {
  unsigned long ms = millis();

  bool redraw = false;
  if (ms - lastPage > PAGE_TIME_MS) {
    lastPage = ms;
    page = (page + 1) % NUM_PAGES;
    redraw = true;
  }
  uint32_t nowS = localEpoch();
  if (redraw || nowS != lastSecDrawn) {
    lastSecDrawn = nowS;
    drawOLED();
  }

  static bool topTurn = true;        // one LCD row per tick
  if (ms - lastLcd > LCD_SCROLL_MS) {
    lastLcd = ms;
    if (topTurn) mqTop.show(0); else mqBottom.show(1);
    topTurn = !topTurn;
  }
}

// ---------- Setup / Loop ----------
unsigned long lastWeather  = 0, weatherRetry = 0;
unsigned long lastTime     = 0, timeRetry    = 0;
unsigned long lastBlynkTry = 0, blynkRetry   = 0;
unsigned long lastWifiTry  = 0;
unsigned long lastPassMs   = 0;
bool firstWeather = true, firstTime = true, firstBlynk = true;

void connectWiFi() {
  lcd.clear(); lcd.print("Connecting WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) delay(500);
}

void setup() {
  Serial.begin(115200);              // does not wait for the Serial Monitor
  Wire.begin();
  u8g2.begin();
  u8g2.setBusClock(400000);
  Wire.setClock(400000);
  lcd.init();
  lcd.backlight();
  lcd.clear();

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 20, "Smart Mirror");
  u8g2.drawStr(0, 40, "Connecting WiFi...");
  u8g2.sendBuffer();

  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(F("WiFi: connected"));
    lcd.clear(); lcd.print("WiFi connected");
  } else {
    Serial.println(F("WiFi: FAILED"));
    lcd.clear(); lcd.print("WiFi failed");
  }

#if USE_BLYNK
  Blynk.config(BLYNK_AUTH_TOKEN);    // connection happens in loop()
#endif
  rebuildLcdText();
  lastPassMs = millis();
}

void loop() {
  // Report any long stall since the previous pass
  unsigned long nowMs = millis();
  if (nowMs - lastPassMs > 1000UL) {
    Serial.print(F("LOOP STALL ")); Serial.print(nowMs - lastPassMs); Serial.println(F(" ms"));
  }
  lastPassMs = nowMs;

  updateDisplays();                  // always first

  unsigned long ms = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (ms - lastWifiTry > 15000UL) {
      lastWifiTry = ms;
      Serial.println(F("WiFi: reconnecting"));
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
    return;
  }

  bool busy = false;                 // allow at most ONE slow task per pass

#if USE_BLYNK
  if (Blynk.connected()) {
    Blynk.run();
    blynkRetry = RETRY_MIN_MS;
  } else if (firstBlynk || ms - lastBlynkTry > blynkRetry) {
    firstBlynk = false;
    lastBlynkTry = ms;
    unsigned long t0 = millis();
    bool ok = Blynk.connect(3000);
    logTask(F("Blynk connect"), ok, t0);
    blynkRetry = ok ? RETRY_MIN_MS : min(blynkRetry * 2, RETRY_MAX_MS);
    if (blynkRetry < RETRY_MIN_MS) blynkRetry = RETRY_MIN_MS;
    busy = true;
  }
#endif

#if USE_NTP
  if (!busy && (firstTime || ms - lastTime > (timeValid ? TIME_SYNC_MS : timeRetry))) {
    firstTime = false;
    lastTime = ms;
    unsigned long t0 = millis();
    bool ok = syncTime(1);
    logTask(F("Time sync"), ok, t0);
    timeRetry = ok ? RETRY_MIN_MS : min(max(timeRetry * 2, RETRY_MIN_MS), RETRY_MAX_MS);
    busy = true;
  }
#endif

#if USE_WEATHER
  if (!busy && (firstWeather || ms - lastWeather > (wx.ok ? WEATHER_INTERVAL_MS : weatherRetry))) {
    firstWeather = false;
    lastWeather = ms;
    unsigned long t0 = millis();
    bool ok = fetchWeather();
    logTask(F("Weather"), ok, t0);
    if (ok) rebuildLcdText();
    else weatherRetry = min(max(weatherRetry * 2, RETRY_MIN_MS), RETRY_MAX_MS);
    busy = true;
  }
#endif
}