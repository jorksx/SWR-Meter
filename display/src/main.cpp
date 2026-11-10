#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include <TAMC_GT911.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <math.h>

// =========================================================================
// WIFI & TARGET SWR METER CONFIGURATION
// =========================================================================
const char* WIFI_SSID = "Kuitunen";
const char* WIFI_PSK  = "12345678";

// Target SWR Meter Hosts (Primary direct IP + fallback mDNS/AP)
const char* SWR_HOSTS[] = {
    "192.168.1.159",
    "swrmeter.local",
    "192.168.4.1"
};
const int SWR_PORT = 2222;
const int SWR_HOST_COUNT = 3;

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
// BACKGROUND TELEMETRY TASK (CORE 0) - RELIABLE HTTP SOCKET POLLING
// =========================================================================
void telemetryTask(void *pvParameters) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PSK);

    int hostIdx = 0;
    int failCount = 0;

    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            tele.connected = false;
            tele.rssi = 0;
            vTaskDelay(pdMS_TO_TICKS(400));
            continue;
        }

        tele.rssi = WiFi.RSSI();
        const char* targetHost = SWR_HOSTS[hostIdx];

        WiFiClient client;
        client.setTimeout(250);

        if (client.connect(targetHost, SWR_PORT, 250)) {
            client.print(String("GET /data HTTP/1.1\r\nHost: ") + targetHost + "\r\nConnection: close\r\n\r\n");

            // Look directly for '{' in the incoming HTTP stream (instant, skips headers cleanly)
            if (client.find("{")) {
                String payload = "{" + client.readStringUntil('}') + "}";
                StaticJsonDocument<512> doc;
                DeserializationError err = deserializeJson(doc, payload);

                if (!err) {
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
                    failCount = 0;

                    // Smart PA/RIG SWR foldback estimation
                    float fwd = tele.fwd_w;
                    float line_s = tele.swr;
                    if (fwd < 5.0f) {
                        tele.pa_swr = 1.0f;
                    } else if (fwd >= 80.0f) {
                        tele.pa_swr = 1.0f; // ATU matched load, full 100W+ safely dispatched
                    } else if (line_s <= 1.5f) {
                        tele.pa_swr = line_s;
                    } else if (fwd >= 65.0f) {
                        float est = 1.0f + (80.0f - fwd) / 15.0f * 0.8f;
                        tele.pa_swr = (est < line_s) ? est : line_s;
                    } else if (fwd >= 45.0f) {
                        float est = 1.8f + (65.0f - fwd) / 20.0f * 1.0f;
                        tele.pa_swr = (est < line_s) ? est : line_s;
                    } else {
                        tele.pa_swr = line_s;
                    }
                }
            }
            client.stop();
        } else {
            failCount++;
            if (failCount > 15) {
                tele.connected = false;
                hostIdx = (hostIdx + 1) % SWR_HOST_COUNT;
                failCount = 0;
            }
        }

        // Fast 40ms polling (~25 updates/sec)
        vTaskDelay(pdMS_TO_TICKS(40));
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
                // Green to Yellow
                uint8_t r = (uint8_t)(t * 2.0f * 31);
                col = (r << 11) | (63 << 5) | 0;
            } else {
                // Yellow to Red
                uint8_t g = (uint8_t)((1.0f - (t - 0.5f) * 2.0f) * 63);
                col = (31 << 11) | (g << 5) | 0;
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

    // Header bar
    canvas->setFont(&FreeSansBold9pt7b);
    canvas->setTextColor(C_CYAN);
    canvas->setCursor(14, 20);
    canvas->print("OLED SWR MONITOR");

    // RSSI & Status
    canvas->setFont(&FreeSans9pt7b);
    if (tele.connected) {
        canvas->setTextColor(C_NEON_GREEN);
        canvas->setCursor(220, 20);
        canvas->printf("ONLINE (%d dBm)", tele.rssi);
    } else {
        canvas->setTextColor(C_AMBER);
        canvas->setCursor(220, 20);
        canvas->print("CONNECTING...");
    }

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

    // Left Column: FORWARD POWER (Vector font)
    canvas->setFont(&FreeSans9pt7b);
    canvas->setTextColor(C_LIGHT_GRAY);
    canvas->setCursor(16, 48);
    canvas->print("FORWARD POWER");

    uint16_t pwrColor = getPwrColor(tele.fwd_w);
    canvas->setFont(&FreeSansBold24pt7b);
    canvas->setTextColor(pwrColor);
    canvas->setCursor(14, 90);
    canvas->printf("%5.1f", tele.fwd_w);

    canvas->setFont(&FreeSansBold12pt7b);
    canvas->setTextColor(C_GRAY);
    canvas->setCursor(155, 90);
    canvas->print("W");

    // Right Column: SWR RATIO
    canvas->setFont(&FreeSans9pt7b);
    canvas->setTextColor(C_LIGHT_GRAY);
    canvas->setCursor(250, 48);
    canvas->print("ANTENNA SWR");

    canvas->setCursor(375, 48);
    canvas->setTextColor(C_GRAY);
    canvas->print("PA SWR");

    uint16_t swrColor = getSwrColor(tele.swr);
    canvas->setFont(&FreeSansBold24pt7b);
    canvas->setTextColor(swrColor);
    canvas->setCursor(248, 90);
    if (tele.swr > 90.0f) {
        canvas->print(">99");
    } else {
        canvas->printf("%4.2f", tele.swr);
    }

    canvas->setFont(&FreeSansBold12pt7b);
    canvas->setTextColor(C_GRAY);
    canvas->setCursor(340, 90);
    canvas->print(":1");

    // PA SWR Number
    uint16_t paColor = getSwrColor(tele.pa_swr);
    canvas->setFont(&FreeSansBold18pt7b);
    canvas->setTextColor(paColor);
    canvas->setCursor(375, 90);
    canvas->printf("%4.2f", tele.pa_swr);

    // Smooth High-Precision Progress Bars
    drawSmoothBar(14, 108, 215, 6, tele.fwd_w, 120.0f, tele.peak_w);
    drawSmoothSwrBar(248, 108, 218, 6, tele.swr);

    // Lower Card: Telemetry Grid (y: 130..220)
    canvas->drawRoundRect(14, 130, 452, 90, 6, C_CARD_BORDER);

    // Sub-readouts row 1
    canvas->setFont(&FreeSans9pt7b);
    canvas->setTextColor(C_GRAY);
    canvas->setCursor(26, 155);
    canvas->print("AVG (VU) POWER");
    canvas->setCursor(26, 175);
    canvas->setTextColor(C_WHITE);
    canvas->setFont(&FreeSansBold12pt7b);
    canvas->printf("%5.1f W", tele.avg_w);

    canvas->setFont(&FreeSans9pt7b);
    canvas->setTextColor(C_GRAY);
    canvas->setCursor(175, 155);
    canvas->print("PEAK HOLD (PEP)");
    canvas->setCursor(175, 175);
    canvas->setTextColor(C_AMBER);
    canvas->setFont(&FreeSansBold12pt7b);
    canvas->printf("%5.1f W", tele.peak_w);

    canvas->setFont(&FreeSans9pt7b);
    canvas->setTextColor(C_GRAY);
    canvas->setCursor(325, 155);
    canvas->print("REFLECTED");
    canvas->setCursor(325, 175);
    canvas->setTextColor(C_BRIGHT_RED);
    canvas->setFont(&FreeSansBold12pt7b);
    canvas->printf("%5.2f W", tele.rev_w);

    // Detector Voltages row 2
    canvas->setFont(&FreeSans9pt7b);
    canvas->setTextColor(C_GRAY);
    canvas->setCursor(26, 205);
    canvas->printf("DETECTORS:  FWD: %4.2f V    REV: %4.2f V", tele.fwd_v, tele.rev_v);

    canvas->setCursor(330, 205);
    canvas->setTextColor(C_CYAN);
    canvas->print("TAP TO CHANGE");

    // Bottom info line
    canvas->drawFastHLine(10, 238, 460, C_CARD_BORDER);
    canvas->setFont(&FreeSans9pt7b);
    canvas->setTextColor(C_GRAY);
    canvas->setCursor(14, 258);
    canvas->print("ESP32-S3 HIGH SPEED SWR & POWER MONITOR");
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
    canvas->setTextColor(C_CYAN);
    canvas->setCursor(12, 18);
    canvas->print("MODERN DASHBOARD");

    canvas->setFont(&FreeSans9pt7b);
    canvas->setTextColor(tele.connected ? C_NEON_GREEN : C_AMBER);
    canvas->setCursor(200, 18);
    canvas->printf("WIFI %d dBm", tele.rssi);

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

    // Authentic Daiwa/Diamond geometry:
    // Left pivot (Reflected needle): starts on LEFT (xL, yL) and sweeps UP & RIGHT!
    // Right pivot (Forward needle): starts on RIGHT (xR, yR) and sweeps UP & LEFT!
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
    int prevRefX = -1, prevRefY = -1;
    for (int i = 0; i <= 30; i++) {
        float t = (float)i / 30.0f;
        int px = (int)((1 - t) * (1 - t) * (W - 45) + 2 * (1 - t) * t * (W - 155) + t * t * 170);
        int py = (int)((1 - t) * (1 - t) * 210 + 2 * (1 - t) * t * 80 + t * t * 40);
        if (prevRefX != -1) {
            canvas->drawLine(prevRefX, prevRefY, px, py, C_BRIGHT_RED);
        }
        prevRefX = px;
        prevRefY = py;
    }

    // Forward Ticks & Labels
    struct ScaleTick { float t; const char* lbl; int ox; int oy; };
    const ScaleTick fwdTicks[] = {
        { 0.00f, "0",   -14,   4 },
        { 0.29f, "10",  -16,  -2 },
        { 0.46f, "25",  -16,  -6 },
        { 0.65f, "50",  -14, -10 },
        { 0.79f, "75",  -10, -12 },
        { 0.91f, "100",  -8, -14 },
        { 1.00f, "120W", -6, -14 }
    };
    canvas->setFont(&FreeSans9pt7b);
    for (int i = 0; i < 7; i++) {
        float t = fwdTicks[i].t;
        int px = (int)((1 - t) * (1 - t) * 45 + 2 * (1 - t) * t * 155 + t * t * 310);
        int py = (int)((1 - t) * (1 - t) * 210 + 2 * (1 - t) * t * 80 + t * t * 40);
        canvas->fillCircle(px, py, 2, C_NEON_GREEN);
        canvas->setTextColor(C_LIGHT_GRAY);
        canvas->setCursor(px + fwdTicks[i].ox, py + fwdTicks[i].oy);
        canvas->print(fwdTicks[i].lbl);
    }

    // Reflected Ticks & Labels
    const ScaleTick refTicks[] = {
        { 0.00f, "0",     6,   4 },
        { 0.45f, "5",     6,  -6 },
        { 0.63f, "10",    4, -10 },
        { 0.89f, "20",    0, -14 },
        { 1.00f, "25W", -12, -14 }
    };
    for (int i = 0; i < 5; i++) {
        float t = refTicks[i].t;
        int px = (int)((1 - t) * (1 - t) * (W - 45) + 2 * (1 - t) * t * (W - 155) + t * t * 170);
        int py = (int)((1 - t) * (1 - t) * 210 + 2 * (1 - t) * t * 80 + t * t * 40);
        canvas->fillCircle(px, py, 2, C_BRIGHT_RED);
        canvas->setTextColor(C_LIGHT_GRAY);
        canvas->setCursor(px + refTicks[i].ox, py + refTicks[i].oy);
        canvas->print(refTicks[i].lbl);
    }

    // Scale Headers
    canvas->setFont(&FreeSansBold9pt7b);
    canvas->setTextColor(C_NEON_GREEN);
    canvas->setCursor(22, 28);
    canvas->print("FORWARD");

    canvas->setTextColor(C_BRIGHT_RED);
    canvas->setCursor(350, 28);
    canvas->print("REFLECTED");

    // Title
    canvas->setFont(&FreeSansBold9pt7b);
    canvas->setTextColor(C_CYAN);
    canvas->setCursor(185, 24);
    canvas->print("CROSS-NEEDLE");

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
    canvas->setTextColor(C_AMBER);
    canvas->setCursor(16, 20);
    canvas->print("RETRO CRT PHOSPHOR");

    canvas->setFont(&FreeSans9pt7b);
    canvas->setCursor(220, 20);
    canvas->printf("RSSI: %d dBm", tele.rssi);

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
// SETUP
// =========================================================================
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n=== 4.3\" SWR & POWER MONITOR DESKTOP STATION ===");

    // 1. Turn on LCD Backlight
    pinMode(GFX_BL, OUTPUT);
    digitalWrite(GFX_BL, HIGH);

    // 2. Initialize Canvas & Display
    canvas->begin();
    canvas->fillScreen(C_BLACK);

    // Initial Splash Screen (Smooth fonts)
    canvas->setFont(&FreeSansBold12pt7b);
    canvas->setTextColor(C_CYAN);
    canvas->setCursor(60, 90);
    canvas->print("SWR & POWER MONITOR");

    canvas->setFont(&FreeSans9pt7b);
    canvas->setTextColor(C_WHITE);
    canvas->setCursor(60, 130);
    canvas->printf("Connecting WiFi: %s...", WIFI_SSID);

    canvas->setTextColor(C_GRAY);
    canvas->setCursor(60, 160);
    canvas->printf("Target: %s:%d", SWR_HOSTS[0], SWR_PORT);

    canvas->flush();

    // 3. Initialize Capacitive Touch
    tp.begin();
    tp.setRotation(ROTATION_NORMAL);

    // 4. Start Background Telemetry Task on Core 0
    xTaskCreatePinnedToCore(
        telemetryTask,
        "TelemetryTask",
        8192,
        NULL,
        1,
        NULL,
        0 // Core 0
    );

    delay(600);
}

// =========================================================================
// MAIN RENDER LOOP (CORE 1)
// =========================================================================
void loop() {
    unsigned long now = millis();

    // 1. Process Touch Input (Switch Themes)
    tp.read();
    if (tp.isTouched && (now - lastTouchTime > 400)) {
        lastTouchTime = now;
        currentTheme = (DisplayTheme)((currentTheme + 1) % THEME_COUNT);
    }

    // 2. Render Active Theme into PSRAM Canvas
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

    // 3. Instant Tear-Free Blit to Display!
    canvas->flush();

    delay(25);
}
