/*
 * =====================================================================================
 *  ESP32 ULTIMATE NETWORK & SYSTEM DASHBOARD
 * =====================================================================================
 *
 *  Zielhardware : ESP32-WROOM-32 (klassisches Modul, Dual-Core Xtensa LX6, BLE 4.2)
 *  Entwicklung  : Arduino IDE, ESP32 Arduino Core 3.x
 *
 *  Dieses Programm verwandelt einen nackten ESP32-WROOM-32 (ohne jede zusaetzliche
 *  Hardware - keine Sensoren, keine Displays, keine Taster/LEDs) in ein kleines,
 *  professionell wirkendes Netzwerk- und System-Management-Dashboard, das vollstaendig
 *  ueber einen Webbrowser bedient wird.
 *
 *  WICHTIGER HINWEIS ZU TECHNISCHEN GRENZEN (bitte lesen):
 *  ---------------------------------------------------------------------------------
 *  - Der ESP32-WROOM-32 besitzt genau EIN WLAN-Funkmodul. Er kann daher zur gleichen
 *    Zeit nur EIN einziges SoftAP-Netzwerk (eine SSID) aussenden. Der "WLAN Beacon
 *    Manager" verwaltet deshalb mehrere SSID-PROFILE, von denen jederzeit nur eines
 *    wirklich aktiv senden kann - optional im automatischen, zeitgetakteten Wechsel
 *    (Rotation). Es werden NIEMALS mehrere echte, gleichzeitige SoftAPs vorgetaeuscht.
 *  - Der ESP32-WROOM-32 nutzt einen klassischen Bluetooth-4.2-Controller (Bluedroid-
 *    Stack). Dieser stellt genau EINE Advertising-Instanz zur Verfuegung (kein
 *    "Bluetooth 5 Extended Advertising" mit mehreren parallelen Advertising-Sets, wie
 *    es z.B. ein ESP32-S3/C3 koennte). Der "BLE Beacon Manager" verwaltet deshalb
 *    mehrere Beacon-PROFILE, von denen ebenfalls nur eines gleichzeitig senden kann -
 *    ebenfalls optional im automatischen Wechsel.
 *  - Die interne Temperatur-Funktion des klassischen ESP32 (temperatureRead()) beruht
 *    auf einem vom Hersteller nie offiziell dokumentierten ROM-Aufruf. Sie wird in
 *    diesem Dashboard angezeigt, aber deutlich als "inoffiziell / unkalibriert"
 *    gekennzeichnet - es wird nichts erfunden oder beschoenigt.
 *  - Es werden ausschliesslich Boardfunktionen genutzt, die im ESP32 Arduino Core
 *    tatsaechlich vorhanden sind. Keine Angriffs-, Stoer- oder Deauth-Funktionen.
 *
 *  ARCHITEKTUR (siehe Abschnittsmarkierungen weiter unten):
 *    [1]  Includes & Konfiguration
 *    [2]  Globale Datenstrukturen & Variablen
 *    [3]  Logging-System
 *    [4]  Persistenz (Preferences/NVS)
 *    [5]  System-Informationen & CPU-Last
 *    [6]  WLAN (Station Mode)
 *    [7]  Access Point / SoftAP
 *    [8]  WLAN-Scanner
 *    [9]  WLAN-Beacon-Manager (SSID-Profile)
 *    [10] BLE Core (Scan)
 *    [11] BLE-Beacon-Manager (iBeacon-Profile)
 *    [12] Netzwerk-Diagnose (Ping/DNS/TCP)
 *    [13] JSON-Hilfsfunktionen
 *    [14] Web-Oberflaeche (HTML/CSS/JS, PROGMEM)
 *    [15] Webserver & REST-API
 *    [16] Setup & Loop
 *
 * =====================================================================================
 */

// =====================================================================================
// [1] INCLUDES & KONFIGURATION
// =====================================================================================

// Vorwaertsdeklaration: Arduino fuegt automatisch generierte Funktions-Prototypen direkt
// nach dem letzten #include ein. Da mehrere Funktionen weiter unten "JsonWriter&" als
// Parameter nutzen, muss der Klassenname bereits VOR allen #include-Zeilen bekannt sein,
// damit die automatisch eingefuegten Prototypen ihn kennen.
class JsonWriter;

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <esp_wifi_ap_get_sta_list.h>
#include <esp_system.h>
#include <esp_bt.h>
#include <esp_task_wdt.h>
#include <lwip/ip_addr.h>
#include "ping/ping_sock.h"

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEAdvertising.h>
#include <BLEBeacon.h>

// Portabler Advertising-Typ "Non-connectable undirected" (Bluetooth-Core-Spezifikation,
// Rohwert 0x03) fuer den iBeacon-Modus im BLE-Beacon-Manager:
//
// Seit Anfang 2025 (ca. ESP32-Arduino-Core 3.2.x) wurde die im Core eingebaute "BLE"-
// Bibliothek vereinheitlicht und kann wahlweise auf dem klassischen Bluedroid-Stack oder
// auf NimBLE laufen (je nach Core-Version/Zielchip/Boardmenue-Standardwert). Je nachdem,
// welcher Stack aktiv ist, aendert sich sowohl die Verfuegbarkeit von esp_gap_ble_api.h
// als auch der von BLEAdvertising::setAdvertisementType() erwartete Parametertyp:
//   - Bluedroid (aeltere Cores, z.B. 3.1.x): Methode erwartet den Enum-Typ
//     esp_ble_adv_type_t aus esp_gap_ble_api.h (dort: ADV_TYPE_NONCONN_IND = 0x03).
//   - Neuere, vereinheitlichte Bibliothek (ab ca. 3.2.x, unabhaengig vom aktiven Stack):
//     Methode erwartet direkt ein uint8_t - esp_gap_ble_api.h existiert dann je nach
//     Zielchip/Boardkonfiguration unter Umstaenden gar nicht mehr im SDK.
// __has_include() entscheidet hier automatisch, welcher Fall vorliegt, damit derselbe
// Sketch unveraendert gegen beide Bibliotheksvarianten kompiliert.
#if __has_include(<esp_gap_ble_api.h>)
#include <esp_gap_ble_api.h>
static const esp_ble_adv_type_t kBeaconAdvType = ADV_TYPE_NONCONN_IND;
#else
static const uint8_t kBeaconAdvType = 0x03;  // ADV_TYPE_NONCONN_IND laut BLE-Spezifikation
#endif

// Groesserer Stack fuer den Arduino-Loop-Task, da der Webserver synchron aus loop()
// bedient wird und dabei zeitweise groessere JSON-Puffer auf dem Stack referenziert.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// ---- Firmware-Identifikation --------------------------------------------------------
#define APP_NAME    "ESP32 Ultimate Dashboard"
#define APP_VERSION "1.0.0"

// ---- Sinnvolle, speicherbewusst gewaehlte Obergrenzen -------------------------------
// Diese Grenzen sind NICHT das Funk-Hardwarelimit (das liegt bei 1, siehe oben),
// sondern die Anzahl an Profilen, die der ESP32-WROOM-32 komfortabel im RAM/NVS
// verwalten kann, ohne den verfuegbaren Heap unnoetig zu belasten.
#define MAX_WIFI_BEACONS   6
#define MAX_BLE_BEACONS    10
#define MAX_BLE_DEVICES    30
#define MAX_LOG_ENTRIES    60
#define WIFI_HW_AP_LIMIT   1   // echte, gleichzeitig aktive SoftAPs
#define BLE_HW_ADV_LIMIT   1   // echte, gleichzeitig aktive BLE-Advertiser

#define HISTORY_LEN        40  // Punkte in den Live-Diagrammen (Heap/RSSI/Scan/BLE)

// ---- Web-Server ----------------------------------------------------------------------
WebServer server(80);

// =====================================================================================
// [2] GLOBALE DATENSTRUKTUREN & VARIABLEN
// =====================================================================================

// ---- Log-Level ------------------------------------------------------------------------
enum LogLevel : uint8_t { LOG_DEBUG = 0, LOG_INFO = 1, LOG_WARN = 2, LOG_ERROR = 3 };

struct LogEntry {
  uint32_t t;       // millis() Zeitstempel
  uint8_t level;
  char msg[72];
};

static LogEntry logBuf[MAX_LOG_ENTRIES];
static uint16_t logHead = 0;     // naechster Schreib-Index
static uint16_t logCount = 0;
static uint8_t  logMinLevel = LOG_INFO;

// ---- Dashboard-Zugangsdaten (HTTP Basic Auth) -----------------------------------------
static char authUser[24] = "admin";
static char authPass[32] = "admin";

// ---- WLAN Station-Status --------------------------------------------------------------
static char staSavedSsid[33] = "";
static char staSavedPass[65] = "";
static bool staAutoConnect = true;
static uint32_t staConnectedSince = 0;
static uint32_t staLastAttempt = 0;
static bool staConnectInProgress = false;
static String lastWifiError = "";

// ---- WLAN Scan Status -------------------------------------------------------------------
static bool scanInProgress = false;
static uint32_t scanStartedAt = 0;
static int16_t  lastScanCount = -999;

// ---- Access Point (aktuell wirksame Konfiguration, von "Access Point"-Seite ODER
//      vom WLAN-Beacon-Manager geschrieben - es gibt nur EIN echtes SoftAP) -------------
struct ApConfig {
  char ssid[33]    = "ESP32-Dashboard";
  char password[65] = "dashboard123";
  uint8_t channel   = 1;
  bool hidden       = false;
  uint8_t maxConn   = 4;
  bool enabled      = false;
};
static ApConfig apCfg;
static uint8_t wifiModeSel = 0;  // 0=STA, 1=AP, 2=AP+STA

// ---- WLAN-Beacon-Manager (SSID-Profile) ------------------------------------------------
struct WifiBeacon {
  char name[24]     = "";
  char ssid[33]     = "";
  char password[65] = "";
  uint8_t channel   = 1;
  bool hidden       = false;
  bool enabled      = false;  // vom Nutzer gewuenscht aktiv
};
static WifiBeacon wifiBeacons[MAX_WIFI_BEACONS];
static uint8_t  wifiBeaconCount = 0;
static int8_t   wifiBeaconActive = -1;     // Index des aktuell sendenden Profils, -1 = keins
static bool     wifiRotationEnabled = false;
static uint32_t wifiRotationIntervalMs = 6000;
static uint32_t wifiRotationLastSwitch = 0;
static uint32_t wifiBeaconLastStart = 0;
static char     wifiBeaconLastError[48] = "";

// ---- BLE Core ---------------------------------------------------------------------------
static bool bleInitialized = false;
BLEScan* pBLEScan = nullptr;
static bool bleScanActive = false;
static uint32_t bleScanStartedAt = 0;
static uint32_t bleScanDurationSec = 8;

struct BleDeviceInfo {
  char addr[18] = "";
  char name[32] = "";
  int rssi = 0;
  bool hasName = false;
  bool hasMfg = false;
  char mfgHex[48] = "";
  bool hasServiceUUID = false;
  char serviceUUID[40] = "";
  uint32_t lastSeen = 0;
  uint32_t firstSeen = 0;
};
static BleDeviceInfo bleDevices[MAX_BLE_DEVICES];
static uint8_t bleDeviceCount = 0;

// ---- BLE-Beacon-Manager (iBeacon-Profile) -----------------------------------------------
struct BleBeaconCfg {
  char name[24]   = "";
  char uuid[37]   = "e1a2b3c4-d5e6-f789-0123-456789abcdef";
  uint16_t major  = 1;
  uint16_t minor  = 1;
  int8_t txPower  = -59;   // kalibrierter Referenzwert im iBeacon-Frame (1m-Messwert)
  uint16_t advIntervalMs = 400;
  bool enabled    = false;
};
static BleBeaconCfg bleBeacons[MAX_BLE_BEACONS];
static uint8_t  bleBeaconCount = 0;
static int8_t   bleBeaconActive = -1;
static bool     bleRotationEnabled = false;
static uint32_t bleRotationIntervalMs = 5000;
static uint32_t bleRotationLastSwitch = 0;
static uint32_t bleBeaconLastStart = 0;
static char     bleBeaconLastError[48] = "";

// ---- Netzwerk-Diagnose: Ping ------------------------------------------------------------
struct PingState {
  volatile bool active = false;
  volatile bool done = false;
  volatile bool resolveError = false;
  char host[64] = "";
  char ip[40] = "";
  volatile uint32_t transmitted = 0;
  volatile uint32_t received = 0;
  volatile float minMs = 0, maxMs = 0, avgMs = 0, lastMs = 0;
  volatile uint32_t sumMs = 0;
};
static PingState pingState;
static esp_ping_handle_t pingHandle = nullptr;

// ---- Verlaufsdaten fuer Live-Diagramme ---------------------------------------------------
static uint32_t heapHistory[HISTORY_LEN];
static int16_t  rssiHistory[HISTORY_LEN];
static uint8_t  wifiCountHistory[HISTORY_LEN];
static uint8_t  bleCountHistory[HISTORY_LEN];
static uint8_t  historyIdx = 0;
static uint32_t lastHistoryTick = 0;

// ---- CPU-Auslastung (gemessen ueber FreeRTOS-Laufzeitstatistik der IDLE-Tasks) -----------
static float cpuUsageCore0 = 0, cpuUsageCore1 = 0;
static uint32_t lastCpuSample = 0;
static uint64_t prevIdle0 = 0, prevIdle1 = 0, prevTotal = 0;
static bool cpuStatsAvailable = false;

// ---- Preferences-Namespaces ---------------------------------------------------------------
Preferences prefsNet;    // STA + AP + Modus
Preferences prefsDash;   // Auth, Loglevel
Preferences prefsWB;     // WLAN-Beacon-Profile
Preferences prefsBB;     // BLE-Beacon-Profile

// ---- Sonstiges --------------------------------------------------------------------------
static uint32_t bootTime = 0;
static char lastErrorMsg[96] = "";
static uint32_t lastErrorTime = 0;

// Vorab-Deklarationen (werden von mehreren Abschnitten gegenseitig benoetigt)
void logMsg(uint8_t level, const char* fmt, ...);
void applyWifiModeInternal();
bool applyApConfig(const char* ssid, const char* pass, uint8_t channel, bool hidden, uint8_t maxConn, bool enable);


// =====================================================================================
// [3] LOGGING-SYSTEM
// =====================================================================================

const char* levelName(uint8_t lvl) {
  switch (lvl) {
    case LOG_DEBUG: return "DEBUG";
    case LOG_INFO:  return "INFO";
    case LOG_WARN:  return "WARN";
    case LOG_ERROR: return "ERROR";
    default: return "?";
  }
}

void logMsg(uint8_t level, const char* fmt, ...) {
  if (level < logMinLevel) return;
  LogEntry &e = logBuf[logHead];
  e.t = millis();
  e.level = level;
  va_list args;
  va_start(args, fmt);
  vsnprintf(e.msg, sizeof(e.msg), fmt, args);
  va_end(args);
  logHead = (logHead + 1) % MAX_LOG_ENTRIES;
  if (logCount < MAX_LOG_ENTRIES) logCount++;

  if (level == LOG_ERROR) {
    strlcpy(lastErrorMsg, e.msg, sizeof(lastErrorMsg));
    lastErrorTime = e.t;
  }
  Serial.printf("[%8lu][%s] %s\n", (unsigned long)e.t, levelName(level), e.msg);
}

void clearLogs() {
  logHead = 0;
  logCount = 0;
}

// =====================================================================================
// [13] JSON-HILFSFUNKTIONEN  (vor [4]-[12] platziert, da von vielen build*Json() genutzt)
// =====================================================================================

class JsonWriter {
public:
  JsonWriter(char* buf, size_t cap) : _buf(buf), _cap(cap), _len(0), _depth(-1) {
    _buf[0] = 0;
  }
  void rawc(char c) {
    if (_len + 1 < _cap) { _buf[_len++] = c; _buf[_len] = 0; }
  }
  void raw(const char* s) {
    size_t l = strlen(s);
    if (_len + l < _cap) { memcpy(_buf + _len, s, l); _len += l; _buf[_len] = 0; }
  }
  void comma() {
    if (_depth >= 0) {
      if (_needComma[_depth]) rawc(',');
      _needComma[_depth] = true;
    }
  }
  void key(const char* k) { comma(); rawc('"'); raw(k); raw("\":"); }

  void beginObj() { comma(); rawc('{'); _pushDepth(); }
  void beginObj(const char* k) { key(k); rawc('{'); _pushDepth(); }
  void endObj() { rawc('}'); _popDepth(); }

  void beginArr() { comma(); rawc('['); _pushDepth(); }
  void beginArr(const char* k) { key(k); rawc('['); _pushDepth(); }
  void endArr() { rawc(']'); _popDepth(); }

  void str(const char* k, const char* v) { key(k); rawc('"'); escape(v); rawc('"'); }
  void str(const char* k, const String& v) { str(k, v.c_str()); }
  void arrStr(const char* v) { comma(); rawc('"'); escape(v); rawc('"'); }

  void num(const char* k, long v) { key(k); char t[24]; snprintf(t, sizeof(t), "%ld", v); raw(t); }
  void numu(const char* k, unsigned long v) { key(k); char t[24]; snprintf(t, sizeof(t), "%lu", v); raw(t); }
  void numu64(const char* k, uint64_t v) { key(k); char t[28]; snprintf(t, sizeof(t), "%llu", (unsigned long long)v); raw(t); }
  void numf(const char* k, double v, int prec = 2) {
    key(k); char t[32]; dtostrf(v, 0, prec, t);
    // dtostrf kann fuehrende Leerzeichen erzeugen -> trimmen
    char* p = t; while (*p == ' ') p++;
    raw(p);
  }
  void boolean(const char* k, bool v) { key(k); raw(v ? "true" : "false"); }
  void nullv(const char* k) { key(k); raw("null"); }

  void escape(const char* v) {
    if (!v) return;
    for (const char* p = v; *p; p++) {
      unsigned char c = (unsigned char)*p;
      switch (c) {
        case '"':  raw("\\\""); break;
        case '\\': raw("\\\\"); break;
        case '\n': raw("\\n"); break;
        case '\r': raw("\\r"); break;
        case '\t': raw("\\t"); break;
        default:
          if (c < 0x20) { char t[8]; snprintf(t, sizeof(t), "\\u%04x", c); raw(t); }
          else rawc((char)c);
      }
    }
  }
  const char* c_str() { return _buf; }

private:
  void _pushDepth() { if (_depth < 7) { _depth++; _needComma[_depth] = false; } }
  void _popDepth() { if (_depth >= 0) _depth--; }
  char* _buf;
  size_t _cap;
  size_t _len;
  int _depth;
  bool _needComma[8];
};

// =====================================================================================
// [4] PERSISTENZ (Preferences / NVS)
// =====================================================================================

void loadDashSettings() {
  prefsDash.begin("dash", true);
  String u = prefsDash.getString("user", "admin");
  String p = prefsDash.getString("pass", "admin");
  logMinLevel = prefsDash.getUChar("loglevel", LOG_INFO);
  prefsDash.end();
  strlcpy(authUser, u.c_str(), sizeof(authUser));
  strlcpy(authPass, p.c_str(), sizeof(authPass));
}

void saveDashAuth(const char* user, const char* pass) {
  prefsDash.begin("dash", false);
  prefsDash.putString("user", user);
  prefsDash.putString("pass", pass);
  prefsDash.end();
  strlcpy(authUser, user, sizeof(authUser));
  strlcpy(authPass, pass, sizeof(authPass));
}

void saveLogLevel(uint8_t lvl) {
  logMinLevel = lvl;
  prefsDash.begin("dash", false);
  prefsDash.putUChar("loglevel", lvl);
  prefsDash.end();
}

void loadNetConfig() {
  prefsNet.begin("net", true);
  String ssid = prefsNet.getString("sta_ssid", "");
  String pass = prefsNet.getString("sta_pass", "");
  staAutoConnect = prefsNet.getBool("sta_auto", true);
  strlcpy(staSavedSsid, ssid.c_str(), sizeof(staSavedSsid));
  strlcpy(staSavedPass, pass.c_str(), sizeof(staSavedPass));

  String apSsid = prefsNet.getString("ap_ssid", "ESP32-Dashboard");
  String apPass = prefsNet.getString("ap_pass", "dashboard123");
  strlcpy(apCfg.ssid, apSsid.c_str(), sizeof(apCfg.ssid));
  strlcpy(apCfg.password, apPass.c_str(), sizeof(apCfg.password));
  apCfg.channel = prefsNet.getUChar("ap_ch", 1);
  apCfg.hidden  = prefsNet.getBool("ap_hidden", false);
  apCfg.maxConn = prefsNet.getUChar("ap_maxc", 4);
  // Default true: auf einem frisch geflashten Geraet (leeres NVS, noch kein gespeichertes
  // WLAN) MUSS der SoftAP aktiv sein, sonst waere das Dashboard nach dem allerersten
  // Boot ueberhaupt nicht erreichbar. Sobald der Nutzer den AP einmal bewusst ueber die
  // Weboberflaeche deaktiviert, wird dieser Wert in der NVS gespeichert und bleibt es.
  apCfg.enabled = prefsNet.getBool("ap_en", true);
  wifiModeSel   = prefsNet.getUChar("mode", 0);
  prefsNet.end();
}

void saveStaCredentials(const char* ssid, const char* pass) {
  prefsNet.begin("net", false);
  prefsNet.putString("sta_ssid", ssid);
  prefsNet.putString("sta_pass", pass);
  prefsNet.end();
  strlcpy(staSavedSsid, ssid, sizeof(staSavedSsid));
  strlcpy(staSavedPass, pass, sizeof(staSavedPass));
}

void clearStaCredentials() {
  prefsNet.begin("net", false);
  prefsNet.remove("sta_ssid");
  prefsNet.remove("sta_pass");
  prefsNet.end();
  staSavedSsid[0] = 0;
  staSavedPass[0] = 0;
}

void saveApConfigPersist() {
  prefsNet.begin("net", false);
  prefsNet.putString("ap_ssid", apCfg.ssid);
  prefsNet.putString("ap_pass", apCfg.password);
  prefsNet.putUChar("ap_ch", apCfg.channel);
  prefsNet.putBool("ap_hidden", apCfg.hidden);
  prefsNet.putUChar("ap_maxc", apCfg.maxConn);
  prefsNet.putBool("ap_en", apCfg.enabled);
  prefsNet.end();
}

void saveWifiMode(uint8_t m) {
  wifiModeSel = m;
  prefsNet.begin("net", false);
  prefsNet.putUChar("mode", m);
  prefsNet.end();
}

// --- WLAN-Beacon-Profile persistieren ---
void loadWifiBeacons() {
  prefsWB.begin("wbeacon", true);
  wifiBeaconCount = prefsWB.getUChar("count", 0);
  if (wifiBeaconCount > MAX_WIFI_BEACONS) wifiBeaconCount = MAX_WIFI_BEACONS;
  for (uint8_t i = 0; i < wifiBeaconCount; i++) {
    char k[8];
    snprintf(k, sizeof(k), "n%u", i); wifiBeacons[i] = WifiBeacon(); // defaults
    String nm = prefsWB.getString(k, "");
    snprintf(k, sizeof(k), "s%u", i); String ss = prefsWB.getString(k, "");
    snprintf(k, sizeof(k), "p%u", i); String pw = prefsWB.getString(k, "");
    snprintf(k, sizeof(k), "c%u", i); uint8_t ch = prefsWB.getUChar(k, 1);
    snprintf(k, sizeof(k), "h%u", i); bool hid = prefsWB.getBool(k, false);
    snprintf(k, sizeof(k), "e%u", i); bool en = prefsWB.getBool(k, false);
    strlcpy(wifiBeacons[i].name, nm.c_str(), sizeof(wifiBeacons[i].name));
    strlcpy(wifiBeacons[i].ssid, ss.c_str(), sizeof(wifiBeacons[i].ssid));
    strlcpy(wifiBeacons[i].password, pw.c_str(), sizeof(wifiBeacons[i].password));
    wifiBeacons[i].channel = ch;
    wifiBeacons[i].hidden = hid;
    wifiBeacons[i].enabled = en;
  }
  wifiRotationEnabled = prefsWB.getBool("rot_en", false);
  wifiRotationIntervalMs = prefsWB.getUInt("rot_iv", 6000);
  prefsWB.end();
}

void saveWifiBeacons() {
  prefsWB.begin("wbeacon", false);
  prefsWB.putUChar("count", wifiBeaconCount);
  for (uint8_t i = 0; i < wifiBeaconCount; i++) {
    char k[8];
    snprintf(k, sizeof(k), "n%u", i); prefsWB.putString(k, wifiBeacons[i].name);
    snprintf(k, sizeof(k), "s%u", i); prefsWB.putString(k, wifiBeacons[i].ssid);
    snprintf(k, sizeof(k), "p%u", i); prefsWB.putString(k, wifiBeacons[i].password);
    snprintf(k, sizeof(k), "c%u", i); prefsWB.putUChar(k, wifiBeacons[i].channel);
    snprintf(k, sizeof(k), "h%u", i); prefsWB.putBool(k, wifiBeacons[i].hidden);
    snprintf(k, sizeof(k), "e%u", i); prefsWB.putBool(k, wifiBeacons[i].enabled);
  }
  prefsWB.putBool("rot_en", wifiRotationEnabled);
  prefsWB.putUInt("rot_iv", wifiRotationIntervalMs);
  prefsWB.end();
  logMsg(LOG_INFO, "WLAN-Beacon-Konfiguration gespeichert (%u Profile)", wifiBeaconCount);
}

// --- BLE-Beacon-Profile persistieren ---
void loadBleBeacons() {
  prefsBB.begin("bbeacon", true);
  bleBeaconCount = prefsBB.getUChar("count", 0);
  if (bleBeaconCount > MAX_BLE_BEACONS) bleBeaconCount = MAX_BLE_BEACONS;
  for (uint8_t i = 0; i < bleBeaconCount; i++) {
    char k[8];
    bleBeacons[i] = BleBeaconCfg();
    snprintf(k, sizeof(k), "n%u", i); String nm = prefsBB.getString(k, "");
    snprintf(k, sizeof(k), "u%u", i); String uu = prefsBB.getString(k, bleBeacons[i].uuid);
    snprintf(k, sizeof(k), "j%u", i); uint16_t maj = prefsBB.getUShort(k, 1);
    snprintf(k, sizeof(k), "m%u", i); uint16_t min_ = prefsBB.getUShort(k, 1);
    snprintf(k, sizeof(k), "t%u", i); int8_t tx = (int8_t)prefsBB.getChar(k, -59);
    snprintf(k, sizeof(k), "i%u", i); uint16_t iv = prefsBB.getUShort(k, 400);
    snprintf(k, sizeof(k), "e%u", i); bool en = prefsBB.getBool(k, false);
    strlcpy(bleBeacons[i].name, nm.c_str(), sizeof(bleBeacons[i].name));
    strlcpy(bleBeacons[i].uuid, uu.c_str(), sizeof(bleBeacons[i].uuid));
    bleBeacons[i].major = maj;
    bleBeacons[i].minor = min_;
    bleBeacons[i].txPower = tx;
    bleBeacons[i].advIntervalMs = iv;
    bleBeacons[i].enabled = en;
  }
  bleRotationEnabled = prefsBB.getBool("rot_en", false);
  bleRotationIntervalMs = prefsBB.getUInt("rot_iv", 5000);
  prefsBB.end();
}

void saveBleBeacons() {
  prefsBB.begin("bbeacon", false);
  prefsBB.putUChar("count", bleBeaconCount);
  for (uint8_t i = 0; i < bleBeaconCount; i++) {
    char k[8];
    snprintf(k, sizeof(k), "n%u", i); prefsBB.putString(k, bleBeacons[i].name);
    snprintf(k, sizeof(k), "u%u", i); prefsBB.putString(k, bleBeacons[i].uuid);
    snprintf(k, sizeof(k), "j%u", i); prefsBB.putUShort(k, bleBeacons[i].major);
    snprintf(k, sizeof(k), "m%u", i); prefsBB.putUShort(k, bleBeacons[i].minor);
    snprintf(k, sizeof(k), "t%u", i); prefsBB.putChar(k, bleBeacons[i].txPower);
    snprintf(k, sizeof(k), "i%u", i); prefsBB.putUShort(k, bleBeacons[i].advIntervalMs);
    snprintf(k, sizeof(k), "e%u", i); prefsBB.putBool(k, bleBeacons[i].enabled);
  }
  prefsBB.putBool("rot_en", bleRotationEnabled);
  prefsBB.putUInt("rot_iv", bleRotationIntervalMs);
  prefsBB.end();
  logMsg(LOG_INFO, "BLE-Beacon-Konfiguration gespeichert (%u Profile)", bleBeaconCount);
}

void factoryReset() {
  logMsg(LOG_WARN, "Factory-Reset ausgeloest - loesche alle gespeicherten Konfigurationen");
  prefsNet.begin("net", false);  prefsNet.clear();  prefsNet.end();
  prefsDash.begin("dash", false); prefsDash.clear(); prefsDash.end();
  prefsWB.begin("wbeacon", false); prefsWB.clear(); prefsWB.end();
  prefsBB.begin("bbeacon", false); prefsBB.clear(); prefsBB.end();
}

// =====================================================================================
// [5] SYSTEM-INFORMATIONEN & CPU-LAST
// =====================================================================================

const char* resetReasonStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_UNKNOWN:    return "Unbekannt";
    case ESP_RST_POWERON:    return "Power-On (Einschalten)";
    case ESP_RST_EXT:        return "Externer Pin";
    case ESP_RST_SW:         return "Software (esp_restart)";
    case ESP_RST_PANIC:      return "Panic / Exception";
    case ESP_RST_INT_WDT:    return "Interrupt-Watchdog";
    case ESP_RST_TASK_WDT:   return "Task-Watchdog";
    case ESP_RST_WDT:        return "Sonstiger Watchdog";
    case ESP_RST_DEEPSLEEP:  return "Aufwachen aus Deep-Sleep";
    case ESP_RST_BROWNOUT:   return "Brownout (Unterspannung)";
    case ESP_RST_SDIO:       return "SDIO";
    case ESP_RST_USB:        return "USB";
    case ESP_RST_JTAG:       return "JTAG";
    case ESP_RST_EFUSE:      return "eFuse-Fehler";
    case ESP_RST_PWR_GLITCH: return "Power-Glitch";
    case ESP_RST_CPU_LOCKUP: return "CPU-Lockup";
    default: return "Unbekannt";
  }
}

// CPU-Auslastung ueber FreeRTOS-Laufzeitstatistik der IDLE-Tasks.
// configGENERATE_RUN_TIME_STATS ist im Arduino-ESP32-Core standardmaessig aktiv,
// daher ist dieser Wert eine echte Messung und keine Schaetzung "aus der Luft".
void sampleCpuUsage() {
  const UBaseType_t maxTasks = 24;
  static TaskStatus_t taskArr[24];
  uint32_t totalRuntime;
  UBaseType_t n = uxTaskGetSystemState(taskArr, maxTasks, &totalRuntime);
  if (n == 0) { cpuStatsAvailable = false; return; }

  uint64_t idle0 = 0, idle1 = 0;
  bool found0 = false, found1 = false;
  for (UBaseType_t i = 0; i < n; i++) {
    if (strcmp(taskArr[i].pcTaskName, "IDLE0") == 0) { idle0 = taskArr[i].ulRunTimeCounter; found0 = true; }
    else if (strcmp(taskArr[i].pcTaskName, "IDLE1") == 0) { idle1 = taskArr[i].ulRunTimeCounter; found1 = true; }
  }
  if (!found0) { cpuStatsAvailable = false; return; }

  uint64_t curTotal = totalRuntime;
  if (prevTotal != 0 && curTotal > prevTotal) {
    uint64_t dTotal = curTotal - prevTotal;
    uint64_t dIdle0 = (idle0 >= prevIdle0) ? (idle0 - prevIdle0) : 0;
    cpuUsageCore0 = 100.0f - (100.0f * dIdle0 / (float)dTotal);
    if (cpuUsageCore0 < 0) cpuUsageCore0 = 0;
    if (cpuUsageCore0 > 100) cpuUsageCore0 = 100;
    if (found1) {
      uint64_t dIdle1 = (idle1 >= prevIdle1) ? (idle1 - prevIdle1) : 0;
      cpuUsageCore1 = 100.0f - (100.0f * dIdle1 / (float)dTotal);
      if (cpuUsageCore1 < 0) cpuUsageCore1 = 0;
      if (cpuUsageCore1 > 100) cpuUsageCore1 = 100;
    }
    cpuStatsAvailable = true;
  }
  prevIdle0 = idle0; prevIdle1 = idle1; prevTotal = curTotal;
}

void buildSystemJson(JsonWriter &j) {
  j.beginObj();
  j.str("chipModel", ESP.getChipModel());
  j.num("chipRevision", ESP.getChipRevision());
  j.num("chipCores", ESP.getChipCores());
  j.numu("cpuFreqMHz", ESP.getCpuFreqMHz());
  j.str("sdkVersion", ESP.getSdkVersion());
  j.str("arduinoCore", String("3.x (arduino-esp32)"));

  j.numu("flashSize", ESP.getFlashChipSize());
  j.numu("flashSpeed", ESP.getFlashChipSpeed());
  j.numu("sketchSize", ESP.getSketchSize());
  j.numu("freeSketchSpace", ESP.getFreeSketchSpace());

  j.numu("heapSize", ESP.getHeapSize());
  j.numu("freeHeap", ESP.getFreeHeap());
  j.numu("minFreeHeap", ESP.getMinFreeHeap());
  j.numu("maxAllocHeap", ESP.getMaxAllocHeap());

  bool psram = psramFound();
  j.boolean("psramFound", psram);
  if (psram) {
    j.numu("psramSize", ESP.getPsramSize());
    j.numu("freePsram", ESP.getFreePsram());
  }

  j.numu("uptimeMs", millis());
  j.str("resetReason", resetReasonStr(esp_reset_reason()));

  char macStr[18];
  uint64_t mac = ESP.getEfuseMac();
  uint8_t* m = (uint8_t*)&mac;
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  j.str("efuseMac", macStr);
  j.str("staMac", WiFi.macAddress());

  j.boolean("cpuStatsAvailable", cpuStatsAvailable);
  j.numf("cpuUsageCore0", cpuUsageCore0, 1);
  j.numf("cpuUsageCore1", cpuUsageCore1, 1);

  float temp = temperatureRead();
  j.numf("tempC", temp, 1);
  j.boolean("tempOfficial", false); // temperatureRead() ist auf klassischem ESP32 inoffiziell

  j.endObj();
}

void buildStatusJson(JsonWriter &j) {
  j.beginObj();
  j.numu("uptimeMs", millis());
  j.numu("freeHeap", ESP.getFreeHeap());
  j.numu("heapSize", ESP.getHeapSize());
  j.numf("heapPct", 100.0 * (ESP.getHeapSize() - ESP.getFreeHeap()) / (float)ESP.getHeapSize(), 1);
  j.numf("cpu0", cpuUsageCore0, 1);
  j.numf("cpu1", cpuUsageCore1, 1);

  j.str("wifiStatus", WiFi.status() == WL_CONNECTED ? "connected" : "disconnected");
  j.str("wifiSsid", WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String(""));
  j.num("wifiRssi", WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
  j.str("wifiIp", WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("0.0.0.0"));
  j.num("wifiChannel", WiFi.status() == WL_CONNECTED ? WiFi.channel() : 0);

  j.boolean("apEnabled", apCfg.enabled);
  j.str("apIp", WiFi.softAPIP().toString());
  j.num("apClients", apCfg.enabled ? WiFi.softAPgetStationNum() : 0);

  j.boolean("bleEnabled", bleInitialized);
  j.boolean("bleScanning", bleScanActive);
  j.num("bleDeviceCount", bleDeviceCount);

  j.num("wifiBeaconCount", wifiBeaconCount);
  j.num("wifiBeaconActive", wifiBeaconActive);
  j.num("bleBeaconCount", bleBeaconCount);
  j.num("bleBeaconActive", bleBeaconActive);

  j.str("lastError", lastErrorMsg);
  j.numu("lastErrorAgoMs", lastErrorTime ? (millis() - lastErrorTime) : 0);

  j.beginArr("heapHistory");
  for (int i = 0; i < HISTORY_LEN; i++) { char t[16]; snprintf(t, sizeof(t), "%lu", (unsigned long)heapHistory[(historyIdx + i) % HISTORY_LEN]); j.comma(); j.raw(t); }
  j.endArr();
  j.beginArr("rssiHistory");
  for (int i = 0; i < HISTORY_LEN; i++) { char t[8]; snprintf(t, sizeof(t), "%d", rssiHistory[(historyIdx + i) % HISTORY_LEN]); j.comma(); j.raw(t); }
  j.endArr();
  j.beginArr("wifiCountHistory");
  for (int i = 0; i < HISTORY_LEN; i++) { char t[6]; snprintf(t, sizeof(t), "%u", wifiCountHistory[(historyIdx + i) % HISTORY_LEN]); j.comma(); j.raw(t); }
  j.endArr();
  j.beginArr("bleCountHistory");
  for (int i = 0; i < HISTORY_LEN; i++) { char t[6]; snprintf(t, sizeof(t), "%u", bleCountHistory[(historyIdx + i) % HISTORY_LEN]); j.comma(); j.raw(t); }
  j.endArr();

  j.endObj();
}

void updateHistory() {
  heapHistory[historyIdx] = ESP.getFreeHeap();
  rssiHistory[historyIdx] = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -100;
  wifiCountHistory[historyIdx] = (lastScanCount > 0) ? (uint8_t)min((int)lastScanCount, 255) : 0;
  bleCountHistory[historyIdx] = bleDeviceCount;
  historyIdx = (historyIdx + 1) % HISTORY_LEN;
}

// =====================================================================================
// [6] WLAN (STATION MODE)
// =====================================================================================

void onWifiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      logMsg(LOG_INFO, "WLAN verbunden (Station)");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      staConnectedSince = millis();
      staConnectInProgress = false;
      lastWifiError = "";
      logMsg(LOG_INFO, "IP erhalten: %s", WiFi.localIP().toString().c_str());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      if (staConnectedSince != 0) logMsg(LOG_WARN, "WLAN getrennt");
      staConnectedSince = 0;
      break;
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      logMsg(LOG_INFO, "Client mit SoftAP verbunden");
      break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      logMsg(LOG_INFO, "Client von SoftAP getrennt");
      break;
    default: break;
  }
}

void wifiConnect(const char* ssid, const char* pass) {
  logMsg(LOG_INFO, "WLAN-Verbindung wird aufgebaut zu '%s'", ssid);
  staConnectInProgress = true;
  staLastAttempt = millis();
  WiFi.begin(ssid, pass);
}

void wifiDisconnect() {
  logMsg(LOG_INFO, "WLAN wird getrennt (Nutzeraktion)");
  WiFi.disconnect(false, false);
  staConnectedSince = 0;
}

void wifiReconnectSaved() {
  if (strlen(staSavedSsid) == 0) { logMsg(LOG_WARN, "Kein gespeichertes WLAN vorhanden"); return; }
  wifiConnect(staSavedSsid, staSavedPass);
}

void wifiForget() {
  clearStaCredentials();
  WiFi.disconnect(true, false);
  logMsg(LOG_INFO, "Gespeicherte WLAN-Zugangsdaten geloescht");
}

const char* phyModeStr() {
  if (WiFi.status() != WL_CONNECTED) return "n/v";
  wifi_phy_mode_t mode;
  if (esp_wifi_sta_get_negotiated_phymode(&mode) != ESP_OK) return "n/v";
  switch (mode) {
    case WIFI_PHY_MODE_11B:  return "802.11b";
    case WIFI_PHY_MODE_11G:  return "802.11g";
    case WIFI_PHY_MODE_HT20: return "802.11n (HT20)";
    case WIFI_PHY_MODE_HT40: return "802.11n (HT40)";
    case WIFI_PHY_MODE_LR:   return "Long Range";
    default: return "unbekannt";
  }
}

void buildWifiJson(JsonWriter &j) {
  bool connected = WiFi.status() == WL_CONNECTED;
  j.beginObj();
  j.boolean("connected", connected);
  j.str("ssid", connected ? WiFi.SSID() : String(""));
  j.str("bssid", connected ? WiFi.BSSIDstr() : String(""));
  j.str("ip", connected ? WiFi.localIP().toString() : String("0.0.0.0"));
  j.str("gateway", connected ? WiFi.gatewayIP().toString() : String("0.0.0.0"));
  j.str("subnet", connected ? WiFi.subnetMask().toString() : String("0.0.0.0"));
  j.str("dns", connected ? WiFi.dnsIP().toString() : String("0.0.0.0"));
  j.num("rssi", connected ? WiFi.RSSI() : 0);
  j.num("channel", connected ? WiFi.channel() : 0);
  j.str("mac", WiFi.macAddress());
  j.str("phyMode", phyModeStr());
  j.numu("connectedSinceMs", staConnectedSince ? (millis() - staConnectedSince) : 0);
  j.boolean("hasSaved", strlen(staSavedSsid) > 0);
  j.str("savedSsid", staSavedSsid);
  j.boolean("autoConnect", staAutoConnect);
  j.boolean("connecting", staConnectInProgress);
  j.str("lastError", lastWifiError);
  j.str("hostname", WiFi.getHostname() ? WiFi.getHostname() : "");
  j.endObj();
}

// =====================================================================================
// [8] WLAN-SCANNER
// =====================================================================================

void startWifiScan() {
  if (scanInProgress) return;
  WiFi.scanDelete();
  int16_t r = WiFi.scanNetworks(true /*async*/, true /*show_hidden*/);
  if (r == WIFI_SCAN_FAILED) {
    logMsg(LOG_ERROR, "WLAN-Scan konnte nicht gestartet werden");
    return;
  }
  scanInProgress = true;
  scanStartedAt = millis();
  logMsg(LOG_INFO, "WLAN-Scan gestartet");
}

const char* authModeStr(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN: return "Offen";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA-PSK";
    case WIFI_AUTH_WPA2_PSK: return "WPA2-PSK";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2-PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Enterprise";
    case WIFI_AUTH_WPA3_PSK: return "WPA3-PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3-PSK";
    case WIFI_AUTH_WAPI_PSK: return "WAPI-PSK";
    case WIFI_AUTH_OWE: return "OWE";
    default: return "Unbekannt";
  }
}

void checkScanDone() {
  if (!scanInProgress) return;
  int16_t n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  scanInProgress = false;
  if (n == WIFI_SCAN_FAILED) {
    logMsg(LOG_ERROR, "WLAN-Scan fehlgeschlagen");
    lastScanCount = -1;
    return;
  }
  lastScanCount = n;
  logMsg(LOG_INFO, "WLAN-Scan beendet: %d Netzwerke gefunden", n);
}

void buildWifiScanJson(JsonWriter &j) {
  j.beginObj();
  j.boolean("scanning", scanInProgress);
  int16_t n = lastScanCount;
  j.num("count", n > 0 ? n : 0);
  j.beginArr("networks");
  if (!scanInProgress && n > 0) {
    // einfache Sortierung nach Signalstaerke (absteigend) via Index-Array
    static int16_t order[64];
    int cnt = min((int)n, 64);
    for (int i = 0; i < cnt; i++) order[i] = i;
    for (int a = 0; a < cnt - 1; a++)
      for (int b = a + 1; b < cnt; b++)
        if (WiFi.RSSI(order[b]) > WiFi.RSSI(order[a])) { int16_t t = order[a]; order[a] = order[b]; order[b] = t; }

    for (int k = 0; k < cnt; k++) {
      int i = order[k];
      j.comma(); j.beginObj();
      String ss = WiFi.SSID(i);
      bool hidden = ss.length() == 0;
      j.str("ssid", hidden ? String("(versteckt)") : ss);
      j.boolean("hidden", hidden);
      j.str("bssid", WiFi.BSSIDstr(i));
      j.num("rssi", WiFi.RSSI(i));
      j.num("channel", WiFi.channel(i));
      j.str("enc", authModeStr(WiFi.encryptionType(i)));
      int q = constrain(2 * (WiFi.RSSI(i) + 100), 0, 100);
      j.num("quality", q);
      j.endObj();
    }
  }
  j.endArr();
  j.endObj();
}


// =====================================================================================
// [7] ACCESS POINT / SOFTAP
// =====================================================================================

bool applyApConfig(const char* ssid, const char* pass, uint8_t channel, bool hidden, uint8_t maxConn, bool enable) {
  strlcpy(apCfg.ssid, ssid, sizeof(apCfg.ssid));
  strlcpy(apCfg.password, pass, sizeof(apCfg.password));
  apCfg.channel = constrain(channel, 1, 13);
  apCfg.hidden = hidden;
  apCfg.maxConn = constrain(maxConn, 1, 8);
  apCfg.enabled = enable;

  if (!enable) {
    WiFi.softAPdisconnect(true);
    logMsg(LOG_INFO, "SoftAP gestoppt");
    return true;
  }

  // sicherstellen, dass der AP-Modus im aktuellen WiFi.mode() enthalten ist
  wifi_mode_t cur = WiFi.getMode();
  if (cur == WIFI_MODE_STA) WiFi.mode(WIFI_MODE_APSTA);
  else if (cur == WIFI_MODE_NULL) WiFi.mode(WIFI_MODE_AP);

  bool usePass = strlen(pass) >= 8;
  bool ok = WiFi.softAP(ssid, usePass ? pass : NULL, apCfg.channel, hidden ? 1 : 0, apCfg.maxConn);
  if (ok) {
    logMsg(LOG_INFO, "SoftAP gestartet: SSID='%s' Kanal=%u", ssid, apCfg.channel);
  } else {
    logMsg(LOG_ERROR, "SoftAP konnte nicht gestartet werden (SSID='%s')", ssid);
  }
  return ok;
}

void buildApJson(JsonWriter &j) {
  j.beginObj();
  j.boolean("enabled", apCfg.enabled);
  j.str("ssid", apCfg.ssid);
  j.boolean("hasPassword", strlen(apCfg.password) >= 8);
  j.num("channel", apCfg.channel);
  j.boolean("hidden", apCfg.hidden);
  j.num("maxConnections", apCfg.maxConn);
  j.num("hwLimit", WIFI_HW_AP_LIMIT);
  j.str("ip", WiFi.softAPIP().toString());
  j.str("mac", WiFi.softAPmacAddress());
  j.num("stationCount", apCfg.enabled ? WiFi.softAPgetStationNum() : 0);
  j.num("wifiMode", wifiModeSel);

  j.beginArr("clients");
  if (apCfg.enabled) {
    wifi_sta_list_t staList;
    if (esp_wifi_ap_get_sta_list(&staList) == ESP_OK) {
      wifi_sta_mac_ip_list_t ipList;
      bool haveIp = esp_wifi_ap_get_sta_list_with_ip(&staList, &ipList) == ESP_OK;
      for (int i = 0; i < staList.num; i++) {
        j.comma(); j.beginObj();
        char macStr[18];
        uint8_t* mm = staList.sta[i].mac;
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", mm[0], mm[1], mm[2], mm[3], mm[4], mm[5]);
        j.str("mac", macStr);
        j.num("rssi", staList.sta[i].rssi);
        if (haveIp && i < ipList.num) {
          IPAddress ip(ipList.sta[i].ip.addr);
          j.str("ip", ip.toString());
        } else {
          j.str("ip", "n/v");
        }
        j.endObj();
      }
    }
  }
  j.endArr();
  j.endObj();
}

// Sicherer Moduswechsel: verhindert, dass sich der ESP32 durch eine fehlerhafte
// STA-Konfiguration selbst vom Netzwerk aussperrt. Ein reiner Stationsmodus wird
// nur dann endgueltig uebernommen, wenn die Verbindung tatsaechlich zustande kam;
// andernfalls bleibt der Access Point sicherheitshalber aktiv erreichbar.
void setWifiModeSafe(uint8_t mode) {
  if (mode == 0) { // reiner STA-Modus angefragt
    if (WiFi.status() == WL_CONNECTED) {
      WiFi.mode(WIFI_MODE_STA);
      apCfg.enabled = false;
      saveApConfigPersist();
      logMsg(LOG_INFO, "Modus gewechselt: reiner Stationsmodus (WLAN bereits verbunden)");
    } else {
      logMsg(LOG_WARN, "Reiner Stationsmodus abgelehnt: keine aktive WLAN-Verbindung - Access Point bleibt aktiv, um Aussperren zu vermeiden");
      return;
    }
  } else if (mode == 1) {
    WiFi.mode(WIFI_MODE_AP);
    logMsg(LOG_INFO, "Modus gewechselt: reiner Access-Point-Modus");
  } else {
    WiFi.mode(WIFI_MODE_APSTA);
    logMsg(LOG_INFO, "Modus gewechselt: Access Point + Station");
  }
  saveWifiMode(mode);
}

// =====================================================================================
// [9] WLAN-BEACON-MANAGER (SSID-PROFILE)
// =====================================================================================
// Siehe Hinweis am Dateianfang: der ESP32-WROOM-32 kann nur EIN SoftAP gleichzeitig
// betreiben. "Start" eines Profils bedeutet: dessen SSID/Passwort/Kanal wird auf das
// eine reale SoftAP-Interface angewendet. Sind mehrere Profile als "aktiv gewuenscht"
// markiert, kann optional eine zeitgetaktete Rotation zwischen ihnen aktiviert werden.

void wbSetCount(uint8_t n) {
  if (n > MAX_WIFI_BEACONS) n = MAX_WIFI_BEACONS;
  if (n > wifiBeaconCount) {
    for (uint8_t i = wifiBeaconCount; i < n; i++) {
      wifiBeacons[i] = WifiBeacon();
      snprintf(wifiBeacons[i].name, sizeof(wifiBeacons[i].name), "Beacon %u", i + 1);
      snprintf(wifiBeacons[i].ssid, sizeof(wifiBeacons[i].ssid), "ESP32-Beacon-%u", i + 1);
      wifiBeacons[i].channel = 1 + (i % 11);
    }
  }
  wifiBeaconCount = n;
  logMsg(LOG_INFO, "Anzahl WLAN-Beacon-Profile auf %u gesetzt", n);
}

bool wbApplyIndex(int idx) {
  if (idx < 0 || idx >= wifiBeaconCount) return false;
  WifiBeacon &b = wifiBeacons[idx];
  bool ok = applyApConfig(b.ssid, b.password, b.channel, b.hidden, apCfg.maxConn, true);
  if (ok) {
    wifiBeaconActive = idx;
    wifiBeaconLastStart = millis();
    wifiBeaconLastError[0] = 0;
    logMsg(LOG_INFO, "WLAN-Beacon '%s' aktiviert (SSID '%s', Kanal %u)", b.name, b.ssid, b.channel);
  } else {
    strlcpy(wifiBeaconLastError, "SoftAP-Start fehlgeschlagen", sizeof(wifiBeaconLastError));
    logMsg(LOG_ERROR, "WLAN-Beacon '%s' konnte nicht gestartet werden", b.name);
  }
  return ok;
}

void wbStopActive() {
  if (wifiBeaconActive < 0) return;
  logMsg(LOG_INFO, "WLAN-Beacon '%s' gestoppt", wifiBeacons[wifiBeaconActive].name);
  applyApConfig(apCfg.ssid, apCfg.password, apCfg.channel, apCfg.hidden, apCfg.maxConn, false);
  wifiBeaconActive = -1;
}

void wbStartAll() {
  int enabledCount = 0;
  for (uint8_t i = 0; i < wifiBeaconCount; i++) if (wifiBeacons[i].enabled) enabledCount++;
  if (enabledCount == 0) { logMsg(LOG_WARN, "Keine WLAN-Beacons zum Starten aktiviert"); return; }
  int first = -1;
  for (uint8_t i = 0; i < wifiBeaconCount; i++) if (wifiBeacons[i].enabled) { first = i; break; }
  wbApplyIndex(first);
  if (enabledCount > 1) {
    wifiRotationEnabled = true;
    wifiRotationLastSwitch = millis();
    logMsg(LOG_INFO, "%d WLAN-Beacons aktiv markiert -> Rotation gestartet (Hardware-Limit: %d gleichzeitig)", enabledCount, WIFI_HW_AP_LIMIT);
  }
}

void wbStopAll() {
  wifiRotationEnabled = false;
  for (uint8_t i = 0; i < wifiBeaconCount; i++) wifiBeacons[i].enabled = false;
  wbStopActive();
  logMsg(LOG_INFO, "Alle WLAN-Beacons gestoppt");
}

void wbLoopTick() {
  if (!wifiRotationEnabled || wifiBeaconActive < 0) return;
  if (millis() - wifiRotationLastSwitch < wifiRotationIntervalMs) return;
  // naechstes aktiviertes Profil suchen (zyklisch)
  int n = wifiBeaconCount;
  if (n == 0) return;
  int idx = wifiBeaconActive;
  for (int step = 1; step <= n; step++) {
    int cand = (idx + step) % n;
    if (wifiBeacons[cand].enabled) {
      wbApplyIndex(cand);
      wifiRotationLastSwitch = millis();
      return;
    }
  }
}

void buildWifiBeaconsJson(JsonWriter &j) {
  j.beginObj();
  j.num("count", wifiBeaconCount);
  j.num("max", MAX_WIFI_BEACONS);
  j.num("hwLimit", WIFI_HW_AP_LIMIT);
  j.num("activeIndex", wifiBeaconActive);
  j.boolean("rotationEnabled", wifiRotationEnabled);
  j.numu("rotationIntervalMs", wifiRotationIntervalMs);
  int enabledCount = 0;
  for (uint8_t i = 0; i < wifiBeaconCount; i++) if (wifiBeacons[i].enabled) enabledCount++;
  j.num("enabledCount", enabledCount);
  j.str("lastError", wifiBeaconLastError);
  j.beginArr("beacons");
  for (uint8_t i = 0; i < wifiBeaconCount; i++) {
    j.comma(); j.beginObj();
    j.num("index", i);
    j.str("name", wifiBeacons[i].name);
    j.str("ssid", wifiBeacons[i].ssid);
    j.boolean("hasPassword", strlen(wifiBeacons[i].password) >= 8);
    j.num("channel", wifiBeacons[i].channel);
    j.boolean("hidden", wifiBeacons[i].hidden);
    j.boolean("enabled", wifiBeacons[i].enabled);
    j.boolean("active", wifiBeaconActive == i);
    j.endObj();
  }
  j.endArr();
  j.endObj();
}

// =====================================================================================
// [10] BLE CORE (SCAN)
// =====================================================================================

class DashboardAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    String addrStr = advertisedDevice.getAddress().toString().c_str();
    int foundIdx = -1;
    for (uint8_t i = 0; i < bleDeviceCount; i++) {
      if (addrStr.equalsIgnoreCase(bleDevices[i].addr)) { foundIdx = i; break; }
    }
    if (foundIdx < 0) {
      if (bleDeviceCount >= MAX_BLE_DEVICES) return; // Kapazitaet erreicht
      foundIdx = bleDeviceCount++;
      bleDevices[foundIdx] = BleDeviceInfo();
      strlcpy(bleDevices[foundIdx].addr, addrStr.c_str(), sizeof(bleDevices[foundIdx].addr));
      bleDevices[foundIdx].firstSeen = millis();
      logMsg(LOG_DEBUG, "BLE-Geraet gefunden: %s", addrStr.c_str());
    }
    BleDeviceInfo &d = bleDevices[foundIdx];
    d.rssi = advertisedDevice.getRSSI();
    d.lastSeen = millis();
    if (advertisedDevice.haveName()) {
      d.hasName = true;
      strlcpy(d.name, advertisedDevice.getName().c_str(), sizeof(d.name));
    }
    if (advertisedDevice.haveManufacturerData()) {
      d.hasMfg = true;
      String mf = advertisedDevice.getManufacturerData();
      char hex[48]; hex[0] = 0;
      size_t maxBytes = min((size_t)16, mf.length());
      for (size_t i = 0; i < maxBytes; i++) {
        char b[4]; snprintf(b, sizeof(b), "%02X", (uint8_t)mf[i]);
        strlcat(hex, b, sizeof(hex));
      }
      strlcpy(d.mfgHex, hex, sizeof(d.mfgHex));
    }
    if (advertisedDevice.haveServiceUUID()) {
      d.hasServiceUUID = true;
      strlcpy(d.serviceUUID, advertisedDevice.getServiceUUID().toString().c_str(), sizeof(d.serviceUUID));
    }
  }
};
static DashboardAdvertisedDeviceCallbacks bleAdvCallbacks;

void onBleScanComplete(BLEScanResults results) {
  bleScanActive = false;
  logMsg(LOG_INFO, "BLE-Scan beendet (%u Geraete insgesamt bekannt)", bleDeviceCount);
}

void bleEnable() {
  if (bleInitialized) return;
  BLEDevice::init("ESP32-Dashboard");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(&bleAdvCallbacks, true, true);
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(90);
  bleInitialized = true;
  logMsg(LOG_INFO, "BLE aktiviert");
}

void bleDisable() {
  if (!bleInitialized) return;
  if (bleScanActive) { pBLEScan->stop(); bleScanActive = false; }
  if (bleBeaconActive >= 0) { BLEDevice::getAdvertising()->stop(); }
  bleBeaconActive = -1;
  BLEDevice::deinit(true);
  pBLEScan = nullptr;
  bleInitialized = false;
  logMsg(LOG_INFO, "BLE deaktiviert");
}

void bleStartScan(uint32_t durationSec) {
  if (!bleInitialized) { logMsg(LOG_WARN, "BLE-Scan nicht moeglich: BLE ist deaktiviert"); return; }
  if (bleScanActive) return;
  bleScanDurationSec = constrain(durationSec, 1, 60);
  bool ok = pBLEScan->start(bleScanDurationSec, onBleScanComplete, false);
  if (ok) {
    bleScanActive = true;
    bleScanStartedAt = millis();
    logMsg(LOG_INFO, "BLE-Scan gestartet (%u s)", bleScanDurationSec);
  } else {
    logMsg(LOG_ERROR, "BLE-Scan konnte nicht gestartet werden");
  }
}

void bleStopScan() {
  if (!bleInitialized || !bleScanActive) return;
  pBLEScan->stop();
  bleScanActive = false;
  logMsg(LOG_INFO, "BLE-Scan manuell gestoppt");
}

void bleClearDevices() {
  bleDeviceCount = 0;
  logMsg(LOG_INFO, "BLE-Geraeteliste geleert");
}

void buildBleJson(JsonWriter &j) {
  j.beginObj();
  j.boolean("initialized", bleInitialized);
  j.boolean("scanning", bleScanActive);
  j.numu("scanDurationSec", bleScanDurationSec);
  j.numu("scanElapsedMs", bleScanActive ? (millis() - bleScanStartedAt) : 0);
  j.num("deviceCount", bleDeviceCount);
  j.num("maxDevices", MAX_BLE_DEVICES);
  j.beginArr("devices");
  for (uint8_t i = 0; i < bleDeviceCount; i++) {
    BleDeviceInfo &d = bleDevices[i];
    j.comma(); j.beginObj();
    j.str("addr", d.addr);
    j.boolean("hasName", d.hasName);
    j.str("name", d.hasName ? d.name : "");
    j.num("rssi", d.rssi);
    j.boolean("hasMfg", d.hasMfg);
    j.str("mfgHex", d.hasMfg ? d.mfgHex : "");
    j.boolean("hasServiceUUID", d.hasServiceUUID);
    j.str("serviceUUID", d.hasServiceUUID ? d.serviceUUID : "");
    j.numu("lastSeenAgoMs", millis() - d.lastSeen);
    j.endObj();
  }
  j.endArr();
  j.endObj();
}

// =====================================================================================
// [11] BLE-BEACON-MANAGER (IBEACON-PROFILE)
// =====================================================================================
// Auch hier gilt die Hardware-Realitaet: der klassische ESP32-Bluedroid-Stack stellt
// genau EINE Advertising-Instanz bereit (kein BLE-5-Multi-Advertising wie bei S3/C3).
// "Start" eines Profils baut einen Standard-iBeacon-Frame (Apple-Spezifikation,
// offen dokumentiert und frei nutzbar fuer eigene Beacons) und sendet ihn auf der
// einen verfuegbaren Advertising-Instanz.

bool bbApplyIndex(int idx) {
  if (!bleInitialized) { strlcpy(bleBeaconLastError, "BLE ist deaktiviert", sizeof(bleBeaconLastError)); return false; }
  if (idx < 0 || idx >= bleBeaconCount) return false;
  BleBeaconCfg &b = bleBeacons[idx];

  BLEAdvertising* pAdv = BLEDevice::getAdvertising();
  pAdv->stop();

  BLEBeacon beacon;
  beacon.setManufacturerId(0x4C00); // Apple-ID, Standard fuer das offene iBeacon-Format
  beacon.setProximityUUID(BLEUUID(String(b.uuid)));
  beacon.setMajor(b.major);
  beacon.setMinor(b.minor);
  beacon.setSignalPower(b.txPower);

  BLEAdvertisementData advData;
  advData.setFlags(0x04); // BR/EDR nicht unterstuetzt
  advData.setManufacturerData(beacon.getData());

  BLEAdvertisementData scanResp;
  scanResp.setName(b.name);

  pAdv->setAdvertisementData(advData);
  pAdv->setScanResponseData(scanResp);
  pAdv->setAdvertisementType(kBeaconAdvType);
  pAdv->setMinInterval(b.advIntervalMs);
  pAdv->setMaxInterval(b.advIntervalMs + 20);
  BLEDevice::setPower((esp_power_level_t)constrain((int)((b.txPower + 12) / 3 + 0), 0, 7), ESP_BLE_PWR_TYPE_ADV);
  pAdv->start();

  bleBeaconActive = idx;
  bleBeaconLastStart = millis();
  bleBeaconLastError[0] = 0;
  logMsg(LOG_INFO, "BLE-Beacon '%s' aktiviert (UUID %s, Major %u, Minor %u)", b.name, b.uuid, b.major, b.minor);
  return true;
}

void bbStopActive() {
  if (bleBeaconActive < 0) return;
  if (bleInitialized) BLEDevice::getAdvertising()->stop();
  logMsg(LOG_INFO, "BLE-Beacon '%s' gestoppt", bleBeacons[bleBeaconActive].name);
  bleBeaconActive = -1;
}

void bbSetCount(uint8_t n) {
  if (n > MAX_BLE_BEACONS) n = MAX_BLE_BEACONS;
  if (n > bleBeaconCount) {
    for (uint8_t i = bleBeaconCount; i < n; i++) {
      bleBeacons[i] = BleBeaconCfg();
      snprintf(bleBeacons[i].name, sizeof(bleBeacons[i].name), "ESP32 Beacon %02u", i + 1);
      bleBeacons[i].minor = i + 1;
    }
  }
  bleBeaconCount = n;
  logMsg(LOG_INFO, "Anzahl BLE-Beacon-Profile auf %u gesetzt", n);
}

void bbStartAll() {
  int enabledCount = 0;
  for (uint8_t i = 0; i < bleBeaconCount; i++) if (bleBeacons[i].enabled) enabledCount++;
  if (enabledCount == 0) { logMsg(LOG_WARN, "Keine BLE-Beacons zum Starten aktiviert"); return; }
  int first = -1;
  for (uint8_t i = 0; i < bleBeaconCount; i++) if (bleBeacons[i].enabled) { first = i; break; }
  bbApplyIndex(first);
  if (enabledCount > 1) {
    bleRotationEnabled = true;
    bleRotationLastSwitch = millis();
    logMsg(LOG_INFO, "%d BLE-Beacons aktiv markiert -> Rotation gestartet (Hardware-Limit: %d gleichzeitig)", enabledCount, BLE_HW_ADV_LIMIT);
  }
}

void bbStopAll() {
  bleRotationEnabled = false;
  for (uint8_t i = 0; i < bleBeaconCount; i++) bleBeacons[i].enabled = false;
  bbStopActive();
  logMsg(LOG_INFO, "Alle BLE-Beacons gestoppt");
}

void bbLoopTick() {
  if (!bleRotationEnabled || bleBeaconActive < 0) return;
  if (millis() - bleRotationLastSwitch < bleRotationIntervalMs) return;
  int n = bleBeaconCount;
  if (n == 0) return;
  int idx = bleBeaconActive;
  for (int step = 1; step <= n; step++) {
    int cand = (idx + step) % n;
    if (bleBeacons[cand].enabled) {
      bbApplyIndex(cand);
      bleRotationLastSwitch = millis();
      return;
    }
  }
}

void buildBleBeaconsJson(JsonWriter &j) {
  j.beginObj();
  j.num("count", bleBeaconCount);
  j.num("max", MAX_BLE_BEACONS);
  j.num("hwLimit", BLE_HW_ADV_LIMIT);
  j.num("activeIndex", bleBeaconActive);
  j.boolean("rotationEnabled", bleRotationEnabled);
  j.numu("rotationIntervalMs", bleRotationIntervalMs);
  int enabledCount = 0;
  for (uint8_t i = 0; i < bleBeaconCount; i++) if (bleBeacons[i].enabled) enabledCount++;
  j.num("enabledCount", enabledCount);
  j.str("lastError", bleBeaconLastError);
  j.beginArr("beacons");
  for (uint8_t i = 0; i < bleBeaconCount; i++) {
    j.comma(); j.beginObj();
    j.num("index", i);
    j.str("name", bleBeacons[i].name);
    j.str("uuid", bleBeacons[i].uuid);
    j.num("major", bleBeacons[i].major);
    j.num("minor", bleBeacons[i].minor);
    j.num("txPower", bleBeacons[i].txPower);
    j.num("intervalMs", bleBeacons[i].advIntervalMs);
    j.boolean("enabled", bleBeacons[i].enabled);
    j.boolean("active", bleBeaconActive == i);
    j.endObj();
  }
  j.endArr();
  j.endObj();
}


// =====================================================================================
// [12] NETZWERK-DIAGNOSE (PING / DNS / TCP)
// =====================================================================================

static void onPingSuccessCb(esp_ping_handle_t hdl, void *args) {
  uint8_t ttl; uint16_t seqno; uint32_t elapsedMs;
  esp_ping_get_profile(hdl, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
  esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
  esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsedMs, sizeof(elapsedMs));
  // Zuweisung statt ++/+= auf volatile-Membern (seit C++20 fuer Compound-Operationen
  // auf volatile deprecatet; einfache Zuweisungen bleiben unproblematisch).
  pingState.received = pingState.received + 1;
  pingState.lastMs = elapsedMs;
  pingState.sumMs = pingState.sumMs + elapsedMs;
  if (pingState.received == 1 || elapsedMs < pingState.minMs) pingState.minMs = elapsedMs;
  if (elapsedMs > pingState.maxMs) pingState.maxMs = elapsedMs;
  pingState.avgMs = pingState.sumMs / (float)pingState.received;
}

static void onPingTimeoutCb(esp_ping_handle_t hdl, void *args) {
  // Zeitueberschreitung fuer ein einzelnes Echo-Paket - wird einfach nicht mitgezaehlt
}

static void onPingEndCb(esp_ping_handle_t hdl, void *args) {
  uint32_t transmitted, received;
  esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
  esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &received, sizeof(received));
  pingState.transmitted = transmitted;
  pingState.received = received;
  pingState.done = true;
  pingState.active = false;
  esp_ping_delete_session(hdl);
  pingHandle = nullptr;
  logMsg(LOG_INFO, "Ping zu %s beendet: %lu/%lu Antworten", pingState.host, (unsigned long)received, (unsigned long)transmitted);
}

bool pingStart(const char* host) {
  if (pingState.active) return false;
  IPAddress ip;
  if (!WiFi.hostByName(host, ip)) {
    pingState.resolveError = true;
    pingState.done = true;
    pingState.active = false;
    strlcpy(pingState.host, host, sizeof(pingState.host));
    pingState.ip[0] = 0;
    logMsg(LOG_WARN, "Ping: Hostname '%s' konnte nicht aufgeloest werden", host);
    return false;
  }
  pingState = PingState();
  strlcpy(pingState.host, host, sizeof(pingState.host));
  snprintf(pingState.ip, sizeof(pingState.ip), "%s", ip.toString().c_str());
  pingState.active = true;

  ip_addr_t target;
  IP_ADDR4(&target, ip[0], ip[1], ip[2], ip[3]);

  esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
  config.target_addr = target;
  config.count = 4;
  config.interval_ms = 800;
  config.timeout_ms = 1000;

  esp_ping_callbacks_t cbs = {};
  cbs.on_ping_success = onPingSuccessCb;
  cbs.on_ping_timeout = onPingTimeoutCb;
  cbs.on_ping_end = onPingEndCb;

  if (esp_ping_new_session(&config, &cbs, &pingHandle) != ESP_OK) {
    pingState.active = false;
    logMsg(LOG_ERROR, "Ping-Sitzung konnte nicht erstellt werden");
    return false;
  }
  esp_ping_start(pingHandle);
  logMsg(LOG_INFO, "Ping gestartet zu %s (%s)", host, pingState.ip);
  return true;
}

void buildPingJson(JsonWriter &j) {
  j.beginObj();
  j.boolean("active", pingState.active);
  j.boolean("done", pingState.done);
  j.boolean("resolveError", pingState.resolveError);
  j.str("host", pingState.host);
  j.str("ip", pingState.ip);
  j.numu("transmitted", pingState.transmitted);
  j.numu("received", pingState.received);
  j.numf("minMs", pingState.minMs, 1);
  j.numf("maxMs", pingState.maxMs, 1);
  j.numf("avgMs", pingState.avgMs, 1);
  j.endObj();
}

bool dnsResolve(const char* host, IPAddress &out) {
  return WiFi.hostByName(host, out) == 1;
}

bool tcpCheck(const char* host, uint16_t port, uint32_t timeoutMs, uint32_t *elapsedOut) {
  WiFiClient client;
  uint32_t start = millis();
  bool ok = client.connect(host, port, timeoutMs) == 1;
  if (elapsedOut) *elapsedOut = millis() - start;
  if (ok) client.stop();
  return ok;
}

void buildNetworkJson(JsonWriter &j) {
  bool connected = WiFi.status() == WL_CONNECTED;
  j.beginObj();
  j.boolean("staConnected", connected);
  j.str("ip", connected ? WiFi.localIP().toString() : String("0.0.0.0"));
  j.str("gateway", connected ? WiFi.gatewayIP().toString() : String("0.0.0.0"));
  j.str("subnet", connected ? WiFi.subnetMask().toString() : String("0.0.0.0"));
  j.str("dns", connected ? WiFi.dnsIP().toString() : String("0.0.0.0"));
  j.str("apIp", WiFi.softAPIP().toString());
  j.str("hostname", WiFi.getHostname() ? WiFi.getHostname() : "");
  j.endObj();
}


// =====================================================================================
// [14] WEB-OBERFLAECHE (HTML/CSS/JS) - liegt komplett im Flash (PROGMEM)
// =====================================================================================

const char PAGE_HTML_HEAD[] PROGMEM = R"RAWPAGE(<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1">
<title>ESP32 Ultimate Dashboard</title>
<style>
:root{
  --bg:#0b0f17; --bg2:#101826; --panel:#131d2e; --panel2:#17233a; --border:#1f2c44;
  --text:#e7edf7; --muted:#8694ab; --accent:#4f8cff; --accent2:#7c5cff;
  --good:#2fd47a; --warn:#ffb648; --bad:#ff5d6c; --radius:14px;
  --shadow:0 4px 18px rgba(0,0,0,.35);
}
*{box-sizing:border-box;}
html,body{margin:0;padding:0;background:var(--bg);color:var(--text);
  font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;}
a{color:inherit;}
::-webkit-scrollbar{width:8px;height:8px;}
::-webkit-scrollbar-thumb{background:#243352;border-radius:8px;}

.layout{display:flex;min-height:100vh;}

/* ---------- Sidebar ---------- */
.sidebar{width:230px;background:var(--bg2);border-right:1px solid var(--border);
  display:flex;flex-direction:column;flex-shrink:0;position:sticky;top:0;height:100vh;z-index:40;}
.brand{display:flex;align-items:center;gap:10px;padding:18px 16px;border-bottom:1px solid var(--border);}
.brand-dot{width:10px;height:10px;border-radius:50%;background:var(--accent);box-shadow:0 0 10px var(--accent);}
.brand b{font-size:15px;}
.brand small{display:block;color:var(--muted);font-size:11px;}
.nav{flex:1;overflow-y:auto;padding:10px 8px;}
.nav a{display:flex;align-items:center;gap:10px;padding:10px 12px;border-radius:10px;
  color:var(--muted);text-decoration:none;font-size:13.5px;margin-bottom:2px;cursor:pointer;}
.nav a svg{width:17px;height:17px;flex-shrink:0;}
.nav a:hover{background:var(--panel2);color:var(--text);}
.nav a.active{background:linear-gradient(90deg,rgba(79,140,255,.18),rgba(124,92,255,.08));color:#fff;
  border:1px solid rgba(79,140,255,.3);}
.nav .sec-label{color:#4e5d78;font-size:10px;text-transform:uppercase;letter-spacing:.08em;
  padding:14px 12px 6px;}
.sidebar-foot{padding:12px 16px;border-top:1px solid var(--border);font-size:11px;color:var(--muted);}

/* ---------- Topbar ---------- */
.topbar{display:flex;align-items:center;justify-content:space-between;gap:12px;
  padding:14px 22px;border-bottom:1px solid var(--border);background:rgba(11,15,23,.7);
  backdrop-filter:blur(6px);position:sticky;top:0;z-index:30;}
.topbar h1{font-size:17px;margin:0;font-weight:600;}
.topbar .sub{color:var(--muted);font-size:12px;}
.hamb{display:none;background:none;border:none;color:var(--text);font-size:20px;cursor:pointer;}
.top-right{display:flex;align-items:center;gap:14px;}
.pill{display:flex;align-items:center;gap:6px;padding:6px 12px;border-radius:999px;
  background:var(--panel2);border:1px solid var(--border);font-size:12px;color:var(--muted);}
.dot{width:8px;height:8px;border-radius:50%;background:var(--muted);}
.dot.ok{background:var(--good);box-shadow:0 0 6px var(--good);}
.dot.bad{background:var(--bad);box-shadow:0 0 6px var(--bad);}
.dot.warn{background:var(--warn);box-shadow:0 0 6px var(--warn);}

.content{flex:1;min-width:0;}
.view{display:none;padding:22px;max-width:1300px;margin:0 auto;}
.view.active{display:block;}

/* ---------- Grid / Cards ---------- */
.grid{display:grid;gap:16px;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));margin-bottom:18px;}
.grid.cols-2{grid-template-columns:repeat(auto-fit,minmax(340px,1fr));}
.card{background:linear-gradient(180deg,var(--panel),var(--panel2));border:1px solid var(--border);
  border-radius:var(--radius);padding:16px 18px;box-shadow:var(--shadow);}
.card h3{margin:0 0 12px;font-size:12.5px;color:var(--muted);text-transform:uppercase;
  letter-spacing:.06em;display:flex;align-items:center;justify-content:space-between;}
.stat{font-size:26px;font-weight:700;margin:2px 0;}
.stat small{font-size:12px;color:var(--muted);font-weight:400;}
.rowline{display:flex;justify-content:space-between;align-items:center;padding:7px 0;
  border-bottom:1px solid rgba(255,255,255,.04);font-size:13px;}
.rowline:last-child{border-bottom:none;}
.rowline span.k{color:var(--muted);}
.rowline span.v{font-weight:600;text-align:right;}

.progress{height:8px;border-radius:6px;background:#0c1220;overflow:hidden;margin-top:8px;}
.progress > div{height:100%;border-radius:6px;background:linear-gradient(90deg,var(--accent),var(--accent2));
  transition:width .4s ease;}
.progress.warn > div{background:linear-gradient(90deg,#ffb648,#ff7a48);}
.progress.bad > div{background:linear-gradient(90deg,#ff5d6c,#ff2e55);}

.badge{display:inline-flex;align-items:center;gap:5px;padding:3px 10px;border-radius:999px;
  font-size:11px;font-weight:600;background:#1b2740;color:var(--muted);}
.badge.good{background:rgba(47,212,122,.15);color:var(--good);}
.badge.bad{background:rgba(255,93,108,.15);color:var(--bad);}
.badge.warn{background:rgba(255,182,72,.15);color:var(--warn);}
.badge.info{background:rgba(79,140,255,.15);color:var(--accent);}

h2.vtitle{font-size:19px;margin:0 0 4px;}
p.vdesc{color:var(--muted);font-size:13px;margin:0 0 18px;}

.panel{background:var(--panel);border:1px solid var(--border);border-radius:var(--radius);
  padding:18px;margin-bottom:18px;}
.panel h3{margin-top:0;font-size:14px;}

table{width:100%;border-collapse:collapse;font-size:12.5px;}
th{text-align:left;color:var(--muted);font-weight:600;font-size:11px;text-transform:uppercase;
  letter-spacing:.04em;padding:8px 10px;border-bottom:1px solid var(--border);}
td{padding:9px 10px;border-bottom:1px solid rgba(255,255,255,.04);}
tr:hover td{background:rgba(255,255,255,.02);}
.tablewrap{overflow-x:auto;}

.btn{display:inline-flex;align-items:center;gap:6px;padding:9px 16px;border-radius:10px;
  border:1px solid var(--border);background:var(--panel2);color:var(--text);font-size:13px;
  cursor:pointer;font-weight:600;transition:.15s;}
.btn:hover{border-color:var(--accent);}
.btn:disabled{opacity:.45;cursor:not-allowed;}
.btn.primary{background:linear-gradient(135deg,var(--accent),var(--accent2));border:none;}
.btn.danger{background:rgba(255,93,108,.12);border-color:rgba(255,93,108,.4);color:#ff8089;}
.btn.sm{padding:5px 10px;font-size:11.5px;border-radius:8px;}
.btn.ghost{background:transparent;}
.btnrow{display:flex;gap:10px;flex-wrap:wrap;margin-top:10px;}

input[type=text],input[type=password],input[type=number],select{
  width:100%;padding:9px 11px;border-radius:9px;border:1px solid var(--border);
  background:#0c1322;color:var(--text);font-size:13px;}
input:focus,select:focus{outline:none;border-color:var(--accent);}
label{display:block;font-size:12px;color:var(--muted);margin-bottom:5px;margin-top:10px;}
.formgrid{display:grid;grid-template-columns:1fr 1fr;gap:0 14px;}
.hint{color:var(--muted);font-size:11.5px;margin-top:4px;}

.switch{position:relative;display:inline-block;width:40px;height:22px;flex-shrink:0;}
.switch input{opacity:0;width:0;height:0;}
.slider-tog{position:absolute;cursor:pointer;inset:0;background:#243352;border-radius:22px;transition:.2s;}
.slider-tog:before{content:"";position:absolute;height:16px;width:16px;left:3px;bottom:3px;
  background:#fff;border-radius:50%;transition:.2s;}
input:checked + .slider-tog{background:var(--accent);}
input:checked + .slider-tog:before{transform:translateX(18px);}
.flexrow{display:flex;align-items:center;justify-content:space-between;gap:10px;}

.sig-bars{display:flex;align-items:flex-end;gap:2px;height:16px;}
.sig-bars i{width:3px;background:#2a3a5a;border-radius:1px;}
.sig-bars i.on{background:var(--accent);}

.beacon-item{border:1px solid var(--border);border-radius:12px;padding:14px;margin-bottom:12px;background:#0f1726;}
.beacon-head{display:flex;justify-content:space-between;align-items:center;margin-bottom:10px;}
.beacon-head b{font-size:13.5px;}

.toasts{position:fixed;bottom:18px;right:18px;z-index:200;display:flex;flex-direction:column;gap:8px;}
.toast{background:var(--panel2);border:1px solid var(--border);border-left:4px solid var(--accent);
  padding:12px 16px;border-radius:10px;font-size:13px;box-shadow:var(--shadow);min-width:230px;
  animation:slidein .25s ease;}
.toast.good{border-left-color:var(--good);}
.toast.bad{border-left-color:var(--bad);}
.toast.warn{border-left-color:var(--warn);}
@keyframes slidein{from{transform:translateX(30px);opacity:0;}to{transform:translateX(0);opacity:1;}}

.modal-bg{position:fixed;inset:0;background:rgba(5,8,14,.6);backdrop-filter:blur(2px);
  display:none;align-items:center;justify-content:center;z-index:300;}
.modal-bg.show{display:flex;}
.modal{background:var(--panel);border:1px solid var(--border);border-radius:16px;padding:22px;
  width:92%;max-width:380px;box-shadow:var(--shadow);}
.modal h3{margin-top:0;}
.modal .btnrow{justify-content:flex-end;}

.searchbar{display:flex;gap:10px;margin-bottom:14px;}
.searchbar input{flex:1;}

canvas.spark{width:100%;height:60px;display:block;}

.security-note{background:rgba(255,182,72,.08);border:1px solid rgba(255,182,72,.3);
  border-radius:10px;padding:10px 14px;font-size:12px;color:#ffcf8a;margin-bottom:16px;}

@media (max-width:900px){
  .sidebar{position:fixed;left:-240px;top:0;transition:.25s;box-shadow:var(--shadow);}
  .sidebar.open{left:0;}
  .hamb{display:block;}
  .formgrid{grid-template-columns:1fr;}
  .view{padding:14px;}
}
</style>
</head>
<body>
<div class="toasts" id="toasts"></div>

<div class="modal-bg" id="modalBg">
  <div class="modal">
    <h3 id="modalTitle">Bestaetigen</h3>
    <p id="modalMsg" style="color:var(--muted);font-size:13px;"></p>
    <div class="btnrow">
      <button class="btn ghost" onclick="closeModal()">Abbrechen</button>
      <button class="btn danger" id="modalConfirmBtn">Bestaetigen</button>
    </div>
  </div>
</div>

<div class="layout">
  <nav class="sidebar" id="sidebar">
    <div class="brand"><div class="brand-dot"></div><div><b>ESP32 Dashboard</b><small>WROOM-32 &middot; v1.0.0</small></div></div>
    <div class="nav" id="navList">
      <div class="sec-label">Uebersicht</div>
      <a data-view="view-dashboard" class="active">&#9679; Dashboard</a>
      <a data-view="view-system">&#9881; System</a>
      <div class="sec-label">Netzwerk</div>
      <a data-view="view-wifi">&#128246; WiFi</a>
      <a data-view="view-scanner">&#128270; WiFi Scanner</a>
      <a data-view="view-ap">&#128225; Access Point</a>
      <a data-view="view-network">&#127760; Network</a>
      <div class="sec-label">Bluetooth</div>
      <a data-view="view-ble">&#128375; BLE Scanner</a>
      <a data-view="view-beacons">&#128737; Beacon Manager</a>
      <div class="sec-label">System</div>
      <a data-view="view-logs">&#128220; Logs</a>
      <a data-view="view-tools">&#128295; Tools</a>
      <a data-view="view-settings">&#9881;&#65039; Settings</a>
    </div>
    <div class="sidebar-foot">HTTP-Dashboard &middot; kein TLS<br>Zugangsdaten nicht mit sensiblen Netzen teilen.</div>
  </nav>

  <div class="content">
    <div class="topbar">
      <div style="display:flex;align-items:center;gap:10px;">
        <button class="hamb" onclick="toggleSidebar()">&#9776;</button>
        <div>
          <h1 id="pageTitle">Dashboard</h1>
          <div class="sub" id="pageSub">Live-Uebersicht aller Systeme</div>
        </div>
      </div>
      <div class="top-right">
        <div class="pill"><span class="dot" id="wifiDot"></span><span id="wifiPillText">WLAN</span></div>
        <div class="pill"><span class="dot" id="bleDot"></span><span id="blePillText">BLE</span></div>
        <div class="pill">&#9201; <span id="uptimePill">--</span></div>
      </div>
    </div>

    <!-- ===================== DASHBOARD ===================== -->
    <div class="view active" id="view-dashboard">
      <h2 class="vtitle">System-Uebersicht</h2>
      <p class="vdesc">Live-Status aller wichtigen Komponenten. Aktualisiert automatisch.</p>
      <div class="grid">
        <div class="card"><h3>CPU</h3>
          <div class="stat" id="d-cpu">--%</div>
          <div class="progress" id="d-cpu-bar"><div style="width:0%"></div></div>
          <div class="hint" id="d-cpu-hint">Core0 / Core1</div>
        </div>
        <div class="card"><h3>RAM (Heap)</h3>
          <div class="stat" id="d-heap">--%</div>
          <div class="progress" id="d-heap-bar"><div style="width:0%"></div></div>
          <div class="hint" id="d-heap-hint">-- / -- KB frei</div>
        </div>
        <div class="card"><h3>Uptime</h3>
          <div class="stat" id="d-uptime">--</div>
          <div class="hint">Seit letztem Neustart</div>
        </div>
        <div class="card"><h3>Letzter Fehler</h3>
          <div class="stat" id="d-lasterr" style="font-size:14px;">Keiner</div>
          <div class="hint" id="d-lasterr-hint"></div>
        </div>
      </div>

      <div class="grid cols-2">
        <div class="card">
          <h3>WiFi <span class="badge" id="d-wifi-badge">--</span></h3>
          <div class="rowline"><span class="k">SSID</span><span class="v" id="d-wifi-ssid">--</span></div>
          <div class="rowline"><span class="k">IP-Adresse</span><span class="v" id="d-wifi-ip">--</span></div>
          <div class="rowline"><span class="k">RSSI</span><span class="v" id="d-wifi-rssi">--</span></div>
          <div class="rowline"><span class="k">Kanal</span><span class="v" id="d-wifi-ch">--</span></div>
          <canvas class="spark" id="chart-rssi"></canvas>
        </div>
        <div class="card">
          <h3>Access Point <span class="badge" id="d-ap-badge">--</span></h3>
          <div class="rowline"><span class="k">AP-SSID</span><span class="v" id="d-ap-ssid">--</span></div>
          <div class="rowline"><span class="k">AP-IP</span><span class="v" id="d-ap-ip">--</span></div>
          <div class="rowline"><span class="k">Verbundene Clients</span><span class="v" id="d-ap-clients">--</span></div>
          <div class="rowline"><span class="k">Hardware-Limit</span><span class="v">1 SoftAP</span></div>
        </div>
        <div class="card">
          <h3>Bluetooth LE <span class="badge" id="d-ble-badge">--</span></h3>
          <div class="rowline"><span class="k">Scan-Status</span><span class="v" id="d-ble-scan">--</span></div>
          <div class="rowline"><span class="k">Gefundene Geraete</span><span class="v" id="d-ble-count">--</span></div>
          <canvas class="spark" id="chart-ble"></canvas>
        </div>
        <div class="card">
          <h3>Netzwerk</h3>
          <div class="rowline"><span class="k">Gateway</span><span class="v" id="d-gw">--</span></div>
          <div class="rowline"><span class="k">DNS</span><span class="v" id="d-dns">--</span></div>
          <div class="rowline"><span class="k">Heap-Verlauf</span><span class="v"></span></div>
          <canvas class="spark" id="chart-heap"></canvas>
        </div>
      </div>
    </div>

    <!-- ===================== SYSTEM ===================== -->
    <div class="view" id="view-system">
      <h2 class="vtitle">System-Informationen</h2>
      <p class="vdesc">Hardware-, Speicher- und Firmware-Details des ESP32-WROOM-32.</p>
      <div class="grid cols-2">
        <div class="panel"><h3>Chip</h3>
          <div class="rowline"><span class="k">Modell</span><span class="v" id="s-model">--</span></div>
          <div class="rowline"><span class="k">Revision</span><span class="v" id="s-rev">--</span></div>
          <div class="rowline"><span class="k">CPU-Kerne</span><span class="v" id="s-cores">--</span></div>
          <div class="rowline"><span class="k">CPU-Takt</span><span class="v" id="s-freq">--</span></div>
          <div class="rowline"><span class="k">SDK-Version</span><span class="v" id="s-sdk">--</span></div>
          <div class="rowline"><span class="k">Reset-Grund</span><span class="v" id="s-reset">--</span></div>
          <div class="rowline"><span class="k">Chip-MAC (eFuse)</span><span class="v" id="s-mac">--</span></div>
        </div>
        <div class="panel"><h3>Speicher</h3>
          <div class="rowline"><span class="k">Flash gesamt</span><span class="v" id="s-flash">--</span></div>
          <div class="rowline"><span class="k">Flash-Taktrate</span><span class="v" id="s-flashspeed">--</span></div>
          <div class="rowline"><span class="k">Sketch-Groesse</span><span class="v" id="s-sketch">--</span></div>
          <div class="rowline"><span class="k">Freier Flash</span><span class="v" id="s-freeflash">--</span></div>
          <div class="rowline"><span class="k">Heap gesamt</span><span class="v" id="s-heaptotal">--</span></div>
          <div class="rowline"><span class="k">Heap frei</span><span class="v" id="s-heapfree">--</span></div>
          <div class="rowline"><span class="k">Minimaler freier Heap</span><span class="v" id="s-heapmin">--</span></div>
          <div class="rowline"><span class="k">PSRAM</span><span class="v" id="s-psram">--</span></div>
        </div>
        <div class="panel"><h3>CPU-Auslastung</h3>
          <div class="rowline"><span class="k">Core 0</span><span class="v" id="s-cpu0">--</span></div>
          <div class="progress" id="s-cpu0-bar"><div style="width:0%"></div></div>
          <div class="rowline" style="margin-top:10px;"><span class="k">Core 1</span><span class="v" id="s-cpu1">--</span></div>
          <div class="progress" id="s-cpu1-bar"><div style="width:0%"></div></div>
          <div class="hint">Gemessen ueber FreeRTOS-Laufzeitstatistik (IDLE-Task), echte Messung.</div>
        </div>
        <div class="panel"><h3>Temperatur (intern)</h3>
          <div class="stat" id="s-temp">-- &deg;C</div>
          <div class="hint">Inoffizielle, undokumentierte ROM-Funktion des klassischen ESP32 - Werte dienen nur als grobe Orientierung, nicht kalibriert.</div>
        </div>
      </div>
    </div>

    <!-- ===================== WIFI ===================== -->
    <div class="view" id="view-wifi">
      <h2 class="vtitle">WiFi (Station Mode)</h2>
      <p class="vdesc">Verbindung des ESP32 zu einem bestehenden WLAN-Netzwerk.</p>
      <div class="grid cols-2">
        <div class="panel"><h3>Status</h3>
          <div class="rowline"><span class="k">Status</span><span class="v" id="w-status">--</span></div>
          <div class="rowline"><span class="k">SSID</span><span class="v" id="w-ssid">--</span></div>
          <div class="rowline"><span class="k">BSSID</span><span class="v" id="w-bssid">--</span></div>
          <div class="rowline"><span class="k">IP-Adresse</span><span class="v" id="w-ip">--</span></div>
          <div class="rowline"><span class="k">Gateway</span><span class="v" id="w-gw">--</span></div>
          <div class="rowline"><span class="k">Subnetz</span><span class="v" id="w-mask">--</span></div>
          <div class="rowline"><span class="k">DNS</span><span class="v" id="w-dns">--</span></div>
          <div class="rowline"><span class="k">RSSI</span><span class="v" id="w-rssi">--</span></div>
          <div class="rowline"><span class="k">Kanal</span><span class="v" id="w-ch">--</span></div>
          <div class="rowline"><span class="k">PHY-Modus</span><span class="v" id="w-phy">--</span></div>
          <div class="rowline"><span class="k">MAC (Station)</span><span class="v" id="w-mac">--</span></div>
          <div class="rowline"><span class="k">Verbunden seit</span><span class="v" id="w-since">--</span></div>
          <div class="btnrow">
            <button class="btn" onclick="api('/api/wifi/reconnect',{})">Neu verbinden</button>
            <button class="btn danger" onclick="confirmAction('WLAN trennen?','WLAN wirklich trennen?',()=>api('/api/wifi/disconnect',{}))">Trennen</button>
          </div>
        </div>
        <div class="panel"><h3>Verbinden</h3>
          <label>SSID</label><input type="text" id="w-in-ssid" maxlength="32" placeholder="Netzwerkname">
          <label>Passwort</label><input type="password" id="w-in-pass" maxlength="64" placeholder="WLAN-Passwort">
          <div class="btnrow">
            <button class="btn primary" onclick="wifiConnectSubmit()">Verbinden &amp; speichern</button>
          </div>
          <div class="hint" id="w-err" style="color:var(--bad);"></div>
          <hr style="border-color:var(--border);margin:16px 0;">
          <h3 style="margin-bottom:6px;">Gespeicherte Konfiguration</h3>
          <div class="rowline"><span class="k">Gespeicherte SSID</span><span class="v" id="w-saved">--</span></div>
          <div class="btnrow">
            <button class="btn danger sm" onclick="confirmAction('WLAN vergessen?','Gespeicherte Zugangsdaten wirklich loeschen?',()=>api('/api/wifi/forget',{}))">Zugangsdaten loeschen</button>
          </div>
        </div>
      </div>
    </div>

    <!-- ===================== WIFI SCANNER ===================== -->
    <div class="view" id="view-scanner">
      <h2 class="vtitle">WiFi Scanner</h2>
      <p class="vdesc">Scan nach verfuegbaren WLAN-Netzwerken in Reichweite.</p>
      <div class="panel">
        <div class="flexrow">
          <div><span class="badge info" id="sc-status">Bereit</span> <span class="hint" id="sc-count"></span></div>
          <button class="btn primary" id="sc-btn" onclick="startScan()">Scan starten</button>
        </div>
        <div class="searchbar" style="margin-top:14px;">
          <input type="text" id="sc-filter" placeholder="SSID filtern..." oninput="renderScan()">
          <select id="sc-sort" onchange="renderScan()">
            <option value="rssi">Sortieren: Signalstaerke</option>
            <option value="channel">Sortieren: Kanal</option>
            <option value="ssid">Sortieren: SSID</option>
          </select>
        </div>
        <div class="tablewrap">
          <table>
            <thead><tr><th>SSID</th><th>Signal</th><th>RSSI</th><th>Kanal</th><th>Verschluesselung</th><th>BSSID</th></tr></thead>
            <tbody id="sc-body"><tr><td colspan="6" style="color:var(--muted);">Noch kein Scan durchgefuehrt.</td></tr></tbody>
          </table>
        </div>
      </div>
    </div>

    <!-- ===================== ACCESS POINT ===================== -->
    <div class="view" id="view-ap">
      <h2 class="vtitle">Access Point</h2>
      <p class="vdesc">Eigener SoftAP des ESP32. Hardware-Limit: 1 aktiver Access Point gleichzeitig.</p>
      <div class="grid cols-2">
        <div class="panel"><h3>Status</h3>
          <div class="rowline"><span class="k">Status</span><span class="v" id="ap-status">--</span></div>
          <div class="rowline"><span class="k">SSID</span><span class="v" id="ap-ssid-v">--</span></div>
          <div class="rowline"><span class="k">AP-IP</span><span class="v" id="ap-ip">--</span></div>
          <div class="rowline"><span class="k">AP-MAC</span><span class="v" id="ap-mac">--</span></div>
          <div class="rowline"><span class="k">Kanal</span><span class="v" id="ap-ch-v">--</span></div>
          <div class="rowline"><span class="k">Verbundene Clients</span><span class="v" id="ap-clients-n">--</span></div>
          <div class="tablewrap" style="margin-top:10px;">
            <table><thead><tr><th>MAC</th><th>RSSI</th><th>IP</th></tr></thead><tbody id="ap-clients-body"><tr><td colspan="3" style="color:var(--muted);">Keine Clients verbunden.</td></tr></tbody></table>
          </div>
        </div>
        <div class="panel"><h3>Konfiguration</h3>
          <label>WLAN-Modus</label>
          <select id="ap-mode" onchange="setMode()">
            <option value="2">Access Point + Station</option>
            <option value="1">Nur Access Point</option>
            <option value="0">Nur Station</option>
          </select>
          <div class="hint">Ein reiner Stationsmodus wird nur uebernommen, wenn bereits eine WLAN-Verbindung besteht - so sperrt sich der ESP32 nie aus.</div>
          <label>SSID</label><input type="text" id="ap-in-ssid" maxlength="32">
          <label>Passwort (min. 8 Zeichen, leer = offen)</label><input type="password" id="ap-in-pass" maxlength="64">
          <div class="formgrid">
            <div><label>Kanal</label><input type="number" id="ap-in-ch" min="1" max="13" value="1"></div>
            <div><label>Max. Clients</label><input type="number" id="ap-in-max" min="1" max="8" value="4"></div>
          </div>
          <div class="flexrow" style="margin-top:12px;"><span>Versteckte SSID</span>
            <label class="switch"><input type="checkbox" id="ap-in-hidden"><span class="slider-tog"></span></label></div>
          <div class="btnrow">
            <button class="btn primary" onclick="apApply(true)">Speichern &amp; aktivieren</button>
            <button class="btn danger" onclick="confirmAction('AP stoppen?','Access Point wirklich deaktivieren?',()=>apApply(false))">Deaktivieren</button>
          </div>
        </div>
      </div>
    </div>

    <!-- ===================== NETWORK (DIAGNOSE) ===================== -->
    <div class="view" id="view-network">
      <h2 class="vtitle">Netzwerk-Diagnose</h2>
      <p class="vdesc">IP-Konfiguration und Verbindungstests.</p>
      <div class="grid cols-2">
        <div class="panel"><h3>IP-Konfiguration</h3>
          <div class="rowline"><span class="k">Station verbunden</span><span class="v" id="n-sta">--</span></div>
          <div class="rowline"><span class="k">Lokale IP</span><span class="v" id="n-ip">--</span></div>
          <div class="rowline"><span class="k">Gateway</span><span class="v" id="n-gw">--</span></div>
          <div class="rowline"><span class="k">Subnetz</span><span class="v" id="n-mask">--</span></div>
          <div class="rowline"><span class="k">DNS-Server</span><span class="v" id="n-dns">--</span></div>
          <div class="rowline"><span class="k">AP-IP</span><span class="v" id="n-apip">--</span></div>
          <div class="rowline"><span class="k">Hostname</span><span class="v" id="n-host">--</span></div>
          <div class="btnrow">
            <button class="btn sm" onclick="api('/api/diag/gwtest',{},r=>toast(r.ok?('Gateway erreichbar ('+r.ms+' ms)'):'Gateway nicht erreichbar',r.ok?'good':'bad'))">Gateway testen</button>
            <button class="btn sm" onclick="api('/api/diag/inettest',{},r=>toast(r.ok?('Internet erreichbar ('+r.ms+' ms)'):'Kein Internetzugriff',r.ok?'good':'bad'))">Internet testen</button>
          </div>
        </div>
        <div class="panel"><h3>Ping</h3>
          <label>Host / IP</label><input type="text" id="p-host" placeholder="z.B. 8.8.8.8 oder google.com">
          <div class="btnrow"><button class="btn primary" onclick="pingSubmit()">Ping starten</button></div>
          <div id="p-result" class="hint" style="margin-top:10px;"></div>
        </div>
        <div class="panel"><h3>DNS-Aufloesung</h3>
          <label>Hostname</label><input type="text" id="dns-host" placeholder="z.B. example.com">
          <div class="btnrow"><button class="btn" onclick="dnsSubmit()">Aufloesen</button></div>
          <div id="dns-result" class="hint" style="margin-top:10px;"></div>
        </div>
        <div class="panel"><h3>TCP-Verbindungstest</h3>
          <div class="formgrid">
            <div><label>Host</label><input type="text" id="tcp-host" placeholder="z.B. example.com"></div>
            <div><label>Port</label><input type="number" id="tcp-port" value="80"></div>
          </div>
          <div class="btnrow"><button class="btn" onclick="tcpSubmit()">Verbindung testen</button></div>
          <div id="tcp-result" class="hint" style="margin-top:10px;"></div>
        </div>
      </div>
    </div>

    <!-- ===================== BLE SCANNER ===================== -->
    <div class="view" id="view-ble">
      <h2 class="vtitle">Bluetooth LE Scanner</h2>
      <p class="vdesc">Scan nach BLE-Geraeten in Reichweite (Hardware: klassischer BLE-4.2-Controller).</p>
      <div class="panel">
        <div class="flexrow">
          <div><span>BLE aktiv</span></div>
          <label class="switch"><input type="checkbox" id="ble-enable-sw" onchange="bleToggle()"><span class="slider-tog"></span></label>
        </div>
      </div>
      <div class="panel">
        <div class="flexrow">
          <div><span class="badge info" id="ble-status">Inaktiv</span> <span class="hint" id="ble-count-txt"></span></div>
          <div class="btnrow" style="margin:0;">
            <input type="number" id="ble-duration" value="8" min="1" max="60" style="width:70px;">
            <button class="btn primary" id="ble-scan-btn" onclick="bleScanStart()">Scan starten</button>
            <button class="btn" onclick="api('/api/ble/scan/stop',{})">Stop</button>
            <button class="btn ghost" onclick="confirmAction('Liste leeren?','Alle gefundenen BLE-Geraete entfernen?',()=>api('/api/ble/clear',{}))">Liste leeren</button>
          </div>
        </div>
        <div class="searchbar" style="margin-top:14px;">
          <input type="text" id="ble-filter" placeholder="Name oder Adresse filtern..." oninput="renderBle()">
        </div>
        <div class="tablewrap">
          <table>
            <thead><tr><th>Name</th><th>Adresse</th><th>RSSI</th><th>Service-UUID</th><th>Hersteller-Daten</th><th>Zuletzt gesehen</th></tr></thead>
            <tbody id="ble-body"><tr><td colspan="6" style="color:var(--muted);">Keine Geraete gefunden.</td></tr></tbody>
          </table>
        </div>
      </div>
    </div>

    <!-- ===================== BEACON MANAGER ===================== -->
    <div class="view" id="view-beacons">
      <h2 class="vtitle">Beacon Manager</h2>
      <p class="vdesc">Verwaltung eigener WLAN- und BLE-Testbeacons. Der ESP32-WROOM-32 kann jeweils nur <b>einen</b> WLAN-AP und <b>einen</b> BLE-Advertiser gleichzeitig senden - mehrere aktivierte Profile werden daher automatisch im Zeittakt rotiert.</p>

      <div class="panel">
        <div style="display:flex;gap:8px;margin-bottom:14px;">
          <button class="btn primary sm" id="tab-wb-btn" onclick="showBeaconTab('wb')">WLAN Beacons</button>
          <button class="btn ghost sm" id="tab-bb-btn" onclick="showBeaconTab('bb')">BLE Beacons</button>
        </div>

        <div id="tab-wb">
          <div class="grid cols-2" style="margin-bottom:6px;">
            <div class="card"><h3>Konfiguriert</h3><div class="stat" id="wb-cfg">0 / 6</div></div>
            <div class="card"><h3>Aktiv / Hardware-Limit</h3><div class="stat" id="wb-active">0 / 1</div></div>
          </div>
          <div class="flexrow">
            <div>Anzahl WLAN-Beacons: <button class="btn sm" onclick="wbCount(-1)">-</button> <b id="wb-count-num">0</b> <button class="btn sm" onclick="wbCount(1)">+</button></div>
            <div class="flexrow" style="gap:8px;">
              <span class="hint">Rotation</span>
              <label class="switch"><input type="checkbox" id="wb-rot-sw" onchange="wbRotToggle()"><span class="slider-tog"></span></label>
              <input type="number" id="wb-rot-iv" value="6000" style="width:80px;" title="Intervall (ms)">
            </div>
          </div>
          <div class="btnrow">
            <button class="btn" onclick="api('/api/beacons/wifi/startall',{})">Alle starten</button>
            <button class="btn" onclick="api('/api/beacons/wifi/stopall',{})">Alle stoppen</button>
            <button class="btn primary" onclick="api('/api/beacons/wifi/save',{})">Konfiguration speichern</button>
          </div>
          <div id="wb-list" style="margin-top:16px;"></div>
        </div>

        <div id="tab-bb" style="display:none;">
          <div class="grid cols-2" style="margin-bottom:6px;">
            <div class="card"><h3>Konfiguriert</h3><div class="stat" id="bb-cfg">0 / 10</div></div>
            <div class="card"><h3>Aktiv / Hardware-Limit</h3><div class="stat" id="bb-active">0 / 1</div></div>
          </div>
          <div class="flexrow">
            <div>Anzahl BLE-Beacons: <button class="btn sm" onclick="bbCount(-1)">-</button> <b id="bb-count-num">0</b> <button class="btn sm" onclick="bbCount(1)">+</button></div>
            <div class="flexrow" style="gap:8px;">
              <span class="hint">Rotation</span>
              <label class="switch"><input type="checkbox" id="bb-rot-sw" onchange="bbRotToggle()"><span class="slider-tog"></span></label>
              <input type="number" id="bb-rot-iv" value="5000" style="width:80px;" title="Intervall (ms)">
            </div>
          </div>
          <div class="btnrow">
            <button class="btn" onclick="api('/api/beacons/ble/startall',{})">Alle starten</button>
            <button class="btn" onclick="api('/api/beacons/ble/stopall',{})">Alle stoppen</button>
            <button class="btn primary" onclick="api('/api/beacons/ble/save',{})">Konfiguration speichern</button>
          </div>
          <div class="hint" style="margin-top:8px;">BLE muss auf der BLE-Scanner-Seite aktiviert sein, damit Beacons gesendet werden koennen.</div>
          <div id="bb-list" style="margin-top:16px;"></div>
        </div>
      </div>
    </div>

    <!-- ===================== LOGS ===================== -->
    <div class="view" id="view-logs">
      <h2 class="vtitle">System-Logs</h2>
      <p class="vdesc">Interne Ereignisprotokollierung (Ringpuffer, RAM-schonend).</p>
      <div class="panel">
        <div class="flexrow">
          <select id="log-level" onchange="logLevelSet()">
            <option value="0">Alle (Debug)</option>
            <option value="1" selected>Info und hoeher</option>
            <option value="2">Nur Warnungen/Fehler</option>
            <option value="3">Nur Fehler</option>
          </select>
          <div class="btnrow" style="margin:0;">
            <button class="btn sm" onclick="downloadLogs()">Herunterladen</button>
            <button class="btn danger sm" onclick="confirmAction('Logs loeschen?','Alle Logeintraege wirklich loeschen?',()=>api('/api/logs/clear',{},loadLogs))">Loeschen</button>
          </div>
        </div>
        <div class="tablewrap" style="margin-top:12px;">
          <table><thead><tr><th>Zeit (s)</th><th>Level</th><th>Nachricht</th></tr></thead><tbody id="log-body"></tbody></table>
        </div>
      </div>
    </div>

    <!-- ===================== TOOLS ===================== -->
    <div class="view" id="view-tools">
      <h2 class="vtitle">System-Tools</h2>
      <p class="vdesc">Diagnose- und Wartungsfunktionen. Destruktive Aktionen erfordern eine Bestaetigung.</p>
      <div class="grid cols-2">
        <div class="panel"><h3>Neustart</h3>
          <p class="hint">Startet den gesamten ESP32 neu.</p>
          <button class="btn danger" onclick="confirmAction('ESP32 neu starten?','Der ESP32 wird neu gestartet und ist kurzzeitig nicht erreichbar.',()=>api('/api/tools/restart',{}))">ESP32 Neustart</button>
        </div>
        <div class="panel"><h3>WLAN-Stack neu starten</h3>
          <p class="hint">Trennt und reinitialisiert den WiFi-Stack (ohne Reboot).</p>
          <button class="btn" onclick="confirmAction('WLAN neu starten?','WiFi wird kurz getrennt und neu initialisiert.',()=>api('/api/tools/wifirestart',{}))">WLAN Neustart</button>
        </div>
        <div class="panel"><h3>BLE-Stack neu starten</h3>
          <p class="hint">Deaktiviert und reaktiviert den Bluetooth-Stack.</p>
          <button class="btn" onclick="confirmAction('BLE neu starten?','Bluetooth wird kurz deaktiviert und neu initialisiert.',()=>api('/api/tools/blerestart',{}))">BLE Neustart</button>
        </div>
        <div class="panel"><h3>Status aktualisieren</h3>
          <p class="hint">Erzwingt eine sofortige Aktualisierung aller Live-Werte.</p>
          <button class="btn" onclick="pollStatus();toast('Status aktualisiert','good')">Jetzt aktualisieren</button>
        </div>
        <div class="panel"><h3>Factory-Reset</h3>
          <p class="hint">Loescht alle gespeicherten Konfigurationen (WLAN, AP, Beacons, Zugangsdaten) unwiderruflich.</p>
          <button class="btn danger" onclick="confirmAction('Factory-Reset?','ALLE gespeicherten Einstellungen werden unwiderruflich geloescht. Fortfahren?',()=>api('/api/tools/factoryreset',{}))">Factory-Reset</button>
        </div>
      </div>
    </div>

    <!-- ===================== SETTINGS ===================== -->
    <div class="view" id="view-settings">
      <h2 class="vtitle">Einstellungen</h2>
      <div class="security-note">&#9888; Dieses Dashboard verwendet HTTP ohne Verschluesselung (kein TLS). Zugangsdaten werden unverschluesselt uebertragen - nur in vertrauenswuerdigen Netzwerken verwenden.</div>
      <div class="grid cols-2">
        <div class="panel"><h3>Dashboard-Zugang</h3>
          <label>Benutzername</label><input type="text" id="set-user" maxlength="23">
          <label>Neues Passwort</label><input type="password" id="set-pass" maxlength="31" placeholder="Leer lassen = unveraendert">
          <div class="btnrow"><button class="btn primary" onclick="authSubmit()">Speichern</button></div>
        </div>
        <div class="panel"><h3>Ueber</h3>
          <div class="rowline"><span class="k">Firmware</span><span class="v">ESP32 Ultimate Dashboard</span></div>
          <div class="rowline"><span class="k">Version</span><span class="v">1.0.0</span></div>
          <div class="rowline"><span class="k">Zielhardware</span><span class="v">ESP32-WROOM-32</span></div>
        </div>
      </div>
    </div>

  </div>
</div>

<script>
)RAWPAGE";

// Der JS-Teil wird als eigener PROGMEM-Block gespeichert und beim Ausliefern per
// Chunked-Response direkt angehaengt (siehe handleRoot()) - so muss die ca. 60 KB
// grosse Seite nie als Ganzes im RAM zusammengebaut werden.
const char PAGE_HTML_TAIL[] PROGMEM = R"RAWTAIL(
</script>
</body>
</html>
)RAWTAIL";


const char PAGE_JS[] PROGMEM = R"RAWJS(
// ---------------------------------------------------------------------------
// ESP32 Ultimate Dashboard - Frontend-Logik (vanilla JS, keine Frameworks)
// ---------------------------------------------------------------------------
let scanData = {networks:[], scanning:false, count:0};
let bleData = {devices:[], scanning:false};
let currentBeaconTab = 'wb';
let statusCache = {};

function qs(id){ return document.getElementById(id); }

// ---- Navigation -----------------------------------------------------------
document.querySelectorAll('.nav a').forEach(a=>{
  a.addEventListener('click', ()=>{
    document.querySelectorAll('.nav a').forEach(x=>x.classList.remove('active'));
    a.classList.add('active');
    document.querySelectorAll('.view').forEach(v=>v.classList.remove('active'));
    qs(a.dataset.view).classList.add('active');
    qs('pageTitle').textContent = a.textContent.trim().replace(/^\S+\s/, '');
    document.getElementById('sidebar').classList.remove('open');
    onViewShown(a.dataset.view);
  });
});
function toggleSidebar(){ qs('sidebar').classList.toggle('open'); }

function onViewShown(id){
  if(id==='view-scanner') renderScan();
  if(id==='view-ble') renderBle();
  if(id==='view-beacons'){ loadWifiBeacons(); loadBleBeacons(); }
  if(id==='view-logs') loadLogs();
  if(id==='view-wifi') loadWifi();
  if(id==='view-ap') loadAp();
  if(id==='view-network') loadNetwork();
  if(id==='view-system') loadSystem();
  if(id==='view-settings'){ qs('set-user').value='admin'; }
}

// ---- Toasts -----------------------------------------------------------------
function toast(msg, type){
  const el = document.createElement('div');
  el.className = 'toast ' + (type||'');
  el.textContent = msg;
  qs('toasts').appendChild(el);
  setTimeout(()=>{ el.style.opacity='0'; el.style.transition='opacity .3s'; setTimeout(()=>el.remove(),300); }, 3800);
}

// ---- Modal (Bestaetigungsdialog) --------------------------------------------
let modalCb = null;
function confirmAction(title, msg, cb){
  qs('modalTitle').textContent = title;
  qs('modalMsg').textContent = msg;
  modalCb = cb;
  qs('modalBg').classList.add('show');
}
function closeModal(){ qs('modalBg').classList.remove('show'); modalCb=null; }
qs('modalConfirmBtn').addEventListener('click', ()=>{ const cb=modalCb; closeModal(); if(cb) cb(); });

// ---- Generischer API-Helfer (form-urlencoded POST) --------------------------
function api(path, params, cb){
  const body = new URLSearchParams(params||{}).toString();
  fetch(path, {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body})
    .then(r=>r.json())
    .then(d=>{ if(d.message) toast(d.message, d.ok===false?'bad':'good'); if(cb) cb(d); })
    .catch(e=>toast('Netzwerkfehler: '+e,'bad'));
}
function apiGet(path, cb){
  fetch(path).then(r=>r.json()).then(cb).catch(e=>toast('Netzwerkfehler: '+e,'bad'));
}

// ---- Sparkline-Diagramme (Canvas, keine Bibliothek) -------------------------
function drawSparkline(id, data, color, minV, maxV){
  const c = qs(id); if(!c) return;
  const ctx = c.getContext('2d');
  const w = c.clientWidth || 260, h = 60;
  c.width = w * (window.devicePixelRatio||1); c.height = h * (window.devicePixelRatio||1);
  ctx.setTransform(window.devicePixelRatio||1,0,0,window.devicePixelRatio||1,0,0);
  ctx.clearRect(0,0,w,h);
  if(!data || data.length<2) return;
  let mn = (minV!==undefined)?minV:Math.min(...data);
  let mx = (maxV!==undefined)?maxV:Math.max(...data);
  if(mx===mn) mx = mn+1;
  ctx.beginPath();
  data.forEach((v,i)=>{
    const x = i/(data.length-1)*w;
    const y = h - ((v-mn)/(mx-mn))*h;
    if(i===0) ctx.moveTo(x,y); else ctx.lineTo(x,y);
  });
  ctx.strokeStyle = color; ctx.lineWidth = 2; ctx.stroke();
  ctx.lineTo(w,h); ctx.lineTo(0,h); ctx.closePath();
  ctx.fillStyle = color+'22'; ctx.fill();
}

// ---- Status-Polling (Dashboard + Topbar) -------------------------------------
function fmtBytes(b){ return (b/1024).toFixed(0)+' KB'; }
function fmtUptime(ms){
  let s = Math.floor(ms/1000);
  const d = Math.floor(s/86400); s%=86400;
  const h = Math.floor(s/3600); s%=3600;
  const m = Math.floor(s/60); s%=60;
  let out='';
  if(d) out += d+'d '; out += (h<10?'0':'')+h+':'+(m<10?'0':'')+m+':'+(s<10?'0':'')+s;
  return out;
}

function pollStatus(){
  apiGet('/api/status', d=>{
    statusCache = d;
    qs('uptimePill').textContent = fmtUptime(d.uptimeMs);
    qs('wifiDot').className = 'dot ' + (d.wifiStatus==='connected'?'ok':'bad');
    qs('wifiPillText').textContent = d.wifiStatus==='connected' ? ('WLAN: '+d.wifiSsid) : 'WLAN: getrennt';
    qs('bleDot').className = 'dot ' + (d.bleEnabled ? (d.bleScanning?'warn':'ok') : 'bad');
    qs('blePillText').textContent = d.bleEnabled ? (d.bleScanning?'BLE: Scan laeuft':'BLE: aktiv') : 'BLE: aus';

    qs('d-cpu').innerHTML = (((d.cpu0+d.cpu1)/2).toFixed(0))+'% <small>Core0 '+d.cpu0.toFixed(0)+'% / Core1 '+d.cpu1.toFixed(0)+'%</small>';
    const cpuAvg = (d.cpu0+d.cpu1)/2;
    const cpuBar = qs('d-cpu-bar'); cpuBar.firstElementChild.style.width = cpuAvg+'%';
    cpuBar.className = 'progress ' + (cpuAvg>85?'bad':(cpuAvg>60?'warn':''));

    qs('d-heap').textContent = d.heapPct.toFixed(0)+'%';
    const heapBar = qs('d-heap-bar'); heapBar.firstElementChild.style.width = d.heapPct+'%';
    heapBar.className = 'progress ' + (d.heapPct>85?'bad':(d.heapPct>65?'warn':''));
    qs('d-heap-hint').textContent = fmtBytes(d.heapSize-d.freeHeap)+' / '+fmtBytes(d.heapSize)+' belegt';

    qs('d-uptime').textContent = fmtUptime(d.uptimeMs);
    qs('d-lasterr').textContent = d.lastError ? d.lastError : 'Keiner';
    qs('d-lasterr-hint').textContent = d.lastError ? ('vor '+Math.round(d.lastErrorAgoMs/1000)+' s') : '';

    qs('d-wifi-badge').className = 'badge ' + (d.wifiStatus==='connected'?'good':'bad');
    qs('d-wifi-badge').textContent = d.wifiStatus==='connected'?'Verbunden':'Getrennt';
    qs('d-wifi-ssid').textContent = d.wifiSsid || '--';
    qs('d-wifi-ip').textContent = d.wifiIp || '--';
    qs('d-wifi-rssi').textContent = d.wifiStatus==='connected' ? d.wifiRssi+' dBm' : '--';
    qs('d-wifi-ch').textContent = d.wifiChannel || '--';

    qs('d-ap-badge').className = 'badge ' + (d.apEnabled?'good':'info');
    qs('d-ap-badge').textContent = d.apEnabled?'Aktiv':'Inaktiv';
    qs('d-ap-ssid').textContent = d.apEnabled ? (statusCache.apSsid||'--') : '--';
    qs('d-ap-ip').textContent = d.apIp;
    qs('d-ap-clients').textContent = d.apClients;

    qs('d-ble-badge').className = 'badge ' + (d.bleEnabled?'good':'info');
    qs('d-ble-badge').textContent = d.bleEnabled?'Aktiv':'Inaktiv';
    qs('d-ble-scan').textContent = d.bleScanning?'Scan laeuft':'Bereit';
    qs('d-ble-count').textContent = d.bleDeviceCount;

    qs('d-gw').textContent = statusCache.gw || '--';
    qs('d-dns').textContent = statusCache.dns || '--';

    drawSparkline('chart-heap', d.heapHistory.map(Number), '#4f8cff');
    drawSparkline('chart-rssi', d.rssiHistory.map(Number), '#7c5cff', -100, -30);
    drawSparkline('chart-ble', d.bleCountHistory.map(Number), '#2fd47a', 0);
  });
  apiGet('/api/network', n=>{
    statusCache.gw = n.gateway; statusCache.dns = n.dns;
    qs('d-gw').textContent = n.gateway; qs('d-dns').textContent = n.dns;
  });
}

// ---- System -------------------------------------------------------------------
function loadSystem(){
  apiGet('/api/system', d=>{
    qs('s-model').textContent = d.chipModel;
    qs('s-rev').textContent = 'Rev. '+d.chipRevision;
    qs('s-cores').textContent = d.chipCores;
    qs('s-freq').textContent = d.cpuFreqMHz+' MHz';
    qs('s-sdk').textContent = d.sdkVersion;
    qs('s-reset').textContent = d.resetReason;
    qs('s-mac').textContent = d.efuseMac;
    qs('s-flash').textContent = fmtBytes(d.flashSize);
    qs('s-flashspeed').textContent = (d.flashSpeed/1000000).toFixed(0)+' MHz';
    qs('s-sketch').textContent = fmtBytes(d.sketchSize);
    qs('s-freeflash').textContent = fmtBytes(d.freeSketchSpace);
    qs('s-heaptotal').textContent = fmtBytes(d.heapSize);
    qs('s-heapfree').textContent = fmtBytes(d.freeHeap);
    qs('s-heapmin').textContent = fmtBytes(d.minFreeHeap);
    qs('s-psram').textContent = d.psramFound ? (fmtBytes(d.freePsram)+' / '+fmtBytes(d.psramSize)) : 'Nicht vorhanden';
    qs('s-cpu0').textContent = d.cpuStatsAvailable ? d.cpuUsageCore0.toFixed(1)+'%' : 'n/v';
    qs('s-cpu1').textContent = d.cpuStatsAvailable ? d.cpuUsageCore1.toFixed(1)+'%' : 'n/v';
    qs('s-cpu0-bar').firstElementChild.style.width = d.cpuUsageCore0+'%';
    qs('s-cpu1-bar').firstElementChild.style.width = d.cpuUsageCore1+'%';
    qs('s-temp').innerHTML = isFinite(d.tempC) ? d.tempC.toFixed(1)+' &deg;C' : 'nicht verfuegbar';
  });
}

// ---- WiFi -----------------------------------------------------------------------
function loadWifi(){
  apiGet('/api/wifi', d=>{
    qs('w-status').innerHTML = d.connected ? '<span class="badge good">Verbunden</span>' : (d.connecting?'<span class="badge warn">Verbinde...</span>':'<span class="badge bad">Getrennt</span>');
    qs('w-ssid').textContent = d.ssid || '--';
    qs('w-bssid').textContent = d.bssid || '--';
    qs('w-ip').textContent = d.ip;
    qs('w-gw').textContent = d.gateway;
    qs('w-mask').textContent = d.subnet;
    qs('w-dns').textContent = d.dns;
    qs('w-rssi').textContent = d.connected ? d.rssi+' dBm' : '--';
    qs('w-ch').textContent = d.channel || '--';
    qs('w-phy').textContent = d.phyMode;
    qs('w-mac').textContent = d.mac;
    qs('w-since').textContent = d.connected ? fmtUptime(d.connectedSinceMs) : '--';
    qs('w-saved').textContent = d.hasSaved ? d.savedSsid : '(keine gespeichert)';
    qs('w-err').textContent = d.lastError || '';
  });
}
function wifiConnectSubmit(){
  const ssid = qs('w-in-ssid').value.trim();
  const pass = qs('w-in-pass').value;
  if(!ssid){ toast('Bitte SSID eingeben','warn'); return; }
  api('/api/wifi/connect', {ssid, pass}, ()=>{ setTimeout(loadWifi, 1500); });
}

// ---- Access Point -----------------------------------------------------------------
function loadAp(){
  apiGet('/api/ap', d=>{
    qs('ap-status').innerHTML = d.enabled ? '<span class="badge good">Aktiv</span>' : '<span class="badge bad">Inaktiv</span>';
    qs('ap-ssid-v').textContent = d.ssid;
    qs('ap-ip').textContent = d.ip;
    qs('ap-mac').textContent = d.mac;
    qs('ap-ch-v').textContent = d.channel;
    qs('ap-clients-n').textContent = d.stationCount;
    qs('ap-mode').value = d.wifiMode;
    qs('ap-in-ssid').value = d.ssid;
    qs('ap-in-ch').value = d.channel;
    qs('ap-in-max').value = d.maxConnections;
    qs('ap-in-hidden').checked = d.hidden;
    const body = qs('ap-clients-body');
    if(d.clients && d.clients.length){
      body.innerHTML = d.clients.map(c=>`<tr><td>${c.mac}</td><td>${c.rssi} dBm</td><td>${c.ip}</td></tr>`).join('');
    } else {
      body.innerHTML = '<tr><td colspan="3" style="color:var(--muted);">Keine Clients verbunden.</td></tr>';
    }
  });
}
function apApply(enable){
  const ssid = qs('ap-in-ssid').value.trim();
  const pass = qs('ap-in-pass').value;
  const ch = qs('ap-in-ch').value;
  const maxc = qs('ap-in-max').value;
  const hidden = qs('ap-in-hidden').checked ? 1 : 0;
  if(enable && !ssid){ toast('Bitte SSID eingeben','warn'); return; }
  api('/api/ap/config', {ssid, pass, channel:ch, maxc, hidden, enable: enable?1:0}, ()=>setTimeout(loadAp,800));
}
function setMode(){
  api('/api/mode', {mode: qs('ap-mode').value}, ()=>setTimeout(loadAp,800));
}

// ---- WiFi Scanner -----------------------------------------------------------------
function startScan(){
  qs('sc-status').textContent = 'Scan laeuft...';
  qs('sc-btn').disabled = true;
  api('/api/wifi/scan/start', {}, ()=>pollScan());
}
function pollScan(){
  apiGet('/api/wifi/scan', d=>{
    scanData = d;
    if(d.scanning){ qs('sc-status').textContent='Scan laeuft...'; setTimeout(pollScan, 700); return; }
    qs('sc-btn').disabled = false;
    qs('sc-status').textContent = 'Fertig';
    qs('sc-count').textContent = d.count + ' Netzwerke gefunden';
    renderScan();
  });
}
function sigBars(rssi){
  const q = Math.max(0, Math.min(4, Math.round((rssi+100)/15)));
  let html='<div class="sig-bars">';
  for(let i=0;i<4;i++) html += `<i style="height:${5+i*4}px" class="${i<q?'on':''}"></i>`;
  html+='</div>';
  return html;
}
function renderScan(){
  const filter = (qs('sc-filter').value||'').toLowerCase();
  const sortBy = qs('sc-sort').value;
  let list = (scanData.networks||[]).filter(n=>n.ssid.toLowerCase().includes(filter));
  if(sortBy==='rssi') list.sort((a,b)=>b.rssi-a.rssi);
  if(sortBy==='channel') list.sort((a,b)=>a.channel-b.channel);
  if(sortBy==='ssid') list.sort((a,b)=>a.ssid.localeCompare(b.ssid));
  const body = qs('sc-body');
  if(!list.length){ body.innerHTML = '<tr><td colspan="6" style="color:var(--muted);">Keine Ergebnisse.</td></tr>'; return; }
  body.innerHTML = list.map(n=>`<tr>
    <td>${n.ssid}${n.hidden?' <span class="badge warn">versteckt</span>':''}</td>
    <td>${sigBars(n.rssi)}</td>
    <td>${n.rssi} dBm</td>
    <td>${n.channel}</td>
    <td>${n.enc}</td>
    <td style="color:var(--muted);">${n.bssid}</td>
  </tr>`).join('');
}

// ---- BLE Scanner -----------------------------------------------------------------
function bleToggle(){
  const en = qs('ble-enable-sw').checked;
  api(en?'/api/ble/enable':'/api/ble/disable', {}, ()=>loadBleStatus());
}
function loadBleStatus(){
  apiGet('/api/ble', d=>{
    bleData = d;
    qs('ble-enable-sw').checked = d.initialized;
    qs('ble-status').textContent = d.scanning ? 'Scan laeuft...' : (d.initialized?'Bereit':'Inaktiv');
    qs('ble-status').className = 'badge ' + (d.scanning?'warn':(d.initialized?'good':'info'));
    qs('ble-count-txt').textContent = d.deviceCount + ' / ' + d.maxDevices + ' Geraete';
    qs('ble-scan-btn').disabled = !d.initialized || d.scanning;
    renderBle();
    if(d.scanning) setTimeout(loadBleStatus, 1200);
  });
}
function bleScanStart(){
  const dur = qs('ble-duration').value || 8;
  api('/api/ble/scan/start', {duration:dur}, ()=>loadBleStatus());
}
function renderBle(){
  const filter = (qs('ble-filter').value||'').toLowerCase();
  let list = (bleData.devices||[]).filter(d=>(d.name||'').toLowerCase().includes(filter) || d.addr.toLowerCase().includes(filter));
  list.sort((a,b)=>b.rssi-a.rssi);
  const body = qs('ble-body');
  if(!list.length){ body.innerHTML = '<tr><td colspan="6" style="color:var(--muted);">Keine Geraete gefunden.</td></tr>'; return; }
  body.innerHTML = list.map(d=>`<tr>
    <td>${d.hasName?d.name:'<span style="color:var(--muted);">(unbenannt)</span>'}</td>
    <td>${d.addr}</td>
    <td>${d.rssi} dBm</td>
    <td style="font-size:11px;color:var(--muted);">${d.hasServiceUUID?d.serviceUUID:'--'}</td>
    <td style="font-size:11px;color:var(--muted);">${d.hasMfg?d.mfgHex:'--'}</td>
    <td>${Math.round(d.lastSeenAgoMs/1000)}s</td>
  </tr>`).join('');
}

// ---- Netzwerk-Diagnose -------------------------------------------------------------
function loadNetwork(){
  apiGet('/api/network', d=>{
    qs('n-sta').innerHTML = d.staConnected?'<span class="badge good">Ja</span>':'<span class="badge bad">Nein</span>';
    qs('n-ip').textContent = d.ip;
    qs('n-gw').textContent = d.gateway;
    qs('n-mask').textContent = d.subnet;
    qs('n-dns').textContent = d.dns;
    qs('n-apip').textContent = d.apIp;
    qs('n-host').textContent = d.hostname;
  });
}
function pingSubmit(){
  const host = qs('p-host').value.trim();
  if(!host){ toast('Bitte Host angeben','warn'); return; }
  qs('p-result').textContent = 'Ping laeuft...';
  api('/api/diag/ping/start', {host}, ()=>pollPing());
}
function pollPing(){
  apiGet('/api/diag/ping/status', d=>{
    if(d.resolveError){ qs('p-result').innerHTML = '<span style="color:var(--bad);">Hostname konnte nicht aufgeloest werden.</span>'; return; }
    if(d.active){ setTimeout(pollPing, 500); qs('p-result').textContent = `Laeuft... (${d.received}/${d.transmitted} Antworten)`; return; }
    if(d.done){
      qs('p-result').innerHTML = d.received>0
        ? `<b>${d.host} (${d.ip})</b>: ${d.received}/${d.transmitted} Antworten - min/avg/max = ${d.minMs}/${d.avgMs}/${d.maxMs} ms`
        : `<span style="color:var(--bad);">${d.host} (${d.ip}): keine Antwort erhalten.</span>`;
    }
  });
}
function dnsSubmit(){
  const host = qs('dns-host').value.trim();
  if(!host){ toast('Bitte Hostname angeben','warn'); return; }
  qs('dns-result').textContent = 'Aufloesung laeuft...';
  apiGet('/api/diag/dns?host='+encodeURIComponent(host), d=>{
    qs('dns-result').innerHTML = d.ok ? `<b>${host}</b> &rarr; ${d.ip}` : `<span style="color:var(--bad);">Aufloesung fehlgeschlagen.</span>`;
  });
}
function tcpSubmit(){
  const host = qs('tcp-host').value.trim();
  const port = qs('tcp-port').value;
  if(!host){ toast('Bitte Host angeben','warn'); return; }
  qs('tcp-result').textContent = 'Teste Verbindung...';
  apiGet('/api/diag/tcp?host='+encodeURIComponent(host)+'&port='+port, d=>{
    qs('tcp-result').innerHTML = d.ok ? `<span style="color:var(--good);">Verbindung erfolgreich (${d.ms} ms)</span>` : `<span style="color:var(--bad);">Verbindung fehlgeschlagen.</span>`;
  });
}

// ---- Beacon Manager: WLAN ----------------------------------------------------------
function showBeaconTab(t){
  currentBeaconTab = t;
  qs('tab-wb').style.display = t==='wb' ? 'block':'none';
  qs('tab-bb').style.display = t==='bb' ? 'block':'none';
  qs('tab-wb-btn').className = 'btn sm ' + (t==='wb'?'primary':'ghost');
  qs('tab-bb-btn').className = 'btn sm ' + (t==='bb'?'primary':'ghost');
}
let wbCache = {beacons:[]}, bbCache = {beacons:[]};
function loadWifiBeacons(){
  apiGet('/api/beacons/wifi', d=>{
    wbCache = d;
    qs('wb-cfg').textContent = d.count+' / '+d.max;
    qs('wb-active').textContent = (d.activeIndex>=0?1:0)+' / '+d.hwLimit;
    qs('wb-count-num').textContent = d.count;
    qs('wb-rot-sw').checked = d.rotationEnabled;
    qs('wb-rot-iv').value = d.rotationIntervalMs;
    const list = qs('wb-list');
    list.innerHTML = d.beacons.map(b=>`
      <div class="beacon-item">
        <div class="beacon-head">
          <b>${b.name} ${b.active?'<span class="badge good">Aktiv</span>':''}</b>
          <label class="switch"><input type="checkbox" ${b.enabled?'checked':''} onchange="wbUpdate(${b.index},this.checked)"><span class="slider-tog"></span></label>
        </div>
        <div class="formgrid">
          <div><label>Name</label><input type="text" value="${b.name}" maxlength="23" onchange="wbField(${b.index},'name',this.value)"></div>
          <div><label>SSID</label><input type="text" value="${b.ssid}" maxlength="32" onchange="wbField(${b.index},'ssid',this.value)"></div>
          <div><label>Passwort (leer=offen)</label><input type="password" placeholder="${b.hasPassword?'********':'(offen)'}" onchange="wbField(${b.index},'password',this.value)"></div>
          <div><label>Kanal</label><input type="number" min="1" max="13" value="${b.channel}" onchange="wbField(${b.index},'channel',this.value)"></div>
        </div>
        <div class="btnrow">
          <button class="btn sm" onclick="api('/api/beacons/wifi/start',{index:${b.index}},loadWifiBeacons)">Starten</button>
          <button class="btn sm ghost" onclick="api('/api/beacons/wifi/stop',{index:${b.index}},loadWifiBeacons)">Stoppen</button>
          <button class="btn sm danger" onclick="confirmAction('Beacon loeschen?','Profil \'${b.name}\' wirklich loeschen?',()=>api('/api/beacons/wifi/delete',{index:${b.index}},loadWifiBeacons))">Loeschen</button>
        </div>
      </div>`).join('') || '<p class="hint">Noch keine WLAN-Beacons angelegt. Anzahl oben erhoehen.</p>';
  });
}
function wbCount(delta){
  const n = Math.max(0, Math.min(wbCache.max||6, (wbCache.count||0)+delta));
  api('/api/beacons/wifi/count', {count:n}, loadWifiBeacons);
}
function wbUpdate(idx, enabled){ api('/api/beacons/wifi/update', {index:idx, enabled: enabled?1:0}, loadWifiBeacons); }
function wbField(idx, field, value){ const p={index:idx}; p[field]=value; api('/api/beacons/wifi/update', p, loadWifiBeacons); }
function wbRotToggle(){ api('/api/beacons/wifi/rotation', {enabled: qs('wb-rot-sw').checked?1:0, interval: qs('wb-rot-iv').value}, loadWifiBeacons); }

// ---- Beacon Manager: BLE ------------------------------------------------------------
function loadBleBeacons(){
  apiGet('/api/beacons/ble', d=>{
    bbCache = d;
    qs('bb-cfg').textContent = d.count+' / '+d.max;
    qs('bb-active').textContent = (d.activeIndex>=0?1:0)+' / '+d.hwLimit;
    qs('bb-count-num').textContent = d.count;
    qs('bb-rot-sw').checked = d.rotationEnabled;
    qs('bb-rot-iv').value = d.rotationIntervalMs;
    const list = qs('bb-list');
    list.innerHTML = d.beacons.map(b=>`
      <div class="beacon-item">
        <div class="beacon-head">
          <b>${b.name} ${b.active?'<span class="badge good">Aktiv</span>':''}</b>
          <label class="switch"><input type="checkbox" ${b.enabled?'checked':''} onchange="bbUpdate(${b.index},this.checked)"><span class="slider-tog"></span></label>
        </div>
        <div class="formgrid">
          <div><label>Name</label><input type="text" value="${b.name}" maxlength="23" onchange="bbField(${b.index},'name',this.value)"></div>
          <div><label>UUID</label><input type="text" value="${b.uuid}" maxlength="36" onchange="bbField(${b.index},'uuid',this.value)"></div>
          <div><label>Major</label><input type="number" min="0" max="65535" value="${b.major}" onchange="bbField(${b.index},'major',this.value)"></div>
          <div><label>Minor</label><input type="number" min="0" max="65535" value="${b.minor}" onchange="bbField(${b.index},'minor',this.value)"></div>
          <div><label>TX-Power (dBm)</label>
            <select onchange="bbField(${b.index},'txpower',this.value)">
              ${[-12,-9,-6,-3,0,3,6,9].map(v=>`<option value="${v}" ${v==b.txPower?'selected':''}>${v} dBm</option>`).join('')}
            </select>
          </div>
          <div><label>Advertising-Intervall (ms)</label><input type="number" min="100" max="2000" step="20" value="${b.intervalMs}" onchange="bbField(${b.index},'interval',this.value)"></div>
        </div>
        <div class="btnrow">
          <button class="btn sm" onclick="api('/api/beacons/ble/start',{index:${b.index}},loadBleBeacons)">Starten</button>
          <button class="btn sm ghost" onclick="api('/api/beacons/ble/stop',{index:${b.index}},loadBleBeacons)">Stoppen</button>
          <button class="btn sm ghost" onclick="api('/api/beacons/ble/duplicate',{index:${b.index}},loadBleBeacons)">Duplizieren</button>
          <button class="btn sm danger" onclick="confirmAction('Beacon loeschen?','Profil \'${b.name}\' wirklich loeschen?',()=>api('/api/beacons/ble/delete',{index:${b.index}},loadBleBeacons))">Loeschen</button>
        </div>
      </div>`).join('') || '<p class="hint">Noch keine BLE-Beacons angelegt. Anzahl oben erhoehen.</p>';
  });
}
function bbCount(delta){
  const n = Math.max(0, Math.min(bbCache.max||10, (bbCache.count||0)+delta));
  api('/api/beacons/ble/count', {count:n}, loadBleBeacons);
}
function bbUpdate(idx, enabled){ api('/api/beacons/ble/update', {index:idx, enabled: enabled?1:0}, loadBleBeacons); }
function bbField(idx, field, value){ const p={index:idx}; p[field]=value; api('/api/beacons/ble/update', p, loadBleBeacons); }
function bbRotToggle(){ api('/api/beacons/ble/rotation', {enabled: qs('bb-rot-sw').checked?1:0, interval: qs('bb-rot-iv').value}, loadBleBeacons); }

// ---- Logs -----------------------------------------------------------------------
function loadLogs(){
  apiGet('/api/logs?level='+qs('log-level').value, d=>{
    const body = qs('log-body');
    const cls = ['', 'badge info','badge warn','badge bad'];
    body.innerHTML = d.logs.map(l=>`<tr><td>${(l.t/1000).toFixed(1)}</td><td><span class="${cls[l.level]}">${l.levelName}</span></td><td>${l.msg}</td></tr>`).reverse().join('')
      || '<tr><td colspan="3" style="color:var(--muted);">Keine Eintraege.</td></tr>';
  });
}
function logLevelSet(){
  api('/api/logs/level', {level: qs('log-level').value}, loadLogs);
}
function downloadLogs(){
  apiGet('/api/logs?level=0', d=>{
    let txt = d.logs.map(l=>`[${(l.t/1000).toFixed(1)}s][${l.levelName}] ${l.msg}`).join('\n');
    const blob = new Blob([txt], {type:'text/plain'});
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'esp32-dashboard-log.txt';
    a.click();
  });
}

// ---- Settings ---------------------------------------------------------------------
function authSubmit(){
  const user = qs('set-user').value.trim();
  const pass = qs('set-pass').value;
  if(!user){ toast('Benutzername erforderlich','warn'); return; }
  api('/api/settings/auth', {user, pass}, ()=>toast('Gespeichert - ggf. erneut anmelden','good'));
}

// ---- Initiales Laden & Polling-Intervalle ------------------------------------------
pollStatus();
setInterval(pollStatus, 1500);
loadWifi();
)RAWJS";


// =====================================================================================
// [15] WEBSERVER & REST-API
// =====================================================================================

static bool restartPending = false;
static uint32_t restartAtMs = 0;
static uint8_t restartMode = 0; // 0=normal reboot, 1=wifi restart, 2=ble restart (kein reboot)

bool checkAuth() {
  if (!server.authenticate(authUser, authPass)) {
    server.requestAuthentication(BASIC_AUTH, "ESP32 Dashboard");
    return false;
  }
  return true;
}

void sendJsonOk(const char* msg = nullptr) {
  static char buf[160];
  JsonWriter j(buf, sizeof(buf));
  j.beginObj();
  j.boolean("ok", true);
  if (msg) j.str("message", msg);
  j.endObj();
  server.send(200, "application/json", j.c_str());
}

void sendJsonErr(const char* msg, int code = 400) {
  static char buf[192];
  JsonWriter j(buf, sizeof(buf));
  j.beginObj();
  j.boolean("ok", false);
  j.str("message", msg);
  j.endObj();
  server.send(code, "application/json", j.c_str());
}

// ---- Seite ---------------------------------------------------------------------------
void handleRoot() {
  if (!checkAuth()) return;
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  server.sendContent_P(PAGE_HTML_HEAD);
  server.sendContent_P(PAGE_JS);
  server.sendContent_P(PAGE_HTML_TAIL);
}

// ---- Status / System / WiFi -----------------------------------------------------------
void handleApiStatus() {
  if (!checkAuth()) return;
  static char buf[2200];
  JsonWriter j(buf, sizeof(buf));
  buildStatusJson(j);
  server.send(200, "application/json", j.c_str());
}

void handleApiSystem() {
  if (!checkAuth()) return;
  static char buf[1600];
  JsonWriter j(buf, sizeof(buf));
  buildSystemJson(j);
  server.send(200, "application/json", j.c_str());
}

void handleApiWifi() {
  if (!checkAuth()) return;
  static char buf[1024];
  JsonWriter j(buf, sizeof(buf));
  buildWifiJson(j);
  server.send(200, "application/json", j.c_str());
}

void handleApiWifiScanGet() {
  if (!checkAuth()) return;
  checkScanDone();
  static char buf[8192];
  JsonWriter j(buf, sizeof(buf));
  buildWifiScanJson(j);
  server.send(200, "application/json", j.c_str());
}

void handleApiWifiScanStart() {
  if (!checkAuth()) return;
  startWifiScan();
  sendJsonOk();
}

void handleApiWifiConnect() {
  if (!checkAuth()) return;
  if (!server.hasArg("ssid") || server.arg("ssid").length() == 0) { sendJsonErr("SSID darf nicht leer sein"); return; }
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  if (ssid.length() > 32 || pass.length() > 64) { sendJsonErr("SSID/Passwort zu lang"); return; }
  saveStaCredentials(ssid.c_str(), pass.c_str());
  wifiConnect(ssid.c_str(), pass.c_str());
  sendJsonOk("Verbindung wird aufgebaut");
}

void handleApiWifiDisconnect() {
  if (!checkAuth()) return;
  wifiDisconnect();
  sendJsonOk("WLAN getrennt");
}

void handleApiWifiReconnect() {
  if (!checkAuth()) return;
  wifiReconnectSaved();
  sendJsonOk("Verbindungsversuch gestartet");
}

void handleApiWifiForget() {
  if (!checkAuth()) return;
  wifiForget();
  sendJsonOk("Zugangsdaten geloescht");
}

// ---- Access Point ----------------------------------------------------------------------
void handleApiAp() {
  if (!checkAuth()) return;
  static char buf[2200];
  JsonWriter j(buf, sizeof(buf));
  buildApJson(j);
  server.send(200, "application/json", j.c_str());
}

void handleApiApConfig() {
  if (!checkAuth()) return;
  bool enable = server.arg("enable").toInt() == 1;
  String ssid = server.hasArg("ssid") ? server.arg("ssid") : String(apCfg.ssid);
  String pass = server.hasArg("pass") && server.arg("pass").length() > 0 ? server.arg("pass") : String(apCfg.password);
  uint8_t ch = server.hasArg("channel") ? constrain(server.arg("channel").toInt(), 1, 13) : apCfg.channel;
  uint8_t maxc = server.hasArg("maxc") ? constrain(server.arg("maxc").toInt(), 1, 8) : apCfg.maxConn;
  bool hidden = server.arg("hidden").toInt() == 1;
  if (enable && ssid.length() == 0) { sendJsonErr("SSID darf nicht leer sein"); return; }
  if (enable && pass.length() > 0 && pass.length() < 8) { sendJsonErr("Passwort muss mindestens 8 Zeichen haben (oder leer fuer offenes Netz)"); return; }
  bool ok = applyApConfig(ssid.c_str(), pass.c_str(), ch, hidden, maxc, enable);
  saveApConfigPersist();
  if (ok) sendJsonOk(enable ? "Access Point aktiviert" : "Access Point deaktiviert");
  else sendJsonErr("Access Point konnte nicht konfiguriert werden");
}

void handleApiMode() {
  if (!checkAuth()) return;
  uint8_t m = constrain(server.arg("mode").toInt(), 0, 2);
  setWifiModeSafe(m);
  sendJsonOk();
}

// ---- BLE Core ---------------------------------------------------------------------------
void handleApiBle() {
  if (!checkAuth()) return;
  static char buf[8192];
  JsonWriter j(buf, sizeof(buf));
  buildBleJson(j);
  server.send(200, "application/json", j.c_str());
}
void handleApiBleEnable()  { if (!checkAuth()) return; bleEnable();  sendJsonOk("BLE aktiviert"); }
void handleApiBleDisable() { if (!checkAuth()) return; bleDisable(); sendJsonOk("BLE deaktiviert"); }
void handleApiBleScanStart() {
  if (!checkAuth()) return;
  uint32_t dur = server.hasArg("duration") ? server.arg("duration").toInt() : 8;
  bleStartScan(dur);
  sendJsonOk();
}
void handleApiBleScanStop() { if (!checkAuth()) return; bleStopScan(); sendJsonOk(); }
void handleApiBleClear()    { if (!checkAuth()) return; bleClearDevices(); sendJsonOk(); }

// ---- WLAN-Beacon-Manager -----------------------------------------------------------------
void handleApiBeaconsWifiGet() {
  if (!checkAuth()) return;
  static char buf[2560];
  JsonWriter j(buf, sizeof(buf));
  buildWifiBeaconsJson(j);
  server.send(200, "application/json", j.c_str());
}
void handleApiBeaconsWifiCount() {
  if (!checkAuth()) return;
  wbSetCount(constrain(server.arg("count").toInt(), 0, MAX_WIFI_BEACONS));
  sendJsonOk();
}
void handleApiBeaconsWifiUpdate() {
  if (!checkAuth()) return;
  int idx = server.arg("index").toInt();
  if (idx < 0 || idx >= wifiBeaconCount) { sendJsonErr("Ungueltiger Index"); return; }
  WifiBeacon &b = wifiBeacons[idx];
  if (server.hasArg("name")) {
    String v = server.arg("name"); if (v.length() > 23) v = v.substring(0, 23);
    strlcpy(b.name, v.c_str(), sizeof(b.name));
  }
  if (server.hasArg("ssid")) {
    String v = server.arg("ssid");
    if (v.length() == 0 || v.length() > 32) { sendJsonErr("SSID muss 1-32 Zeichen lang sein"); return; }
    strlcpy(b.ssid, v.c_str(), sizeof(b.ssid));
  }
  if (server.hasArg("password")) {
    String v = server.arg("password");
    if (v.length() > 0 && v.length() < 8) { sendJsonErr("Passwort muss leer oder >= 8 Zeichen sein"); return; }
    if (v.length() > 64) v = v.substring(0, 64);
    strlcpy(b.password, v.c_str(), sizeof(b.password));
  }
  if (server.hasArg("channel")) b.channel = constrain(server.arg("channel").toInt(), 1, 13);
  if (server.hasArg("hidden")) b.hidden = server.arg("hidden").toInt() == 1;
  if (server.hasArg("enabled")) b.enabled = server.arg("enabled").toInt() == 1;
  sendJsonOk();
}
void handleApiBeaconsWifiDelete() {
  if (!checkAuth()) return;
  int idx = server.arg("index").toInt();
  if (idx < 0 || idx >= wifiBeaconCount) { sendJsonErr("Ungueltiger Index"); return; }
  if (wifiBeaconActive == idx) wbStopActive();
  for (int i = idx; i < wifiBeaconCount - 1; i++) wifiBeacons[i] = wifiBeacons[i + 1];
  wifiBeaconCount--;
  if (wifiBeaconActive > idx) wifiBeaconActive--;
  sendJsonOk("Beacon geloescht");
}
void handleApiBeaconsWifiStart() { if (!checkAuth()) return; int idx = server.arg("index").toInt(); wbApplyIndex(idx) ? sendJsonOk("Beacon gestartet") : sendJsonErr("Start fehlgeschlagen"); }
void handleApiBeaconsWifiStop()  { if (!checkAuth()) return; wbStopActive(); sendJsonOk(); }
void handleApiBeaconsWifiStartAll() { if (!checkAuth()) return; wbStartAll(); sendJsonOk(); }
void handleApiBeaconsWifiStopAll()  { if (!checkAuth()) return; wbStopAll();  sendJsonOk(); }
void handleApiBeaconsWifiSave() { if (!checkAuth()) return; saveWifiBeacons(); sendJsonOk("Gespeichert"); }
void handleApiBeaconsWifiRotation() {
  if (!checkAuth()) return;
  wifiRotationEnabled = server.arg("enabled").toInt() == 1;
  if (server.hasArg("interval")) wifiRotationIntervalMs = constrain(server.arg("interval").toInt(), 1000, 60000);
  wifiRotationLastSwitch = millis();
  sendJsonOk();
}

// ---- BLE-Beacon-Manager -------------------------------------------------------------------
void handleApiBeaconsBleGet() {
  if (!checkAuth()) return;
  static char buf[3584];
  JsonWriter j(buf, sizeof(buf));
  buildBleBeaconsJson(j);
  server.send(200, "application/json", j.c_str());
}
void handleApiBeaconsBleCount() {
  if (!checkAuth()) return;
  bbSetCount(constrain(server.arg("count").toInt(), 0, MAX_BLE_BEACONS));
  sendJsonOk();
}
bool validUuidChars(const String &s) {
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (!isxdigit((unsigned char)c) && c != '-') return false;
  }
  return true;
}
void handleApiBeaconsBleUpdate() {
  if (!checkAuth()) return;
  int idx = server.arg("index").toInt();
  if (idx < 0 || idx >= bleBeaconCount) { sendJsonErr("Ungueltiger Index"); return; }
  BleBeaconCfg &b = bleBeacons[idx];
  if (server.hasArg("name")) {
    String v = server.arg("name"); if (v.length() > 23) v = v.substring(0, 23);
    strlcpy(b.name, v.c_str(), sizeof(b.name));
  }
  if (server.hasArg("uuid")) {
    String v = server.arg("uuid");
    if (v.length() != 36 || !validUuidChars(v)) { sendJsonErr("UUID muss im Format xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx sein"); return; }
    strlcpy(b.uuid, v.c_str(), sizeof(b.uuid));
  }
  if (server.hasArg("major")) b.major = constrain(server.arg("major").toInt(), 0, 65535);
  if (server.hasArg("minor")) b.minor = constrain(server.arg("minor").toInt(), 0, 65535);
  if (server.hasArg("txpower")) b.txPower = (int8_t)constrain(server.arg("txpower").toInt(), -30, 20);
  if (server.hasArg("interval")) b.advIntervalMs = constrain(server.arg("interval").toInt(), 100, 5000);
  if (server.hasArg("enabled")) b.enabled = server.arg("enabled").toInt() == 1;
  sendJsonOk();
}
void handleApiBeaconsBleDelete() {
  if (!checkAuth()) return;
  int idx = server.arg("index").toInt();
  if (idx < 0 || idx >= bleBeaconCount) { sendJsonErr("Ungueltiger Index"); return; }
  if (bleBeaconActive == idx) bbStopActive();
  for (int i = idx; i < bleBeaconCount - 1; i++) bleBeacons[i] = bleBeacons[i + 1];
  bleBeaconCount--;
  if (bleBeaconActive > idx) bleBeaconActive--;
  sendJsonOk("Beacon geloescht");
}
void handleApiBeaconsBleDuplicate() {
  if (!checkAuth()) return;
  int idx = server.arg("index").toInt();
  if (idx < 0 || idx >= bleBeaconCount) { sendJsonErr("Ungueltiger Index"); return; }
  if (bleBeaconCount >= MAX_BLE_BEACONS) { sendJsonErr("Maximale Anzahl erreicht"); return; }
  BleBeaconCfg copy = bleBeacons[idx];
  snprintf(copy.name, sizeof(copy.name), "%.16s Kopie", bleBeacons[idx].name);
  copy.enabled = false;
  bleBeacons[bleBeaconCount++] = copy;
  sendJsonOk("Dupliziert");
}
void handleApiBeaconsBleStart() { if (!checkAuth()) return; int idx = server.arg("index").toInt(); bbApplyIndex(idx) ? sendJsonOk("Beacon gestartet") : sendJsonErr(bleBeaconLastError[0] ? bleBeaconLastError : "Start fehlgeschlagen"); }
void handleApiBeaconsBleStop()  { if (!checkAuth()) return; bbStopActive(); sendJsonOk(); }
void handleApiBeaconsBleStartAll() { if (!checkAuth()) return; bbStartAll(); sendJsonOk(); }
void handleApiBeaconsBleStopAll()  { if (!checkAuth()) return; bbStopAll();  sendJsonOk(); }
void handleApiBeaconsBleSave() { if (!checkAuth()) return; saveBleBeacons(); sendJsonOk("Gespeichert"); }
void handleApiBeaconsBleRotation() {
  if (!checkAuth()) return;
  bleRotationEnabled = server.arg("enabled").toInt() == 1;
  if (server.hasArg("interval")) bleRotationIntervalMs = constrain(server.arg("interval").toInt(), 500, 60000);
  bleRotationLastSwitch = millis();
  sendJsonOk();
}

// ---- Netzwerk-Diagnose --------------------------------------------------------------------
void handleApiNetwork() {
  if (!checkAuth()) return;
  static char buf[1024];
  JsonWriter j(buf, sizeof(buf));
  buildNetworkJson(j);
  server.send(200, "application/json", j.c_str());
}

void handleApiDiagGwTest() {
  if (!checkAuth()) return;
  static char buf[192];
  JsonWriter j(buf, sizeof(buf));
  j.beginObj();
  if (WiFi.status() != WL_CONNECTED) { j.boolean("ok", false); j.str("message", "Nicht mit WLAN verbunden"); j.endObj(); server.send(200, "application/json", j.c_str()); return; }
  uint32_t ms = 0;
  bool ok = tcpCheck(WiFi.gatewayIP().toString().c_str(), 80, 1000, &ms);
  if (!ok) ok = tcpCheck(WiFi.gatewayIP().toString().c_str(), 443, 800, &ms);
  j.boolean("ok", ok);
  j.numu("ms", ms);
  j.endObj();
  server.send(200, "application/json", j.c_str());
}

void handleApiDiagInetTest() {
  if (!checkAuth()) return;
  static char buf[192];
  JsonWriter j(buf, sizeof(buf));
  j.beginObj();
  if (WiFi.status() != WL_CONNECTED) { j.boolean("ok", false); j.endObj(); server.send(200, "application/json", j.c_str()); return; }
  uint32_t ms = 0;
  bool ok = tcpCheck("8.8.8.8", 53, 1200, &ms);
  j.boolean("ok", ok);
  j.numu("ms", ms);
  j.endObj();
  server.send(200, "application/json", j.c_str());
}

void handleApiDiagPingStart() {
  if (!checkAuth()) return;
  if (!server.hasArg("host") || server.arg("host").length() == 0) { sendJsonErr("Host erforderlich"); return; }
  pingStart(server.arg("host").c_str());
  sendJsonOk();
}
void handleApiDiagPingStatus() {
  if (!checkAuth()) return;
  static char buf[384];
  JsonWriter j(buf, sizeof(buf));
  buildPingJson(j);
  server.send(200, "application/json", j.c_str());
}
void handleApiDiagDns() {
  if (!checkAuth()) return;
  static char buf[192];
  JsonWriter j(buf, sizeof(buf));
  j.beginObj();
  if (!server.hasArg("host")) { j.boolean("ok", false); j.endObj(); server.send(200, "application/json", j.c_str()); return; }
  IPAddress ip;
  bool ok = dnsResolve(server.arg("host").c_str(), ip);
  j.boolean("ok", ok);
  if (ok) j.str("ip", ip.toString());
  j.endObj();
  server.send(200, "application/json", j.c_str());
}
void handleApiDiagTcp() {
  if (!checkAuth()) return;
  static char buf[192];
  JsonWriter j(buf, sizeof(buf));
  j.beginObj();
  if (!server.hasArg("host") || !server.hasArg("port")) { j.boolean("ok", false); j.endObj(); server.send(200, "application/json", j.c_str()); return; }
  uint32_t ms = 0;
  bool ok = tcpCheck(server.arg("host").c_str(), server.arg("port").toInt(), 2000, &ms);
  j.boolean("ok", ok);
  j.numu("ms", ms);
  j.endObj();
  server.send(200, "application/json", j.c_str());
}

// ---- Logs ----------------------------------------------------------------------------------
void handleApiLogsGet() {
  if (!checkAuth()) return;
  uint8_t minLvl = server.hasArg("level") ? server.arg("level").toInt() : 0;
  static char buf[9000];
  JsonWriter j(buf, sizeof(buf));
  j.beginObj();
  j.beginArr("logs");
  uint16_t start = (logCount < MAX_LOG_ENTRIES) ? 0 : logHead;
  for (uint16_t i = 0; i < logCount; i++) {
    uint16_t idx = (start + i) % MAX_LOG_ENTRIES;
    if (logBuf[idx].level < minLvl) continue;
    j.comma(); j.beginObj();
    j.numu("t", logBuf[idx].t);
    j.num("level", logBuf[idx].level);
    j.str("levelName", levelName(logBuf[idx].level));
    j.str("msg", logBuf[idx].msg);
    j.endObj();
  }
  j.endArr();
  j.endObj();
  server.send(200, "application/json", j.c_str());
}
void handleApiLogsClear() { if (!checkAuth()) return; clearLogs(); sendJsonOk("Logs geleert"); }
void handleApiLogsLevel() {
  if (!checkAuth()) return;
  saveLogLevel(constrain(server.arg("level").toInt(), 0, 3));
  sendJsonOk();
}

// ---- Tools -----------------------------------------------------------------------------------
void handleApiToolsRestart() {
  if (!checkAuth()) return;
  sendJsonOk("ESP32 wird neu gestartet...");
  restartPending = true; restartAtMs = millis() + 600; restartMode = 0;
}
void handleApiToolsWifiRestart() {
  if (!checkAuth()) return;
  sendJsonOk("WLAN wird neu gestartet...");
  restartPending = true; restartAtMs = millis() + 300; restartMode = 1;
}
void handleApiToolsBleRestart() {
  if (!checkAuth()) return;
  sendJsonOk("BLE wird neu gestartet...");
  restartPending = true; restartAtMs = millis() + 300; restartMode = 2;
}
void handleApiToolsFactoryReset() {
  if (!checkAuth()) return;
  factoryReset();
  sendJsonOk("Factory-Reset durchgefuehrt. Neustart...");
  restartPending = true; restartAtMs = millis() + 800; restartMode = 0;
}

// ---- Settings ---------------------------------------------------------------------------------
void handleApiSettingsAuth() {
  if (!checkAuth()) return;
  String user = server.arg("user");
  String pass = server.arg("pass");
  if (user.length() == 0 || user.length() > 23) { sendJsonErr("Ungueltiger Benutzername"); return; }
  if (pass.length() == 0) pass = String(authPass); // Passwort unveraendert lassen
  if (pass.length() > 31) { sendJsonErr("Passwort zu lang"); return; }
  saveDashAuth(user.c_str(), pass.c_str());
  sendJsonOk("Zugangsdaten aktualisiert");
}

void handleNotFound() {
  sendJsonErr("Nicht gefunden", 404);
}

void setupRoutes() {
  server.on("/", HTTP_GET, handleRoot);

  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/system", HTTP_GET, handleApiSystem);

  server.on("/api/wifi", HTTP_GET, handleApiWifi);
  server.on("/api/wifi/scan", HTTP_GET, handleApiWifiScanGet);
  server.on("/api/wifi/scan/start", HTTP_POST, handleApiWifiScanStart);
  server.on("/api/wifi/connect", HTTP_POST, handleApiWifiConnect);
  server.on("/api/wifi/disconnect", HTTP_POST, handleApiWifiDisconnect);
  server.on("/api/wifi/reconnect", HTTP_POST, handleApiWifiReconnect);
  server.on("/api/wifi/forget", HTTP_POST, handleApiWifiForget);

  server.on("/api/ap", HTTP_GET, handleApiAp);
  server.on("/api/ap/config", HTTP_POST, handleApiApConfig);
  server.on("/api/mode", HTTP_POST, handleApiMode);

  server.on("/api/ble", HTTP_GET, handleApiBle);
  server.on("/api/ble/enable", HTTP_POST, handleApiBleEnable);
  server.on("/api/ble/disable", HTTP_POST, handleApiBleDisable);
  server.on("/api/ble/scan/start", HTTP_POST, handleApiBleScanStart);
  server.on("/api/ble/scan/stop", HTTP_POST, handleApiBleScanStop);
  server.on("/api/ble/clear", HTTP_POST, handleApiBleClear);

  server.on("/api/beacons/wifi", HTTP_GET, handleApiBeaconsWifiGet);
  server.on("/api/beacons/wifi/count", HTTP_POST, handleApiBeaconsWifiCount);
  server.on("/api/beacons/wifi/update", HTTP_POST, handleApiBeaconsWifiUpdate);
  server.on("/api/beacons/wifi/delete", HTTP_POST, handleApiBeaconsWifiDelete);
  server.on("/api/beacons/wifi/start", HTTP_POST, handleApiBeaconsWifiStart);
  server.on("/api/beacons/wifi/stop", HTTP_POST, handleApiBeaconsWifiStop);
  server.on("/api/beacons/wifi/startall", HTTP_POST, handleApiBeaconsWifiStartAll);
  server.on("/api/beacons/wifi/stopall", HTTP_POST, handleApiBeaconsWifiStopAll);
  server.on("/api/beacons/wifi/save", HTTP_POST, handleApiBeaconsWifiSave);
  server.on("/api/beacons/wifi/rotation", HTTP_POST, handleApiBeaconsWifiRotation);

  server.on("/api/beacons/ble", HTTP_GET, handleApiBeaconsBleGet);
  server.on("/api/beacons/ble/count", HTTP_POST, handleApiBeaconsBleCount);
  server.on("/api/beacons/ble/update", HTTP_POST, handleApiBeaconsBleUpdate);
  server.on("/api/beacons/ble/delete", HTTP_POST, handleApiBeaconsBleDelete);
  server.on("/api/beacons/ble/duplicate", HTTP_POST, handleApiBeaconsBleDuplicate);
  server.on("/api/beacons/ble/start", HTTP_POST, handleApiBeaconsBleStart);
  server.on("/api/beacons/ble/stop", HTTP_POST, handleApiBeaconsBleStop);
  server.on("/api/beacons/ble/startall", HTTP_POST, handleApiBeaconsBleStartAll);
  server.on("/api/beacons/ble/stopall", HTTP_POST, handleApiBeaconsBleStopAll);
  server.on("/api/beacons/ble/save", HTTP_POST, handleApiBeaconsBleSave);
  server.on("/api/beacons/ble/rotation", HTTP_POST, handleApiBeaconsBleRotation);

  server.on("/api/network", HTTP_GET, handleApiNetwork);
  server.on("/api/diag/gwtest", HTTP_POST, handleApiDiagGwTest);
  server.on("/api/diag/inettest", HTTP_POST, handleApiDiagInetTest);
  server.on("/api/diag/ping/start", HTTP_POST, handleApiDiagPingStart);
  server.on("/api/diag/ping/status", HTTP_GET, handleApiDiagPingStatus);
  server.on("/api/diag/dns", HTTP_GET, handleApiDiagDns);
  server.on("/api/diag/tcp", HTTP_GET, handleApiDiagTcp);

  server.on("/api/logs", HTTP_GET, handleApiLogsGet);
  server.on("/api/logs/clear", HTTP_POST, handleApiLogsClear);
  server.on("/api/logs/level", HTTP_POST, handleApiLogsLevel);

  server.on("/api/tools/restart", HTTP_POST, handleApiToolsRestart);
  server.on("/api/tools/wifirestart", HTTP_POST, handleApiToolsWifiRestart);
  server.on("/api/tools/blerestart", HTTP_POST, handleApiToolsBleRestart);
  server.on("/api/tools/factoryreset", HTTP_POST, handleApiToolsFactoryReset);

  server.on("/api/settings/auth", HTTP_POST, handleApiSettingsAuth);

  server.onNotFound(handleNotFound);
}


// =====================================================================================
// [16] SETUP & LOOP
// =====================================================================================

void setup() {
  Serial.begin(115200);
  delay(200); // kurze Pause, damit der serielle Monitor rechtzeitig verbindet
  bootTime = millis();

  logMsg(LOG_INFO, "=== %s v%s startet ===", APP_NAME, APP_VERSION);
  logMsg(LOG_INFO, "Reset-Grund: %s", resetReasonStr(esp_reset_reason()));

  loadDashSettings();
  loadNetConfig();
  loadWifiBeacons();
  loadBleBeacons();

  WiFi.onEvent(onWifiEvent);
  WiFi.setHostname("esp32-dashboard");

  // WLAN-Modus gemaess gespeicherter Konfiguration herstellen (sicher, ohne Aussperren)
  if (wifiModeSel == 1) {
    WiFi.mode(WIFI_MODE_AP);
  } else {
    WiFi.mode(WIFI_MODE_APSTA); // Default: AP+STA, damit das Dashboard immer erreichbar bleibt
  }

  if (apCfg.enabled || wifiModeSel != 0) {
    applyApConfig(apCfg.ssid, apCfg.password, apCfg.channel, apCfg.hidden, apCfg.maxConn, true);
  }

  if (staAutoConnect && strlen(staSavedSsid) > 0) {
    wifiConnect(staSavedSsid, staSavedPass);
  }

  setupRoutes();
  server.begin();
  logMsg(LOG_INFO, "Webserver gestartet auf Port 80");

  for (int i = 0; i < HISTORY_LEN; i++) { heapHistory[i] = ESP.getFreeHeap(); rssiHistory[i] = -100; wifiCountHistory[i] = 0; bleCountHistory[i] = 0; }

  logMsg(LOG_INFO, "Setup abgeschlossen");
}

void loop() {
  server.handleClient();

  uint32_t now = millis();

  // WLAN-Scan-Status nicht-blockierend pruefen
  checkScanDone();

  // WLAN-Verbindungsversuch mit Timeout ueberwachen (verhindert ewiges Haengen)
  if (staConnectInProgress && (now - staLastAttempt > 15000)) {
    staConnectInProgress = false;
    lastWifiError = "Zeitueberschreitung beim Verbindungsaufbau";
    logMsg(LOG_WARN, "WLAN-Verbindungsversuch abgebrochen (Timeout)");
  }

  // Beacon-Rotation (nicht-blockierend)
  wbLoopTick();
  bbLoopTick();

  // CPU-Last alle 2 Sekunden messen
  if (now - lastCpuSample >= 2000) {
    lastCpuSample = now;
    sampleCpuUsage();
  }

  // Verlaufsdaten (Diagramme) einmal pro Sekunde aktualisieren
  if (now - lastHistoryTick >= 1000) {
    lastHistoryTick = now;
    updateHistory();
  }

  // Ausstehenden Neustart/Teilneustart ausfuehren (nach Antwortversand an den Client)
  if (restartPending && (int32_t)(now - restartAtMs) >= 0) {
    restartPending = false;
    if (restartMode == 0) {
      logMsg(LOG_INFO, "Neustart wird ausgefuehrt...");
      delay(50);
      ESP.restart();
    } else if (restartMode == 1) {
      logMsg(LOG_INFO, "WLAN-Stack wird neu initialisiert...");
      bool wasApEnabled = apCfg.enabled;
      WiFi.disconnect(true, false);
      delay(100);
      WiFi.mode(wifiModeSel == 1 ? WIFI_MODE_AP : WIFI_MODE_APSTA);
      if (wasApEnabled) applyApConfig(apCfg.ssid, apCfg.password, apCfg.channel, apCfg.hidden, apCfg.maxConn, true);
      if (staAutoConnect && strlen(staSavedSsid) > 0) wifiConnect(staSavedSsid, staSavedPass);
      logMsg(LOG_INFO, "WLAN-Stack neu gestartet");
    } else if (restartMode == 2) {
      logMsg(LOG_INFO, "BLE-Stack wird neu initialisiert...");
      bool wasOn = bleInitialized;
      bleDisable();
      delay(100);
      if (wasOn) bleEnable();
      logMsg(LOG_INFO, "BLE-Stack neu gestartet");
    }
  }
}
