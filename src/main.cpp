// -----------------------------------------------------------------------------
// ÖBB Abfahrtsmonitor für ESP32 mit TFT Display (beta v0.1)
// Für LilyGo T-Display-S3
// -----------------------------------------------------------------------------

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <WiFiManager.h>
#include <time.h>
#include <TFT_eSPI.h>
#include <U8g2_for_TFT_eSPI.h>

// TODO: Für JPEG später - erstmal auskommentiert wegen File-Konflikt
// TODO: Uhrzeit dynamisch anzeigen
// TODO: Blink bei Daten Update entfernen
// #include <LittleFS.h>
// #include <JPEGDEC.h>

// -----------------------------------------------------------------------------
// Funktions-Prototypen (Forward Declarations)
// -----------------------------------------------------------------------------
void setupWiFi();
void setupTime();
String getCurrentTime(bool withSeconds = false);
void fetchAndDisplayDepartures();
String fetchOebbData(const String& stationId);
void parseOebbJson(const String& jsonPayload);
void drawScreen();
void drawHeader();
void drawDeparture(int index, int yPos);
void drawInfoLine(int index, int yPos);
void drawErrorScreen(const String& message);

// -----------------------------------------------------------------------------
// Globale Objekte und Variablen
// -----------------------------------------------------------------------------
TFT_eSPI tft = TFT_eSPI();
WiFiManager wm;
U8g2_for_TFT_eSPI u8g2;

// Konfiguration
char oebb_station_id[64] = "8100151"; // Gmunden
char oebb_track_id[8] = "";

// API URL
String oebbApiBaseUrl = "http://168.119.111.217:3000/oebb-departures";

// Timing
unsigned long lastApiCallTime = 0;
const long apiCallInterval = 30000; // 30 Sekunden
unsigned long lastInfoScrollTime = 0;
const int infoScrollInterval = 100;

// Abfahrtsinfo
struct DepartureInfo {
  String trainName;
  String trainType;
  String destination;
  String scheduledTime;
  String estimatedTime;
  String platform;
  String infoLine;
  bool isDelayed = false;
  bool dataValid = false;
  long delayInMinutes = 0;
  int infoScrollOffset = 0;
  bool needsScrolling = false;
};
DepartureInfo departures[2];

// Zeit
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3600;
const int daylightOffset_sec = 3600;

#define OBB_BLUE        0x001F        // dunkelblau (Hintergrund)
#define OBB_DARK_BLUE   0x000F        // noch dunkler, Header-Balken
#define OBB_RED         0xF800        // ÖBB-Rot
#define OBB_PURPLE      0x0801        // Violetter Pill-Hintergrund
#define OBB_LIGHT_BLUE  0x3AEF        // helleres Blau
#define TEXT_WHITE      0xFFFF
#define TEXT_YELLOW     0xFFE0
#define TEXT_GREY       0x8410

// ------------------ Helper: Textbereich komplett entfernen ------------------
#define CLEAR_AREA(x, y, w, h, col)  tft.fillRect((x), (y), (w), (h), (col))


// -----------------------------------------------------------------------------
//  Layout
// -----------------------------------------------------------------------------
const int HEADER_HEIGHT       = 30;
const int CARD_HEADER_HEIGHT  = 0;
const int DEPARTURE_HEIGHT    = 106;
const int INFO_HEIGHT         = 24;
const int TIME_PILL_RADIUS    = 6;

#define FONT_SMALL   u8g2_font_helvR10_tf
#define FONT_MEDIUM  u8g2_font_helvR14_tf
#define FONT_BIG     u8g2_font_helvR24_tf

// -----------------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nÖBB Abfahrtsmonitor startet...");

  // Display initialisieren
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(OBB_BLUE);

  // Backlight einschalten
  #ifdef TFT_BL
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);
  #endif

  // Startbildschirm
  tft.setTextColor(TEXT_WHITE, OBB_BLUE);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("OBB Monitor", tft.width()/2, tft.height()/2 - 20, 4);
  tft.drawString("Initialisiere...", tft.width()/2, tft.height()/2 + 20, 2);
  tft.setTextDatum(TL_DATUM);

  u8g2.begin(tft);
  u8g2.setFont(u8g2_font_helvR14_tf);
  u8g2.setFontMode(1);
  u8g2.setFontDirection(0);
  u8g2.setForegroundColor(TEXT_WHITE);

  // WiFi Setup
  setupWiFi();

  // Zeit synchronisieren
  setupTime();

  // Erste Datenabfrage
  fetchAndDisplayDepartures();
  lastApiCallTime = millis();
}

// -----------------------------------------------------------------------------
// WiFi Setup
// -----------------------------------------------------------------------------
void setupWiFi() {
  WiFiManagerParameter custom_station_id("station", "Bahnhofs-ID", oebb_station_id, 64);
  WiFiManagerParameter custom_track_id("track", "Gleis (leer = alle)", oebb_track_id, 8);

  wm.addParameter(&custom_station_id);
  wm.addParameter(&custom_track_id);
  wm.setConfigPortalTimeout(180);

  tft.fillScreen(OBB_BLUE);
  tft.setTextColor(TEXT_WHITE, OBB_BLUE);
  tft.drawString("WiFi Setup", 10, 10, 4);
  tft.drawString("Verbinde mit:", 10, 50, 2);
  tft.drawString("OEBB-Monitor-Setup", 10, 80, 2);

  if (!wm.autoConnect("OEBB-Monitor-Setup")) {
    Serial.println("WiFi Verbindung fehlgeschlagen!");
    tft.fillScreen(OBB_RED);
    tft.drawString("WiFi Fehler!", 10, 50, 4);
    delay(3000);
    ESP.restart();
  }

  strcpy(oebb_station_id, custom_station_id.getValue());
  strcpy(oebb_track_id, custom_track_id.getValue());

  Serial.println("WiFi verbunden!");
  Serial.print("IP: "); Serial.println(WiFi.localIP());
  Serial.print("Station: "); Serial.println(oebb_station_id);
  Serial.print("Gleis: "); Serial.println(strlen(oebb_track_id) == 0 ? "ALLE" : oebb_track_id);

  // Warte auf DNS
  Serial.println("Warte auf DNS...");
  delay(2000);

  // DNS Test
  IPAddress ip;
  if (WiFi.hostByName("google.com", ip)) {
    Serial.println("DNS funktioniert!");
  } else {
    Serial.println("DNS Problem - setze manuellen DNS");
    WiFi.disconnect();
    delay(100);
    WiFi.begin();
    WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(),
                IPAddress(8,8,8,8), IPAddress(8,8,4,4));
    delay(2000);
  }
}

// -----------------------------------------------------------------------------
// Zeit Setup
// -----------------------------------------------------------------------------
void setupTime() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 10000)) {
    Serial.println("NTP Fehler");
  }
}

String getCurrentTime(bool withSeconds) {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo, 1000)) {
    return withSeconds ? "--:--:--" : "--:--";
  }
  char timeStr[9];
  strftime(timeStr, sizeof(timeStr), withSeconds ? "%H:%M:%S" : "%H:%M", &timeinfo);
  return String(timeStr);
}

// -----------------------------------------------------------------------------
// Daten abrufen und anzeigen
// -----------------------------------------------------------------------------
void fetchAndDisplayDepartures() {
  // Reset der Daten
  for(int i = 0; i < 2; i++) {
    departures[i].dataValid = false;
  }

  if (WiFi.status() == WL_CONNECTED) {
    String apiResponse = fetchOebbData(oebb_station_id);
    if (!apiResponse.isEmpty()) {
      parseOebbJson(apiResponse);
    } else {
      Serial.println("Keine API Antwort erhalten");
      drawErrorScreen("Keine Daten");
      return;
    }
  } else {
    drawErrorScreen("Kein WiFi");
    return;
  }

  drawScreen();
}

// -----------------------------------------------------------------------------
// API Abruf
// -----------------------------------------------------------------------------
String fetchOebbData(const String& stationId) {
  HTTPClient http;
  WiFiClient client;

  String url = oebbApiBaseUrl + "?stationID=" + stationId;
  Serial.println("Rufe ab: " + url);

  http.begin(client, url);
  http.setTimeout(15000);
  http.addHeader("User-Agent", "ESP32-OeBB-Monitor/1.0");
  http.addHeader("Accept", "application/json");

  int httpCode = http.GET();
  Serial.println("HTTP Code: " + String(httpCode));

  String payload = "";
  if (httpCode == HTTP_CODE_OK) {
    payload = http.getString();
    Serial.println("Payload Länge: " + String(payload.length()));
    if (payload.length() > 0) {
      Serial.println("Payload Anfang: " + payload.substring(0, 200));
    }
  } else {
    Serial.println("HTTP Fehler: " + http.errorToString(httpCode));
    String errorPayload = http.getString();
    if (errorPayload.length() > 0) {
      Serial.println("Error Response: " + errorPayload);
    }
  }

  http.end();
  return payload;
}

// -----------------------------------------------------------------------------
// JSON Parsing
// -----------------------------------------------------------------------------
void parseOebbJson(const String& jsonPayload) {
  DynamicJsonDocument doc(32768);
  DeserializationError error = deserializeJson(doc, jsonPayload);

  if (error) {
    Serial.println("JSON Parse Fehler: " + String(error.c_str()));
    return;
  }

  JsonArray departuresArray = doc["departures"];
  if (departuresArray.isNull()) {
    Serial.println("Keine 'departures' im JSON gefunden");
    return;
  }

  Serial.println("Anzahl Abfahrten: " + String(departuresArray.size()));

  int depIndex = 0;
  bool filterByTrack = (strlen(oebb_track_id) > 0);

  // Durchlaufe alle Abfahrten
  for (JsonObject dep : departuresArray) {
    if (depIndex >= 1) break;

    // Gleis-Filter
    if (filterByTrack) {
      String platform = dep["platform"] | "";
      if (platform != String(oebb_track_id)) continue;
    }

    // Parse Departure Info
    DepartureInfo& current = departures[depIndex];

    // Zugname aus der komplexen Struktur extrahieren
    String lineName = dep["line"]["name"] | "???";
    // Format ist z.B. "R 53 (Zug-Nr. 3210)" - extrahieren zu "R 53"
    int bracketPos = lineName.indexOf(" (");
    if (bracketPos > 0) {
      lineName = lineName.substring(0, bracketPos);
    }

    // Teile in Typ und Nummer
    int spacePos = lineName.indexOf(' ');
    if (spacePos > 0) {
      current.trainType = lineName.substring(0, spacePos);
      current.trainName = lineName.substring(spacePos + 1);
    } else {
      current.trainType = lineName;
      current.trainName = "";
    }

    current.destination = dep["direction"] | "Unbekannt";
    current.platform = dep["platform"] | "";

    // Zeiten
    String plannedWhen = dep["plannedWhen"] | "";
    String when = dep["when"] | "";

    // Extrahiere Zeit aus ISO-Format (2025-05-26T10:16:00+02:00)
    if (plannedWhen.length() >= 19) {
      current.scheduledTime = plannedWhen.substring(11, 16);
    } else {
      current.scheduledTime = "--:--";
    }

    if (when.length() >= 19) {
      current.estimatedTime = when.substring(11, 16);
    } else {
      current.estimatedTime = current.scheduledTime;
    }

    // Verspätung
    if (dep.containsKey("delay") && !dep["delay"].isNull()) {
      long delaySeconds = dep["delay"];
      current.isDelayed = (delaySeconds > 0);
      current.delayInMinutes = delaySeconds / 60;
    } else {
      current.isDelayed = (current.estimatedTime != current.scheduledTime);
    }

    // Info-Zeile aus remarks
    current.infoLine = "";
    JsonArray remarks = dep["remarks"];
    if (!remarks.isNull()) {
      // Suche nach interessanten Hinweisen
      for (JsonObject remark : remarks) {
        String code = remark["code"] | "";
        String text = remark["text"] | "";

        // Wichtige Codes herausfiltern
        if (code == "FK" || code == "OB" || code == "RO") {
          if (!current.infoLine.isEmpty()) current.infoLine += " | ";
          current.infoLine += text;
          if (current.infoLine.length() > 50) break;
        }
      }
    }

    current.dataValid = true;
    current.infoScrollOffset = 0;

    Serial.print("Abfahrt "); Serial.print(depIndex);
    Serial.print(": "); Serial.print(current.trainType);
    Serial.print(" "); Serial.print(current.trainName);
    Serial.print(" -> "); Serial.print(current.destination);
    Serial.print(" um "); Serial.print(current.scheduledTime);
    Serial.print(" Gleis "); Serial.println(current.platform);

    depIndex++;
  }

  Serial.println("Geparste Abfahrten: " + String(depIndex));
}

//------------------------------------------------------------------
//  Hilfsfunktionen
//------------------------------------------------------------------
static inline void setU8(const uint16_t txtColor)
{
  u8g2.setFontMode(1);
  u8g2.setForegroundColor(txtColor);
}

//------------------------------------------------------------------
//  Bildschirm komplett aufbauen
//------------------------------------------------------------------
void drawScreen()
{
  tft.fillScreen(OBB_BLUE);
  drawHeader();

  int yPos = HEADER_HEIGHT;
  for (int i = 0; i < 2; ++i) {
    if (departures[i].dataValid) {
      drawDeparture(i, yPos);
      yPos += DEPARTURE_HEIGHT;
    }
  }

  // Fläche unter allen Karten
  tft.fillRect(0, yPos, tft.width(), tft.height() - yPos, OBB_DARK_BLUE);

  if (!departures[0].dataValid) {
    u8g2.setFont(FONT_MEDIUM);
    u8g2.setForegroundColor(TEXT_WHITE);
    u8g2.setBackgroundColor(OBB_DARK_BLUE);
    u8g2.setCursor(tft.width()/2 - u8g2.getUTF8Width("Keine Abfahrten")/2,
                   tft.height()/2 + u8g2.getFontAscent()/2);
    u8g2.print("Keine Abfahrten");
  }
}


//------------------------------------------------------------------
//  Globaler Header – ÖBB-Logo links, Uhrzeit rechts
//------------------------------------------------------------------
void drawHeader()
{
  tft.fillRect(0, 0, tft.width(), HEADER_HEIGHT, OBB_DARK_BLUE);

  u8g2.setFontMode(1);

  // -------- linker Teil: ÖBB + Zugtyp/-nr --------------------------------
  u8g2.setFont(FONT_MEDIUM);

  int yBase = 6 + u8g2.getFontAscent();

  // "ÖBB" rot
  u8g2.setForegroundColor(OBB_RED);
  u8g2.setBackgroundColor(OBB_DARK_BLUE);
  u8g2.setCursor(10, yBase);
  u8g2.print("ÖBB");

  // unmittelbar dahinter: R 53, RJX 768, …
  if (departures[0].dataValid) {
    String train = departures[0].trainType;
    if (!departures[0].trainName.isEmpty()) train += " " + departures[0].trainName;

    u8g2.setForegroundColor(TEXT_WHITE);
    u8g2.setBackgroundColor(OBB_DARK_BLUE);
    u8g2.setCursor(60, yBase);
    u8g2.print(train);
  }

  // -------- rechter Teil: Zeit-Pill --------------------------------------
  String clk = getCurrentTime(true);
  u8g2.setFont(FONT_SMALL);
  int pillW = u8g2.getUTF8Width(clk.c_str()) + 14;
  int pillX = tft.width() - pillW - 10;

  CLEAR_AREA(pillX, 0, pillW, HEADER_HEIGHT, OBB_DARK_BLUE);
  tft.fillRoundRect(pillX, 4, pillW, HEADER_HEIGHT - 8,
                    TIME_PILL_RADIUS, OBB_PURPLE);

  u8g2.setForegroundColor(TEXT_WHITE);
  u8g2.setBackgroundColor(OBB_DARK_BLUE);
  u8g2.setCursor(pillX + 7, yBase);
  u8g2.print(clk);
}



//------------------------------------------------------------------
//  Eine Abfahrtskarte
//------------------------------------------------------------------
void drawDeparture(int idx, int yTop)
{
  DepartureInfo& d = departures[idx];
  tft.fillRect(0, yTop, tft.width(), DEPARTURE_HEIGHT, OBB_BLUE);

  u8g2.setFontMode(1);


  // ---------- Planzeit ---------------------------------------------------
  u8g2.setFont(FONT_BIG);
  int yTime = yTop + 8 + u8g2.getFontAscent();
  int wTime = u8g2.getUTF8Width(d.scheduledTime.c_str());
  CLEAR_AREA(10, yTop + 4, wTime + 4, 40, OBB_BLUE);

  u8g2.setForegroundColor(TEXT_WHITE);
  u8g2.setBackgroundColor(OBB_BLUE);
  u8g2.setCursor(10, yTime);
  u8g2.print(d.scheduledTime);

  // ---------- Estimated (gelb) -------------------------------------------
  if (d.isDelayed && d.estimatedTime != d.scheduledTime) {
    u8g2.setFont(FONT_MEDIUM);
    int xEst = 10 + wTime + 8;
    int yEst = yTop + 10 + u8g2.getFontAscent();
    int wEst = u8g2.getUTF8Width(d.estimatedTime.c_str());
    CLEAR_AREA(xEst, yTop + 4, wEst + 4, 28, OBB_BLUE);
    u8g2.setForegroundColor(TEXT_YELLOW);
    u8g2.setCursor(xEst, yEst);
    u8g2.print(d.estimatedTime);
  }

  // ---------- Destination -------------------------------------------------
  u8g2.setFont(FONT_MEDIUM);
  u8g2.setForegroundColor(TEXT_WHITE);
  u8g2.setBackgroundColor(OBB_BLUE);

  String dest = d.destination;
  int maxW = tft.width() - 10;
  while (u8g2.getUTF8Width(dest.c_str()) > maxW && dest.length() > 3) dest.remove(dest.length()-1);
  if (dest != d.destination) dest += "…";

  int yDest = yTop + 48 + u8g2.getFontAscent();
  CLEAR_AREA(10, yTop + 40, tft.width() - 20, 28, OBB_BLUE);
  u8g2.setCursor(10, yDest);
  u8g2.print(dest);

  // ---------- Info-Bar ----------------------------------------------------
  if (!d.infoLine.isEmpty())
    drawInfoLine(idx, yTop + DEPARTURE_HEIGHT - INFO_HEIGHT);
}



//------------------------------------------------------------------
//  Info-Bar einer Karte (UTF-8 + Scrolling)
//------------------------------------------------------------------
void drawInfoLine(int idx, int yPos)
{
  DepartureInfo& d = departures[idx];

  // grauer Balken
  tft.fillRect(0, yPos, tft.width(), INFO_HEIGHT, TEXT_GREY);
  // dunkelblauer Abschluss
  tft.fillRect(0, yPos + INFO_HEIGHT, tft.width(), 6, OBB_DARK_BLUE);

  u8g2.setFont(u8g2_font_helvR10_tf);
  u8g2.setFontMode(1);

  u8g2.setForegroundColor(OBB_DARK_BLUE);

  int txtW = u8g2.getUTF8Width(d.infoLine.c_str());
  int view = tft.width() - 20;
  int base = yPos + 4 + u8g2.getFontAscent();

  if (txtW > view) {
    d.needsScrolling = true;
    int x = 10 - d.infoScrollOffset;

    u8g2.setCursor(x, base);
    u8g2.print(d.infoLine);
    if (x + txtW < tft.width()) {
      u8g2.setCursor(x + txtW + 50, base);
      u8g2.print(d.infoLine);
    }

    if (millis() - lastInfoScrollTime > infoScrollInterval) {
      d.infoScrollOffset = (d.infoScrollOffset + 2) % (txtW + 50);
      lastInfoScrollTime = millis();
    }
  }
  else {
    d.needsScrolling = false;
    u8g2.setCursor(10, base);
    u8g2.print(d.infoLine);
  }
}



void drawErrorScreen(const String& message) {
  tft.fillScreen(OBB_RED);
  tft.setTextColor(TEXT_WHITE, OBB_RED);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("FEHLER", tft.width()/2, tft.height()/2 - 20, 4);
  tft.drawString(message, tft.width()/2, tft.height()/2 + 20, 2);
  tft.setTextDatum(TL_DATUM);
}

// -----------------------------------------------------------------------------
// Loop
// -----------------------------------------------------------------------------
void loop() {
  // WiFiManager Process
  wm.process();

  // Periodische Datenaktualisierung
  if (millis() - lastApiCallTime > apiCallInterval) {
    fetchAndDisplayDepartures();
    lastApiCallTime = millis();
  }

  // Info-Zeilen Scrolling Update
  bool needsRedraw = false;
  for (int i = 0; i < 2; i++) {
    if (departures[i].dataValid && departures[i].needsScrolling) {
      if (millis() - lastInfoScrollTime > infoScrollInterval) {
        needsRedraw = true;
      }
    }
  }

  if (needsRedraw) {
    // Nur Info-Zeilen neu zeichnen
    int yPos = HEADER_HEIGHT;
    for (int i = 0; i < 2; i++) {
      if (departures[i].dataValid && !departures[i].infoLine.isEmpty()) {
        drawInfoLine(i, yPos + DEPARTURE_HEIGHT - INFO_HEIGHT);
      }
      yPos += DEPARTURE_HEIGHT;
    }
  }

  delay(10);
}