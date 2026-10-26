import tkinter as tk
import http.client
import json
import threading
import time
import ctypes
import math
import os
from PIL import Image, ImageDraw, ImageFont, ImageTk

# Ensisijaisesti suora IP (ei DNS-viivettä), varalla mDNS
TARGETS = [
    ("192.168.1.159", 2222),
    ("swrmeter.local", 2222),
    ("192.168.1.170", 2222)
]

THEMES = ["MODERN", "CROSS-NEEDLE", "MATRIX", "RETRO RIG", "CLEAN OLED"]
THEME_DIMENSIONS = {
    "MODERN": (495, 410),
    "CROSS-NEEDLE": (495, 460),
    "MATRIX": (495, 325),
    "RETRO RIG": (495, 440),
    "CLEAN OLED": (495, 285),
}
CONFIG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "swr_settings.json")

# Fonttien lataus Windowsista
try:
    FONT_TITLE = ImageFont.truetype("C:/Windows/Fonts/arialbd.ttf", 22)
    FONT_BIG   = ImageFont.truetype("C:/Windows/Fonts/arialbd.ttf", 26)
    FONT_BOLD  = ImageFont.truetype("C:/Windows/Fonts/arialbd.ttf", 20)
    FONT_NUM   = ImageFont.truetype("C:/Windows/Fonts/arialbd.ttf", 19)
    FONT_SM    = ImageFont.truetype("C:/Windows/Fonts/arialbd.ttf", 15)
    FONT_SWR   = ImageFont.truetype("C:/Windows/Fonts/arialbd.ttf", 17)
    FONT_TINY  = ImageFont.truetype("C:/Windows/Fonts/arial.ttf", 13)
except Exception:
    FONT_TITLE = ImageFont.load_default()
    FONT_BIG   = ImageFont.load_default()
    FONT_BOLD  = ImageFont.load_default()
    FONT_NUM   = ImageFont.load_default()
    FONT_SM    = ImageFont.load_default()
    FONT_SWR   = ImageFont.load_default()
    FONT_TINY  = ImageFont.load_default()

def get_swr_color(s):
    """Laskee dynaamisen liukuvan värin SWR-arvolle vihreästä punaiseen"""
    if s <= 1.0:
        return "#00ff88"
    if s >= 3.0:
        return "#ff1e56"
    if s <= 1.5:
        t = (s - 1.0) / 0.5
        r = int(0 + t * 166)
        g = 255
        b = int(136 * (1 - t))
    elif s <= 2.2:
        t = (s - 1.5) / 0.7
        r = 255
        g = int(255 - t * 102)
        b = 0
    else:
        t = min(1.0, (s - 2.2) / 0.8)
        r = 255
        g = int(153 * (1 - t) + 30 * t)
        b = int(86 * t)
    return f"#{r:02x}{g:02x}{b:02x}"

def get_pwr_color(w):
    """Laskee dynaamisen liukuvan värin teholle: 0W (vihreä) -> 50W (keltainen) -> 100W+ (punainen)"""
    if w <= 0.0:
        return "#00ff88"
    if w >= 100.0:
        return "#ff1e56"
    if w <= 50.0:
        t = w / 50.0
        r = int(0 + t * 255)
        g = 255
        b = int(136 * (1 - t))
    else:
        t = (w - 50.0) / 50.0
        r = 255
        g = int(255 * (1 - t) + 30 * t)
        b = int(86 * t)
    return f"#{r:02x}{g:02x}{b:02x}"

def make_gradient_lut(width, stops):
    """Generoi valmiin liukuväritaulukon (LUT) palkkien piirtoon"""
    lut = []
    for x in range(width):
        t = x / max(1, width - 1)
        for i in range(len(stops) - 1):
            t0, c0 = stops[i]
            t1, c1 = stops[i + 1]
            if t <= t1 or i == len(stops) - 2:
                seg_t = min(1.0, max(0.0, (t - t0) / max(0.0001, t1 - t0)))
                r = int(c0[0] + seg_t * (c1[0] - c0[0]))
                g = int(c0[1] + seg_t * (c1[1] - c0[1]))
                b = int(c0[2] + seg_t * (c1[2] - c0[2]))
                lut.append(f"#{r:02x}{g:02x}{b:02x}")
                break
    return lut

def draw_gradient_bar(canvas, w, h, fill_px, lut, peak_w=0.0):
    """Piirtää liukuväripalkin ja PEAK-viivan"""
    canvas.delete("all")
    if w <= 1:
        return
    if fill_px > 0 and lut:
        step = 2
        for x in range(0, min(fill_px, w), step):
            idx = min(len(lut) - 1, int((x / w) * len(lut)))
            canvas.create_rectangle(x, 0, min(x + step, fill_px), h, fill=lut[idx], width=0)
    if peak_w > 0.0:
        peak_px = int(min(w - 2, (peak_w / 120.0) * w))
        if peak_px > 0:
            canvas.create_line(peak_px, 0, peak_px, h, fill="#ffffff", width=2)


class SWRMeterApp:
    def __init__(self, root):
        self.root = root
        self.root.title("SWR & Power Monitor")
        self.root.resizable(False, False)

        # Frameless ikkuna
        self.root.overrideredirect(True)
        self.root.after(10, self.set_app_window)

        self.always_on_top = False
        self.drag_x = 0
        self.drag_y = 0
        self.saved_x = None
        self.saved_y = None

        # Teemat
        self.current_theme = "MODERN"
        self.theme_idx = 0

        # Ladataan tallennetut asetukset (viimeisin teema ja ikkunan sijainti)
        self.load_settings()

        screen_w = self.root.winfo_screenwidth()
        screen_h = self.root.winfo_screenheight()
        w, h = THEME_DIMENSIONS.get(self.current_theme, (495, 410))
        if self.saved_x is not None and self.saved_y is not None:
            x = max(10, min(self.saved_x, screen_w - w - 10))
            y = max(10, min(self.saved_y, screen_h - h - 40))
            self.root.geometry(f"{w}x{h}+{x}+{y}")
        else:
            self.root.geometry(f"{w}x{h}")

        self.root.configure(bg="#090d14")

        # Mittausdata
        self.fwd_w  = 0.0
        self.avg_w  = 0.0
        self.rev_w  = 0.00
        self.peak_w = 0.0
        self.swr    = 1.00
        self.fwd_v  = 0.00
        self.rev_v  = 0.00
        self.tx     = False
        self.rssi   = 0
        self.connected = False

        # Viisareiden vaimennus / inertia (rauhoittaa mekaanisten neulojen tärinää)
        self.cn_fwd_damped = 0.0
        self.cn_rev_damped = 0.0
        self.retro_pwr_damped = 0.0
        self.retro_swr_damped = 1.0

        # Anomaliavalvonta
        self.prev_swr = 1.0
        self.alert_text = ""
        self.alert_until = 0.0
        self.alert_active = False
        self.was_tx = False
        self.tx_start_time = 0.0
        self.matrix_cursor_blink = 0

        # Valmiiksi lasketut liukuvärit MODERN-teemalle
        pwr_stops = [
            (0.00, (0, 255, 136)),
            (0.25, (112, 224, 0)),
            (0.46, (255, 234, 0)),
            (0.67, (255, 133, 0)),
            (0.83, (255, 30, 86)),
            (1.00, (208, 0, 0))
        ]
        swr_stops = [
            (0.00, (0, 255, 136)),
            (0.12, (56, 176, 0)),
            (0.25, (255, 208, 0)),
            (0.38, (255, 121, 0)),
            (0.50, (255, 30, 86)),
            (1.00, (192, 0, 0))
        ]
        self.pwr_lut = make_gradient_lut(460, pwr_stops)
        self.swr_lut = make_gradient_lut(460, swr_stops)

        # Cross-needle ja Retro Rig taustakuvat
        self.cross_bg = None
        self.cross_photo = None
        self.retro_bg = None
        self.retro_photo = None

        self.setup_ui()
        self.init_cross_needle_bg()
        self.init_retro_rig_bg()
        self.apply_theme(self.current_theme)

        if self.always_on_top:
            self.root.attributes("-topmost", True)
            self.ontop_btn.configure(fg="#00ff88", bg="#143324")

        self.running = True
        self.thread = threading.Thread(target=self.network_loop, daemon=True)
        self.thread.start()

        self.update_ui()

    def load_settings(self):
        """Lataa viimeksi käytetyn teeman ja ikkunan koordinaatit levyltä"""
        try:
            if os.path.exists(CONFIG_FILE):
                with open(CONFIG_FILE, "r", encoding="utf-8") as f:
                    data = json.load(f)
                    saved_theme = data.get("theme", "MODERN")
                    if saved_theme in THEMES:
                        self.current_theme = saved_theme
                        self.theme_idx = THEMES.index(saved_theme)
                    self.saved_x = data.get("x", None)
                    self.saved_y = data.get("y", None)
                    self.always_on_top = data.get("always_on_top", False)
        except Exception:
            pass

    def save_settings(self):
        """Tallentaa teeman ja ikkunan koordinaatit levylle"""
        try:
            x = self.root.winfo_x()
            y = self.root.winfo_y()
            if x is not None and x > -2000 and y is not None and y > -2000:
                data = {
                    "theme": self.current_theme,
                    "x": x,
                    "y": y,
                    "always_on_top": self.always_on_top
                }
                with open(CONFIG_FILE, "w", encoding="utf-8") as f:
                    json.dump(data, f, indent=2)
        except Exception:
            pass

    def set_app_window(self):
        """Pitää frameless-ikkunan näkyvissä Windowsin tehtäväpalkissa"""
        try:
            hwnd = ctypes.windll.user32.GetParent(self.root.winfo_id())
            style = ctypes.windll.user32.GetWindowLongW(hwnd, -20)
            style = style & ~0x00000080
            style = style | 0x00040000
            ctypes.windll.user32.SetWindowLongW(hwnd, -20, style)
            self.root.withdraw()
            self.root.after(10, self.root.deiconify)
        except Exception:
            pass

    def close_app(self):
        self.save_settings()
        self.running = False
        self.root.destroy()

    def check_anomalies(self, fwd_w, rev_w, swr, fwd_v, rev_v, is_tx):
        now = time.time()
        anomaly = None

        if is_tx:
            if not self.was_tx:
                self.tx_start_time = now
                self.was_tx = True
            tx_duration = now - self.tx_start_time

            if tx_duration > 0.30 and fwd_w > 8.0:
                if rev_v > (fwd_v * 1.05) and rev_v > 0.15:
                    anomaly = f"ANOMALIA: REV-JÄNNITE ({rev_v:.2f}V) > FWD ({fwd_v:.2f}V)!"
                elif fwd_v >= 3.05:
                    anomaly = "VAROITUS: ADC-LEIKKAUS (>3.05V) - TEHO LIIAN SUURI!"
                elif (swr - self.prev_swr) >= 0.85 and self.prev_swr < 2.5:
                    anomaly = f"ÄKILLINEN SWR-HYPPY (+{swr - self.prev_swr:.2f} -> {swr:.2f}:1)!"
                elif swr >= 3.0:
                    anomaly = f"KORKEA SWR ({swr:.2f}:1) - TARKISTA ANTENNI!"

                self.prev_swr = swr
        else:
            self.was_tx = False
            self.prev_swr = 1.0

        if anomaly:
            self.alert_text = anomaly
            self.alert_until = now + 4.0

    def setup_ui(self):
        self.main_container = tk.Frame(self.root, bg="#090d14", highlightthickness=1, highlightbackground="#243048")
        self.main_container.pack(fill="both", expand=True)

        # Custom Yläpalkki (Draggable)
        self.header_frame = tk.Frame(self.main_container, bg="#121824", highlightthickness=1, highlightbackground="#1a2334")
        self.header_frame.pack(fill="x", padx=10, pady=(10, 5))

        self.theme_btn = tk.Button(self.header_frame, text=f"🎨 {self.current_theme}", font=("Consolas", 9, "bold"),
                                   bg="#182030", fg="#00e5ff", activebackground="#243048",
                                   activeforeground="#ffffff", relief="flat", cursor="hand2",
                                   padx=10, pady=3, bd=0, command=self.cycle_theme)
        self.theme_btn.pack(side="left", padx=(8, 4), pady=6)

        self.close_btn = tk.Button(self.header_frame, text="✕", font=("Arial", 9, "bold"),
                                   bg="#182030", fg="#8899aa", activebackground="#ff1e56",
                                   activeforeground="#ffffff", relief="flat", cursor="hand2",
                                   padx=8, pady=2, bd=0, command=self.close_app)
        self.close_btn.pack(side="right", padx=(4, 6), pady=6)

        def on_enter_close(e):
            self.close_btn.configure(bg="#ff1e56", fg="#ffffff")
        def on_leave_close(e):
            self.update_btn_colors()
        self.close_btn.bind("<Enter>", on_enter_close)
        self.close_btn.bind("<Leave>", on_leave_close)

        self.ontop_btn = tk.Button(self.header_frame, text="PIN 📌", font=("Arial", 8, "bold"), bg="#182030", fg="#62728d",
                                   relief="flat", cursor="hand2", command=self.toggle_pin)
        self.ontop_btn.pack(side="right", padx=4, pady=6)

        self.status_lbl = tk.Label(self.header_frame, text="CONNECTING", font=("Consolas", 8, "bold"),
                                   fg="#62728d", bg="#182030", padx=6, pady=3)
        self.status_lbl.pack(side="right", padx=4, pady=6)

        self.wifi_lbl = tk.Label(self.header_frame, text="📶 -- dBm", font=("Consolas", 8, "bold"),
                                 fg="#62728d", bg="#182030", padx=5, pady=3)
        self.wifi_lbl.pack(side="right", padx=4, pady=6)

        def start_move(event):
            self.drag_x = event.x_root - self.root.winfo_x()
            self.drag_y = event.y_root - self.root.winfo_y()

        def do_move(event):
            x = event.x_root - self.drag_x
            y = event.y_root - self.drag_y
            self.root.geometry(f"+{x}+{y}")

        def stop_move(event):
            self.save_settings()

        self.header_frame.bind("<Button-1>", start_move)
        self.header_frame.bind("<B1-Motion>", do_move)
        self.header_frame.bind("<ButtonRelease-1>", stop_move)

        self.alert_frame = tk.Frame(self.main_container, bg="#ff1e56", highlightthickness=1, highlightbackground="#ffffff")
        self.alert_lbl = tk.Label(self.alert_frame, text="", font=("Arial", 9, "bold"), fg="#ffffff", bg="#ff1e56", pady=4)
        self.alert_lbl.pack(fill="x", expand=True)
        self.alert_active = False

        self.view_container = tk.Frame(self.main_container, bg="#090d14")
        self.view_container.pack(fill="both", expand=True, padx=10, pady=5)

        # =========================================================================
        # THEME 1: MODERN
        # =========================================================================
        self.view_modern = tk.Frame(self.view_container, bg="#090d14")

        self.mod_pwr_card = tk.Frame(self.view_modern, bg="#121824", highlightthickness=1, highlightbackground="#1a2334")
        self.mod_pwr_card.pack(fill="x", pady=(0, 6))

        tk.Label(self.mod_pwr_card, text="FORWARD POWER", font=("Arial", 8, "bold"), fg="#62728d", bg="#121824").pack(anchor="w", padx=12, pady=(6, 0))
        pwr_val_f = tk.Frame(self.mod_pwr_card, bg="#121824")
        pwr_val_f.pack(anchor="w", padx=12, pady=(0, 2))
        self.mod_pwr_num = tk.Label(pwr_val_f, text="0.0", font=("Consolas", 32, "bold"), fg="#00ff88", bg="#121824")
        self.mod_pwr_num.pack(side="left")
        tk.Label(pwr_val_f, text=" W", font=("Consolas", 14), fg="#62728d", bg="#121824").pack(side="left", pady=(10, 0))

        pep_row = tk.Frame(self.mod_pwr_card, bg="#121824")
        pep_row.pack(fill="x", padx=12)
        tk.Label(pep_row, text="PEP", font=("Consolas", 9, "bold"), fg="#62728d", bg="#121824").pack(side="left")
        self.mod_pep_txt = tk.Label(pep_row, text="0.0 W", font=("Consolas", 10, "bold"), fg="#00ff88", bg="#121824")
        self.mod_pep_txt.pack(side="right")
        self.mod_pep_canvas = tk.Canvas(self.mod_pwr_card, height=13, bg="#080c14", highlightthickness=1, highlightbackground="#1a2334")
        self.mod_pep_canvas.pack(fill="x", padx=12, pady=(2, 4))

        avg_row = tk.Frame(self.mod_pwr_card, bg="#121824")
        avg_row.pack(fill="x", padx=12)
        tk.Label(avg_row, text="AVG", font=("Consolas", 9, "bold"), fg="#62728d", bg="#121824").pack(side="left")
        self.mod_avg_txt = tk.Label(avg_row, text="0.0 W", font=("Consolas", 10, "bold"), fg="#00ff88", bg="#121824")
        self.mod_avg_txt.pack(side="right")
        self.mod_avg_canvas = tk.Canvas(self.mod_pwr_card, height=11, bg="#080c14", highlightthickness=1, highlightbackground="#1a2334")
        self.mod_avg_canvas.pack(fill="x", padx=12, pady=(2, 8))

        self.mod_swr_card = tk.Frame(self.view_modern, bg="#121824", highlightthickness=1, highlightbackground="#1a2334")
        self.mod_swr_card.pack(fill="x", pady=4)
        tk.Label(self.mod_swr_card, text="SWR", font=("Arial", 8, "bold"), fg="#62728d", bg="#121824").pack(anchor="w", padx=12, pady=(6, 0))
        swr_val_f = tk.Frame(self.mod_swr_card, bg="#121824")
        swr_val_f.pack(anchor="w", padx=12, pady=(0, 2))
        self.mod_swr_num = tk.Label(swr_val_f, text="1.00", font=("Consolas", 32, "bold"), fg="#00ff88", bg="#121824")
        self.mod_swr_num.pack(side="left")
        tk.Label(swr_val_f, text=" :1", font=("Consolas", 14), fg="#62728d", bg="#121824").pack(side="left", pady=(10, 0))
        self.mod_swr_canvas = tk.Canvas(self.mod_swr_card, height=13, bg="#080c14", highlightthickness=1, highlightbackground="#1a2334")
        self.mod_swr_canvas.pack(fill="x", padx=12, pady=(2, 8))

        self.mod_footer = tk.Frame(self.view_modern, bg="#121824", highlightthickness=1, highlightbackground="#1a2334")
        self.mod_footer.pack(fill="x", pady=(4, 0))
        self.mod_rev_lbl = tk.Label(self.mod_footer, text="REFLECTED: 0.00 W", font=("Consolas", 9, "bold"), fg="#ffffff", bg="#121824")
        self.mod_rev_lbl.pack(side="left", padx=12, pady=6)
        self.mod_peak_lbl = tk.Label(self.mod_footer, text="PEAK: 0.0 W", font=("Consolas", 9, "bold"), fg="#ffb703", bg="#121824")
        self.mod_peak_lbl.pack(side="left", expand=True, pady=6)
        self.mod_volt_lbl = tk.Label(self.mod_footer, text="0.00V / 0.00V", font=("Consolas", 9, "bold"), fg="#62728d", bg="#121824")
        self.mod_volt_lbl.pack(side="right", padx=12, pady=6)

        # =========================================================================
        # THEME 2: CROSS-NEEDLE (Tumma tyylikäs mittaritaulu ja vaimennetut neulat)
        # =========================================================================
        self.view_cross = tk.Frame(self.view_container, bg="#090d14")
        self.cross_canvas = tk.Canvas(self.view_cross, width=460, height=320, bg="#090d14", highlightthickness=0)
        self.cross_canvas.pack(fill="none", expand=True, pady=(0, 4))

        self.cross_footer = tk.Frame(self.view_cross, bg="#121824", highlightthickness=1, highlightbackground="#1a2334")
        self.cross_footer.pack(fill="x", pady=(2, 0))

        cf_row1 = tk.Frame(self.cross_footer, bg="#121824")
        cf_row1.pack(fill="x", padx=12, pady=(5, 1))
        self.cn_fwd_lbl = tk.Label(cf_row1, text="FWD: 0.0 W", font=("Consolas", 10, "bold"), fg="#00ff88", bg="#121824")
        self.cn_fwd_lbl.pack(side="left")
        self.cn_swr_lbl = tk.Label(cf_row1, text="SWR: 1.00:1", font=("Consolas", 11, "bold"), fg="#00ff88", bg="#121824")
        self.cn_swr_lbl.pack(side="left", expand=True)
        self.cn_rev_lbl = tk.Label(cf_row1, text="REF: 0.00 W", font=("Consolas", 10, "bold"), fg="#ff1e56", bg="#121824")
        self.cn_rev_lbl.pack(side="right")

        cf_row2 = tk.Frame(self.cross_footer, bg="#121824")
        cf_row2.pack(fill="x", padx=12, pady=(1, 5))
        self.cn_pep_lbl = tk.Label(cf_row2, text="PEAK: 0.0 W", font=("Consolas", 9, "bold"), fg="#ffb703", bg="#121824")
        self.cn_pep_lbl.pack(side="left")
        self.cn_avg_lbl = tk.Label(cf_row2, text="AVG: 0.0 W", font=("Consolas", 9), fg="#8fa0b5", bg="#121824")
        self.cn_avg_lbl.pack(side="left", expand=True)
        self.cn_volt_lbl = tk.Label(cf_row2, text="0.00V / 0.00V", font=("Consolas", 9), fg="#62728d", bg="#121824")
        self.cn_volt_lbl.pack(side="right")

        # =========================================================================
        # THEME 3: MATRIX
        # =========================================================================
        self.view_matrix = tk.Frame(self.view_container, bg="#020703")

        self.mat_terminal = tk.Label(self.view_matrix, text="", font=("Consolas", 10),
                                     fg="#00ff41", bg="#041007", justify="left", anchor="nw",
                                     padx=12, pady=10, relief="solid", bd=1, highlightbackground="#0a3d16", highlightthickness=1)
        self.mat_terminal.pack(fill="both", expand=True)

        # =========================================================================
        # THEME 4: RETRO RIG (Erittäin suuret pyöreät mittarit)
        # =========================================================================
        self.view_retro = tk.Frame(self.view_container, bg="#100b07")
        self.retro_canvas = tk.Canvas(self.view_retro, width=470, height=295, bg="#100b07", highlightthickness=0)
        self.retro_canvas.pack(fill="none", expand=True, pady=(0, 4))

        self.retro_footer = tk.Frame(self.view_retro, bg="#1c1209", highlightthickness=1, highlightbackground="#3d2613")
        self.retro_footer.pack(fill="x", pady=(2, 0))

        rf_row1 = tk.Frame(self.retro_footer, bg="#1c1209")
        rf_row1.pack(fill="x", padx=12, pady=(6, 2))
        self.rf_fwd_lbl = tk.Label(rf_row1, text="FWD: 0.0 W", font=("Consolas", 10, "bold"), fg="#ffaa00", bg="#1c1209")
        self.rf_fwd_lbl.pack(side="left")
        self.rf_swr_lbl = tk.Label(rf_row1, text="SWR: 1.00:1", font=("Consolas", 11, "bold"), fg="#ffaa00", bg="#1c1209")
        self.rf_swr_lbl.pack(side="left", expand=True)
        self.rf_rev_lbl = tk.Label(rf_row1, text="REF: 0.00 W", font=("Consolas", 10, "bold"), fg="#ff7700", bg="#1c1209")
        self.rf_rev_lbl.pack(side="right")

        rf_row2 = tk.Frame(self.retro_footer, bg="#1c1209")
        rf_row2.pack(fill="x", padx=12, pady=(2, 6))
        self.rf_pep_lbl = tk.Label(rf_row2, text="PEAK: 0.0 W", font=("Consolas", 9, "bold"), fg="#ffcc00", bg="#1c1209")
        self.rf_pep_lbl.pack(side="left")
        self.rf_volt_lbl = tk.Label(rf_row2, text="0.00V / 0.00V", font=("Consolas", 9), fg="#996024", bg="#1c1209")
        self.rf_volt_lbl.pack(side="right")

        # =========================================================================
        # THEME 5: CLEAN OLED
        # =========================================================================
        self.view_clean = tk.Frame(self.view_container, bg="#000000")

        cl_top = tk.Frame(self.view_clean, bg="#000000")
        cl_top.pack(fill="x", padx=10, pady=(15, 5))

        fwd_box = tk.Frame(cl_top, bg="#000000")
        fwd_box.pack(side="left", padx=(0, 20))
        self.cl_pwr_num = tk.Label(fwd_box, text="0.0", font=("Consolas", 38, "bold"), fg="#ffffff", bg="#000000")
        self.cl_pwr_num.pack(anchor="w")
        tk.Label(fwd_box, text="WATTS FORWARD", font=("Arial", 8, "bold"), fg="#55657e", bg="#000000").pack(anchor="w")

        swr_box = tk.Frame(cl_top, bg="#000000")
        swr_box.pack(side="left", padx=10)
        self.cl_swr_num = tk.Label(swr_box, text="1.00", font=("Consolas", 38, "bold"), fg="#00e5ff", bg="#000000")
        self.cl_swr_num.pack(anchor="w")
        tk.Label(swr_box, text="SWR RATIO", font=("Arial", 8, "bold"), fg="#55657e", bg="#000000").pack(anchor="w")

        tele_box = tk.Frame(cl_top, bg="#000000")
        tele_box.pack(side="right", padx=(10, 0))
        self.cl_peak_lbl = tk.Label(tele_box, text="PEAK: 0.0 W", font=("Consolas", 10, "bold"), fg="#ffffff", bg="#000000")
        self.cl_peak_lbl.pack(anchor="e")
        self.cl_avg_lbl = tk.Label(tele_box, text="AVG:  0.0 W", font=("Consolas", 10), fg="#8899aa", bg="#000000")
        self.cl_avg_lbl.pack(anchor="e")
        self.cl_rev_lbl = tk.Label(tele_box, text="REF:  0.00 W", font=("Consolas", 10), fg="#ff3366", bg="#000000")
        self.cl_rev_lbl.pack(anchor="e")
        self.cl_volt_lbl = tk.Label(tele_box, text="0.00V / 0.00V", font=("Consolas", 9), fg="#445566", bg="#000000")
        self.cl_volt_lbl.pack(anchor="e")

        tk.Label(self.view_clean, text="POWER OUTPUT LEVEL (0-120W)", font=("Arial", 8, "bold"), fg="#445566", bg="#000000").pack(anchor="w", padx=10, pady=(25, 2))
        self.cl_pwr_canvas = tk.Canvas(self.view_clean, height=4, bg="#0c1017", highlightthickness=0)
        self.cl_pwr_canvas.pack(fill="x", padx=10, pady=(0, 15))

        tk.Label(self.view_clean, text="SWR MATCH LEVEL (1.0 - 5.0)", font=("Arial", 8, "bold"), fg="#445566", bg="#000000").pack(anchor="w", padx=10, pady=(0, 2))
        self.cl_swr_canvas = tk.Canvas(self.view_clean, height=4, bg="#0c1017", highlightthickness=0)
        self.cl_swr_canvas.pack(fill="x", padx=10, pady=(0, 15))

    def init_cross_needle_bg(self):
        """Esilaskee täsmällisen tumman Cross-Needle -taulun, joka sopii tummaan teemaan ja on sopivan kapea"""
        W, H = 460, 320
        scale = 2
        img = Image.new("RGB", (W * scale, H * scale), (14, 19, 29))
        d = ImageDraw.Draw(img)

        # Casing-reunukset tummassa instrumenttityylissä
        d.rounded_rectangle([(6*scale, 6*scale), ((W-6)*scale, (H-6)*scale)], radius=8*scale, fill=(18, 24, 36), outline=(35, 48, 70), width=2*scale)
        d.rounded_rectangle([(10*scale, 10*scale), ((W-10)*scale, (H-10)*scale)], radius=5*scale, fill=(13, 17, 26), outline=(24, 33, 48), width=scale)

        # Tekstit tumman teeman sävyissä
        d.text((32*scale, 20*scale), "Watts", font=FONT_TITLE, fill=(130, 150, 175))
        d.text((26*scale, 100*scale), "FORWARD", font=FONT_BOLD, fill=(0, 255, 136))
        d.text(((W-118)*scale, 100*scale), "REFLECTED", font=FONT_BOLD, fill=(255, 45, 85))

        # Punainen SWR-teksti alhaalla vasemmalla
        d.text((75*scale, 222*scale), "SWR", font=FONT_BIG, fill=(255, 45, 85))
        d.text(((W-90)*scale, (H-22)*scale), "400-0084A", font=FONT_TINY, fill=(70, 90, 115))

        cx0, cy0 = int(230 * scale), int(246 * scale)

        # Kaaret
        fwd_arc_pts = []
        ref_arc_pts = []
        for i in range(50):
            t = i / 49.0
            px_f = int((1-t)**2 * 48*scale + 2*(1-t)*t * 150*scale + t**2 * 300*scale)
            py_f = int((1-t)**2 * 208*scale + 2*(1-t)*t * 80*scale + t**2 * 42*scale)
            fwd_arc_pts.append((px_f, py_f))

            px_r = int((1-t)**2 * (W-48)*scale + 2*(1-t)*t * (W-150)*scale + t**2 * 160*scale)
            py_r = int((1-t)**2 * 208*scale + 2*(1-t)*t * 80*scale + t**2 * 42*scale)
            ref_arc_pts.append((px_r, py_r))

        # Punaiset SWR-viuhkaviivat (alkavat alakeskuksen polttopisteestä cx0, cy0)
        swr_targets = [
            ("1.1", int(375 * scale), int(208 * scale)),
            ("1.3", int(342 * scale), int(160 * scale)),
            ("1.5", int(315 * scale), int(122 * scale)),
            ("2",   int(282 * scale), int(88 * scale)),
            ("2.5", int(230 * scale), int(68 * scale)),
            ("6",   int(178 * scale), int(88 * scale)),
            ("8",   int(145 * scale), int(136 * scale)),
            ("∞",   int(115 * scale), int(208 * scale)),
        ]

        for lbl, tx, ty in swr_targets:
            pts = []
            steps = 30
            for i in range(steps + 1):
                t = i / steps
                ctrl_x = int((cx0 + tx) / 2 + (230*scale - (cx0 + tx)/2) * 0.15)
                ctrl_y = int((cy0 + ty) / 2 + 10 * scale)
                px = int((1-t)**2 * cx0 + 2*(1-t)*t * ctrl_x + t**2 * tx)
                py = int((1-t)**2 * cy0 + 2*(1-t)*t * ctrl_y + t**2 * ty)
                pts.append((px, py))
            d.line(pts, fill=(255, 45, 85, 230), width=2*scale)
            lx = pts[int(steps * 0.65)][0]
            ly = pts[int(steps * 0.65)][1]
            d.text((lx + (5 if tx >= cx0 else -18)*scale, ly - 7*scale), lbl, font=FONT_SWR, fill=(255, 60, 95))

        # Perusviivat nollasta keskustaan
        d.line([(48*scale, 208*scale), (cx0, cy0)], fill=(45, 60, 85), width=2*scale)
        d.line([(cx0, cy0), ((W-48)*scale, 208*scale)], fill=(45, 60, 85), width=2*scale)

        # Kaariviivat
        d.line(fwd_arc_pts, fill=(0, 255, 136, 220), width=3*scale)
        d.line(ref_arc_pts, fill=(255, 45, 85, 220), width=3*scale)

        # Forward jakoviivat ja numerot (0, 10, 25, 50, 75, 100, 120W)
        fwd_ticks = [(0, '0', 0.0), (10, '10', 0.28), (25, '25', 0.45), (50, '50', 0.64), (75, '75', 0.79), (100, '100', 0.91), (120, '120W', 1.0)]
        for p, lbl, t in fwd_ticks:
            idx = int(t * 49)
            x, y = fwd_arc_pts[idx]
            dx = - (fwd_arc_pts[min(49, idx+1)][1] - fwd_arc_pts[max(0, idx-1)][1])
            dy = (fwd_arc_pts[min(49, idx+1)][0] - fwd_arc_pts[max(0, idx-1)][0])
            l = math.hypot(dx, dy)
            nx, ny = int(14 * scale * dx / l), int(14 * scale * dy / l)
            d.line([(x, y), (x + nx, y + ny)], fill=(0, 255, 136), width=2*scale)
            d.text((x + nx*2 - 8*scale, y + ny*2 - 8*scale), lbl, font=FONT_SM, fill=(0, 255, 136))

        # Reflected jakoviivat ja numerot (0, 2, 5, 10, 15, 20, 25W)
        ref_ticks = [(0, '0', 0.0), (2, '2', 0.28), (5, '5', 0.44), (10, '10', 0.63), (15, '15', 0.77), (20, '20', 0.89), (25, '25W', 1.0)]
        for p, lbl, t in ref_ticks:
            idx = int(t * 49)
            x, y = ref_arc_pts[idx]
            dx = - (ref_arc_pts[min(49, idx+1)][1] - ref_arc_pts[max(0, idx-1)][1])
            dy = (ref_arc_pts[min(49, idx+1)][0] - ref_arc_pts[max(0, idx-1)][0])
            l = math.hypot(dx, dy)
            nx, ny = int(-14 * scale * dx / l), int(-14 * scale * dy / l)
            d.line([(x, y), (x + nx, y + ny)], fill=(255, 45, 85), width=2*scale)
            d.text((x + nx*2 - 8*scale, y + ny*2 - 8*scale), lbl, font=FONT_SM, fill=(255, 45, 85))

        # Punainen polttopiste alakeskellä
        d.ellipse([(cx0 - 4*scale, cy0 - 4*scale), (cx0 + 4*scale, cy0 + 4*scale)], fill=(255, 45, 85))

        self.cross_bg = img

    def init_retro_rig_bg(self):
        """Esilaskee Vintage Dual Round Gauges -taustan erittäin suurilla mittareilla ja isoilla numeroilla"""
        W, H = 470, 295
        scale = 2
        img = Image.new("RGBA", (W * scale, H * scale), (16, 11, 7, 255))
        d = ImageDraw.Draw(img)

        # Suuret mittarikellot: Säde r = 108 px
        cx1, cy = int(120 * scale), int(142 * scale)
        cx2 = int(350 * scale)
        r = int(108 * scale)

        for cx, title, is_swr in [(cx1, "POWER (W)", False), (cx2, "SWR", True)]:
            d.ellipse([(cx - r, cy - r), (cx + r, cy + r)], fill=(28, 20, 14, 255), outline=(120, 85, 40, 255), width=4*scale)
            d.ellipse([(cx - r + 6*scale, cy - r + 6*scale), (cx + r - 6*scale, cy + r - 6*scale)], fill=(12, 8, 5, 255), outline=(60, 42, 20, 255), width=2*scale)
            d.text((cx - (40 if not is_swr else 20)*scale, cy + 55*scale), title, font=FONT_BOLD, fill=(255, 170, 0, 255))

            if not is_swr:
                for p in [0, 20, 40, 60, 80, 100, 120]:
                    ang = 210.0 - (p / 120.0) * 240.0
                    th = math.radians(ang)
                    x1 = cx + int((r - 24*scale) * math.cos(th))
                    y1 = cy - int((r - 24*scale) * math.sin(th))
                    x2 = cx + int((r - 12*scale) * math.cos(th))
                    y2 = cy - int((r - 12*scale) * math.sin(th))
                    d.line([(x1, y1), (x2, y2)], fill=(255, 170, 0, 255), width=3*scale)
                    xl = cx + int((r - 42*scale) * math.cos(th))
                    yl = cy - int((r - 42*scale) * math.sin(th))
                    d.text((xl - 10*scale, yl - 10*scale), str(p), font=FONT_BIG, fill=(255, 185, 20, 255))
            else:
                swr_ticks = [(1.0, "1.0", (0, 255, 136, 255)), (1.5, "1.5", (0, 255, 136, 255)),
                             (2.0, "2.0", (255, 215, 0, 255)), (3.0, "3.0", (255, 120, 0, 255)),
                             (5.0, "5.0", (255, 45, 85, 255))]
                for s, lbl, col in swr_ticks:
                    ratio = min(1.0, max(0.0, (s - 1.0) / 4.0))
                    ang = 210.0 - ratio * 240.0
                    th = math.radians(ang)
                    x1 = cx + int((r - 24*scale) * math.cos(th))
                    y1 = cy - int((r - 24*scale) * math.sin(th))
                    x2 = cx + int((r - 12*scale) * math.cos(th))
                    y2 = cy - int((r - 12*scale) * math.sin(th))
                    d.line([(x1, y1), (x2, y2)], fill=col, width=3*scale)
                    xl = cx + int((r - 42*scale) * math.cos(th))
                    yl = cy - int((r - 42*scale) * math.sin(th))
                    d.text((xl - 12*scale, yl - 10*scale), lbl, font=FONT_BIG, fill=col)

        self.retro_bg = img

    def toggle_pin(self):
        self.always_on_top = not self.always_on_top
        self.root.attributes("-topmost", self.always_on_top)
        self.save_settings()
        self.update_btn_colors()

    def update_btn_colors(self):
        theme = self.current_theme
        if self.always_on_top:
            self.ontop_btn.configure(fg="#00ff88", bg="#143324")
        else:
            bg_col = "#182030" if theme in ["MODERN", "CROSS-NEEDLE"] else ("#082410" if theme == "MATRIX" else ("#2a1608" if theme == "RETRO RIG" else "#0e131d"))
            fg_col = "#62728d" if theme in ["MODERN", "CROSS-NEEDLE"] else ("#008f11" if theme == "MATRIX" else ("#a36518" if theme == "RETRO RIG" else "#55657e"))
            self.ontop_btn.configure(fg=fg_col, bg=bg_col)

        close_bg = "#182030" if theme in ["MODERN", "CROSS-NEEDLE"] else ("#082410" if theme == "MATRIX" else ("#2a1608" if theme == "RETRO RIG" else "#0e131d"))
        close_fg = "#8899aa" if theme in ["MODERN", "CROSS-NEEDLE"] else ("#008f11" if theme == "MATRIX" else ("#a36518" if theme == "RETRO RIG" else "#55657e"))
        self.close_btn.configure(bg=close_bg, fg=close_fg)

        if theme == "MODERN":
            self.theme_btn.configure(bg="#182030", fg="#00e5ff")
        elif theme == "CROSS-NEEDLE":
            self.theme_btn.configure(bg="#182030", fg="#ffaa00")
        elif theme == "MATRIX":
            self.theme_btn.configure(bg="#082410", fg="#00ff41")
        elif theme == "RETRO RIG":
            self.theme_btn.configure(bg="#2a1608", fg="#ffaa00")
        elif theme == "CLEAN OLED":
            self.theme_btn.configure(bg="#0e131d", fg="#f0f6fc")

    def cycle_theme(self):
        self.theme_idx = (self.theme_idx + 1) % len(THEMES)
        self.current_theme = THEMES[self.theme_idx]
        self.apply_theme(self.current_theme)
        self.save_settings()

    def apply_theme(self, theme):
        self.current_theme = theme
        self.theme_btn.configure(text=f"🎨 {theme}")
        self.update_btn_colors()

        for v in [self.view_modern, self.view_cross, self.view_matrix, self.view_retro, self.view_clean]:
            v.pack_forget()

        if theme == "MODERN":
            self.view_modern.pack(fill="both", expand=True)
            self.root.configure(bg="#090d14")
            self.main_container.configure(bg="#090d14", highlightbackground="#243048")
            self.header_frame.configure(bg="#121824", highlightbackground="#1a2334")
            self.view_container.configure(bg="#090d14")

        elif theme == "CROSS-NEEDLE":
            self.view_cross.pack(fill="both", expand=True)
            self.root.configure(bg="#090d14")
            self.main_container.configure(bg="#090d14", highlightbackground="#243048")
            self.header_frame.configure(bg="#121824", highlightbackground="#1a2334")
            self.view_container.configure(bg="#090d14")

        elif theme == "MATRIX":
            self.view_matrix.pack(fill="both", expand=True)
            self.root.configure(bg="#020703")
            self.main_container.configure(bg="#020703", highlightbackground="#0a3d16")
            self.header_frame.configure(bg="#041207", highlightbackground="#072b10")
            self.view_container.configure(bg="#020703")

        elif theme == "RETRO RIG":
            self.view_retro.pack(fill="both", expand=True)
            self.root.configure(bg="#100b07")
            self.main_container.configure(bg="#100b07", highlightbackground="#3d220a")
            self.header_frame.configure(bg="#180f08", highlightbackground="#2b1706")
            self.view_container.configure(bg="#100b07")

        elif theme == "CLEAN OLED":
            self.view_clean.pack(fill="both", expand=True)
            self.root.configure(bg="#000000")
            self.main_container.configure(bg="#000000", highlightbackground="#18202e")
            self.header_frame.configure(bg="#05070a", highlightbackground="#10151f")
            self.view_container.configure(bg="#000000")

        self.update_window_size()
        self.save_settings()

    def update_window_size(self):
        """Skaalaa ikkunan koon valitun teeman ja anomaliavalvonnan mukaiseksi ilman tyhjää tilaa"""
        w, h = THEME_DIMENSIONS.get(self.current_theme, (495, 410))
        if self.alert_active:
            h += 32

        try:
            x = self.root.winfo_x()
            y = self.root.winfo_y()
            if x is not None and x > -2000 and y is not None and y > -2000:
                screen_h = self.root.winfo_screenheight()
                if y + h > screen_h - 35:
                    y = max(10, screen_h - h - 45)
                self.root.geometry(f"{w}x{h}+{x}+{y}")
            else:
                self.root.geometry(f"{w}x{h}")
        except Exception:
            self.root.geometry(f"{w}x{h}")

    def network_loop(self):
        """Käyttää suoraa http.client-yhteyttä, joka ei jäädy DNS-hakuihin"""
        conn = None
        current_target = None

        while self.running:
            got_data = False
            for host, port in TARGETS:
                try:
                    if conn is None or current_target != (host, port):
                        conn = http.client.HTTPConnection(host, port, timeout=0.25)
                        current_target = (host, port)

                    conn.request("GET", "/data", headers={"Connection": "keep-alive"})
                    resp = conn.getresponse()
                    if resp.status == 200:
                        payload = resp.read().decode()
                        d = json.loads(payload)
                        self.fwd_w  = d.get("fwd_w", 0.0)
                        self.avg_w  = d.get("avg_w", 0.0)
                        self.rev_w  = d.get("rev_w", 0.0)
                        self.peak_w = d.get("peak_w", 0.0)
                        self.swr    = d.get("swr", 1.0)
                        self.fwd_v  = d.get("fwd_v", 0.0)
                        self.rev_v  = d.get("rev_v", 0.0)
                        self.tx     = d.get("tx", False)
                        self.rssi   = d.get("rssi", 0)
                        self.connected = True
                        got_data = True
                        break
                    else:
                        conn.close()
                        conn = None
                except Exception:
                    if conn:
                        try:
                            conn.close()
                        except:
                            pass
                        conn = None
                    continue

            if not got_data:
                self.connected = False

            time.sleep(0.06)

    def update_ui(self):
        theme = self.current_theme

        if self.connected:
            if self.tx:
                self.status_lbl.configure(text="TX", fg="#ff2a6d", bg="#3d1422")
            else:
                self.status_lbl.configure(text="RX", fg="#00b4d8", bg="#112d3d")

            if self.rssi != 0:
                wifi_color = "#00ff88" if self.rssi >= -67 else ("#ffd166" if self.rssi >= -78 else "#ff1e56")
                self.wifi_lbl.configure(text=f"📶 {self.rssi} dBm", fg=wifi_color)
            else:
                self.wifi_lbl.configure(text="📶 -- dBm", fg="#62728d")

            # Anomaliavalvonta
            self.check_anomalies(self.fwd_w, self.rev_w, self.swr, self.fwd_v, self.rev_v, self.tx)
        else:
            self.status_lbl.configure(text="OFFLINE", fg="#ff2a6d", bg="#291219")
            self.wifi_lbl.configure(text="📶 OFFLINE", fg="#62728d")

        now = time.time()
        if now < self.alert_until:
            if not self.alert_active:
                self.alert_frame.pack(fill="x", padx=10, pady=(0, 4), before=self.view_container)
                self.alert_active = True
                self.update_window_size()

            is_flash = (int(now * 4) % 2 == 0)
            flash_bg = "#ff1e56" if is_flash else "#ffea00"
            flash_fg = "#ffffff" if is_flash else "#000000"
            self.alert_frame.configure(bg=flash_bg, highlightbackground="#ffffff")
            self.alert_lbl.configure(bg=flash_bg, fg=flash_fg, text=f"⚠️ {self.alert_text}")
            self.main_container.configure(highlightbackground=flash_bg)
        else:
            if self.alert_active:
                self.alert_frame.pack_forget()
                self.alert_active = False
                self.update_window_size()
                orig_b = "#0a3d16" if theme == "MATRIX" else ("#3d220a" if theme == "RETRO RIG" else ("#18202e" if theme == "CLEAN OLED" else "#243048"))
                self.main_container.configure(highlightbackground=orig_b)

        # =========================================================================
        # THEME 1: MODERN PÄIVITYS
        # =========================================================================
        if theme == "MODERN":
            pwr_c = get_pwr_color(self.fwd_w)
            avg_c = get_pwr_color(self.avg_w)
            swr_c = get_swr_color(self.swr)
            swr_str = ">99" if self.swr > 90 else f"{self.swr:.2f}"

            self.mod_pwr_num.configure(text=f"{self.fwd_w:.1f}", fg=pwr_c)
            self.mod_pep_txt.configure(text=f"{self.fwd_w:.1f} W", fg=pwr_c)
            self.mod_avg_txt.configure(text=f"{self.avg_w:.1f} W", fg=avg_c)
            self.mod_swr_num.configure(text=swr_str, fg=swr_c)

            self.mod_rev_lbl.configure(text=f"REFLECTED: {self.rev_w:.2f} W")
            self.mod_peak_lbl.configure(text=f"PEAK: {self.peak_w:.1f} W")
            self.mod_volt_lbl.configure(text=f"{self.fwd_v:.2f}V / {self.rev_v:.2f}V")

            w_p = self.mod_pep_canvas.winfo_width()
            if w_p > 1:
                fill_px = int(min(w_p, (self.fwd_w / 120.0) * w_p))
                draw_gradient_bar(self.mod_pep_canvas, w_p, 13, fill_px, self.pwr_lut, peak_w=self.peak_w)

            w_a = self.mod_avg_canvas.winfo_width()
            if w_a > 1:
                fill_avg = int(min(w_a, (self.avg_w / 120.0) * w_a))
                draw_gradient_bar(self.mod_avg_canvas, w_a, 11, fill_avg, self.pwr_lut, peak_w=0.0)

            w_s = self.mod_swr_canvas.winfo_width()
            if w_s > 1:
                ratio_s = 1.0 if self.swr > 90 else min(1.0, max(0.0, (self.swr - 1.0) / 4.0))
                fill_swr = int(w_s * ratio_s)
                draw_gradient_bar(self.mod_swr_canvas, w_s, 13, fill_swr, self.swr_lut, peak_w=0.0)

        # =========================================================================
        # THEME 2: CROSS-NEEDLE PÄIVITYS (Tumma tausta ja vaimennetut neulat)
        # =========================================================================
        elif theme == "CROSS-NEEDLE":
            W, H = 460, 320
            scale = 2
            frame = self.cross_bg.copy()
            d = ImageDraw.Draw(frame)

            # Viisareiden pehmennys/vaimennus (estää tärinän ja luo analogisen viisarin inertian)
            if self.tx and self.fwd_w > 0.4:
                f_speed = 0.28 if self.fwd_w > self.cn_fwd_damped else 0.16
                self.cn_fwd_damped += f_speed * (self.fwd_w - self.cn_fwd_damped)
                r_speed = 0.28 if self.rev_w > self.cn_rev_damped else 0.16
                self.cn_rev_damped += r_speed * (self.rev_w - self.cn_rev_damped)
            else:
                self.cn_fwd_damped += 0.24 * (0.0 - self.cn_fwd_damped)
                self.cn_rev_damped += 0.24 * (0.0 - self.cn_rev_damped)
                if self.cn_fwd_damped < 0.1: self.cn_fwd_damped = 0.0
                if self.cn_rev_damped < 0.05: self.cn_rev_damped = 0.0

            xL, yL = int(100 * scale), int(300 * scale)
            xR, yR = int((W - 100) * scale), int(300 * scale)

            # Forward-viisari oikeasta koneistosta vasemmalle kaarelle
            tF = math.sqrt(min(120.0, max(0.0, self.cn_fwd_damped)) / 120.0)
            txF = int(((1-tF)**2 * 48 + 2*(1-tF)*tF * 150 + tF**2 * 300) * scale)
            tyF = int(((1-tF)**2 * 208 + 2*(1-tF)*tF * 80 + tF**2 * 42) * scale)

            # Reflected-viisari vasemmasta koneistosta oikealle kaarelle
            tR = math.sqrt(min(25.0, max(0.0, self.cn_rev_damped)) / 25.0)
            txR = int(((1-tR)**2 * (W-48) + 2*(1-tR)*tR * (W-150) + tR**2 * 160) * scale)
            tyR = int(((1-tR)**2 * 208 + 2*(1-tR)*tR * 80 + tR**2 * 42) * scale)

            # Hehkuvat selkeät neulat
            d.line([(xR, yR), (txF, tyF)], fill=(0, 255, 136), width=3*scale)
            d.line([(xL, yL), (txR, tyR)], fill=(255, 45, 85), width=3*scale)

            # Neulojen risteyspisteen laskenta
            dxF, dyF = txF - xR, tyF - yR
            dxR, dyR = txR - xL, tyR - yL
            denom = dxF * (-dyR) - dyF * (-dxR)
            if abs(denom) > 1e-4:
                u = ((xL - xR) * (-dyR) - (yL - yR) * (-dxR)) / denom
                if 0.1 <= u <= 1.05:
                    cx = int(xR + u * dxF)
                    cy = int(yR + u * dyF)
                    d.ellipse([(cx - 7*scale, cy - 7*scale), (cx + 7*scale, cy + 7*scale)], fill=(255, 200, 0, 190), outline=(255, 255, 255), width=2*scale)
                    d.ellipse([(cx - 2*scale, cy - 2*scale), (cx + 2*scale, cy + 2*scale)], fill=(255, 255, 255))

            img_res = frame.resize((W, H), Image.Resampling.BILINEAR)
            self.cross_photo = ImageTk.PhotoImage(img_res)
            self.cross_canvas.delete("all")
            self.cross_canvas.create_image(0, 0, anchor="nw", image=self.cross_photo)

            pwr_c = get_pwr_color(self.fwd_w)
            swr_c = get_swr_color(self.swr)
            swr_str = ">99" if self.swr > 90 else f"{self.swr:.2f}"
            self.cn_fwd_lbl.configure(text=f"FWD: {self.fwd_w:.1f} W", fg=pwr_c)
            self.cn_swr_lbl.configure(text=f"SWR: {swr_str}:1", fg=swr_c)
            self.cn_rev_lbl.configure(text=f"REF: {self.rev_w:.2f} W")
            self.cn_pep_lbl.configure(text=f"PEAK: {self.peak_w:.1f} W")
            self.cn_avg_lbl.configure(text=f"AVG: {self.avg_w:.1f} W")
            self.cn_volt_lbl.configure(text=f"{self.fwd_v:.2f}V / {self.rev_v:.2f}V")

        # =========================================================================
        # THEME 3: MATRIX PÄIVITYS
        # =========================================================================
        elif theme == "MATRIX":
            self.matrix_cursor_blink = (self.matrix_cursor_blink + 1) % 6
            cur = "█" if self.matrix_cursor_blink < 3 else " "

            p_rat = min(1.0, max(0.0, self.fwd_w / 120.0))
            a_rat = min(1.0, max(0.0, self.avg_w / 120.0))
            s_rat = 1.0 if self.swr > 90 else min(1.0, max(0.0, (self.swr - 1.0) / 4.0))

            num_b = 26
            p_lit = int(p_rat * num_b)
            a_lit = int(a_rat * num_b)
            s_lit = int(s_rat * num_b)

            p_bar = "█" * p_lit + "░" * (num_b - p_lit)
            a_bar = "█" * a_lit + "░" * (num_b - a_lit)
            s_bar = "█" * s_lit + "░" * (num_b - s_lit)

            eff = 0.0
            if self.fwd_w > 0.5:
                eff = max(0.0, min(100.0, (1.0 - (self.rev_w / self.fwd_w)) * 100.0))

            swr_str = ">99" if self.swr > 90 else f"{self.swr:.2f}"
            carrier_msg = "[CARRIER ACTIVE / TRANSMITTING]" if self.tx else "[STANDBY / RX SCANNING]"
            carrier_wave = "▂▃▅▆▇██▇▆▅▃▂" if self.tx else "            "

            lines = [
                "┌──[ SWR & RF POWER MONITORING TERMINAL v3.2 ]──────────────┐",
                f"│ TARGET: 192.168.1.159:2222    STATUS: [{'TX' if self.tx else 'RX'}]    WIFI: {self.rssi:3d} dBm  │",
                "├───────────────────────────────────────────────────────────┤",
                f"│ FWD PWR : [{p_bar}] {self.fwd_w:5.1f} W PEP │",
                f"│ AVG PWR : [{a_bar}] {self.avg_w:5.1f} W AVG │",
                f"│ SWR RAT : [{s_bar}] {swr_str:>5s} : 1   │",
                "│                                                           │",
                "├─ TELEMETRY & SYSTEM DIAGNOSTICS ──────────────────────────┤",
                f"│ REFLECTED RF: {self.rev_w:5.2f} W         DETECTOR FWD: {self.fwd_v:4.2f} V       │",
                f"│ PEAK ENVELOPE: {self.peak_w:4.1f} W         DETECTOR REV: {self.rev_v:4.2f} V       │",
                f"│ MATCH EFF   : {eff:5.1f} %        STATUS      : {'NOMINAL' if self.swr < 2.0 else 'MISMATCH'}     │",
                "│                                                           │",
                "├─ REALTIME RF OSCILLOGRAM / ACTIVITY ──────────────────────┤",
                f"│ {carrier_wave} {carrier_msg} {cur}  │",
                "└───────────────────────────────────────────────────────────┘"
            ]
            self.mat_terminal.configure(text="\n".join(lines))

        # =========================================================================
        # THEME 4: RETRO RIG PÄIVITYS (Erittäin suuret pyöreät mittarit)
        # =========================================================================
        elif theme == "RETRO RIG":
            W, H = 470, 295
            scale = 2
            cx1, cy = int(120 * scale), int(142 * scale)
            cx2 = int(350 * scale)
            r = int(108 * scale)

            frame = self.retro_bg.copy()
            d = ImageDraw.Draw(frame)

            # Neulojen pehmennys Retro Rigille
            if self.tx and self.fwd_w > 0.4:
                self.retro_pwr_damped += 0.28 * (self.fwd_w - self.retro_pwr_damped)
                self.retro_swr_damped += 0.28 * (self.swr - self.retro_swr_damped)
            else:
                self.retro_pwr_damped += 0.24 * (0.0 - self.retro_pwr_damped)
                self.retro_swr_damped += 0.24 * (1.0 - self.retro_swr_damped)
                if self.retro_pwr_damped < 0.1: self.retro_pwr_damped = 0.0

            p_ang = 210.0 - (min(120.0, max(0.0, self.retro_pwr_damped)) / 120.0) * 240.0
            th_p = math.radians(p_ang)
            tip_p = (cx1 + int((r - 18*scale) * math.cos(th_p)), cy - int((r - 18*scale) * math.sin(th_p)))
            d.line([(cx1, cy), tip_p], fill=(255, 170, 0, 255), width=4*scale)
            d.ellipse([(cx1 - 9*scale, cy - 9*scale), (cx1 + 9*scale, cy + 9*scale)], fill=(60, 45, 25, 255), outline=(150, 110, 50, 255), width=2*scale)

            s_val = self.retro_swr_damped
            s_rat = 1.0 if s_val > 90 else min(1.0, max(0.0, (s_val - 1.0) / 4.0))
            s_ang = 210.0 - s_rat * 240.0
            th_s = math.radians(s_ang)
            tip_s = (cx2 + int((r - 18*scale) * math.cos(th_s)), cy - int((r - 18*scale) * math.sin(th_s)))
            col_s = (0, 255, 136, 255) if s_val <= 1.5 else ((255, 210, 0, 255) if s_val <= 2.0 else (255, 45, 85, 255))
            d.line([(cx2, cy), tip_s], fill=col_s, width=4*scale)
            d.ellipse([(cx2 - 9*scale, cy - 9*scale), (cx2 + 9*scale, cy + 9*scale)], fill=(60, 45, 25, 255), outline=(150, 110, 50, 255), width=2*scale)

            img_res = frame.resize((W, H), Image.Resampling.BILINEAR)
            self.retro_photo = ImageTk.PhotoImage(img_res)
            self.retro_canvas.delete("all")
            self.retro_canvas.create_image(0, 0, anchor="nw", image=self.retro_photo)

            swr_str = ">99" if self.swr > 90 else f"{self.swr:.2f}"
            self.rf_fwd_lbl.configure(text=f"FWD: {self.fwd_w:.1f} W")
            self.rf_swr_lbl.configure(text=f"SWR: {swr_str}:1")
            self.rf_rev_lbl.configure(text=f"REF: {self.rev_w:.2f} W")
            self.rf_pep_lbl.configure(text=f"PEAK: {self.peak_w:.1f} W")
            self.rf_volt_lbl.configure(text=f"{self.fwd_v:.2f}V / {self.rev_v:.2f}V")

        # =========================================================================
        # THEME 5: CLEAN OLED PÄIVITYS
        # =========================================================================
        elif theme == "CLEAN OLED":
            swr_str = ">99" if self.swr > 90 else f"{self.swr:.2f}"
            self.cl_pwr_num.configure(text=f"{self.fwd_w:.1f}")
            self.cl_swr_num.configure(text=swr_str, fg="#00e5ff" if self.swr < 2.0 else "#ff3366")

            self.cl_peak_lbl.configure(text=f"PEAK: {self.peak_w:.1f} W")
            self.cl_avg_lbl.configure(text=f"AVG:  {self.avg_w:.1f} W")
            self.cl_rev_lbl.configure(text=f"REF:  {self.rev_w:.2f} W")
            self.cl_volt_lbl.configure(text=f"{self.fwd_v:.2f}V / {self.rev_v:.2f}V")

            w_p = self.cl_pwr_canvas.winfo_width()
            if w_p > 1:
                self.cl_pwr_canvas.delete("all")
                p_fill = int(min(w_p, (self.fwd_w / 120.0) * w_p))
                if p_fill > 0:
                    self.cl_pwr_canvas.create_rectangle(0, 0, p_fill, 4, fill="#00e5ff", width=0)

            w_s = self.cl_swr_canvas.winfo_width()
            if w_s > 1:
                self.cl_swr_canvas.delete("all")
                s_rat = 1.0 if self.swr > 90 else min(1.0, max(0.0, (self.swr - 1.0) / 4.0))
                s_fill = int(w_s * s_rat)
                if s_fill > 0:
                    col = "#ff3366" if self.swr >= 2.0 else "#00e5ff"
                    self.cl_swr_canvas.create_rectangle(0, 0, s_fill, 4, fill=col, width=0)

        self.root.after(45, self.update_ui)

if __name__ == "__main__":
    root = tk.Tk()
    app = SWRMeterApp(root)
    root.mainloop()
