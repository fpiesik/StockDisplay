/*
  LilyGo T-Display S3 (ESP32-S3, ST7789 170x320) – Crypto Ticker (nur CoinGecko)
  Features:
    - bis zu 4 Krypto-Assets (Preis + 24h-Änderung, CoinGecko)
    - Rotation per linker Taste (GPIO 0)
    - Helligkeit per rechter Taste (GPIO 14), 5 Stufen
    - WLAN-Daten & Asset-Liste persistent (Preferences / Flash)
    - WLAN per Serial eingeben, Assets per Serial ändern:
        ASSETS?                          -> zeigt aktuelle Liste
        SET_ASSETS BTC-USD,ETH-USD,...   -> setzt 1..4 Symbole (Komma-getrennt)
        RESET_WIFI                       -> WLAN-Zugangsdaten löschen
  Voraussetzungen:
    - ESP32 Core 2.0.17
    - TFT_eSPI aus offiziellem LilyGo-Repo (dein Setup läuft ja nun)
    - ArduinoJson >= 6
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <Preferences.h>
#include <esp_adc_cal.h>
#include <math.h>
#include <time.h>         // NTP / Zeitfunktionen


TFT_eSPI tft;
Preferences prefs;

// =================== SETTINGS ===================
const unsigned long REFRESH_MS = 60UL * 1000UL; // Update-Intervall
const int BTN_ROTATE = 0;   // linker Knopf
const int BTN_BRIGHT = 14;  // rechter Knopf
const int PIN_BAT_VOLT = 4; // Akku-Messung

// Display
int currentRotation = 1;    // 1 = Kabel rechts, 3 = Kabel links
int brightnessLevel = 3;    // 1–5
const int BRIGHT_MIN = 40;
const int BRIGHT_MAX = 255;
const int BRIGHT_STEPS = 5;
// PWM fürs Backlight stabilisieren
const int BL_PWM_PIN   = 38;     // TFT_BL
const int BL_PWM_CH    = 1;      // LEDC-Kanal
const int BL_PWM_FREQ  = 20000;  // 20 kHz (oberhalb Hörbereich)
const int BL_PWM_BITS  = 12;     // 12-bit Auflösung (0..4095)
const int BL_DUTY_MAX  = 4010;   // bewusst < 4095, um 100%-Edgecases zu vermeiden
const int BL_DUTY_MIN  = 30;     // nicht ganz aus (Stabilität/Display-Treiber)

// Zeitzone für Deutschland (automatische Sommer-/Winterzeit)
#define MY_TZ "CET-1CEST,M3.5.0,M10.5.0/3"
bool timeSynced = false;

// Tasten-Entprellung
bool lastBtnStateRot = HIGH;
bool lastBtnStateBright = HIGH;
unsigned long lastDebounceRot = 0;
unsigned long lastDebounceBright = 0;
const unsigned long debounceDelay = 150;

// WLAN + Assets
String WIFI_SSID = "";
String WIFI_PASS = "";
const uint8_t WIFI_PROFILE_LIMIT = 5;   // maximale Anzahl gemerkter WLANs
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;
const unsigned long WIFI_RECONNECT_COOLDOWN_MS = 15000;

struct WiFiProfile {
  String ssid;
  String pass;
};

WiFiProfile wifiProfiles[WIFI_PROFILE_LIMIT];
uint8_t wifiProfileCount = 0;
int currentWifiProfile = -1;
unsigned long lastWifiReconnectAttempt = 0;
String symbols[4] = {"BTC-USD", "SOL-USD", "ETH-USD", "ADA-USD"};
const uint8_t NUM_SYMBOLS = sizeof(symbols) / sizeof(symbols[0]);

// Anzeigeparameter
int16_t SCREEN_W, SCREEN_H;
int16_t HEADER_H  = 24;
int16_t FOOTER_H  = 18;
int16_t CONTENT_Y, CONTENT_H;
int16_t ROWS = 4, ROW_H, MARGIN = 6;
int16_t COL_SYMBOL_X, COL_PRICE_XR, COL_CHG_XR;

// Batterie-Anzeige
const unsigned long BATTERY_REFRESH_MS = 10UL * 1000UL; // alle 10 s aktualisieren
const uint32_t BATTERY_USB_THRESHOLD_MV = 4300;
bool batteryValid = false;
bool batteryPresent = false;
float batteryVoltage = 0.0f;

// Kursdaten
struct Quote {
  String symbol;
  double price = NAN;
  double changePct = NAN; // CoinGecko: 24h change
  bool ok = false;
};
Quote quotes[4];
unsigned long lastRefresh = 0;

// =================== DISPLAY & UI ===================
// --- oberhalb: zwei neue Konstanten (falls du Invert brauchst, auf true stellen)
const bool BL_INVERT = false;  // wenn "höhere Stufe = dunkler", dann true

// --- diese Funktion vollständig ersetzen
void setBrightnessFromLevel(int level) {
  // 5 Stufen auf 8-bit Duty (0..255), nie exakt 0/255 (verhindert Flackern/Glitches)
  static const uint8_t lut[5] = { 12, 60, 120, 190, 250 }; // 1..5
  int idx  = constrain(level, 1, 5) - 1;
  int duty = lut[idx];

  if (BL_INVERT) duty = 255 - duty;

  ledcWrite(BL_PWM_CH, duty);
  // Debug:
  // Serial.printf("BL level=%d duty=%d\n", level, duty);
}

void drawStatus(const String& msg) {
  tft.fillRect(0, 0, SCREEN_W, 20, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString(msg, 4, 2, 2);
}

void drawHeader() {
  tft.fillRect(0, 0, SCREEN_W, HEADER_H, TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  //tft.setTextDatum(TL_DATUM);
  //tft.drawString("💱  Krypto-Kurse", MARGIN, 3, 4);

  //tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Symbol", MARGIN, HEADER_H - 16, 2);

  tft.setTextDatum(TR_DATUM);
  tft.drawString("Preis", COL_PRICE_XR, HEADER_H - 16, 2);
  tft.drawString("Δ 24h", COL_CHG_XR,   HEADER_H - 16, 2);
}

String nowTime() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo);
    return String(buf);
  }
  return String("00:00:00");  // Fallback
}


void drawRow(uint8_t idx, const Quote& q) {
  if (idx >= ROWS) return;
  const int y = CONTENT_Y + idx * ROW_H;
  const int h = ROW_H;

  uint16_t bg = (idx % 2 == 0) ? TFT_BLACK : tft.color565(10,10,10);
  tft.fillRect(0, y, SCREEN_W, h, bg);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, bg);
  tft.drawString(symbols[idx], COL_SYMBOL_X, y + 2, 4);

  if (!q.ok || isnan(q.price)) {
    tft.setTextColor(TFT_RED, bg);
    tft.setTextDatum(TR_DATUM);
    tft.drawString("n/a", COL_PRICE_XR, y + 2, 4);
    tft.drawString("--",  COL_CHG_XR,   y + 2, 4);
    return;
  }

  char priceBuf[24];
  if (q.price >= 1000.0)      dtostrf(q.price, 0, 0, priceBuf);
  else if (q.price >= 10.0)   dtostrf(q.price, 0, 2, priceBuf);
  else if (q.price >= 1.0)    dtostrf(q.price, 0, 3, priceBuf);
  else                        dtostrf(q.price, 0, 5, priceBuf);

  tft.setTextColor(TFT_WHITE, bg);
  tft.setTextDatum(TR_DATUM);
  tft.drawString(String(priceBuf), COL_PRICE_XR, y + 2, 4);

  bool up = (!isnan(q.changePct) && q.changePct >= 0.0);
  uint16_t col = up ? TFT_GREEN : TFT_RED;
  char pctBuf[16];
  if (isnan(q.changePct)) snprintf(pctBuf, sizeof(pctBuf), "--");
  else snprintf(pctBuf, sizeof(pctBuf), "%+0.2f%%", q.changePct);

  tft.setTextColor(col, bg);
  tft.drawString(String(pctBuf), COL_CHG_XR, y + 2, 4);
}

String batteryStatusText() {
  if (!batteryValid) return String("Batt: --");
  if (!batteryPresent) return String("Batt: USB");

  char buf[16];
  snprintf(buf, sizeof(buf), "Batt: %.2fV", batteryVoltage);
  return String(buf);
}

void drawFooter() {
  tft.fillRect(0, SCREEN_H - FOOTER_H, SCREEN_W, FOOTER_H, TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);

  tft.setTextDatum(TL_DATUM);
  tft.drawString(batteryStatusText(), MARGIN, SCREEN_H - FOOTER_H + 2, 2);

  tft.setTextDatum(TR_DATUM);
  tft.drawString("Aktualisiert: " + nowTime(), SCREEN_W - 4, SCREEN_H - FOOTER_H + 2, 2);
}

bool updateBattery(bool force = false) {
  static unsigned long lastSample = 0;
  static esp_adc_cal_characteristics_t adcChars;
  static bool adcInit = false;

  unsigned long now = millis();
  if (!force && batteryValid && (now - lastSample) < BATTERY_REFRESH_MS) {
    return false;
  }

  lastSample = now;

  if (!adcInit) {
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &adcChars);
    adcInit = true;
  }

  uint32_t raw = analogRead(PIN_BAT_VOLT);
  uint32_t mv = esp_adc_cal_raw_to_voltage(raw, &adcChars) * 2; // Spannungsteiler 1/2

  bool prevValid = batteryValid;
  bool prevPresent = batteryPresent;
  float prevVoltage = batteryVoltage;

  batteryValid = true;
  batteryPresent = (mv <= BATTERY_USB_THRESHOLD_MV);
  batteryVoltage = mv / 1000.0f;

  if (!prevValid) return true;
  if (prevPresent != batteryPresent) return true;
  if (fabsf(prevVoltage - batteryVoltage) > 0.02f) return true; // >20 mV Unterschied

  return false;
}

void persistWifiProfiles() {
  String list = "";
  for (uint8_t i = 0; i < wifiProfileCount && i < WIFI_PROFILE_LIMIT; ++i) {
    if (wifiProfiles[i].ssid.length() == 0) continue;
    if (list.length()) list += '\n';
    list += wifiProfiles[i].ssid;
    list += '\t';
    list += wifiProfiles[i].pass;
  }

  prefs.putString("wifi_list", list);
  if (wifiProfileCount > 0) {
    prefs.putString("wifi_ssid", wifiProfiles[0].ssid);
    prefs.putString("wifi_pass", wifiProfiles[0].pass);
  } else {
    prefs.putString("wifi_ssid", "");
    prefs.putString("wifi_pass", "");
  }
}

void loadWifiProfiles() {
  wifiProfileCount = 0;
  String list = prefs.getString("wifi_list", "");
  int start = 0;
  while (start < list.length() && wifiProfileCount < WIFI_PROFILE_LIMIT) {
    int end = list.indexOf('\n', start);
    if (end < 0) end = list.length();
    String line = list.substring(start, end);
    int sep = line.indexOf('\t');
    if (sep >= 0) {
      String ssid = line.substring(0, sep);
      String pass = line.substring(sep + 1);
      if (ssid.length() > 0) {
        wifiProfiles[wifiProfileCount].ssid = ssid;
        wifiProfiles[wifiProfileCount].pass = pass;
        wifiProfileCount++;
      }
    }
    start = end + 1;
  }

  if (wifiProfileCount == 0) {
    String legacySsid = prefs.getString("wifi_ssid", "");
    String legacyPass = prefs.getString("wifi_pass", "");
    if (legacySsid.length() > 0) {
      wifiProfiles[0].ssid = legacySsid;
      wifiProfiles[0].pass = legacyPass;
      wifiProfileCount = 1;
    }
  }

  if (wifiProfileCount > 0) {
    WIFI_SSID = wifiProfiles[0].ssid;
    WIFI_PASS = wifiProfiles[0].pass;
    currentWifiProfile = 0;
  } else {
    WIFI_SSID = "";
    WIFI_PASS = "";
    currentWifiProfile = -1;
  }
}

void addOrPromoteWifiCredential(const String& ssid, const String& pass, bool persist = true) {
  if (ssid.length() == 0) return;

  int existing = -1;
  for (uint8_t i = 0; i < wifiProfileCount; ++i) {
    if (wifiProfiles[i].ssid == ssid) {
      existing = i;
      break;
    }
  }

  if (existing >= 0) {
    wifiProfiles[existing].pass = pass;
    WiFiProfile entry = wifiProfiles[existing];
    for (int i = existing; i > 0; --i) {
      wifiProfiles[i] = wifiProfiles[i - 1];
    }
    wifiProfiles[0] = entry;
  } else {
    if (wifiProfileCount < WIFI_PROFILE_LIMIT) {
      wifiProfileCount++;
    }
    for (int i = wifiProfileCount - 1; i > 0; --i) {
      wifiProfiles[i] = wifiProfiles[i - 1];
    }
    wifiProfiles[0].ssid = ssid;
    wifiProfiles[0].pass = pass;
  }

  WIFI_SSID = wifiProfiles[0].ssid;
  WIFI_PASS = wifiProfiles[0].pass;
  currentWifiProfile = 0;

  if (persist) {
    persistWifiProfiles();
  }
}

// =================== SETTINGS (PERSISTENT) ===================
void saveSettings() {
  prefs.putInt("rotation", currentRotation);
  prefs.putInt("brightness", brightnessLevel);
  persistWifiProfiles();
  // Assets speichern (Komma-Liste)
  String aset = "";
  for (uint8_t i=0;i<NUM_SYMBOLS;i++){
    if (i) aset += ",";
    aset += symbols[i];
  }
  prefs.putString("assets", aset);
}

void loadSettings() {
  currentRotation = prefs.getInt("rotation", 1);
  brightnessLevel = prefs.getInt("brightness", 3);
  loadWifiProfiles();
  String assetStr = prefs.getString("assets", "BTC-USD,SOL-USD,ETH-USD,ADA-USD");
  int idx = 0;
  while (assetStr.length() > 0 && idx < NUM_SYMBOLS) {
    int comma = assetStr.indexOf(',');
    if (comma < 0) { symbols[idx++] = assetStr; break; }
    symbols[idx++] = assetStr.substring(0, comma);
    assetStr = assetStr.substring(comma + 1);
  }
}

// =================== BUTTONS ===================
void redrawAll() {
  tft.fillScreen(TFT_BLACK);
  drawHeader();
  for (uint8_t i = 0; i < ROWS; i++) drawRow(i, quotes[i]);
  updateBattery(true);
  drawFooter();
}

void toggleRotation() {
  currentRotation = (currentRotation == 1) ? 3 : 1;
  prefs.putInt("rotation", currentRotation);
  tft.setRotation(currentRotation);
  // Maße neu holen & Layout neu berechnen
  SCREEN_W = tft.width();  SCREEN_H = tft.height();
  CONTENT_Y = HEADER_H;    CONTENT_H = SCREEN_H - HEADER_H - FOOTER_H;
  ROW_H = CONTENT_H / ROWS;
  COL_SYMBOL_X = MARGIN;   COL_PRICE_XR = SCREEN_W - 88;  COL_CHG_XR = SCREEN_W - MARGIN;

  redrawAll();
  Serial.printf("Rotation geändert & gespeichert: %d\n", currentRotation);
}

void nextBrightness() {
  brightnessLevel++;
  if (brightnessLevel > BRIGHT_STEPS) brightnessLevel = 1;
  prefs.putInt("brightness", brightnessLevel);
  setBrightnessFromLevel(brightnessLevel);
  Serial.printf("Helligkeit auf Stufe %d\n", brightnessLevel);

  // kleine Anzeige im Footer
  drawFooter();
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_DARKGREY);
  tft.drawString("Brightness " + String(brightnessLevel), SCREEN_W/2, SCREEN_H - FOOTER_H/2, 2);
}

// =================== WLAN ===================
void askForWiFiCredentials() {
  Serial.println();
  Serial.println("⚙️ WLAN nicht verbunden.");
  Serial.println("Bitte SSID eingeben (Enter): ");
  while (Serial.available()) Serial.read();
  while (Serial.available() == 0) delay(10);
  String ssid = Serial.readStringUntil('\n'); ssid.trim();

  Serial.println("Bitte Passwort eingeben (Enter): ");
  while (Serial.available()) Serial.read();
  while (Serial.available() == 0) delay(10);
  String pass = Serial.readStringUntil('\n'); pass.trim();

  addOrPromoteWifiCredential(ssid, pass, true);
  Serial.println("✅ WLAN-Daten gespeichert!");
}

bool attemptWiFiProfile(uint8_t idx) {
  if (idx >= wifiProfileCount) return false;
  const String& ssid = wifiProfiles[idx].ssid;
  const String& pass = wifiProfiles[idx].pass;
  if (ssid.length() == 0) return false;

  Serial.println();
  Serial.print("🔌 Verbinde mit: ");
  Serial.println(ssid);

  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.print("Verbindung herstellen ...");

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < WIFI_CONNECT_TIMEOUT_MS) {
    Serial.print(".");
    delay(500);
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("✅ Verbunden! IP: "); Serial.println(WiFi.localIP());
    drawStatus("WLAN: " + WiFi.localIP().toString());
    addOrPromoteWifiCredential(ssid, pass, true);
    return true;
  }

  Serial.println("❌ Verbindung fehlgeschlagen.");
  return false;
}

bool connectWiFi(bool interactive = true) {
  if (wifiProfileCount == 0) {
    if (!interactive) return false;
    askForWiFiCredentials();
  }

  while (wifiProfileCount > 0) {
    Serial.println();
    Serial.println("🔌 Verwende gespeicherte WLAN-Daten:");
    for (uint8_t i = 0; i < wifiProfileCount; ++i) {
      Serial.printf("  %d/%d: %s\n", i + 1, wifiProfileCount, wifiProfiles[i].ssid.c_str());
    }

    WiFi.mode(WIFI_STA);

    for (uint8_t i = 0; i < wifiProfileCount; ++i) {
      WiFi.disconnect(true);
      delay(100);
      if (attemptWiFiProfile(i)) {
        currentWifiProfile = 0;
        lastWifiReconnectAttempt = millis();
        return true;
      }
    }

    if (!interactive) break;
    Serial.println("❌ Keine gespeicherten WLANs erreichbar. Bitte neue Daten eingeben.");
    askForWiFiCredentials();
  }

  return false;
}

void setupTime() {
  // NTP-Server einrichten
  configTzTime(MY_TZ, "pool.ntp.org", "time.nist.gov");
  Serial.println("⏳ Warte auf NTP-Zeit...");

  // Auf gültige Zeit warten (max. 10 s)
  struct tm timeinfo;
  for (int i = 0; i < 20; i++) {
    if (getLocalTime(&timeinfo)) {
      timeSynced = true;
      Serial.println("✅ Zeit synchronisiert:");
      Serial.println(&timeinfo, "%A, %d.%m.%Y %H:%M:%S");
      return;
    }
    delay(500);
  }

  Serial.println("⚠️  Keine Zeit vom NTP-Server erhalten.");
  timeSynced = false;
}

// =================== DATEN (COINGECKO) ===================
bool isCrypto(const String& sym, String& id) {
  String s = sym; s.toLowerCase();
  if (!s.endsWith("-usd")) return false;

  String base = s.substring(0, s.length() - 4); // ohne "-usd"
  // gängige mappings
  if      (base == "btc") id = "bitcoin";
  else if (base == "eth") id = "ethereum";
  else if (base == "sol") id = "solana";
  else if (base == "ada") id = "cardano";
  else if (base == "xrp") id = "ripple";
  else if (base == "dot") id = "polkadot";
  else if (base == "avax") id = "avalanche-2";
  else if (base == "doge") id = "dogecoin";
  else if (base == "ltc") id = "litecoin";
  else if (base == "bch") id = "bitcoin-cash";
  else if (base == "link") id = "chainlink";
  else if (base == "matic" || base == "pol") id = "polygon";
  else id = base; // fallback: probiere Basis als ID
  return true;
}

bool fetchCrypto(Quote* q, uint8_t n) {
  // IDs sammeln
  String ids = "";
  bool any = false;
  for (uint8_t i=0;i<n;i++){
    if (symbols[i].length()==0) continue;
    String id;
    if (isCrypto(symbols[i], id)) {
      if (ids.length()) ids += ",";
      ids += id;
      any = true;
    } else {
      // kein valides Krypto-Symbol: Flag auf "nicht ok"
      q[i].symbol = symbols[i];
      q[i].ok = false;
    }
  }
  if (!any) return true; // nichts zu holen

  String url="https://api.coingecko.com/api/v3/simple/price?vs_currencies=usd&include_24hr_change=true&ids="+ids;
  WiFiClientSecure c; c.setInsecure();
  HTTPClient h; h.setUserAgent("Mozilla/5.0");

  Serial.print("GET "); Serial.println(url);
  if(!h.begin(c,url)) return false;
  int code=h.GET();
  Serial.print("CG HTTP: "); Serial.println(code);
  if(code!=HTTP_CODE_OK){h.end();return false;}

  DynamicJsonDocument d(8192);
  DeserializationError err = deserializeJson(d, h.getString());
  h.end();
  if (err) { Serial.printf("CG JSON err: %s\n", err.c_str()); return false; }

  // Werte auf Quotes mappen
  for(uint8_t i=0;i<n;i++){
    String id;
    if (isCrypto(symbols[i], id) && d.containsKey(id)) {
      double price = d[id]["usd"] | NAN;
      double chg   = d[id]["usd_24h_change"] | NAN;
      q[i].symbol = symbols[i];
      q[i].price = price;
      q[i].changePct = chg;
      q[i].ok = !isnan(price);
    }
  }
  return true;
}

bool fetchQuotesAll(Quote* q, uint8_t n){
  for(uint8_t i=0;i<n;i++){ q[i] = Quote(); q[i].symbol = symbols[i]; }
  bool ok = fetchCrypto(q, n);
  return ok;
}

// =================== SERIAL COMMANDS ===================
void handleSerialCommands() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length()==0) return;

  if (line.equalsIgnoreCase("ASSETS?")) {
    Serial.println("Aktuelle Assets:");
    for (uint8_t i=0;i<NUM_SYMBOLS;i++){
      Serial.print("  "); Serial.print(i); Serial.print(": "); Serial.println(symbols[i]);
    }
    return;
  }

  if (line.startsWith("SET_ASSETS")) {
    int sp = line.indexOf(' ');
    if (sp < 0 || sp == (int)line.length()-1) {
      Serial.println("Syntax: SET_ASSETS sym1,sym2[,sym3[,sym4]]  (z.B. SET_ASSETS BTC-USD,ETH-USD)");
      return;
    }
    String list = line.substring(sp+1);
    list.trim();

    // parse 1..4 symbole
    String newSyms[4]; int count=0;
    while (list.length() > 0 && count < 4) {
      int comma = list.indexOf(',');
      String tok = (comma < 0) ? list : list.substring(0, comma);
      tok.trim();
      if (tok.length() > 0) newSyms[count++] = tok;
      if (comma < 0) break;
      list = list.substring(comma+1);
    }
    // übernehmen & auffüllen
    for (int i=0;i<4;i++){
      if (i < count) symbols[i] = newSyms[i];
      else symbols[i] = "";
    }
    saveSettings();
    Serial.println("✅ Assets gespeichert:");
    for (uint8_t i=0;i<NUM_SYMBOLS;i++){
      Serial.print("  "); Serial.print(i); Serial.print(": "); Serial.println(symbols[i]);
    }

    // sofort neu laden & zeichnen
    fetchQuotesAll(quotes, NUM_SYMBOLS);
    redrawAll();
    return;
  }

  if (line.equalsIgnoreCase("RESET_WIFI")) {
    wifiProfileCount = 0;
    WIFI_SSID = "";
    WIFI_PASS = "";
    currentWifiProfile = -1;
    persistWifiProfiles();
    Serial.println("🔄 WLAN-Daten gelöscht. Bitte neu starten (Reset).");
    return;
  }

  Serial.println("Unbekannter Befehl. Verfügbar: ASSETS? | SET_ASSETS ... | RESET_WIFI");
}

// =================== SETUP & LOOP ===================
void setup() {
  Serial.begin(115200);
  delay(100);

  //turn display on in case only battery is attached
  pinMode(15, OUTPUT);
  digitalWrite(15, HIGH);
  
  prefs.begin("display", false);
  loadSettings();                      // <-- zuerst laden!

  pinMode(BTN_ROTATE, INPUT_PULLUP);
  pinMode(BTN_BRIGHT, INPUT_PULLUP);
  pinMode(PIN_BAT_VOLT, INPUT);

  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BAT_VOLT, ADC_11db);

  tft.init();                          // <-- Display zuerst initialisieren
  tft.setRotation(currentRotation);
  tft.fillScreen(TFT_BLACK);

  // PWM erst NACH tft.init() anhängen (falls Lib vorher den Pin setzt)
  // und auf robuste 5 kHz / 8-bit gehen
  ledcSetup(BL_PWM_CH, 5000 /*Hz*/, 8 /*bits*/);
  ledcAttachPin(BL_PWM_PIN, BL_PWM_CH);
  setBrightnessFromLevel(brightnessLevel);   // <-- jetzt greift dein gespeicherter Level

  // Layout berechnen
  SCREEN_W = tft.width();  SCREEN_H = tft.height();
  CONTENT_Y = HEADER_H;    CONTENT_H = SCREEN_H - HEADER_H - FOOTER_H;
  ROW_H = CONTENT_H / ROWS;
  COL_SYMBOL_X = MARGIN;   COL_PRICE_XR = SCREEN_W - 88;  COL_CHG_XR = SCREEN_W - MARGIN;

  while (!connectWiFi(true)) {
    drawStatus("WLAN: keine Verbindung");
    delay(1000);
  }

  setupTime();

  drawHeader();
  fetchQuotesAll(quotes, NUM_SYMBOLS);
  for (uint8_t i=0;i<ROWS;i++) drawRow(i, quotes[i]);
  updateBattery(true);
  drawFooter();
  lastRefresh = millis();

  Serial.println();
  Serial.println("Befehle:");
  Serial.println("  ASSETS?                      -> aktuelle Assetliste");
  Serial.println("  SET_ASSETS BTC-USD,ETH-USD  -> neue Liste setzen (1..4)");
  Serial.println("  RESET_WIFI                   -> WLAN-Daten löschen");
}

void loop() {
  // Serielle Kommandos
  handleSerialCommands();

  if (WiFi.status() != WL_CONNECTED) {
    unsigned long now = millis();
    if (now - lastWifiReconnectAttempt > WIFI_RECONNECT_COOLDOWN_MS) {
      Serial.println("📶 WLAN getrennt. Versuche gespeicherte Netzwerke...");
      drawStatus("WLAN: neu verbinden...");
      bool connected = connectWiFi(false);
      lastWifiReconnectAttempt = millis();
      if (!connected) {
        drawStatus("WLAN: getrennt");
      }
    }
  }

  if (updateBattery()) {
    drawFooter();
  }

  // Rotation
  bool btnRot = digitalRead(BTN_ROTATE);
  if (btnRot != lastBtnStateRot) { lastDebounceRot = millis(); lastBtnStateRot = btnRot; }
  if ((millis() - lastDebounceRot) > debounceDelay && btnRot == LOW) { toggleRotation(); delay(300); }

  // Helligkeit
  bool btnBright = digitalRead(BTN_BRIGHT);
  if (btnBright != lastBtnStateBright) { lastDebounceBright = millis(); lastBtnStateBright = btnBright; }
  if ((millis() - lastDebounceBright) > debounceDelay && btnBright == LOW) { nextBrightness(); delay(300); }

  // Daten-Refresh
  if (millis() - lastRefresh >= REFRESH_MS) {
    drawStatus("Aktualisiere...");
    if (fetchQuotesAll(quotes, NUM_SYMBOLS)) {
      drawHeader();
      for (uint8_t i=0;i<ROWS;i++) drawRow(i, quotes[i]);
      drawFooter();
    }
    lastRefresh = millis();
  }

  delay(50);
}
