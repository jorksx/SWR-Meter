#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include <TAMC_GT911.h>
#include <math.h>

// =========================================================================
// WIFI & TARGET SWR METER CONFIGURATION
// =========================================================================
const char* WIFI_SSID = "Kuitunen";
const char* WIFI_PSK  = "12345678";

// Target endpoints to query for live telemetry
const char* TARGET_HOSTS[] = {
    "http://192.168.1.159:2222/data",
    "http://swrmeter.local:2222/data",
    "http://192.168.4.1:2222/data"
};
const int TARGET_HOST_COUNT = 3;

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

// PSRAM Full Frame Canvas for 100% Tear-Free, Flicker-Free Rendering!
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
// RGB565 COLOR DEFINITIONS
// =========================================================================
#define C_BG           0x0842  // Very dark navy background (#080c14)
#define C_CARD_BG      0x10C6  // Card background (#121824)
#define C_CARD_BORDER  0x1949  // Border (#1a2334)
#define C_HEADER       0x08A4  // Top bar background (#090d14)
#define C_WHITE        0xFFFF
#define C_BLACK        0x0000
#define C_CYAN         0x07FF  // #00e5ff
#define C_GREEN        0x07E0  // #00ff00
#define C_NEON_GREEN   0x07EC  // #00ff88
#define C_YELLOW       0xFFE0  // #ffff00
#define C_ORANGE       0xFD20  // #ff9900
#define C_RED          0xF800  // #ff0000
#define C_BRIGHT_RED   0xF904  // #ff1e56
#define C_GRAY         0x6392  // #62728d
#define C_DARK_GRAY    0x2124  // #20242c
#define C_AMBER        0xFD00  // #ffaa00

// =========================================================================
// SHARED TELEMETRY DATA
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

// Themes
enum DisplayTheme {
    THEME_MODERN = 0,
    THEME_CROSS_NEEDLE,
    THEME_RETRO_AMBER,
    THEME_COUNT
};
DisplayTheme currentTheme = THEME_MODERN;

unsigned long lastTouchTime = 0;
String activeHostUrl = TARGET_HOSTS[0];

// =========================================================================
// HELPER FUNCTIONS: COLOR MAPPING & DRAWING
// =========================================================================
uint16_t getPowerColor(float w) {
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

void drawGradientHBar(int x, int y, int w, int h, float val, float max_val, float peak_val = 0.0f) {
    canvas->drawRect(x, y, w, h, C_CARD_BORDER);
    int innerW = w - 2;
    int innerH = h - 2;
    if (innerW <= 0 || innerH <= 0) return;

    canvas->fillRect(x + 1, y + 1, innerW, innerH, C_BLACK);

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
                uint8_t g = 63;
                col = (r << 11) | (g << 5) | 0;
            } else {
                // Yellow to Red
                uint8_t r = 31;
                uint8_t g = (uint8_t)((1.0f - (t - 0.5f) * 2.0f) * 63);
                col = (r << 11) | (g << 5) | 0;
            }
            canvas->drawFastVLine(x + 1 + i, y + 1, innerH, col);
        }
    }

    // Peak hold line
    if (peak_val > 0.0f) {
        float peakRatio = peak_val / max_val;
        if (peakRatio > 1.0f) peakRatio = 1.0f;
        int peakX = x + 1 + (int)(peakRatio * (innerW - 1));
        canvas->drawFastVLine(peakX, y + 1, innerH, C_WHITE);
    }
}

void drawSwrBar(int x, int y, int w, int h, float s) {
    canvas->drawRect(x, y, w, h, C_CARD_BORDER);
    int innerW = w - 2;
    int innerH = h - 2;
    canvas->fillRect(x + 1, y + 1, innerW, innerH, C_BLACK);

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
            if (t < 0.35f) {
                col = C_NEON_GREEN;
            } else if (t < 0.65f) {
                col = C_YELLOW;
            } else if (t < 0.85f) {
                col = C_ORANGE;
            } else {
                col = C_BRIGHT_RED;
            }
            canvas->drawFastVLine(x + 1 + i, y + 1, innerH, col);
        }
    }
}

// =========================================================================
// BACKGROUND TELEMETRY TASK (CORE 0)
// =========================================================================
void telemetryTask(void *pvParameters) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PSK);

    HTTPClient http;
    http.setReuse(true); // HTTP Keep-Alive for low-latency continuous polling
    http.setTimeout(400);

    int hostIdx = 0;
    int failCount = 0;

    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            tele.connected = false;
            tele.rssi = 0;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        tele.rssi = WiFi.RSSI();

        String url = TARGET_HOSTS[hostIdx];
        activeHostUrl = url;

        http.begin(url);
        int httpCode = http.GET();

        if (httpCode == 200) {
            String payload = http.getString();
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

                // Smart PA SWR estimation (Foldback curve)
                float fwd = tele.fwd_w;
                float line_s = tele.swr;
                if (fwd < 5.0f) {
                    tele.pa_swr = 1.0f;
                } else if (fwd >= 80.0f) {
                    tele.pa_swr = 1.0f; // ATU matched!
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
        } else {
            failCount++;
            if (failCount > 6) {
                tele.connected = false;
                // Try next host candidate
                hostIdx = (hostIdx + 1) % TARGET_HOST_COUNT;
                failCount = 0;
            }
        }
        http.end();

        // 60ms interval (~16 FPS telemetry refresh rate)
        vTaskDelay(pdMS_TO_TICKS(60));
    }
}

// =========================================================================
// THEME 1: MODERN DIGITAL DASHBOARD (ICOM STYLE)
// =========================================================================
void renderModernTheme() {
    canvas->fillScreen(C_BG);

    // 1. TOP HEADER (y: 0..26)
    canvas->fillRect(0, 0, 480, 26, C_HEADER);
    canvas->drawFastHLine(0, 26, 480, C_CARD_BORDER);

    // Theme Switch Button
    canvas->fillRect(6, 3, 90, 20, C_CARD_BG);
    canvas->drawRect(6, 3, 90, 20, C_CARD_BORDER);
    canvas->setTextSize(1);
    canvas->setTextColor(C_CYAN);
    canvas->setCursor(14, 9);
    canvas->print("DIGITAL");

    // Wi-Fi Status & RSSI
    canvas->setTextColor(tele.connected ? C_NEON_GREEN : C_YELLOW);
    canvas->setCursor(115, 9);
    if (WiFi.status() == WL_CONNECTED) {
        canvas->printf("WIFI: %d dBm", tele.rssi);
    } else {
        canvas->print("WIFI: CONNECTING...");
    }

    // Connection Status
    canvas->setCursor(240, 9);
    if (tele.connected) {
        canvas->setTextColor(C_NEON_GREEN);
        canvas->print("SWR-METER ONLINE");
    } else {
        canvas->setTextColor(C_AMBER);
        canvas->print("STANDBY / SEARCHING");
    }

    // TX / RX Badge
    if (tele.is_tx) {
        canvas->fillRect(418, 3, 56, 20, C_BRIGHT_RED);
        canvas->setTextColor(C_WHITE);
        canvas->setTextSize(1);
        canvas->setCursor(434, 9);
        canvas->print("TX");
    } else {
        canvas->fillRect(418, 3, 56, 20, 0x11A8);
        canvas->setTextColor(C_CYAN);
        canvas->setTextSize(1);
        canvas->setCursor(434, 9);
        canvas->print("RX");
    }

    // 2. FORWARD POWER CARD (y: 31..128, h: 98)
    canvas->fillRect(6, 31, 468, 98, C_CARD_BG);
    canvas->drawRect(6, 31, 468, 98, C_CARD_BORDER);

    canvas->setTextSize(1);
    canvas->setTextColor(C_GRAY);
    canvas->setCursor(16, 38);
    canvas->print("FORWARD POWER");

    canvas->setCursor(300, 38);
    canvas->printf("AVG: %5.1f W   PEP: %5.1f W", tele.avg_w, tele.peak_w);

    // Big Power Digits
    uint16_t pwrColor = getPowerColor(tele.fwd_w);
    canvas->setTextSize(4);
    canvas->setTextColor(pwrColor);
    canvas->setCursor(16, 52);
    canvas->printf("%5.1f", tele.fwd_w);

    canvas->setTextSize(2);
    canvas->setTextColor(C_GRAY);
    canvas->setCursor(160, 68);
    canvas->print("W");

    // Power Gradient Bars (PEP and AVG)
    drawGradientHBar(195, 56, 265, 14, tele.fwd_w, 120.0f, tele.peak_w);
    drawGradientHBar(195, 76, 265, 10, tele.avg_w, 120.0f);

    // Scale ticks
    canvas->setTextSize(1);
    canvas->setTextColor(C_GRAY);
    canvas->setCursor(195, 92);
    canvas->print("0W      25W      50W      75W     100W    120W");

    // 3. SWR CARD (y: 133..232, h: 100)
    canvas->fillRect(6, 133, 468, 100, C_CARD_BG);
    canvas->drawRect(6, 133, 468, 100, C_CARD_BORDER);

    canvas->setTextSize(1);
    canvas->setTextColor(C_GRAY);
    canvas->setCursor(16, 140);
    canvas->print("ANTENNA SWR");

    canvas->setCursor(330, 140);
    canvas->print("PA / RIG SWR");

    // Big SWR Digits (Antenna Line SWR)
    uint16_t swrColor = getSwrColor(tele.swr);
    canvas->setTextSize(4);
    canvas->setTextColor(swrColor);
    canvas->setCursor(16, 155);
    if (tele.swr > 90.0f) {
        canvas->print(" >99");
    } else {
        canvas->printf("%4.2f", tele.swr);
    }

    canvas->setTextSize(2);
    canvas->setTextColor(C_GRAY);
    canvas->setCursor(136, 171);
    canvas->print(":1");

    // PA SWR Digits (Right Side)
    uint16_t paColor = getSwrColor(tele.pa_swr);
    canvas->setTextSize(3);
    canvas->setTextColor(paColor);
    canvas->setCursor(330, 158);
    canvas->printf("%4.2f :1", tele.pa_swr);

    // Dual SWR Bars
    drawSwrBar(16, 200, 448, 12, tele.swr);
    drawSwrBar(16, 216, 448, 6, tele.pa_swr);

    // SWR scale numbers
    canvas->setTextSize(1);
    canvas->setTextColor(C_GRAY);
    canvas->setCursor(16, 188);
    canvas->print("1.0     1.2      1.5      2.0      2.5      3.0      4.0+");

    // 4. FOOTER (y: 236..268, h: 32)
    canvas->fillRect(6, 236, 468, 32, C_CARD_BG);
    canvas->drawRect(6, 236, 468, 32, C_CARD_BORDER);

    canvas->setTextSize(1);
    canvas->setTextColor(C_WHITE);
    canvas->setCursor(16, 247);
    canvas->printf("REFLECTED: %5.2f W", tele.rev_w);

    canvas->setTextColor(C_AMBER);
    canvas->setCursor(190, 247);
    canvas->printf("PEAK: %5.1f W", tele.peak_w);

    canvas->setTextColor(C_GRAY);
    canvas->setCursor(340, 247);
    canvas->printf("DET: %4.2fV / %4.2fV", tele.fwd_v, tele.rev_v);
}

// =========================================================================
// THEME 2: CROSS-NEEDLE ANALOG METER
// =========================================================================
void renderCrossNeedleTheme() {
    canvas->fillScreen(0x0000); // Pure deep black meter face

    // Header
    canvas->fillRect(0, 0, 480, 24, C_HEADER);
    canvas->drawFastHLine(0, 24, 480, C_CARD_BORDER);

    canvas->fillRect(6, 2, 110, 20, C_CARD_BG);
    canvas->drawRect(6, 2, 110, 20, C_CARD_BORDER);
    canvas->setTextSize(1);
    canvas->setTextColor(C_AMBER);
    canvas->setCursor(12, 8);
    canvas->print("CROSS-NEEDLE");

    canvas->setTextColor(tele.connected ? C_NEON_GREEN : C_YELLOW);
    canvas->setCursor(130, 8);
    canvas->printf("RSSI: %d dBm", tele.rssi);

    canvas->setTextColor(tele.is_tx ? C_BRIGHT_RED : C_CYAN);
    canvas->setCursor(420, 8);
    canvas->print(tele.is_tx ? "[ TX ]" : "[ RX ]");

    // Meter Dial Background Arc
    int pivotLeftX = 80;
    int pivotLeftY = 245;
    int pivotRightX = 400;
    int pivotRightY = 245;
    int needleLen = 210;

    // SWR Reference Lines (Hyperbolic arcs)
    const float swrRefs[] = { 1.0f, 1.2f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f };
    for (float sRef : swrRefs) {
        float gamma = (sRef - 1.0f) / (sRef + 1.0f);
        int prevX = -1, prevY = -1;
        for (int p = 5; p <= 120; p += 8) {
            float pf = (float)p;
            float pr = pf * gamma * gamma;
            float tF = sqrtf(pf / 120.0f);
            float aF = 0.25f + tF * 0.95f; // Radians
            int xF = pivotLeftX + (int)(needleLen * 0.85f * cosf(aF));
            int yF = pivotLeftY - (int)(needleLen * 0.85f * sinf(aF));

            float tR = sqrtf(pr / 30.0f);
            if (tR > 1.0f) tR = 1.0f;
            float aR = 3.14159f - (0.25f + tR * 0.95f);
            int xR = pivotRightX + (int)(needleLen * 0.85f * cosf(aR));
            int yR = pivotRightY - (int)(needleLen * 0.85f * sinf(aR));

            int midX = (xF + xR) / 2;
            int midY = (yF + yR) / 2;
            if (prevX != -1) {
                canvas->drawLine(prevX, prevY, midX, midY, 0x3186); // Subtle slate line
            }
            prevX = midX;
            prevY = midY;
        }
    }

    // Scale texts
    canvas->setTextSize(1);
    canvas->setTextColor(C_NEON_GREEN);
    canvas->setCursor(45, 60);
    canvas->print("FWD (W)");
    canvas->setCursor(40, 75);
    canvas->print("0 20 50 100 120");

    canvas->setTextColor(C_BRIGHT_RED);
    canvas->setCursor(380, 60);
    canvas->print("REF (W)");
    canvas->setCursor(375, 75);
    canvas->print("30 20 10 5 0");

    // SWR labels in the center
    canvas->setTextColor(0x7BEF);
    canvas->setCursor(225, 65);
    canvas->print("SWR");
    canvas->setCursor(205, 85);
    canvas->print("1.0 1.5 2.0 3.0");

    // Smooth mechanical needle damping
    float targetFwd = tele.fwd_w;
    float targetRev = tele.rev_w;
    damped_fwd += 0.24f * (targetFwd - damped_fwd);
    damped_rev += 0.24f * (targetRev - damped_rev);
    if (damped_fwd < 0.05f) damped_fwd = 0.0f;
    if (damped_rev < 0.05f) damped_rev = 0.0f;

    // Calculate Forward Needle Coordinates (Left pivot swinging right)
    float tF = sqrtf(damped_fwd / 120.0f);
    if (tF > 1.0f) tF = 1.0f;
    float angF = 0.25f + tF * 1.00f; // 0.25 to 1.25 radians
    int endLeftX = pivotLeftX + (int)(needleLen * cosf(angF));
    int endLeftY = pivotLeftY - (int)(needleLen * sinf(angF));

    // Calculate Reflected Needle Coordinates (Right pivot swinging left)
    float tR = sqrtf(damped_rev / 30.0f);
    if (tR > 1.0f) tR = 1.0f;
    float angR = 3.14159f - (0.25f + tR * 1.00f);
    int endRightX = pivotRightX + (int)(needleLen * cosf(angR));
    int endRightY = pivotRightY - (int)(needleLen * sinf(angR));

    // Draw Needles (2px wide lines)
    canvas->drawLine(pivotLeftX, pivotLeftY, endLeftX, endLeftY, C_NEON_GREEN);
    canvas->drawLine(pivotLeftX + 1, pivotLeftY, endLeftX + 1, endLeftY, C_NEON_GREEN);

    canvas->drawLine(pivotRightX, pivotRightY, endRightX, endRightY, C_BRIGHT_RED);
    canvas->drawLine(pivotRightX + 1, pivotRightY, endRightX + 1, endRightY, C_BRIGHT_RED);

    // Calculate and draw intersection point
    float dxF = endLeftX - pivotLeftX;
    float dyF = endLeftY - pivotLeftY;
    float dxR = endRightX - pivotRightX;
    float dyR = endRightY - pivotRightY;
    float denom = dxF * (-dyR) - dyF * (-dxR);

    if (fabsf(denom) > 1e-4) {
        float u = ((pivotRightX - pivotLeftX) * (-dyR) - (pivotRightY - pivotLeftY) * (-dxR)) / denom;
        if (u >= 0.15f && u <= 1.05f) {
            int crossX = pivotLeftX + (int)(u * dxF);
            int crossY = pivotLeftY + (int)(u * dyF);
            canvas->fillCircle(crossX, crossY, 6, C_AMBER);
            canvas->fillCircle(crossX, crossY, 2, C_WHITE);
        }
    }

    // Pivot Hubs
    canvas->fillCircle(pivotLeftX, pivotLeftY, 9, C_DARK_GRAY);
    canvas->drawCircle(pivotLeftX, pivotLeftY, 9, C_WHITE);
    canvas->fillCircle(pivotRightX, pivotRightY, 9, C_DARK_GRAY);
    canvas->drawCircle(pivotRightX, pivotRightY, 9, C_WHITE);

    // Footer readouts
    canvas->fillRect(0, 246, 480, 26, C_HEADER);
    canvas->drawFastHLine(0, 246, 480, C_CARD_BORDER);

    canvas->setTextSize(1);
    canvas->setTextColor(C_NEON_GREEN);
    canvas->setCursor(16, 254);
    canvas->printf("FWD: %5.1f W", tele.fwd_w);

    canvas->setTextColor(getSwrColor(tele.swr));
    canvas->setCursor(185, 254);
    if (tele.swr > 90.0f) {
        canvas->print("SWR: >99 :1");
    } else {
        canvas->printf("SWR: %4.2f :1", tele.swr);
    }

    canvas->setTextColor(C_BRIGHT_RED);
    canvas->setCursor(340, 254);
    canvas->printf("REF: %5.2f W", tele.rev_w);
}

// =========================================================================
// THEME 3: RETRO AMBER CRT RIG
// =========================================================================
void renderRetroAmberTheme() {
    canvas->fillScreen(0x1080); // Very deep amber phosphor tint

    // Header
    canvas->fillRect(0, 0, 480, 24, 0x2100);
    canvas->drawFastHLine(0, 24, 480, C_AMBER);

    canvas->setTextSize(1);
    canvas->setTextColor(C_AMBER);
    canvas->setCursor(12, 8);
    canvas->print("AMBER RETRO RIG");

    canvas->setCursor(200, 8);
    canvas->printf("WIFI: %d dBm", tele.rssi);

    canvas->setCursor(410, 8);
    canvas->print(tele.is_tx ? "[TX ON]" : "[RX]");

    // Large Retro Power
    canvas->fillRect(10, 32, 460, 95, 0x1880);
    canvas->drawRect(10, 32, 460, 95, C_AMBER);

    canvas->setCursor(20, 40);
    canvas->print("TRANSMIT POWER (W)");

    canvas->setTextSize(4);
    canvas->setTextColor(C_AMBER);
    canvas->setCursor(20, 56);
    canvas->printf("%5.1f W", tele.fwd_w);

    // Discrete Amber LED Segments for Power
    int segCount = 30;
    int litSegs = (int)((tele.fwd_w / 120.0f) * segCount);
    for (int i = 0; i < segCount; i++) {
        uint16_t c = (i < litSegs) ? C_AMBER : 0x3180;
        canvas->fillRect(20 + i * 14, 98, 10, 18, c);
    }

    // Large Retro SWR
    canvas->fillRect(10, 134, 460, 95, 0x1880);
    canvas->drawRect(10, 134, 460, 95, C_AMBER);

    canvas->setTextSize(1);
    canvas->setCursor(20, 142);
    canvas->printf("STANDING WAVE RATIO (SWR)              PA SWR: %4.2f :1", tele.pa_swr);

    canvas->setTextSize(4);
    canvas->setTextColor(C_AMBER);
    canvas->setCursor(20, 158);
    if (tele.swr > 90.0f) {
        canvas->print(" >99 :1");
    } else {
        canvas->printf("%4.2f :1", tele.swr);
    }

    // Discrete Amber LED Segments for SWR
    int swrLit = 0;
    if (tele.swr > 1.0f) {
        swrLit = (int)(((tele.swr - 1.0f) / 3.0f) * segCount);
        if (swrLit > segCount) swrLit = segCount;
    }
    for (int i = 0; i < segCount; i++) {
        uint16_t c = (i < swrLit) ? C_AMBER : 0x3180;
        canvas->fillRect(20 + i * 14, 200, 10, 18, c);
    }

    // Footer
    canvas->setTextSize(1);
    canvas->setTextColor(C_AMBER);
    canvas->setCursor(20, 246);
    canvas->printf("REFLECTED: %5.2f W        PEP: %5.1f W        DETECTOR: %4.2f V / %4.2f V",
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
    canvas->fillScreen(C_BG);

    // Initial Splash Screen
    canvas->setTextSize(2);
    canvas->setTextColor(C_CYAN);
    canvas->setCursor(50, 90);
    canvas->print("SWR & POWER MONITOR");

    canvas->setTextSize(1);
    canvas->setTextColor(C_WHITE);
    canvas->setCursor(50, 130);
    canvas->printf("Connecting to WiFi: %s...", WIFI_SSID);

    canvas->setTextColor(C_GRAY);
    canvas->setCursor(50, 155);
    canvas->print("Target: http://192.168.1.159:2222");

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

    delay(800);
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
        case THEME_MODERN:
            renderModernTheme();
            break;
        case THEME_CROSS_NEEDLE:
            renderCrossNeedleTheme();
            break;
        case THEME_RETRO_AMBER:
            renderRetroAmberTheme();
            break;
    }

    // 3. Instant Tear-Free Blit to Display!
    canvas->flush();

    // Maintain smooth 30-35 FPS render loop
    delay(25);
}
