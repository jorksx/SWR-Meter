import serial
import time
import subprocess
import sys

print("Odotetaan, että ESP32-S3 siirtyy ohjelmointitilaan...")
print("Paina laudasta:")
print("1. Pidä BOOT pohjassa")
print("2. Napsauta RST kerran")
print("3. Päästä BOOT irti\n")

port = "COM7"

while True:
    try:
        s = serial.Serial(port, 115200, timeout=0.2)
        line = s.readline()
        s.close()
        # Jos lauta tulostaa vielä "50%", se on vanhassa ohjelmassa
        if b'50%' in line:
            print("Lauta suorittaa vielä vanhaa koodia... Odota napinpainallusta.", end='\r')
            time.sleep(0.3)
            continue
    except Exception as e:
        time.sleep(0.2)
        continue

    # Testataan esptoolilla yhteys
    cmd = ["esptool", "--port", port, "--chip", "esp32s3", "chip-id"]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if "Detecting chip type... ESP32-S3" in res.stdout or "Chip is ESP32-S3" in res.stdout:
        print("\n ESP32-S3 havaittu ohjelmointitilassa!")
        print("Aloitetaan firmwaren asennus...")
        upload_cmd = ["pio", "run", "--target", "upload", "--upload-port", port]
        env = dict(**dict(PYTHONIOENCODING="utf-8"))
        p = subprocess.run(upload_cmd, text=True)
        if p.returncode == 0:
            print("\n ONNISTUI! Firmware ladattu onnistuneesti!")
        else:
            print("\n Virhe latauksessa.")
        break
    time.sleep(0.5)
