import urllib.request, time, json

print("Taustaloggaaja kaynnissa...")
with open("c:\\Users\\Jorks\\Desktop\\swr meter\\live_tx_log.txt", "w") as f:
    f.write("Log started\n")

while True:
    try:
        req = urllib.request.urlopen("http://192.168.1.159:2222/data", timeout=0.2)
        d = json.loads(req.read().decode())
        v_fwd = d.get("fwd_v", 0.0)
        v_rev = d.get("rev_v", 0.0)
        swr = d.get("swr", 1.0)
        tx = d.get("tx", False)
        if tx or v_fwd > 0.02 or v_rev > 0.02:
            line = f"TX: FWD={v_fwd:.3f}V REV={v_rev:.3f}V PWR={d.get('fwd_w',0):.1f}W SWR={swr:.2f}\n"
            print(line.strip())
            with open("c:\\Users\\Jorks\\Desktop\\swr meter\\live_tx_log.txt", "a") as f:
                f.write(line)
    except Exception:
        pass
    time.sleep(0.08)
