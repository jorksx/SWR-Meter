#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <Preferences.h>

// ==========================================
// PINS & CALIBRATION (CLASSIC ESP32)
// ==========================================
#define PIN_FWD 34
#define PIN_REV 35

// Calibration defaults (adjustable live in Web UI at /cal, saved to NVS)
volatile float pwrCalFactor  = 12.80f; // 100 W = 2.80 V (100 / 2.80^2 = 12.76)
volatile float revCalFactor  = 1.55f;  // Coupler SWR scale multiplier (matches rig)
volatile float dirLeakFactor = 0.192f; // Stockton bridge directivity isolation floor (-14.3 dB)

// ==========================================
// WIFI SETTINGS & NVS STORAGE
// ==========================================
// Default fallback credentials if not yet configured via Web UI (/wifi)
const char* DEFAULT_WIFI_SSID = "Kuitunen";
const char* DEFAULT_WIFI_PASS = "12345678";

// Standalone SoftAP for initial setup or portable / Field Day use
const char* AP_SSID = "SWR-Meter-AP";
const char* AP_PASS = "12345678";

Preferences preferences;
WebServer server(2222);

// Telemetry variables
volatile float currentVfwd = 0.0;
volatile float currentVrev = 0.0;
volatile float currentPfwd = 0.0;
volatile float avgPfwd     = 0.0;  // Smoothed voice power (True AVG / VU)
volatile float peakPfwd    = 0.0;  // Peak Hold (PEP)
volatile float currentPrev = 0.0;
volatile float currentSwr  = 1.0;
volatile bool  isTransmitting = false;
volatile int   lastRawFwd  = 0;
volatile int   lastRawRev  = 0;

unsigned long lastPeakTime = 0;

// ==========================================
// WEB UI (PORT 2222) - CLEAN & ENGLISH
// ==========================================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>SWR & Power Monitor</title>
  <link rel="preconnect" href="https://fonts.googleapis.com">
  <link href="https://fonts.googleapis.com/css2?family=JetBrains+Mono:wght@500;700;800&family=Rajdhani:wght@600;700&display=swap" rel="stylesheet">
  <style>
    :root {
      --bg: #090d14;
      --card-bg: #121824;
      --card-border: #1a2334;
      --text-main: #f0f4fc;
      --text-muted: #62728d;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      background-color: var(--bg);
      color: var(--text-main);
      font-family: 'Rajdhani', sans-serif;
      min-height: 100vh;
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      padding: 1.25rem;
    }
    .container {
      width: 100%;
      max-width: 800px;
      display: flex;
      flex-direction: column;
      gap: 1.25rem;
    }
    .header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      background: var(--card-bg);
      padding: 0.85rem 1.5rem;
      border-radius: 14px;
      border: 1px solid var(--card-border);
    }
    .title-box h1 {
      font-size: 1.4rem;
      font-weight: 700;
      letter-spacing: 2px;
      text-transform: uppercase;
      color: #fff;
    }
    .status-badge {
      font-family: 'JetBrains Mono', monospace;
      font-size: 0.9rem;
      font-weight: 700;
      padding: 0.35rem 0.9rem;
      border-radius: 6px;
      letter-spacing: 1.5px;
      background: #182030;
      color: var(--text-muted);
      border: 1px solid #243048;
      transition: all 0.15s ease;
    }
    .status-badge.tx {
      background: rgba(255, 30, 80, 0.2);
      color: #ff2a6d;
      border-color: #ff2a6d;
      box-shadow: 0 0 14px rgba(255, 42, 109, 0.4);
    }
    .status-badge.rx {
      background: rgba(0, 180, 216, 0.15);
      color: #00b4d8;
      border-color: #00b4d8;
    }
    .wifi-badge {
      font-family: 'JetBrains Mono', monospace;
      font-size: 0.85rem;
      font-weight: 700;
      padding: 0.35rem 0.75rem;
      border-radius: 6px;
      background: #182030;
      color: var(--text-muted);
      border: 1px solid #243048;
      display: flex;
      align-items: center;
      gap: 0.35rem;
      transition: all 0.2s ease;
    }

    .grid {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 1.25rem;
    }
    @media (max-width: 680px) {
      .grid { grid-template-columns: 1fr; }
    }
    .card {
      background: var(--card-bg);
      border: 1px solid var(--card-border);
      border-radius: 16px;
      padding: 1.5rem;
      display: flex;
      flex-direction: column;
    }
    .card-label {
      font-size: 0.95rem;
      text-transform: uppercase;
      letter-spacing: 2px;
      color: var(--text-muted);
      font-weight: 600;
      margin-bottom: 0.25rem;
    }
    .value-display {
      font-family: 'JetBrains Mono', monospace;
      font-size: 3.8rem;
      font-weight: 800;
      line-height: 1;
      margin: 0.25rem 0 0.85rem 0;
      display: flex;
      align-items: baseline;
      gap: 0.5rem;
    }
    .unit {
      font-size: 1.3rem;
      color: var(--text-muted);
      font-weight: 500;
    }

    .bars-wrap {
      display: flex;
      flex-direction: column;
      gap: 0.65rem;
      margin-top: auto;
    }
    .bar-row {
      display: flex;
      flex-direction: column;
      gap: 0.25rem;
    }
    .bar-row-label {
      font-family: 'JetBrains Mono', monospace;
      font-size: 0.95rem;
      font-weight: 700;
      color: var(--text-muted);
      display: flex;
      justify-content: space-between;
    }
    .bar-container {
      width: 100%;
      height: 14px;
      background: #080c14;
      border-radius: 7px;
      overflow: hidden;
      border: 1px solid #1a2334;
      position: relative;
    }
    .bar-fill {
      height: 100%;
      width: 100%;
      position: absolute;
      left: 0;
      top: 0;
      border-radius: 7px;
      clip-path: inset(0 100% 0 0);
    }
    /* Aito liukuväri vasemmalta oikealle: Vihreä -> Keltainen -> Punainen */
    .bar-fill.pep {
      background: linear-gradient(90deg, #00ff88 0%, #a6ff00 25%, #ffea00 46%, #ff8500 68%, #ff1e56 83%, #d00000 100%);
      box-shadow: 0 0 10px rgba(0, 255, 136, 0.25);
      transition: clip-path 0.08s linear;
    }
    .bar-fill.avg {
      background: linear-gradient(90deg, #00ff88 0%, #a6ff00 25%, #ffea00 46%, #ff8500 68%, #ff1e56 83%, #d00000 100%);
      box-shadow: 0 0 10px rgba(0, 255, 136, 0.25);
      transition: clip-path 0.15s ease-out;
    }
    .bar-fill.swr {
      background: linear-gradient(90deg, #00ff88 0%, #56b000 12%, #ffd000 25%, #ff7900 38%, #ff1e56 50%, #c00000 80%);
      box-shadow: 0 0 10px rgba(0, 255, 136, 0.25);
      transition: clip-path 0.08s linear;
    }
    .bar-peak {
      position: absolute;
      top: 0;
      bottom: 0;
      width: 3px;
      background: #ffffff;
      box-shadow: 0 0 6px #ffffff;
      left: 0%;
      transition: left 0.1s ease;
    }

    .footer-card {
      background: var(--card-bg);
      border: 1px solid var(--card-border);
      border-radius: 14px;
      padding: 0.85rem 1.5rem;
      display: flex;
      justify-content: space-around;
      align-items: center;
      font-family: 'JetBrains Mono', monospace;
      font-size: 0.9rem;
    }
    .info-item { display: flex; flex-direction: column; align-items: center; gap: 0.2rem; }
    .info-label { font-size: 0.75rem; color: var(--text-muted); text-transform: uppercase; font-weight: 700; }
    .info-val { font-weight: 800; font-size: 1.15rem; color: #fff; }

    /* Varoitusbanneri */
    .alert-banner {
      display: none;
      background: #ff1e56;
      color: #ffffff;
      font-family: 'JetBrains Mono', monospace;
      font-size: 0.95rem;
      font-weight: 800;
      letter-spacing: 0.5px;
      padding: 0.75rem 1.25rem;
      border-radius: 12px;
      margin-bottom: 1.25rem;
      text-align: center;
      border: 1px solid #ffffff;
      box-shadow: 0 0 20px rgba(255, 30, 86, 0.6);
      animation: alertPulse 0.5s infinite alternate;
    }
    @keyframes alertPulse {
      0% {
        transform: scale(1);
        background: #ff1e56;
        color: #ffffff;
        box-shadow: 0 0 25px rgba(255, 30, 86, 0.7);
      }
      100% {
        transform: scale(1.02);
        background: #ffea00;
        color: #000000;
        box-shadow: 0 0 35px rgba(255, 234, 0, 0.8);
      }
    }
  </style>
</head>
<body>
  <div class="container">
    <div class="header">
      <div class="title-box">
        <h1>SWR & POWER MONITOR</h1>
      </div>
      <div style="display: flex; gap: 0.6rem; align-items: center;">
        <a href="/cal" style="text-decoration: none; color: #62728d; background: #182030; border: 1px solid #243048; border-radius: 6px; padding: 0.35rem 0.75rem; font-size: 0.85rem; font-weight: 700; font-family: 'JetBrains Mono', monospace; display: flex; align-items: center; gap: 4px; transition: all 0.2s;" onmouseover="this.style.color='#fff'; this.style.borderColor='#00ff88'" onmouseout="this.style.color='#62728d'; this.style.borderColor='#243048'">🎯 CAL</a>
        <a href="/wifi" style="text-decoration: none; color: #62728d; background: #182030; border: 1px solid #243048; border-radius: 6px; padding: 0.35rem 0.75rem; font-size: 0.85rem; font-weight: 700; font-family: 'JetBrains Mono', monospace; display: flex; align-items: center; gap: 4px; transition: all 0.2s;" onmouseover="this.style.color='#fff'; this.style.borderColor='#00ff88'" onmouseout="this.style.color='#62728d'; this.style.borderColor='#243048'">⚙️ WIFI</a>
        <div id="wifi-badge" class="wifi-badge">📶 <span id="rssi-val">--</span> dBm</div>
        <div id="tx-badge" class="status-badge rx">RX</div>
      </div>
    </div>

    <!-- ALERT BANNER -->
    <div id="alert-banner" class="alert-banner">
      ⚠️ <span id="alert-msg">WARNING</span>
    </div>

    <div class="grid">
      <!-- FORWARD POWER -->
      <div class="card">
        <div class="card-label">Forward power</div>
        <div class="value-display">
          <span id="pwr-val" style="color: #00ff88;">0.0</span>
          <span class="unit">W</span>
        </div>
        
        <div class="bars-wrap">
          <!-- PEP -->
          <div class="bar-row">
            <div class="bar-row-label">
              <span>PEP</span>
              <span id="pep-num" style="color: #00ff88;">0.0 W</span>
            </div>
            <div class="bar-container">
              <div id="pwr-bar" class="bar-fill pep"></div>
              <div id="pwr-peak" class="bar-peak"></div>
            </div>
          </div>

          <!-- AVG -->
          <div class="bar-row">
            <div class="bar-row-label">
              <span>AVG</span>
              <span id="avg-num" style="color: #ff007f;">0.0 W</span>
            </div>
            <div class="bar-container" style="height: 11px;">
              <div id="avg-bar" class="bar-fill avg"></div>
            </div>
          </div>
        </div>
      </div>

      <!-- SWR -->
      <div class="card">
        <div class="card-label">SWR</div>
        <div class="value-display">
          <span id="swr-val" style="color: #00ff88;">1.00</span>
          <span class="unit">:1</span>
        </div>
        
        <div class="bars-wrap" style="margin-top: auto;">
          <div class="bar-row">
            <div class="bar-row-label">
              <span>SWR</span>
              <span id="swr-sub">1.0:1</span>
            </div>
            <div class="bar-container">
              <div id="swr-bar" class="bar-fill swr"></div>
            </div>
          </div>
        </div>
      </div>
    </div>

    <!-- FOOTER -->
    <div class="footer-card">
      <div class="info-item">
        <span class="info-label">REFLECTED POWER</span>
        <span id="rev-val" class="info-val">0.00 W</span>
      </div>
      <div class="info-item">
        <span class="info-label">PEAK</span>
        <span id="peak-val" class="info-val">0.0 W</span>
      </div>
      <div class="info-item">
        <span class="info-label">V_FWD / V_REV</span>
        <span id="volt-val" class="info-val">0.00V / 0.00V</span>
      </div>
    </div>
  </div>

  <script>
    const pwrVal  = document.getElementById('pwr-val');
    const pwrBar  = document.getElementById('pwr-bar');
    const pwrPeak = document.getElementById('pwr-peak');
    const pepNum  = document.getElementById('pep-num');
    const avgBar  = document.getElementById('avg-bar');
    const avgNum  = document.getElementById('avg-num');
    const swrVal  = document.getElementById('swr-val');
    const swrSub  = document.getElementById('swr-sub');
    const swrBar  = document.getElementById('swr-bar');
    const revVal  = document.getElementById('rev-val');
    const peakVal = document.getElementById('peak-val');
    const voltVal = document.getElementById('volt-val');
    const txBadge = document.getElementById('tx-badge');
    const rssiVal = document.getElementById('rssi-val');
    const alertBanner = document.getElementById('alert-banner');
    const alertMsg = document.getElementById('alert-msg');
    let prevSwr = 1.0;
    let alertUntil = 0;
    let wasTxState = false;
    let txStartTime = 0;

    // Teho -> Liukuva väri vihreästä keltaisen kautta punaiseen (0 - 100W)
    function getPwrColor(w) {
      if (w <= 0) return '#00ff88';
      if (w >= 100) return '#ff1e56';
      const ratio = Math.min(1.0, Math.max(0.0, w / 100.0));
      const hue = Math.max(0, 145 - (ratio * 145));
      return `hsl(${hue}, 100%, 50%)`;
    }

    // SWR -> Liukuva väri vihreästä keltaisen kautta punaiseen (1.0 - 3.0)
    function getSwrColor(s) {
      if (s <= 1.0) return '#00ff88';
      if (s >= 3.0) return '#ff1e56';
      // Lasketaan väri HSL:llä: 1.0 = 145 (vihreä), 3.0 = 0 (punainen)
      const ratio = Math.min(1.0, (s - 1.0) / 2.0); // 0.0 -> 1.0
      const hue = Math.max(0, 145 - (ratio * 145));
      return `hsl(${hue}, 100%, 50%)`;
    }

    let isFetching = false;

    async function fetchData() {
      if (isFetching) return;
      isFetching = true;

      const controller = new AbortController();
      const timeoutId = setTimeout(() => controller.abort(), 400);

      try {
        const res = await fetch('/data', { signal: controller.signal });
        if (!res.ok) throw new Error();
        const d = await res.json();

        if (d.rssi !== undefined) {
          rssiVal.textContent = d.rssi;
          if (d.rssi >= -67) {
            rssiVal.style.color = '#00ff88';
          } else if (d.rssi >= -78) {
            rssiVal.style.color = '#ffd166';
          } else {
            rssiVal.style.color = '#ff1e56';
          }
        }

        if (d.tx) {
          txBadge.textContent = 'TX';
          txBadge.className = 'status-badge tx';
        } else {
          txBadge.textContent = 'RX';
          txBadge.className = 'status-badge rx';
        }

        const pwrColor = getPwrColor(d.fwd_w);
        const avgColor = getPwrColor(d.avg_w);

        pwrVal.textContent = d.fwd_w.toFixed(1);
        pwrVal.style.color = pwrColor;

        pepNum.textContent = d.fwd_w.toFixed(1) + ' W';
        pepNum.style.color = pwrColor;

        avgNum.textContent = d.avg_w.toFixed(1) + ' W';
        avgNum.style.color = avgColor;

        revVal.textContent = d.rev_w.toFixed(2) + ' W';
        peakVal.textContent = d.peak_w.toFixed(1) + ' W';
        voltVal.textContent = d.fwd_v.toFixed(2) + 'V / ' + d.rev_v.toFixed(2) + 'V';

        // Palkit (0 - 120 W)
        let pwrPct = Math.min(100, (d.fwd_w / 120.0) * 100);
        let avgPct = Math.min(100, (d.avg_w / 120.0) * 100);
        let peakPct = Math.min(100, (d.peak_w / 120.0) * 100);
        
        pwrBar.style.clipPath = `inset(0 ${Math.max(0, 100 - pwrPct)}% 0 0)`;
        avgBar.style.clipPath = `inset(0 ${Math.max(0, 100 - avgPct)}% 0 0)`;
        pwrPeak.style.left = peakPct + '%';

        // SWR Liukuva väri ja aito gradienttipalkki
        const sColor = getSwrColor(d.swr);
        if (d.swr > 90) {
          swrVal.textContent = '>99';
          swrSub.textContent = '>99:1';
          swrVal.style.color = '#ff1e56';
          swrSub.style.color = '#ff1e56';
          swrBar.style.clipPath = 'inset(0 0% 0 0)';
        } else {
          swrVal.textContent = d.swr.toFixed(2);
          swrSub.textContent = d.swr.toFixed(2) + ':1';
          let swrPct = Math.min(100, ((d.swr - 1.0) / 4.0) * 100);
          swrBar.style.clipPath = `inset(0 ${Math.max(0, 100 - Math.max(1, swrPct))}% 0 0)`;
          swrVal.style.color = sColor;
          swrSub.style.color = sColor;
        }

        // Anomaliavalvonta ja varoitusbanneri (300ms pehmennys kytkentätransienteille)
        const nowMs = Date.now();
        let anomaly = null;
        if (d.tx) {
          if (!wasTxState) {
            txStartTime = nowMs;
            wasTxState = true;
          }
          const txDuration = nowMs - txStartTime;

          // Tarkistetaan anomaliat vasta kun lähetys on kestänyt yli 300 ms (ei reagoida releiden kytkentäpiikkeihin)
          if (txDuration > 300 && d.fwd_w > 8.0) {
            if (d.rev_v > (d.fwd_v * 1.05) && d.rev_v > 0.15) {
              anomaly = `ANOMALIA: REV-JÄNNITE (${d.rev_v.toFixed(2)}V) > FWD (${d.fwd_v.toFixed(2)}V)!`;
            } else if (d.fwd_v >= 3.05) {
              anomaly = 'VAROITUS: ADC-LEIKKAUS (>3.05V) - TEHO LIIAN SUURI!';
            } else if ((d.swr - prevSwr) >= 0.85 && prevSwr < 2.5) {
              anomaly = `ÄKILLINEN SWR-HYPPY (+${(d.swr - prevSwr).toFixed(2)} -> ${d.swr.toFixed(2)}:1)!`;
            } else if (d.swr >= 3.0) {
              anomaly = `KORKEA SWR (${d.swr.toFixed(2)}:1) - TARKISTA ANTENNI!`;
            }
            prevSwr = d.swr;
          }
        } else {
          wasTxState = false;
          prevSwr = 1.0;
        }

        if (anomaly) {
          alertMsg.textContent = anomaly;
          alertUntil = nowMs + 4000;
        }

        if (nowMs < alertUntil) {
          alertBanner.style.display = 'block';
        } else {
          alertBanner.style.display = 'none';
        }
      } catch (err) {
        rssiVal.textContent = '--';
        rssiVal.style.color = 'var(--text-muted)';
      } finally {
        clearTimeout(timeoutId);
        isFetching = false;
      }
    }

    setInterval(fetchData, 80);
  </script>
</body>
</html>
)rawliteral";

const char WIFI_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>WiFi Setup - SWR & Power Monitor</title>
  <link rel="preconnect" href="https://fonts.googleapis.com">
  <link href="https://fonts.googleapis.com/css2?family=JetBrains+Mono:wght@500;700&family=Rajdhani:wght@600;700&display=swap" rel="stylesheet">
  <style>
    :root {
      --bg: #090d14;
      --card-bg: #121824;
      --card-border: #1a2334;
      --accent: #00ff88;
      --text-main: #f0f4fc;
      --text-muted: #62728d;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      background-color: var(--bg);
      color: var(--text-main);
      font-family: 'Rajdhani', sans-serif;
      min-height: 100vh;
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      padding: 1.5rem;
    }
    .card {
      background: var(--card-bg);
      border: 1px solid var(--card-border);
      border-radius: 14px;
      padding: 2rem;
      width: 100%;
      max-width: 440px;
      box-shadow: 0 10px 30px rgba(0,0,0,0.5);
    }
    h2 {
      font-size: 1.5rem;
      letter-spacing: 2px;
      text-transform: uppercase;
      margin-bottom: 0.5rem;
      color: #fff;
    }
    p {
      color: var(--text-muted);
      font-size: 0.95rem;
      margin-bottom: 1.5rem;
      line-height: 1.4;
    }
    label {
      display: block;
      font-size: 0.85rem;
      font-weight: 700;
      color: var(--text-muted);
      letter-spacing: 1px;
      text-transform: uppercase;
      margin-bottom: 0.4rem;
    }
    input[type="text"], input[type="password"] {
      width: 100%;
      padding: 0.75rem 1rem;
      background: #090d14;
      border: 1px solid #243048;
      border-radius: 8px;
      color: #fff;
      font-family: 'JetBrains Mono', monospace;
      font-size: 1rem;
      margin-bottom: 1.25rem;
      outline: none;
      transition: border-color 0.2s;
    }
    input:focus {
      border-color: var(--accent);
    }
    .btn {
      width: 100%;
      padding: 0.85rem;
      background: #00ff88;
      color: #090d14;
      font-family: 'Rajdhani', sans-serif;
      font-size: 1.1rem;
      font-weight: 700;
      letter-spacing: 1px;
      border: none;
      border-radius: 8px;
      cursor: pointer;
      text-transform: uppercase;
      transition: background 0.2s, transform 0.1s;
    }
    .btn:hover {
      background: #00e077;
    }
    .btn:active {
      transform: scale(0.98);
    }
    .back-link {
      display: block;
      text-align: center;
      margin-top: 1.25rem;
      color: var(--text-muted);
      text-decoration: none;
      font-size: 0.9rem;
      font-weight: 600;
      transition: color 0.2s;
    }
    .back-link:hover {
      color: #fff;
    }
  </style>
</head>
<body>
  <div class="card">
    <h2>⚙️ WiFi Configuration</h2>
    <p>Configure your local network SSID and password. Settings are permanently stored in ESP32 flash memory (NVS).</p>
    <form action="/save_wifi" method="POST">
      <label for="ssid">WiFi Network Name (SSID)</label>
      <input type="text" id="ssid" name="ssid" required placeholder="Enter WiFi SSID" value="{CURRENT_SSID}">
      
      <label for="pass">WiFi Password</label>
      <input type="password" id="pass" name="pass" placeholder="Enter WiFi Password">
      
      <button type="submit" class="btn">💾 Save & Reboot</button>
    </form>
    <a href="/" class="back-link">← Return to Monitor</a>
  </div>
</body>
</html>
)rawliteral";

// ==========================================
// REST ROUTES
// ==========================================
void handleWifi() {
  preferences.begin("swr_cfg", true);
  String curSsid = preferences.getString("ssid", DEFAULT_WIFI_SSID);
  preferences.end();

  String html = FPSTR(WIFI_HTML);
  html.replace("{CURRENT_SSID}", curSsid);
  server.sendHeader("Connection", "close");
  server.send(200, "text/html", html);
}

void handleSaveWifi() {
  if (server.hasArg("ssid")) {
    String newSsid = server.arg("ssid");
    String newPass = server.arg("pass");
    newSsid.trim();
    newPass.trim();

    preferences.begin("swr_cfg", false);
    preferences.putString("ssid", newSsid);
    preferences.putString("pass", newPass);
    preferences.end();

    String resp = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>"
                  "<title>Saved</title><style>body{background:#090d14;color:#f0f4fc;font-family:sans-serif;display:flex;align-items:center;justify-content:center;height:100vh;margin:0;}"
                  ".box{background:#121824;border:1px solid #1a2334;border-radius:12px;padding:2rem;text-align:center;max-width:420px;box-shadow:0 10px 30px rgba(0,0,0,0.5);}"
                  "h2{color:#00ff88;margin-bottom:0.75rem;}p{color:#8fa0b5;line-height:1.5;}</style></head>"
                  "<body><div class='box'><h2>✅ WiFi Settings Saved!</h2>"
                  "<p>Connecting to <b>" + newSsid + "</b>.<br>Device is rebooting in 2 seconds...</p>"
                  "<p style='font-size:0.85rem;color:#62728d;margin-top:1.5rem;'>Reconnecting to <b>http://swrmeter.local:2222</b> or your router's assigned IP.</p></div>"
                  "<script>setTimeout(function(){ window.location.href = 'http://swrmeter.local:2222'; }, 4000);</script></body></html>";
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", resp);
    delay(1500);
    ESP.restart();
  } else {
    server.send(400, "text/plain", "Missing SSID parameter");
  }
}

const char CAL_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Calibration - SWR & Power Monitor</title>
  <link rel="preconnect" href="https://fonts.googleapis.com">
  <link href="https://fonts.googleapis.com/css2?family=JetBrains+Mono:wght@500;700&family=Rajdhani:wght@600;700&display=swap" rel="stylesheet">
  <style>
    :root {
      --bg: #090d14;
      --card-bg: #121824;
      --card-border: #1a2334;
      --accent: #00ff88;
      --text-main: #f0f4fc;
      --text-muted: #62728d;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      background-color: var(--bg);
      color: var(--text-main);
      font-family: 'Rajdhani', sans-serif;
      min-height: 100vh;
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      padding: 1.5rem;
    }
    .card {
      background: var(--card-bg);
      border: 1px solid var(--card-border);
      border-radius: 14px;
      padding: 2rem;
      width: 100%;
      max-width: 500px;
      box-shadow: 0 10px 30px rgba(0,0,0,0.5);
    }
    h2 {
      font-size: 1.5rem;
      letter-spacing: 2px;
      text-transform: uppercase;
      margin-bottom: 0.5rem;
      color: #fff;
    }
    .tele-box {
      background: #090d14;
      border: 1px solid #1a2334;
      border-radius: 10px;
      padding: 1rem;
      margin-bottom: 1.5rem;
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 0.75rem;
    }
    .tele-item { display: flex; flex-direction: column; }
    .tele-label { font-size: 0.8rem; color: var(--text-muted); font-weight: 700; text-transform: uppercase; }
    .tele-val { font-family: 'JetBrains Mono', monospace; font-size: 1.3rem; font-weight: 700; color: #fff; }
    label {
      display: block;
      font-size: 0.85rem;
      font-weight: 700;
      color: var(--text-muted);
      letter-spacing: 1px;
      text-transform: uppercase;
      margin-bottom: 0.3rem;
    }
    .hint {
      font-size: 0.8rem;
      color: #55667e;
      margin-bottom: 0.6rem;
      line-height: 1.3;
    }
    input[type="number"] {
      width: 100%;
      padding: 0.75rem 1rem;
      background: #090d14;
      border: 1px solid #243048;
      border-radius: 8px;
      color: #fff;
      font-family: 'JetBrains Mono', monospace;
      font-size: 1.1rem;
      margin-bottom: 0.4rem;
      outline: none;
      transition: border-color 0.2s;
    }
    input:focus { border-color: var(--accent); }
    .btn {
      width: 100%;
      padding: 0.85rem;
      background: #00ff88;
      color: #090d14;
      font-family: 'Rajdhani', sans-serif;
      font-size: 1.1rem;
      font-weight: 700;
      letter-spacing: 1px;
      border: none;
      border-radius: 8px;
      cursor: pointer;
      text-transform: uppercase;
      margin-top: 1rem;
      transition: background 0.2s;
    }
    .btn:hover { background: #00e077; }
    .toast {
      display: none;
      background: #143324;
      border: 1px solid #00ff88;
      color: #00ff88;
      padding: 0.75rem;
      border-radius: 8px;
      text-align: center;
      margin-top: 1rem;
      font-weight: 700;
    }
    .back-link {
      display: block;
      text-align: center;
      margin-top: 1.25rem;
      color: var(--text-muted);
      text-decoration: none;
      font-size: 0.9rem;
      font-weight: 600;
    }
    .back-link:hover { color: #fff; }
  </style>
</head>
<body>
  <div class="card">
    <h2>🎯 Live Calibration</h2>
    
    <div class="tele-box">
      <div class="tele-item">
        <span class="tele-label">FWD Voltage</span>
        <span id="fwd-v" class="tele-val" style="color:#00ff88;">0.00 V</span>
      </div>
      <div class="tele-item">
        <span class="tele-label">REV Voltage</span>
        <span id="rev-v" class="tele-val" style="color:#ff2a6d;">0.00 V</span>
      </div>
      <div class="tele-item">
        <span class="tele-label">Forward Power</span>
        <span id="fwd-p" class="tele-val">0.0 W</span>
      </div>
      <div class="tele-item">
        <span class="tele-label">Calculated SWR</span>
        <span id="swr-val" class="tele-val" style="color:#00e5ff;">1.00:1</span>
      </div>
    </div>

    <form id="cal-form" onsubmit="saveCal(event)">
      <label for="pwr_cal">Forward Power Multiplier (PWR_CAL)</label>
      <div class="hint">Adjusts power scale. 12.80 = 100W at 2.80V (100 / 2.80² ≈ 12.8).</div>
      <input type="number" step="0.01" id="pwr_cal" name="pwr_cal" value="{PWR_CAL}" required>

      <label for="rev_cal" style="margin-top:0.75rem;">SWR Scale Multiplier (REV_CAL)</label>
      <div class="hint">Calibrates SWR sensitivity curve. Default 1.55 matches Rig SWR.</div>
      <input type="number" step="0.01" id="rev_cal" name="rev_cal" value="{REV_CAL}" required>

      <label for="dir_leak" style="margin-top:0.75rem;">Coupler Directivity Isolation (DIR_LEAK)</label>
      <div class="hint">Cancels internal bridge leakage. Default 0.192 (~ -14.3 dB) gives 1.00 on matched loads.</div>
      <input type="number" step="0.001" id="dir_leak" name="dir_leak" value="{DIR_LEAK}" required>

      <button type="submit" class="btn">💾 Save Calibration (Live)</button>
    </form>
    <div id="toast" class="toast">✅ Saved to Flash & applied instantly!</div>
    <a href="/" class="back-link">← Return to Monitor</a>
  </div>

  <script>
    async function updateTele() {
      try {
        const res = await fetch('/data');
        if (res.ok) {
          const d = await res.json();
          document.getElementById('fwd-v').textContent = d.fwd_v.toFixed(2) + ' V';
          document.getElementById('rev-v').textContent = d.rev_v.toFixed(2) + ' V';
          document.getElementById('fwd-p').textContent = d.fwd_w.toFixed(1) + ' W';
          document.getElementById('swr-val').textContent = (d.swr > 90 ? '>99' : d.swr.toFixed(2)) + ':1';
        }
      } catch(e) {}
    }
    setInterval(updateTele, 150);

    async function saveCal(e) {
      e.preventDefault();
      const pwr = document.getElementById('pwr_cal').value;
      const rev = document.getElementById('rev_cal').value;
      const leak = document.getElementById('dir_leak').value;
      
      const params = new URLSearchParams();
      params.append('pwr_cal', pwr);
      params.append('rev_cal', rev);
      params.append('dir_leak', leak);

      try {
        const res = await fetch('/save_cal', { method: 'POST', body: params });
        if (res.ok) {
          const t = document.getElementById('toast');
          t.style.display = 'block';
          setTimeout(() => t.style.display = 'none', 3000);
        }
      } catch(err) {
        alert('Failed to save calibration');
      }
    }
  </script>
</body>
</html>
)rawliteral";

void handleCal() {
  String html = FPSTR(CAL_HTML);
  html.replace("{PWR_CAL}", String(pwrCalFactor, 2));
  html.replace("{REV_CAL}", String(revCalFactor, 2));
  html.replace("{DIR_LEAK}", String(dirLeakFactor, 3));
  server.sendHeader("Connection", "close");
  server.send(200, "text/html", html);
}

void handleSaveCal() {
  if (server.hasArg("pwr_cal") && server.hasArg("rev_cal")) {
    float newPwr = server.arg("pwr_cal").toFloat();
    float newRev = server.arg("rev_cal").toFloat();
    float newLeak = server.hasArg("dir_leak") ? server.arg("dir_leak").toFloat() : 
                   (server.hasArg("rev_offset") ? server.arg("rev_offset").toFloat() / 1000.0f : 0.192f);

    if (newPwr > 0.1f && newPwr < 200.0f) pwrCalFactor = newPwr;
    if (newRev > 0.05f && newRev < 10.0f) revCalFactor = newRev;
    if (newLeak >= 0.0f && newLeak < 0.5f) dirLeakFactor = newLeak;

    preferences.begin("swr_cfg", false);
    preferences.putFloat("pwr_cal", pwrCalFactor);
    preferences.putFloat("rev_cal", revCalFactor);
    preferences.putFloat("dir_leak", dirLeakFactor);
    preferences.end();

    server.sendHeader("Connection", "close");
    server.send(200, "application/json", "{\"status\":\"ok\"}");
  } else {
    server.send(400, "text/plain", "Missing parameters");
  }
}

void handleRoot() {
  server.sendHeader("Connection", "close");
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleData() {
  String json = "{";
  json += "\"fwd_w\":" + String(currentPfwd, 1) + ",";
  json += "\"avg_w\":" + String(avgPfwd, 1) + ",";
  json += "\"rev_w\":" + String(currentPrev, 2) + ",";
  json += "\"peak_w\":" + String(peakPfwd, 1) + ",";
  json += "\"swr\":" + String(currentSwr, 2) + ",";
  json += "\"fwd_v\":" + String(currentVfwd, 3) + ",";
  json += "\"rev_v\":" + String(currentVrev, 3) + ",";
  json += "\"raw_fwd\":" + String(lastRawFwd) + ",";
  json += "\"raw_rev\":" + String(lastRawRev) + ",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"pwr_cal\":" + String(pwrCalFactor, 2) + ",";
  json += "\"rev_cal\":" + String(revCalFactor, 2) + ",";
  json += "\"dir_leak\":" + String(dirLeakFactor, 3) + ",";
  json += "\"filt\":true,";
  json += "\"tx\":" + String(isTransmitting ? "true" : "false");
  json += "}";

  server.sendHeader("Connection", "close");
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

// ==========================================
// SAMPLING TASK (CORE 0)
// ==========================================
void samplingTask(void *pvParameters) {
  analogSetPinAttenuation(PIN_FWD, ADC_11db);
  analogSetPinAttenuation(PIN_REV, ADC_11db);

  float filtVfwd = 0.0f;
  float filtVrev = 0.0f;
  float filtSwr  = 1.0f;
  bool wasTx = false;
  int txSettleCycles = 0;

  for (;;) {
    const int SAMPLES = 24;
    long sumRawFwd = 0;
    long sumFwd = 0;

    // Luetaan ensin pelkkä FWD-kanava (ei ADC-kanavahyppelyä)
    for (int i = 0; i < SAMPLES; i++) {
      sumRawFwd += analogRead(PIN_FWD);
      sumFwd    += analogReadMilliVolts(PIN_FWD);
      delayMicroseconds(40);
    }

    delayMicroseconds(100); // Mux-rauhoittumisaika

    // Luetaan sitten pelkkä REV-kanava
    long sumRawRev = 0;
    long sumRev = 0;
    for (int i = 0; i < SAMPLES; i++) {
      sumRawRev += analogRead(PIN_REV);
      sumRev    += analogReadMilliVolts(PIN_REV);
      delayMicroseconds(40);
    }

    int avgRawFwd = sumRawFwd / SAMPLES;
    int avgRawRev = sumRawRev / SAMPLES;
    lastRawFwd = avgRawFwd;
    lastRawRev = avgRawRev;

    // Tunnistetaan lepotila (RX) luotettavasti raaka-arvosta:
    if (avgRawFwd < 45) {
      isTransmitting = false;
      wasTx = false;
      txSettleCycles = 0;
      filtVfwd = 0.0f;
      filtVrev = 0.0f;
      filtSwr  = 1.0f;

      currentVfwd = 0.0f;
      currentVrev = 0.0f;
      currentPfwd = 0.0f;
      currentPrev = 0.0f;
      currentSwr  = 1.0f;
      
      avgPfwd = avgPfwd * 0.985f;
      if (avgPfwd < 0.1f) avgPfwd = 0.0f;

      if (millis() - lastPeakTime > 1800) {
        peakPfwd = peakPfwd * 0.92f;
        if (peakPfwd < 0.1f) peakPfwd = 0.0f;
      }
    } else {
      isTransmitting = true;
      txSettleCycles++;

      // analogReadMilliVolts antaa kalibroidun jännitteen suoraan millivoltteina
      float instVfwd = (sumFwd / (float)SAMPLES) / 1000.0f;
      if (instVfwd < 0.0f) instVfwd = 0.0f;

      float rawVrev = (sumRev / (float)SAMPLES) / 1000.0f;
      if (rawVrev < 0.0f) rawVrev = 0.0f;

      // Suuntakytkimen eristysvuodon poisto (Directivity isolation in quadrature):
      // Stockton-sillan luontainen eristysvuoto (D ~ 0.192, n. -14.3 dB)
      float gamma = 0.0f;
      if (instVfwd > 0.05f) {
        float ratio = rawVrev / instVfwd;
        float diffSq = ratio * ratio - dirLeakFactor * dirLeakFactor;
        if (diffSq > 0.0f) {
          gamma = sqrtf(diffSq) * revCalFactor;
          if (gamma > 0.98f) gamma = 0.98f;
        }
      }

      // Todellinen puhdistettu heijastusjännite:
      float instVrev = gamma * instVfwd;

      // Ensimmäinen TX-sykli: alustetaan jännitteet heti
      if (!wasTx) {
        filtVfwd = instVfwd;
        filtVrev = instVrev;
        filtSwr = 1.0f;
        currentSwr = 1.0f;
        wasTx = true;
      } else {
        float alphaFwd = (instVfwd > filtVfwd) ? 0.35f : 0.10f;
        filtVfwd += alphaFwd * (instVfwd - filtVfwd);

        float alphaRev = (instVrev > filtVrev) ? 0.35f : 0.10f;
        filtVrev += alphaRev * (instVrev - filtVrev);
      }

      currentVfwd = filtVfwd;
      currentVrev = filtVrev;

      float pFwd = pwrCalFactor * (filtVfwd * filtVfwd);
      float filtGamma = (filtVfwd > 0.001f) ? (filtVrev / filtVfwd) : 0.0f;
      if (filtGamma > 0.98f) filtGamma = 0.98f;
      float pRev = pFwd * (filtGamma * filtGamma);

      currentPfwd = pFwd;
      currentPrev = pRev;

      // True AVG / VU ballistic filter:
      if (pFwd > avgPfwd) {
        avgPfwd += 0.025f * (pFwd - avgPfwd);
      } else {
        avgPfwd += 0.005f * (pFwd - avgPfwd);
      }

      if (pFwd > peakPfwd) {
        peakPfwd = pFwd;
        lastPeakTime = millis();
      } else if (millis() - lastPeakTime > 1500) {
        peakPfwd = peakPfwd * 0.96f;
      }

      // SWR laskenta suodatetuista jännitteistä vasta kun kantoaalto on tasaantunut (~20ms)
      if (txSettleCycles >= 3) {
        if (filtVfwd > 0.08f) {
          float rawSwr = (1.0f + filtGamma) / (1.0f - filtGamma);
          if (rawSwr > 99.0f) rawSwr = 99.0f;
          
          if (txSettleCycles == 3) {
            filtSwr = rawSwr;
          } else {
            filtSwr += 0.12f * (rawSwr - filtSwr);
          }
          currentSwr = filtSwr;
        } else {
          currentSwr = 1.0f;
        }
      } else {
        currentSwr = 1.0f;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ==========================================
// SETUP
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(500);

  // Asetetaan ADC 12-bittiseksi ja 11 dB vaimennukselle (mittausalue 0 - 3.1V)
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  analogSetPinAttenuation(PIN_FWD, ADC_11db);
  analogSetPinAttenuation(PIN_REV, ADC_11db);

  xTaskCreatePinnedToCore(
    samplingTask,
    "SamplingTask",
    4096,
    NULL,
    1,
    NULL,
    0 // Core 0
  );

  // Load WiFi settings and Calibration from Flash (NVS)
  preferences.begin("swr_cfg", false);
  String activeSsid = preferences.getString("ssid", DEFAULT_WIFI_SSID);
  String activePass = preferences.getString("pass", DEFAULT_WIFI_PASS);
  pwrCalFactor  = preferences.getFloat("pwr_cal", 12.80f);
  revCalFactor  = preferences.getFloat("rev_cal", 1.55f);
  dirLeakFactor = preferences.getFloat("dir_leak", 0.192f);
  preferences.end();

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm); // Maximum Wi-Fi output (+19.5 dBm)

  // Always enable SoftAP so the device is accessible anywhere (Field Day / initial setup)
  WiFi.softAP(AP_SSID, AP_PASS);

  if (activeSsid.length() > 0 && activeSsid != "YOUR_WIFI_SSID") {
    Serial.printf("Connecting to WiFi: %s\n", activeSsid.c_str());
    WiFi.begin(activeSsid.c_str(), activePass.c_str());
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 25) {
      delay(200);
      retries++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("WiFi Connected! IP: ");
      Serial.println(WiFi.localIP());
    } else {
      Serial.println("WiFi connection failed. Access Point active: SWR-Meter-AP (192.168.4.1:2222)");
    }
  }

  if (MDNS.begin("swrmeter")) {
    MDNS.addService("http", "tcp", 2222);
  }

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/wifi", handleWifi);
  server.on("/save_wifi", HTTP_POST, handleSaveWifi);
  server.on("/cal", handleCal);
  server.on("/save_cal", HTTP_POST, handleSaveCal);

  // Web OTA päivitys suoraan selaimesta
  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
    delay(250);
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("Päivitys alkaa: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {
        Serial.printf("Päivitys valmis: %u tavua\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });

  server.begin();

  // ArduinoOTA langaton päivitys (PlatformIO / IDE)
  ArduinoOTA.setHostname("swrmeter");
  ArduinoOTA.begin();
}

// ==========================================
// LOOP (Core 1)
// ==========================================
void loop() {
  server.handleClient();
  ArduinoOTA.handle();
  delay(2);
}
