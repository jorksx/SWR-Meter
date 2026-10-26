# ESP32 HF SWR & Power Meter (1.8 – 50 MHz)

A high-speed, wireless, and highly accurate HF (160m – 6m) amateur radio SWR and RF power meter.

Powered by a **Stockton / Tandem-Match** directional coupler and an **ESP32** microcontroller. Features a responsive standalone Web UI (port 2222) and a custom frameless desktop application for Windows with 5 auto-scaling themes — including an authentic mechanically-damped **Cross-Needle** meter.

---

## ⚠️ CRITICAL WARNING: 10 kΩ BLEEDER RESISTORS (CHINESE KITS CAVEAT!)

> [!CAUTION]
> ### 🔴 CHINESE TANDEM MATCH / STOCKTON KITS HAVE A FATAL OMISSION!
> Cheap Stockton / Tandem-Match DIY directional coupler kits from China (AliExpress, eBay, Banggood) **DO NOT INCLUDE** bleeder/pulldown load resistors on the detector outputs, or they are omitted from the PCB layout entirely!
>
> **Why is this critical?**
> The directional coupler's RF diodes rectify RF voltage into DC pulses, charging the filter capacitors. The ESP32's ADC inputs (GPIO 34 and GPIO 35) have an extremely high input impedance (>10 MΩ). **Without a DC path to ground, the capacitors NEVER discharge!**
>
> **What happens if you omit the 10 kΩ resistors:**
> 1. The capacitors charge to peak voltage on your first transmission and **stay permanently charged** — the meter remains frozen showing power and high SWR even after you release the PTT and return to RX!
> 2. The high-impedance ADC inputs float, picking up 50/60 Hz mains hum and static electricity, causing "phantom power" readings in RX mode.
> 3. SWR calculations become completely erroneous and wildly unstable.
>
> ### ✅ SOLUTION:
> You **MUST** install two standard **10 kΩ resistors** (1/4 W through-hole or SMD):
> - **1x 10 kΩ resistor between FWD output and GROUND (GND)**
> - **1x 10 kΩ resistor between REV output and GROUND (GND)**
>
> *(You can solder these resistors directly between the FWD/REV terminal pads and the ground plane on the coupler PCB, or across ESP32 pins 34/35 and GND).*

---

## 🔌 Hardware Wiring & Pinout

```
 [ Transceiver / Rig ] ---> [ Stockton / Tandem Coupler ] ---> [ Antenna / Dummy Load ]
                                   |           |
                                  FWD         REV
                                   |           |
                                   +--[10kΩ]   +--[10kΩ]   (PULLDOWN BLEEDERS TO GND!)
                                   |    |      |    |
                                  ( )  GND    ( )  GND
                                   |           |
                                   v           v
                              ESP32 GPIO 34  ESP32 GPIO 35
                              (PIN_FWD)      (PIN_REV)

 Common Ground: Coupler GND  ===============>  ESP32 GND
 Power Supply:  ESP32 Micro-USB (5V) or VIN Pin
```

### Pin Connection Table:
| ESP32 Pin | Signal | Notes |
|---|---|---|
| **GPIO 34** | `FWD` (Forward Voltage) | + 10 kΩ pulldown resistor to GND |
| **GPIO 35** | `REV` (Reflected Voltage) | + 10 kΩ pulldown resistor to GND |
| **GND** | Ground | Connect to directional coupler ground plane |
| **5V / USB** | VCC | 5V DC power input |

---

## 📶 WiFi Configuration (No Hardcoded WiFi Required!)

You do not need to hardcode your WiFi password in the firmware. The ESP32 stores WiFi credentials permanently in Flash memory (**NVS Preferences**).

### Option 1: Web Interface Configuration (Easiest)
1. Turn on the ESP32.
2. If it cannot connect to a known network, it automatically broadcasts its own SoftAP:
   - **SSID:** `SWR-Meter-AP`
   - **Password:** `12345678`
   - **Setup URL:** `http://192.168.4.1:2222` (or click the **⚙️ WIFI** button in the header)
3. Enter your home/shack WiFi network name (SSID) and Password.
4. Click **💾 Save & Reboot**.
5. The device saves the credentials to NVS, restarts, and connects to your home network. You can now access it at `http://swrmeter.local:2222` or its assigned local IP.

### Option 2: Changing WiFi Anytime
Even when connected to your home network, you can click the **⚙️ WIFI** button in the web UI header (or visit `http://<ip>:2222/wifi`) to update your network settings on the fly.

### Option 3: Default Credentials in Source (Optional)
If you prefer hardcoding default fallback credentials, edit lines 25–26 in [src/main.cpp](file:///src/main.cpp):
```cpp
const char* DEFAULT_WIFI_SSID = "YOUR_WIFI_SSID";
const char* DEFAULT_WIFI_PASS = "YOUR_WIFI_PASSWORD";
```

---

## 🎯 Step-by-Step Calibration Guide

Calibration takes only a couple of minutes. You need your **transceiver**, a trusted **50 Ω Dummy Load**, and an accurate reference power meter (or your transceiver's internal calibrated wattmeter).

### Step 1: Forward Power Calibration (`PWR_CAL_FACTOR`)
1. Connect: **Rig ➔ Coupler TX port ➔ Coupler ANT port ➔ 50 Ω Dummy Load**.
2. Transmit a steady carrier (CW, FM, or RTTY) at a known power level, e.g. **100 W** (or 50 W).
3. Check the detected voltage **`fwd_v`** displayed in the desktop app footer or Web UI (e.g. `2.68 V`).
4. Calculate your calibration constant using the formula:
   $$\text{PWR\_CAL\_FACTOR} = \frac{\text{Actual Power (W)}}{(V_{\text{fwd}})^2}$$
   *Example:* If actual power is 100 W and $V_{\text{fwd}} = 2.68\text{ V}$:
   $$\text{PWR\_CAL\_FACTOR} = \frac{100}{2.68 \times 2.68} = \frac{100}{7.1824} \approx 13.92$$
5. Update line 15 in [src/main.cpp](file:///src/main.cpp):
   ```cpp
   const float PWR_CAL_FACTOR = 13.85; // Set your calculated value here
   ```

### Step 2: Reverse Channel Balance (`REV_CAL_FACTOR`)
Due to minor component tolerances in diodes and ferrite binocular transformers, the reverse channel sensitivity is balanced against the forward channel:
1. **Reverse the RF coaxial cables on the coupler:**
   - Connect your Radio to the coupler's **ANT** port.
   - Connect the 50 Ω Dummy Load to the coupler's **TX** port.
2. Transmit at low power (e.g. **10 W** or **20 W**).
3. The forward power now flows through the reverse detection winding, appearing as reflected voltage (`rev_v`).
4. Observe `fwd_v` and `rev_v` in the monitor. Adjust `REV_CAL_FACTOR` on line 18 in [src/main.cpp](file:///src/main.cpp) so that `rev_v` exactly matches `fwd_v` (i.e., measured reflected power equals forward power):
   ```cpp
   const float REV_CAL_FACTOR = 1.35; // Adjust as needed for symmetry
   ```
5. **Restore the RF cables to normal:** (Radio ➔ TX, ANT ➔ Antenna / Dummy Load).

### Step 3: SWR Verification
- Transmit into your 50 Ω dummy load: SWR should read **1.00 : 1** (to 1.05 : 1) and reflected power `0.00 W`.
- (Optional) If you connect a 100 Ω load or a 25 Ω load (two 50 Ω loads in parallel), the meter should read **2.00 : 1** SWR.

---

## 💻 Software & User Interfaces

### 1. ESP32 Firmware ([src/main.cpp](file:///src/main.cpp))
- **200 Hz FreeRTOS Sampling Task** on Core 0: ultra-fast ADC sampling with key-up transient blanking to prevent false SWR spikes when keying up.
- **PEP & True AVG Ballistics:** Fast Peak Hold (1.5s decay) and voice-power averaging (VU meter ballistics for SSB).
- **Built-in Web Server (Port 2222):** Mobile-friendly, dark-themed responsive dashboard with dynamic gradient bars and RSSI meter (`http://swrmeter.local:2222`).
- **Web OTA Updates:** Flash new firmware wirelessly from your browser via `http://<ip>:2222/update`.

### 2. Windows Desktop Monitor ([swr_monitor.py](file:///swr_monitor.py))
A modern, borderless desktop GUI window featuring custom draggable header, close button, and always-on-top toggle (`PIN 📌`).

**5 Dynamically Auto-Scaling Themes (Zero Wasted Space):**
1. **MODERN:** Dual gradient power cards, PEP & AVG bars, live color-shifting SWR meter, and detailed telemetry footer.
2. **CROSS-NEEDLE:** Authentic Daiwa/Nissei-style crossed meter with dark slate instrument faceplate, low-pass damped needle inertia (zero jitter), and center radial SWR fan lines.
3. **MATRIX:** Cyberpunk green terminal display with live ASCII power/SWR bars and a real-time RF carrier oscillogram.
4. **RETRO RIG:** Dual illuminated amber round dial gauges with authentic metallic bezels.
5. **CLEAN OLED:** Ultra-compact, minimalist high-contrast HUD display.

**Intelligent Real-Time Anomaly Detection:**
- High SWR alert (>3.0:1).
- Sudden mid-transmission SWR jumps.
- Reverse voltage anomalies (`REV > FWD`).
- ESP32 ADC clipping warning (`> 3.05 V`).
- Window expands dynamically during alerts to display flashing warning banners without obstructing meter readouts.

---

## 🚀 Quick Start Guide

### 1. Build & Upload Firmware to ESP32
Using [PlatformIO](https://platformio.org/):
```bash
pio run --target upload
```

### 2. Launch the Desktop Client
Ensure Python 3 is installed with Pillow:
```bash
pip install pillow
```
Launch by double-clicking:
**`Kaynnista_SWR_Mittari.bat`**

The app automatically searches for the meter via direct IP, mDNS (`swrmeter.local`), and SoftAP (`192.168.4.1`), and remembers your window position and theme!

---

## 📄 License
MIT License – Free to use, modify, and distribute for all amateur radio enthusiasts. 73!
