import urllib.request, time, json

print("Kuunnellaan mittaria 15 sekuntia...")
t0 = time.time()
while time.time() - t0 < 15:
    try:
        req = urllib.request.urlopen("http://192.168.1.159:2222/data", timeout=0.3)
        d = json.loads(req.read().decode())
        v_fwd = d.get("fwd_v", 0.0)
        v_rev = d.get("rev_v", 0.0)
        pwr = d.get("fwd_w", 0.0)
        swr = d.get("swr", 1.0)
        tx = d.get("tx", False)
        if tx or v_fwd > 0.01 or v_rev > 0.01:
            print(f"TX HAVAITTU: FWD={v_fwd:.3f}V | REV={v_rev:.3f}V | Pwr={pwr:.1f}W | SWR={swr:.2f}")
    except Exception:
        pass
    time.sleep(0.1)
print("Valmis.")
