#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <Arduino_GFX_Library.h>
#include <TAMC_GT911.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <math.h>

// =========================================================================
// CONFIGURATION & PERSISTENT NVS STORAGE (Preferences)
// =========================================================================
Preferences prefs;
String wifi_ssid     = "Kuitunen";
String wifi_psk      = "12345678";
String bridge_ip     = "192.168.1.159";
uint16_t bridge_port = 2222;

void loadConfig() {
    prefs.begin("swr_disp", false);
    wifi_ssid   = prefs.getString("ssid", "Kuitunen");
    wifi_psk    = prefs.getString("psk", "12345678");
    bridge_ip   = prefs.getString("b_ip", "192.168.1.159");
    bridge_port = prefs.getUShort("b_port", 2222);
    prefs.end();

    if (wifi_ssid.length() == 0) wifi_ssid = "Kuitunen";
    if (wifi_psk.length() == 0) wifi_psk = "12345678";
    if (bridge_ip.length() < 7) bridge_ip = "192.168.1.159";
    if (bridge_port == 0) bridge_port = 2222;

    Serial.printf("[CONFIG] Loaded: SSID='%s', Bridge=%s:%u\n", wifi_ssid.c_str(), bridge_ip.c_str(), bridge_port);
}

void saveConfig(const String& new_ssid, const String& new_psk, const String& new_ip, uint16_t new_port) {
    prefs.begin("swr_disp", false);
    prefs.putString("ssid", new_ssid);
    prefs.putString("psk", new_psk);
    prefs.putString("b_ip", new_ip);
    prefs.putUShort("b_port", new_port);
    prefs.end();
    Serial.printf("[CONFIG] Saved: SSID='%s', Bridge=%s:%u\n", new_ssid.c_str(), new_ip.c_str(), new_port);
}

// Global display status string shown in the top header
char dbgStatus[64] = "CONNECTING...";

// Web Server on Port 80
WebServer otaServer(80);

// Captive Portal DNS Server
DNSServer dnsServer;
const byte DNS_PORT = 53;
volatile bool apModeActive = false;
volatile bool serverRoutesConfigured = false;

// =========================================================================
// STANDBY TIMEOUT (15 MINUTES INACTIVITY -> SCREEN OFF, TX / TOUCH WAKES UP)
// =========================================================================
const unsigned long STANDBY_TIMEOUT_MS = 15UL * 60UL * 1000UL; // 15 min = 900,000 ms
unsigned long lastActivityTime = 0;
bool isBacklightOn = true;

// Clean Dark Mode Web UI for Config & OTA Updates
const char WEB_INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fi">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>SWR N&auml;ytt&ouml; - Hallintapaneeli</title>
<style>
* { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; }
body { background: #080c14; color: #fff; display: flex; justify-content: center; align-items: center; min-height: 100vh; padding: 1rem; }
.card { background: #121824; border: 1px solid #202838; border-radius: 12px; padding: 1.8rem; max-width: 460px; width: 100%; box-shadow: 0 10px 30px rgba(0,0,0,0.6); }
h1 { font-size: 1.35rem; color: #00e5ff; margin-bottom: 0.3rem; font-weight: 700; text-align: center; }
p.sub { font-size: 0.85rem; color: #9aaabb; margin-bottom: 1.2rem; text-align: center; }
.info-box { background: #080c14; border: 1px solid #1a2334; border-radius: 8px; padding: 0.8rem; margin-bottom: 1.2rem; font-size: 0.82rem; line-height: 1.7; }
.info-row { display: flex; justify-content: space-between; }
.info-label { color: #62728d; }
.val-online { color: #00ff88; font-weight: 600; }
.val-offline { color: #ffaa00; font-weight: 600; }
.sec-title { font-size: 0.92rem; color: #00e5ff; font-weight: 700; margin: 1.2rem 0 0.6rem 0; border-bottom: 1px solid #202838; padding-bottom: 0.3rem; }
label { display: block; font-size: 0.82rem; color: #9aaabb; margin-bottom: 0.3rem; font-weight: 500; }
input[type=text], input[type=number], input[type=password] { width: 100%; background: #0e1420; border: 1px solid #243048; padding: 0.65rem 0.8rem; border-radius: 8px; color: #fff; font-size: 0.95rem; margin-bottom: 0.8rem; outline: none; }
input:focus { border-color: #00e5ff; }
.row-2 { display: flex; gap: 0.8rem; }
.row-2 > div { flex: 1; }
button.btn-save { background: #00e5ff; color: #080c14; border: none; font-weight: 700; padding: 0.8rem; border-radius: 8px; cursor: pointer; width: 100%; font-size: 0.95rem; transition: background 0.2s; margin-top: 0.4rem; }
button.btn-save:hover { background: #00ff88; }
button.btn-ota { background: #182232; color: #00e5ff; border: 1px solid #00e5ff; font-weight: 700; padding: 0.75rem; border-radius: 8px; cursor: pointer; width: 100%; font-size: 0.9rem; transition: all 0.2s; margin-top: 0.6rem; }
button.btn-ota:hover { background: #00e5ff; color: #080c14; }
button:disabled { background: #2a3548; color: #62728d; border-color: #2a3548; cursor: not-allowed; }
input[type=file] { display: block; width: 100%; background: #0e1420; border: 1px solid #243048; padding: 0.6rem; border-radius: 8px; color: #fff; font-size: 0.85rem; cursor: pointer; }
#progress-wrap { display: none; margin-top: 1rem; }
.bar-bg { background: #080c14; border: 1px solid #243048; border-radius: 6px; height: 14px; overflow: hidden; }
.bar-fill { background: linear-gradient(90deg, #00e5ff, #00ff88); width: 0%; height: 100%; transition: width 0.1s; }
#status-txt { margin-top: 0.5rem; font-size: 0.82rem; color: #00e5ff; text-align: center; font-weight: 600; }
</style>
</head>
<body>
<div class="card">
  <h1>ESP32-S3 SWR N&auml;ytt&ouml;</h1>
  <p class="sub">Asetukset &amp; Ohjelmistop&auml;ivitys</p>
  <div class="info-box">
    <div class="info-row"><span class="info-label">Wi-Fi Tila:</span><span>%WIFI_STATUS%</span></div>
    <div class="info-row"><span class="info-label">SWR Silta:</span><span>%BRIDGE_STATUS%</span></div>
    <div class="info-row"><span class="info-label">Standby-aika:</span><span>15 min automaatti (TX her&auml;tt&auml;&auml;)</span></div>
  </div>
  <form action="/save" method="POST">
    <div class="sec-title">&#128225; SWR Silta (Bridge Kohde)</div>
    <div class="row-2">
      <div style="flex: 2;">
        <label>Silta IP-osoite</label>
        <input type="text" name="bridge_ip" value="%BRIDGE_IP%" required>
      </div>
      <div style="flex: 1;">
        <label>Portti</label>
        <input type="number" name="bridge_port" value="%BRIDGE_PORT%" required>
      </div>
    </div>
    <div class="sec-title">&#128246; Wi-Fi Verkko</div>
    <label>Wi-Fi Verkon Nimi (SSID)</label>
    <input type="text" name="wifi_ssid" value="%WIFI_SSID%" required>
    <label>Wi-Fi Salasana</label>
    <input type="password" name="wifi_psk" value="%WIFI_PSK%" placeholder="Salasana...">
    <button type="submit" class="btn-save">&#128190; Tallenna ja k&auml;ynnist&auml; uudelleen</button>
  </form>
  <div class="sec-title" style="margin-top: 1.5rem;">&#128640; Langaton OTA P&auml;ivitys</div>
  <form id="upload-form">
    <label>Valitse uusi .bin ohjelmistotiedosto</label>
    <input type="file" id="file" name="update" accept=".bin" required>
    <button type="submit" id="btn" class="btn-ota">🚀 Asenna p&auml;ivitys</button>
  </form>
  <div id="progress-wrap">
    <div class="bar-bg"><div class="bar-fill" id="bar"></div></div>
    <div id="status-txt">Ladataan: 0%</div>
  </div>
</div>
<script>
document.getElementById('upload-form').onsubmit = function(e) {
  e.preventDefault();
  var fileInput = document.getElementById('file');
  if (!fileInput.files.length) return;
  var file = fileInput.files[0];
  var btn = document.getElementById('btn');
  var pWrap = document.getElementById('progress-wrap');
  var bar = document.getElementById('bar');
  var txt = document.getElementById('status-txt');

  btn.disabled = true;
  pWrap.style.display = 'block';

  var xhr = new XMLHttpRequest();
  xhr.open('POST', '/update', true);

  xhr.upload.onprogress = function(pe) {
    if (pe.lengthComputable) {
      var pct = Math.round((pe.loaded / pe.total) * 100);
      bar.style.width = pct + '%';
      txt.textContent = 'Siirretään: ' + pct + '%...';
    }
  };

  xhr.onload = function() {
    if (xhr.status === 200) {
      bar.style.width = '100%';
      txt.innerHTML = '&#9989; Päivitys onnistui! Näyttö käynnistyy uudelleen...';
      setTimeout(function() { location.href = '/'; }, 6000);
    } else {
      txt.innerHTML = '&#10060; Virhe: ' + xhr.responseText;
      btn.disabled = false;
    }
  };

  xhr.onerror = function() {
    txt.innerHTML = '&#10060; Yhteysvirhe siirron aikana.';
    btn.disabled = false;
  };

  var formData = new FormData();
  formData.append('update', file);
  xhr.send(formData);
};
</script>
</body>
</html>
)rawliteral";

// =========================================================================
// HARDWARE SETTINGS (Guition JC4827W543C / ESP32-4827S043 4.3" IPS)
// =========================================================================
#define GFX_BL 1

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    45 /* cs */, 47 /* sck */, 21 /* d0 */, 48 /* d1 */, 40 /* d2 */, 39 /* d3 */
);

Arduino_GFX *gfx = new Arduino_NV3041A(
    bus, GFX_NOT_DEFINED /* RST */, 0 /* rotation */, true /* IPS */
);

// PSRAM Full Frame Canvas (480x272) for 100% Flicker-Free Smooth Rendering!
Arduino_Canvas *canvas = new Arduino_Canvas(480, 272, gfx);

// Capacitive Touch Controller GT911
#define TOUCH_SDA 8
#define TOUCH_SCL 4
#define TOUCH_INT 3
#define TOUCH_RST 38
#define TOUCH_WIDTH 480
#define TOUCH_HEIGHT 272

TAMC_GT911 tp = TAMC_GT911(TOUCH_SDA, TOUCH_SCL, TOUCH_INT, TOUCH_RST, TOUCH_WIDTH, TOUCH_HEIGHT);

// Touch coordinate corrector (Inverts 180° rotation)
void getCorrectedTouch(int rawX, int rawY, int &tx, int &ty) {
    tx = 479 - rawX;
    ty = 271 - rawY;
    if (tx < 0) tx = 0;
    if (tx > 479) tx = 479;
    if (ty < 0) ty = 0;
    if (ty > 271) ty = 271;
}

// =========================================================================
// RGB565 COLOR PALETTE
// =========================================================================
#define C_BLACK        0x0000
#define C_WHITE        0xFFFF
#define C_DARK_BG      0x0842  // #080c14
#define C_CARD_BG      0x10C6  // #121824
#define C_CARD_BORDER  0x2128  // #202838
#define C_HEADER       0x08A4  // #090d14
#define C_CYAN         0x07FF  // #00e5ff
#define C_NEON_GREEN   0x07EC  // #00ff88
#define C_GREEN        0x05E0  // #00bc40
#define C_YELLOW       0xFFE0  // #ffff00
#define C_ORANGE       0xFD20  // #ff9900
#define C_BRIGHT_RED   0xF904  // #ff1e56
#define C_AMBER        0xFD00  // #ffaa00
#define C_GRAY         0x6392  // #62728d
#define C_LIGHT_GRAY   0x9CF3  // #9aaabb
#define C_DARK_GRAY    0x2104  // #222222

// =========================================================================
// TELEMETRY DATA STRUCTURE
// =========================================================================
struct Telemetry {
    volatile float fwd_w;
    volatile float avg_w;
    volatile float peak_w;
    volatile float rev_w;
    volatile float swr;
    volatile float fwd_v;
    volatile float rev_v;
    volatile float pa_swr;
    volatile int   rssi;
    volatile bool  is_tx;
    volatile bool  connected;
    volatile unsigned long last_seen_ms;
};

Telemetry tele = { 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0, false, false, 0 };

// Damped values for smooth analog needle motion
float damped_fwd = 0.0f;
float damped_rev = 0.0f;

enum DisplayTheme {
    THEME_CLEAN_OLED = 0,  // Pure black minimalist OLED
    THEME_MODERN,          // Icom IC-7300 style dark cards
    THEME_CROSS_NEEDLE,    // Authentic Daiwa/Diamond cross-needle
    THEME_AMBER_RIG,       // Retro CRT amber glow
    THEME_COUNT
};
DisplayTheme currentTheme = THEME_CLEAN_OLED;

unsigned long lastTouchTime = 0;

// Forward declarations
void startSetupAP();
void setupWebServerRoutes();
String getIndexHtml();

// =========================================================================
// COLOR HELPERS
// =========================================================================
uint16_t getPwrColor(float w) {
    if (w < 1.0f) return C_GRAY;
    if (w < 40.0f) return C_NEON_GREEN;
    if (w < 75.0f) return C_YELLOW;
    if (w < 95.0f) return C_ORANGE;
    return C_BRIGHT_RED;
}

uint16_t getSwrColor(float s) {
    if (s <= 1.5f) return C_NEON_GREEN;
    if (s <= 2.2f) return C_YELLOW;
    if (s <= 2.8f) return C_ORANGE;
    return C_BRIGHT_RED;
}

// =========================================================================
// DIRECT SOCKET SWR TELEMETRY FETCH
// =========================================================================
bool fetchTelemetryFromBridge(IPAddress targetIp, uint16_t port) {
    WiFiClient client;
    client.setTimeout(600); // 600ms timeout

    if (!client.connect(targetIp, port, 600)) {
        return false;
    }

    client.print(F("GET /data HTTP/1.1\r\nHost: swrmeter\r\nConnection: close\r\n\r\n"));

    unsigned long startMs = millis();
    String response = "";
    response.reserve(1024);

    // Read full response until connection closes or 600ms timeout
    while ((client.connected() || client.available() > 0) && (millis() - startMs < 600)) {
        while (client.available() > 0) {
            char c = (char)client.read();
            response += c;
        }
        vTaskDelay(pdMS_TO_TICKS(4));
    }
    client.stop();

    if (response.length() == 0) return false;

    // Split headers and body at \r\n\r\n
    int bodyIdx = response.indexOf("\r\n\r\n");
    if (bodyIdx < 0) return false;

    String json = response.substring(bodyIdx + 4);
    json.trim();
    if (json.length() == 0) return false;

    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) return false;

    tele.fwd_w   = doc["fwd_w"] | 0.0f;
    tele.avg_w   = doc["avg_w"] | 0.0f;
    tele.peak_w  = doc["peak_w"] | 0.0f;
    tele.rev_w   = doc["rev_w"] | 0.0f;
    tele.swr     = doc["swr"] | 1.0f;
    tele.fwd_v   = doc["fwd_v"] | 0.0f;
    tele.rev_v   = doc["rev_v"] | 0.0f;
    tele.is_tx   = doc["tx"] | false;
    tele.connected = true;
    tele.last_seen_ms = millis();

    // Smart PA/RIG SWR foldback estimation
    float fwd = tele.fwd_w;
    float line_s = tele.swr;
    if (fwd < 5.0f) {
        tele.pa_swr = 1.0f;
    } else if (fwd >= 75.0f) {
        tele.pa_swr = 1.0f;
    } else if (line_s <= 1.5f) {
        tele.pa_swr = line_s;
    } else if (fwd >= 65.0f) {
        float est = 1.0f + (75.0f - fwd) / 10.0f * 0.8f;
        tele.pa_swr = (est < line_s) ? est : line_s;
    } else if (fwd >= 45.0f) {
        float est = 1.8f + (65.0f - fwd) / 20.0f * 1.0f;
        tele.pa_swr = (est < line_s) ? est : line_s;
    } else {
        tele.pa_swr = line_s;
    }

    return true;
}

// =========================================================================
// BACKGROUND TELEMETRY TASK (CORE 0)
// =========================================================================
void telemetryTask(void *pvParameters) {
    for (;;) {
        // If WiFi is disconnected and AP mode is not active:
        if (WiFi.status() != WL_CONNECTED && !apModeActive) {
            tele.connected = false;
            snprintf(dbgStatus, sizeof(dbgStatus), "WIFI: %s...", wifi_ssid.c_str());
            vTaskDelay(pdMS_TO_TICKS(400));
            continue;
        }

        if (apModeActive) {
            snprintf(dbgStatus, sizeof(dbgStatus), "AP: SWR-Display-Setup");
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        tele.rssi = WiFi.RSSI();

        // Multi-target list: Configured IP, Known SWR Bridge IP, AP Fallback IP
        IPAddress targetIps[3];
        int numTargets = 0;

        IPAddress cfgIp;
        if (cfgIp.fromString(bridge_ip)) {
            targetIps[numTargets++] = cfgIp;
        }
        IPAddress primaryIp(192, 168, 1, 159);
        if (cfgIp != primaryIp) {
            targetIps[numTargets++] = primaryIp;
        }
        IPAddress apIp(192, 168, 4, 1);
        if (cfgIp != apIp) {
            targetIps[numTargets++] = apIp;
        }

        bool success = false;
        for (int i = 0; i < numTargets; i++) {
            if (fetchTelemetryFromBridge(targetIps[i], bridge_port)) {
                success = true;
                if (targetIps[i] != cfgIp) {
                    bridge_ip = targetIps[i].toString();
                    saveConfig(wifi_ssid, wifi_psk, bridge_ip, bridge_port);
                }
                break;
            }
        }

        if (success) {
            snprintf(dbgStatus, sizeof(dbgStatus), "ONLINE (%d dBm)", tele.rssi);
            vTaskDelay(pdMS_TO_TICKS(40));
        } else {
            tele.connected = false;
            tele.fwd_w  = 0.0f;
            tele.avg_w  = 0.0f;
            tele.peak_w = 0.0f;
            tele.rev_w  = 0.0f;
            tele.swr    = 1.0f;
            tele.pa_swr = 1.0f;
            tele.is_tx  = false;
            snprintf(dbgStatus, sizeof(dbgStatus), "BRIDGE OFFLINE (%s)", WiFi.localIP().toString().c_str());
            vTaskDelay(pdMS_TO_TICKS(350));
        }
    }
}

// =========================================================================
// SMOOTH BAR GRAPH DRAWING
// =========================================================================
void drawSmoothBar(int x, int y, int w, int h, float val, float max_val, float peak_val = 0.0f) {
    canvas->drawRoundRect(x, y, w, h, 2, C_CARD_BORDER);
    int innerW = w - 4;
    int innerH = h - 4;
    canvas->fillRect(x + 2, y + 2, innerW, innerH, C_BLACK);

    float ratio = val / max_val;
    if (ratio > 1.0f) ratio = 1.0f;
    if (ratio < 0.0f) ratio = 0.0f;
    int fillW = (int)(ratio * innerW);

    if (fillW > 0) {
        for (int i = 0; i < fillW; i++) {
            float t = (float)i / (float)innerW;
            uint16_t col;
            if (t < 0.5f) {
                col = C_NEON_GREEN;
            } else if (t < 0.75f) {
                col = C_YELLOW;
            } else if (t < 0.90f) {
                col = C_ORANGE;
            } else {
                col = C_BRIGHT_RED;
            }
            canvas->drawFastVLine(x + 2 + i, y + 2, innerH, col);
        }
    }

    if (peak_val > 0.0f) {
        float peakRatio = peak_val / max_val;
        if (peakRatio > 1.0f) peakRatio = 1.0f;
        int peakX = x + 2 + (int)(peakRatio * (innerW - 1));
        canvas->drawFastVLine(peakX, y + 1, innerH + 2, C_WHITE);
    }
}

void drawSmoothSwrBar(int x, int y, int w, int h, float s) {
    canvas->drawRoundRect(x, y, w, h, 2, C_CARD_BORDER);
    int innerW = w - 4;
    int innerH = h - 4;
    canvas->fillRect(x + 2, y + 2, innerW, innerH, C_BLACK);

    float ratio = 0.0f;
    if (s > 1.0f) {
        ratio = (s - 1.0f) / 3.0f; // Scale 1.0 to 4.0
        if (ratio > 1.0f) ratio = 1.0f;
    }
    int fillW = (int)(ratio * innerW);

    if (fillW > 0) {
        for (int i = 0; i < fillW; i++) {
            float t = (float)i / (float)innerW;
            uint16_t col;
            if (t < 0.35f) col = C_NEON_GREEN;
            else if (t < 0.65f) col = C_YELLOW;
            else if (t < 0.85f) col = C_ORANGE;
            else col = C_BRIGHT_RED;
            canvas->drawFastVLine(x + 2 + i, y + 2, innerH, col);
        }
    }
}

// =========================================================================
// THEME 0: CLEAN OLED (Pitch Black, Minimalist, High-Res Vector Fonts)
// =========================================================================
void renderCleanOledTheme() {
    canvas->fillScreen(C_BLACK);

    // Top Status & TX/RX
    canvas->setFont(&FreeSansBold9pt7b);
    canvas->setTextColor(tele.connected ? C_NEON_GREEN : C_AMBER);
    canvas->setCursor(14, 20);
    canvas->print(dbgStatus);

    // TX/RX Badge
    if (tele.is_tx) {
        canvas->fillRoundRect(420, 4, 48, 20, 3, C_BRIGHT_RED);
        canvas->setFont(&FreeSansBold9pt7b);
        canvas->setTextColor(C_WHITE);
        canvas->setCursor(433, 19);
        canvas->print("TX");
    } else {
        canvas->drawRoundRect(420, 4, 48, 20, 3, C_CARD_BORDER);
        canvas->setFont(&FreeSans9pt7b);
        canvas->setTextColor(C_GRAY);
        canvas->setCursor(434, 19);
        canvas->print("RX");
    }

    // Horizontal divider
    canvas->drawFastHLine(10, 28, 460, C_CARD_BORDER);

    // -------------------------------------------------------------
    // PRIMARY ROW: FORWARD POWER (Left) + SWR (Right)
    // -------------------------------------------------------------

    // Left Column: FORWARD POWER (x: 14 .. 225)
    canvas->setFont(&FreeSans9pt7b);
    canvas->setTextColor(C_LIGHT_GRAY);
    canvas->setCursor(16, 50);
    canvas->print("FORWARD POWER");

    uint16_t pwrColor = getPwrColor(tele.fwd_w);
    canvas->setFont(&FreeSansBold24pt7b);
    canvas->setTextColor(pwrColor);
    canvas->setCursor(14, 96);
    canvas->printf("%5.1f", tele.fwd_w);

    canvas->setFont(&FreeSansBold12pt7b);
    canvas->setTextColor(C_GRAY);
    canvas->setCursor(158, 96);
    canvas->print("W");

    // Power bar
    drawSmoothBar(14, 114, 215, 8, tele.fwd_w, 120.0f, tele.peak_w);

    // Right Columns: SWR - Separate Boxes
    // ANT SWR Box (x: 248 .. 350)
    canvas->setFont(&FreeSans9pt7b);
    canvas->setTextColor(C_LIGHT_GRAY);
    canvas->setCursor(248, 50);
    canvas->print("ANT SWR");

    uint16_t swrColor = getSwrColor(tele.swr);
    canvas->setFont(&FreeSansBold18pt7b);
    canvas->setTextColor(swrColor);
    canvas->setCursor(248, 96);
    if (tele.swr > 90.0f) {
        canvas->print(">99");
    } else {
        canvas->printf("%4.2f", tele.swr);
    }
    drawSmoothSwrBar(248, 114, 100, 8, tele.swr);

    // PA SWR Box (x: 365 .. 465)
    canvas->setFont(&FreeSans9pt7b);
    canvas->setTextColor(C_LIGHT_GRAY);
    canvas->setCursor(365, 50);
    canvas->print("PA SWR");

    uint16_t paColor = getSwrColor(tele.pa_swr);
    canvas->setFont(&FreeSansBold18pt7b);
    canvas->setTextColor(paColor);
    canvas->setCursor(365, 96);
    canvas->printf("%4.2f", tele.pa_swr);
    drawSmoothSwrBar(365, 114, 100, 8, tele.pa_swr);

    // Horizontal divider
    canvas->drawFastHLine(10, 134, 460, C_CARD_BORDER);

    // -------------------------------------------------------------
    // SECONDARY ROW: AVG (VU) POWER & PEAK HOLD
    // -------------------------------------------------------------
    canvas->setFont(&FreeSans9pt7b);
    canvas->setTextColor(C_LIGHT_GRAY);
    canvas->setCursor(16, 156);
    canvas->print("AVG (VU) POWER");

    canvas->setFont(&FreeSansBold18pt7b);
    canvas->setTextColor(C_CYAN);
    canvas->setCursor(14, 192);
    canvas->printf("%5.1f", tele.avg_w);
    canvas->setFont(&FreeSansBold9pt7b);
    canvas->setTextColor(C_GRAY);
    canvas->setCursor(110, 192);
    canvas->print("W");

    drawSmoothBar(14, 206, 215, 6, tele.avg_w, 120.0f);

    // Peak Hold
    canvas->setFont(&FreeSans9pt7b);
    canvas->setTextColor(C_LIGHT_GRAY);
    canvas->setCursor(250, 156);
    canvas->print("PEAK HOLD");

    canvas->setFont(&FreeSansBold18pt7b);
    canvas->setTextColor(C_AMBER);
    canvas->setCursor(248, 192);
    canvas->printf("%5.1f", tele.peak_w);
    canvas->setFont(&FreeSansBold9pt7b);
    canvas->setTextColor(C_GRAY);
    canvas->setCursor(344, 192);
    canvas->print("W");

    // Reflected Power
    canvas->setFont(&FreeSans9pt7b);
    canvas->setTextColor(C_LIGHT_GRAY);
    canvas->setCursor(375, 156);
    canvas->print("REFLECTED");

    canvas->setFont(&FreeSansBold18pt7b);
    canvas->setTextColor(C_BRIGHT_RED);
    canvas->setCursor(373, 192);
    canvas->printf("%4.1f", tele.rev_w);
    canvas->setFont(&FreeSansBold9pt7b);
    canvas->setTextColor(C_GRAY);
    canvas->setCursor(450, 192);
    canvas->print("W");

    // Horizontal divider
    canvas->drawFastHLine(10, 224, 460, C_CARD_BORDER);

    // Detector voltages row
    canvas->setFont(&FreeSans9pt7b);
    canvas->setTextColor(C_GRAY);
    canvas->setCursor(24, 246);
    canvas->printf("DETECTORS:   FWD: %4.2f V       REV: %4.2f V", tele.fwd_v, tele.rev_v);
}

// =========================================================================
// THEME 1: MODERN ICOM (Rounded Dark Cards, Full Color Bars)
// =========================================================================
void renderModernTheme() {
    canvas->fillScreen(C_DARK_BG);

    // Header
    canvas->fillRect(0, 0, 480, 26, C_HEADER);
    canvas->drawFastHLine(0, 26, 480, C_CARD_BORDER);

    canvas->setFont(&FreeSansBold9pt7b);
    canvas->setTextColor(tele.connected ? C_NEON_GREEN : C_AMBER);
    canvas->setCursor(14, 18);
    canvas->print(dbgStatus);

    // TX/RX
    canvas->setCursor(410, 18);
    canvas->setTextColor(tele.is_tx ? C_BRIGHT_RED : C_CYAN);
    canvas->print(tele.is_tx ? "[ TX ]" : "[ RX ]");

    // Power Card (y: 32..124, h: 92)
    canvas->fillRoundRect(8, 32, 464, 92, 6, C_CARD_BG);
    canvas->drawRoundRect(8, 32, 464, 92, 6, C_CARD_BORDER);

    canvas->setFont(&FreeSans9pt7b);
    canvas->setTextColor(C_LIGHT_GRAY);
    canvas->setCursor(20, 50);
    canvas->print("FORWARD POWER");

    canvas->setCursor(290, 50);
    canvas->setTextColor(C_GRAY);
    canvas->printf("PEP: %5.1f W   AVG: %5.1f W", tele.peak_w, tele.avg_w);

    canvas->setFont(&FreeSansBold24pt7b);
    canvas->setTextColor(getPwrColor(tele.fwd_w));
    canvas->setCursor(18, 92);
    canvas->printf("%5.1f", tele.fwd_w);

    canvas->setFont(&FreeSansBold12pt7b);
    canvas->setTextColor(C_GRAY);
    canvas->setCursor(158, 92);
    canvas->print("W");

    drawSmoothBar(195, 66, 265, 14, tele.fwd_w, 120.0f, tele.peak_w);
    drawSmoothBar(195, 88, 265, 10, tele.avg_w, 120.0f);

    // SWR Card (y: 130..222, h: 92)
    canvas->fillRoundRect(8, 130, 464, 92, 6, C_CARD_BG);
    canvas->drawRoundRect(8, 130, 464, 92, 6, C_CARD_BORDER);

    canvas->setFont(&FreeSans9pt7b);
    canvas->setTextColor(C_LIGHT_GRAY);
    canvas->setCursor(20, 148);
    canvas->print("ANTENNA SWR");

    canvas->setCursor(340, 148);
    canvas->setTextColor(C_GRAY);
    canvas->print("PA / RIG SWR");

    canvas->setFont(&FreeSansBold24pt7b);
    canvas->setTextColor(getSwrColor(tele.swr));
    canvas->setCursor(18, 190);
    if (tele.swr > 90.0f) {
        canvas->print(">99");
    } else {
        canvas->printf("%4.2f", tele.swr);
    }

    canvas->setFont(&FreeSansBold12pt7b);
    canvas->setTextColor(C_GRAY);
    canvas->setCursor(110, 190);
    canvas->print(":1");

    canvas->setFont(&FreeSansBold18pt7b);
    canvas->setTextColor(getSwrColor(tele.pa_swr));
    canvas->setCursor(340, 190);
    canvas->printf("%4.2f :1", tele.pa_swr);

    drawSmoothSwrBar(140, 168, 185, 12, tele.swr);
    drawSmoothSwrBar(140, 184, 185, 6, tele.pa_swr);

    // Footer (y: 228..266, h: 38)
    canvas->fillRoundRect(8, 228, 464, 38, 4, C_CARD_BG);
    canvas->drawRoundRect(8, 228, 464, 38, 4, C_CARD_BORDER);

    canvas->setFont(&FreeSans9pt7b);
    canvas->setTextColor(C_WHITE);
    canvas->setCursor(20, 252);
    canvas->printf("REF: %5.2f W", tele.rev_w);

    canvas->setTextColor(C_AMBER);
    canvas->setCursor(180, 252);
    canvas->printf("PEAK: %5.1f W", tele.peak_w);

    canvas->setTextColor(C_GRAY);
    canvas->setCursor(330, 252);
    canvas->printf("DET: %4.2fV / %4.2fV", tele.fwd_v, tele.rev_v);
}

// =========================================================================
// THEME 2: AUTHENTIC CROSS-NEEDLE METER (DAIWA / DIAMOND ARCHITECTURE)
// =========================================================================
void renderCrossNeedleTheme() {
    canvas->fillScreen(0x0821); // Deep instrument dark blue/black

    // Outer Bezel
    canvas->drawRoundRect(4, 4, 472, 264, 8, C_CARD_BORDER);
    canvas->drawRoundRect(6, 6, 468, 260, 6, 0x1908);

    // Dial background area
    int W = 480;
    int cx0 = 240;
    int cy0 = 250; // Central baseline anchor for SWR fan lines

    int xL = 95, yL = 252;
    int xR = 385, yR = 252;

    // SWR Fan Lines curving up from bottom-center
    struct SwrTarget {
        const char* label;
        int tx, ty;
    };
    const SwrTarget swrFan[] = {
        { "1.1", 380, 210 },
        { "1.2", 355, 170 },
        { "1.5", 325, 125 },
        { "2.0", 285, 90  },
        { "3.0", 240, 70  },
        { "5.0", 195, 90  },
        { "8.0", 155, 135 },
        { "oo",  115, 210 }
    };

    // Draw SWR Fan curves
    for (int s = 0; s < 8; s++) {
        int tx = swrFan[s].tx;
        int ty = swrFan[s].ty;
        int prevX = -1, prevY = -1;
        const int steps = 20;

        for (int i = 0; i <= steps; i++) {
            float t = (float)i / (float)steps;
            float ctrl_x = (cx0 + tx) / 2.0f + (240.0f - (cx0 + tx) / 2.0f) * 0.15f;
            float ctrl_y = (cy0 + ty) / 2.0f + 12.0f;
            int px = (int)((1 - t) * (1 - t) * cx0 + 2 * (1 - t) * t * ctrl_x + t * t * tx);
            int py = (int)((1 - t) * (1 - t) * cy0 + 2 * (1 - t) * t * ctrl_y + t * t * ty);

            if (prevX != -1) {
                canvas->drawLine(prevX, py, px, py, 0x9186); // Soft red/pink SWR curves
            }
            prevX = px;
            prevY = py;

            if (i == 14) {
                canvas->setFont(&FreeSans9pt7b);
                canvas->setTextColor(C_BRIGHT_RED);
                canvas->setCursor(px + (tx >= cx0 ? 4 : -16), py - 2);
                canvas->print(swrFan[s].label);
            }
        }
    }

    // Baseline reference line
    canvas->drawLine(45, 210, cx0, cy0, 0x31A6);
    canvas->drawLine(cx0, cy0, W - 45, 210, 0x31A6);

    // Forward Power Scale Arc (Green)
    int prevFwdX = -1, prevFwdY = -1;
    for (int i = 0; i <= 30; i++) {
        float t = (float)i / 30.0f;
        int px = (int)((1 - t) * (1 - t) * 45 + 2 * (1 - t) * t * 155 + t * t * 310);
        int py = (int)((1 - t) * (1 - t) * 210 + 2 * (1 - t) * t * 80 + t * t * 40);
        if (prevFwdX != -1) {
            canvas->drawLine(prevFwdX, prevFwdY, px, py, C_NEON_GREEN);
        }
        prevFwdX = px;
        prevFwdY = py;
    }

    // Reflected Power Scale Arc (Red)
    int prevRevX = -1, prevRevY = -1;
    for (int i = 0; i <= 30; i++) {
        float t = (float)i / 30.0f;
        int px = (int)((1 - t) * (1 - t) * (W - 45) + 2 * (1 - t) * t * (W - 155) + t * t * 170);
        int py = (int)((1 - t) * (1 - t) * 210 + 2 * (1 - t) * t * 80 + t * t * 40);
        if (prevRevX != -1) {
            canvas->drawLine(prevRevX, prevRevY, px, py, C_BRIGHT_RED);
        }
        prevRevX = px;
        prevRevY = py;
    }

    // Scale Headers & Status
    canvas->setFont(&FreeSansBold9pt7b);
    canvas->setTextColor(C_NEON_GREEN);
    canvas->setCursor(22, 28);
    canvas->print("FORWARD");

    canvas->setFont(&FreeSans9pt7b);
    canvas->setTextColor(tele.connected ? C_NEON_GREEN : C_AMBER);
    canvas->setCursor(140, 28);
    canvas->print(dbgStatus);

    canvas->setFont(&FreeSansBold9pt7b);
    canvas->setTextColor(C_BRIGHT_RED);
    canvas->setCursor(350, 28);
    canvas->print("REFLECTED");

    // Mechanical Needle Inertial Damping
    float targetFwd = tele.fwd_w;
    float targetRev = tele.rev_w;
    damped_fwd += 0.25f * (targetFwd - damped_fwd);
    damped_rev += 0.25f * (targetRev - damped_rev);
    if (damped_fwd < 0.05f) damped_fwd = 0.0f;
    if (damped_rev < 0.05f) damped_rev = 0.0f;

    // Forward Needle (Pivot at RIGHT xR, yR -> Points to Left Arc)
    float tF = sqrtf(damped_fwd / 120.0f);
    if (tF > 1.0f) tF = 1.0f;
    int txF = (int)((1 - tF) * (1 - tF) * 45 + 2 * (1 - tF) * tF * 155 + tF * tF * 310);
    int tyF = (int)((1 - tF) * (1 - tF) * 210 + 2 * (1 - tF) * tF * 80 + tF * tF * 40);

    // Reflected Needle (Pivot at LEFT xL, yL -> Points to Right Arc)
    float tR = sqrtf(damped_rev / 25.0f);
    if (tR > 1.0f) tR = 1.0f;
    int txR = (int)((1 - tR) * (1 - tR) * (W - 45) + 2 * (1 - tR) * tR * (W - 155) + tR * tR * 170);
    int tyR = (int)((1 - tR) * (1 - tR) * 210 + 2 * (1 - tR) * tR * 80 + tR * tR * 40);

    // Draw Forward Needle (Green, thick)
    canvas->drawLine(xR, yR, txF, tyF, C_NEON_GREEN);
    canvas->drawLine(xR - 1, yR, txF - 1, tyF, C_NEON_GREEN);
    canvas->drawLine(xR + 1, yR, txF + 1, tyF, C_NEON_GREEN);

    // Draw Reflected Needle (Red, thick)
    canvas->drawLine(xL, yL, txR, tyR, C_BRIGHT_RED);
    canvas->drawLine(xL - 1, yL, txR - 1, tyR, C_BRIGHT_RED);
    canvas->drawLine(xL + 1, yL, txR + 1, tyR, C_BRIGHT_RED);

    // Calculate Intersection Point
    float dxF = txF - xR;
    float dyF = tyF - yR;
    float dxR = txR - xL;
    float dyR = tyR - yL;
    float denom = dxF * (-dyR) - dyF * (-dxR);

    if (fabsf(denom) > 1e-4) {
        float u = ((xL - xR) * (-dyR) - (yL - yR) * (-dxR)) / denom;
        if (u >= 0.15f && u <= 1.05f) {
            int crossX = (int)(xR + u * dxF);
            int crossY = (int)(yR + u * dyF);
            canvas->fillCircle(crossX, crossY, 7, C_AMBER);
            canvas->fillCircle(crossX, crossY, 3, C_WHITE);
        }
    }

    // Pivot hubs
    canvas->fillCircle(xL, yL, 8, C_BLACK);
    canvas->drawCircle(xL, yL, 8, C_WHITE);
    canvas->fillCircle(xR, yR, 8, C_BLACK);
    canvas->drawCircle(xR, yR, 8, C_WHITE);

    // Bottom digital summary banner
    canvas->fillRect(10, 240, 460, 24, C_HEADER);
    canvas->setFont(&FreeSansBold9pt7b);
    canvas->setTextColor(C_NEON_GREEN);
    canvas->setCursor(20, 256);
    canvas->printf("FWD: %5.1f W", tele.fwd_w);

    canvas->setTextColor(getSwrColor(tele.swr));
    canvas->setCursor(185, 256);
    if (tele.swr > 90.0f) {
        canvas->print("SWR: >99 :1");
    } else {
        canvas->printf("SWR: %4.2f :1", tele.swr);
    }

    canvas->setTextColor(C_BRIGHT_RED);
    canvas->setCursor(340, 256);
    canvas->printf("REF: %5.2f W", tele.rev_w);
}

// =========================================================================
// THEME 3: RETRO AMBER CRT RIG
// =========================================================================
void renderRetroAmberTheme() {
    canvas->fillScreen(C_BLACK);

    canvas->drawRoundRect(4, 4, 472, 264, 6, C_AMBER);
    canvas->drawFastHLine(6, 26, 468, C_AMBER);

    canvas->setFont(&FreeSansBold9pt7b);
    canvas->setTextColor(tele.connected ? C_AMBER : 0xFD20);
    canvas->setCursor(16, 20);
    canvas->print(dbgStatus);

    canvas->setCursor(410, 20);
    canvas->print(tele.is_tx ? "[TX ON]" : "[RX]");

    // Large Power Readout
    canvas->setFont(&FreeSans9pt7b);
    canvas->setCursor(20, 48);
    canvas->print("POWER OUTPUT (W)");

    canvas->setFont(&FreeSansBold24pt7b);
    canvas->setCursor(18, 92);
    canvas->printf("%5.1f W", tele.fwd_w);

    // Segmented LED bar
    int segCount = 28;
    int litSegs = (int)((tele.fwd_w / 120.0f) * segCount);
    for (int i = 0; i < segCount; i++) {
        uint16_t c = (i < litSegs) ? C_AMBER : 0x2100;
        canvas->fillRect(20 + i * 15, 102, 11, 14, c);
    }

    // Large SWR Readout
    canvas->setFont(&FreeSans9pt7b);
    canvas->setCursor(20, 142);
    canvas->printf("STANDING WAVE RATIO                   PA SWR: %4.2f :1", tele.pa_swr);

    canvas->setFont(&FreeSansBold24pt7b);
    canvas->setCursor(18, 186);
    if (tele.swr > 90.0f) {
        canvas->print(">99 :1");
    } else {
        canvas->printf("%4.2f :1", tele.swr);
    }

    int swrLit = 0;
    if (tele.swr > 1.0f) {
        swrLit = (int)(((tele.swr - 1.0f) / 3.0f) * segCount);
        if (swrLit > segCount) swrLit = segCount;
    }
    for (int i = 0; i < segCount; i++) {
        uint16_t c = (i < swrLit) ? C_AMBER : 0x2100;
        canvas->fillRect(20 + i * 15, 196, 11, 14, c);
    }

    // Footer
    canvas->setFont(&FreeSans9pt7b);
    canvas->setCursor(20, 246);
    canvas->printf("REFLECTED: %5.2f W   PEP: %5.1f W   DET: %4.2fV / %4.2fV",
                   tele.rev_w, tele.peak_w, tele.fwd_v, tele.rev_v);
}

// =========================================================================
// CAPTIVE PORTAL & WEB SERVER IMPLEMENTATION
// =========================================================================
String getIndexHtml() {
    String html = FPSTR(WEB_INDEX_HTML);
    html.replace("%BRIDGE_IP%", bridge_ip);
    html.replace("%BRIDGE_PORT%", String(bridge_port));
    html.replace("%WIFI_SSID%", wifi_ssid);
    html.replace("%WIFI_PSK%", wifi_psk);

    String wifiStatus;
    if (WiFi.status() == WL_CONNECTED) {
        wifiStatus = "<span class='val-online'>Yhdistetty (" + WiFi.SSID() + ", " + WiFi.localIP().toString() + ", " + String(WiFi.RSSI()) + " dBm)</span>";
    } else if (apModeActive) {
        wifiStatus = "<span class='val-offline'>Setup AP aktiivinen (192.168.4.1)</span>";
    } else {
        wifiStatus = "<span class='val-offline'>Yhdistet&auml;&auml;n...</span>";
    }
    html.replace("%WIFI_STATUS%", wifiStatus);

    String bridgeStatus;
    if (tele.connected) {
        bridgeStatus = "<span class='val-online'>ONLINE (P&auml;ivittyy)</span>";
    } else {
        bridgeStatus = "<span class='val-offline'>OFFLINE</span>";
    }
    html.replace("%BRIDGE_STATUS%", bridgeStatus);

    return html;
}

bool captivePortal() {
    if (!apModeActive) return false;
    String host = otaServer.hostHeader();
    if (host.indexOf("192.168.4.1") < 0) {
        Serial.printf("[CAPTIVE] Redirecting host '%s' to http://192.168.4.1/\n", host.c_str());
        otaServer.sendHeader("Location", "http://192.168.4.1/", true);
        otaServer.send(302, "text/plain", "");
        return true;
    }
    return false;
}

void handleRoot() {
    otaServer.sendHeader("Connection", "close");
    otaServer.send(200, "text/html", getIndexHtml());
}

void handleSave() {
    String new_ip = otaServer.arg("bridge_ip");
    String new_port_str = otaServer.arg("bridge_port");
    String new_ssid = otaServer.arg("wifi_ssid");
    String new_psk = otaServer.arg("wifi_psk");

    new_ip.trim();
    new_ssid.trim();
    uint16_t new_port = new_port_str.toInt();
    if (new_port == 0) new_port = 2222;

    if (new_ip.length() > 0 && new_ssid.length() > 0) {
        saveConfig(new_ssid, new_psk, new_ip, new_port);
        bridge_ip = new_ip;
        bridge_port = new_port;
        wifi_ssid = new_ssid;
        wifi_psk = new_psk;

        String html = "<!DOCTYPE html><html lang='fi'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>"
                      "<style>body{background:#080c14;color:#fff;font-family:sans-serif;display:flex;justify-content:center;align-items:center;min-height:100vh;text-align:center;padding:1rem;}"
                      ".box{background:#121824;padding:2rem;border-radius:12px;border:1px solid #202838;max-width:400px;box-shadow:0 10px 30px rgba(0,0,0,0.6);}"
                      "h2{color:#00ff88;margin-bottom:1rem;}p{color:#9aaabb;margin-bottom:0.6rem;line-height:1.5;}</style></head><body>"
                      "<div class='box'><h2>&#9989; Asetukset tallennettu!</h2>"
                      "<p><strong>SWR Silta:</strong> " + new_ip + ":" + String(new_port) + "</p>"
                      "<p><strong>Wi-Fi:</strong> " + new_ssid + "</p>"
                      "<p style='color:#00e5ff;margin-top:1.2rem;font-weight:600;'>N&auml;ytt&ouml; k&auml;ynnistyy nyt uudelleen...</p></div>"
                      "<script>setTimeout(function(){location.href='/';}, 6000);</script></body></html>";

        otaServer.sendHeader("Connection", "close");
        otaServer.send(200, "text/html", html);
        delay(600);
        ESP.restart();
    } else {
        otaServer.send(400, "text/plain", "Virheelliset parametrit!");
    }
}

void setupWebServerRoutes() {
    if (serverRoutesConfigured) return;

    otaServer.on("/", HTTP_GET, handleRoot);
    otaServer.on("/save", HTTP_POST, handleSave);

    // Captive Portal Detection endpoints (Android, iOS, Windows)
    otaServer.on("/generate_204", []() {
        if (!captivePortal()) handleRoot();
    });
    otaServer.on("/hotspot-detect.html", []() {
        if (!captivePortal()) handleRoot();
    });
    otaServer.on("/canonical.html", []() {
        if (!captivePortal()) handleRoot();
    });
    otaServer.on("/connecttest.txt", []() {
        if (!captivePortal()) handleRoot();
    });
    otaServer.on("/ncsi.txt", []() {
        if (!captivePortal()) handleRoot();
    });

    // OTA Update Endpoint
    otaServer.on("/update", HTTP_POST, []() {
        otaServer.sendHeader("Connection", "close");
        otaServer.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
        delay(250);
        ESP.restart();
    }, []() {
        HTTPUpload& upload = otaServer.upload();
        if (upload.status == UPLOAD_FILE_START) {
            Serial.printf("[OTA] Update Start: %s\n", upload.filename.c_str());
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                Update.printError(Serial);
            }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                Update.printError(Serial);
            }
        } else if (upload.status == UPLOAD_FILE_END) {
            if (Update.end(true)) {
                Serial.printf("[OTA] Update Success: %u bytes\n", upload.totalSize);
            } else {
                Update.printError(Serial);
            }
        }
    });

    otaServer.onNotFound([]() {
        if (captivePortal()) return;
        otaServer.send(404, "text/plain", "404 Not Found");
    });

    serverRoutesConfigured = true;
    Serial.println("[HTTP] WebServer routes registered.");
}

void startSetupAP() {
    if (apModeActive) return;
    Serial.println("[AP] Starting open Setup AP: SWR-Display-Setup...");
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
    WiFi.softAP("SWR-Display-Setup"); // Open network, no password!
    delay(100);
    dnsServer.start(DNS_PORT, "*", IPAddress(192, 168, 4, 1));
    setupWebServerRoutes();
    otaServer.begin();
    apModeActive = true;
    Serial.println("[AP] Setup AP running at 192.168.4.1 (DNS port 53 active)");
}

// =========================================================================
// OTA & MDNS INITIALIZATION (CALLED ONCE WIFI STA IS CONNECTED)
// =========================================================================
static bool mdnsInitialized = false;

void initOTAOnceConnected() {
    if (WiFi.status() != WL_CONNECTED) return;

    setupWebServerRoutes();
    otaServer.begin();

    if (!mdnsInitialized) {
        if (MDNS.begin("swrdisplay")) {
            MDNS.addService("http", "tcp", 80);
            MDNS.addService("arduino", "tcp", 3232);
            Serial.printf("[OTA] mDNS active: http://swrdisplay.local (IP: %s)\n", WiFi.localIP().toString().c_str());
            mdnsInitialized = true;
        }
        ArduinoOTA.setHostname("swrdisplay");
        ArduinoOTA.begin();
        Serial.println("[OTA] ArduinoOTA and WebServer started successfully!");
    }
}

// =========================================================================
// SETUP
// =========================================================================
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n=== 4.3\" SWR & POWER MONITOR DESKTOP STATION ===");

    // 0. Load Configuration from NVS
    loadConfig();

    // 1. Turn on LCD Backlight
    pinMode(GFX_BL, OUTPUT);
    digitalWrite(GFX_BL, HIGH);

    // 2. Initialize Canvas & Display
    canvas->begin();
    canvas->fillScreen(C_BLACK);

    // Initial Splash Screen
    canvas->setFont(&FreeSansBold12pt7b);
    canvas->setTextColor(C_CYAN);
    canvas->setCursor(60, 90);
    canvas->print("SWR & POWER MONITOR");

    canvas->setFont(&FreeSans9pt7b);
    canvas->setTextColor(C_WHITE);
    canvas->setCursor(60, 130);
    canvas->printf("Connecting WiFi: %s...", wifi_ssid.c_str());

    canvas->setTextColor(C_GRAY);
    canvas->setCursor(60, 160);
    canvas->printf("Target: http://%s:%u/data", bridge_ip.c_str(), bridge_port);
    canvas->flush();

    // 3. Initialize Capacitive Touch
    tp.begin();
    tp.setRotation(ROTATION_NORMAL);

    // 4. Connect to WiFi directly here in setup() (Fast 2-3 sec connection!)
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);
    Serial.printf("[WIFI] Connecting to %s...\n", wifi_ssid.c_str());
    WiFi.begin(wifi_ssid.c_str(), wifi_psk.c_str());

    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 25) {
        delay(200);
        retries++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WIFI] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("[WIFI] Connection failed, starting Setup AP...");
        startSetupAP();
    }

    // 5. Start Background Telemetry Task on Core 0
    xTaskCreatePinnedToCore(
        telemetryTask,
        "TelemetryTask",
        8192,
        NULL,
        1,
        NULL,
        0 // Core 0
    );

    lastActivityTime = millis();
    isBacklightOn = true;
}

// =========================================================================
// MAIN RENDER LOOP (CORE 1)
// =========================================================================
void loop() {
    unsigned long now = millis();

    // 1. Service Network & OTA
    if (apModeActive) {
        dnsServer.processNextRequest();
        otaServer.handleClient();
    } else if (WiFi.status() == WL_CONNECTED) {
        initOTAOnceConnected();
        otaServer.handleClient();
        ArduinoOTA.handle();
    }

    // 2. Transmit activity detection -> Wakes screen and resets 15-minute standby timer
    bool isTxActive = tele.is_tx || (tele.fwd_w > 1.5f);
    if (isTxActive) {
        if (!isBacklightOn) {
            digitalWrite(GFX_BL, HIGH);
            isBacklightOn = true;
            Serial.println("[STANDBY] TX active -> Screen woke up!");
        }
        lastActivityTime = now;
    }

    // 3. Process Touch Input (Wake screen or cycle themes)
    tp.read();
    if (tp.isTouched && (now - lastTouchTime > 350)) {
        lastTouchTime = now;
        lastActivityTime = now;

        if (!isBacklightOn) {
            // First tap wakes up the screen
            digitalWrite(GFX_BL, HIGH);
            isBacklightOn = true;
            Serial.println("[STANDBY] Screen tapped -> Screen woke up!");
        } else {
            // Screen is awake -> cycle through themes
            currentTheme = (DisplayTheme)((currentTheme + 1) % THEME_COUNT);
        }
    }

    // 4. Standby timeout check (15 minutes of inactivity)
    if (isBacklightOn && (now - lastActivityTime >= STANDBY_TIMEOUT_MS)) {
        digitalWrite(GFX_BL, LOW);
        isBacklightOn = false;
        Serial.println("[STANDBY] 15min inactivity reached -> Backlight turned OFF");
    }

    // 5. Render Active Theme into PSRAM Canvas
    if (isBacklightOn) {
        switch (currentTheme) {
            case THEME_CLEAN_OLED:
                renderCleanOledTheme();
                break;
            case THEME_MODERN:
                renderModernTheme();
                break;
            case THEME_CROSS_NEEDLE:
                renderCrossNeedleTheme();
                break;
            case THEME_AMBER_RIG:
                renderRetroAmberTheme();
                break;
        }

        // Instant Tear-Free Blit to Display!
        canvas->flush();
        delay(25);
    } else {
        // In standby: save CPU and power, but keep loop alive for OTA & telemetry polling
        delay(80);
    }
}
