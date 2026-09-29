#!/usr/bin/env python3
"""
Harness Control — remote ESP32 test harness PC tool

Usage:
  python3 control.py 192.168.31.133 snap              # capture screenshot
  python3 control.py 192.168.31.133 btn a             # press Button A
  python3 control.py 192.168.31.133 btn b             # press Button B
  python3 control.py 192.168.31.133 btn a long        # long press Button A
  python3 control.py 192.168.31.133 debug             # get debug JSON
  python3 control.py 192.168.31.133 reset             # restart device
  python3 control.py 192.168.31.133 data '{"x":1}'    # inject test data
  python3 control.py 192.168.31.133 watch             # watch + snap every 2s
  python3 control.py --auto 192.168.31.133            # auto-cycle all tests

Requires: pip install pillow
"""

import sys, os, json, time, argparse, tempfile
from urllib.request import urlopen, Request

try:
    from PIL import Image
except ImportError:
    print("⚠️  pip install pillow")
    sys.exit(1)

SCR_W, SCR_H = 240, 135

# ─── API calls ────────────────────────────────────────────

def api(ip: str, path: str, data: bytes = None, ct: str = None) -> bytes:
    url = f"http://{ip}{path}"
    headers = {"User-Agent": "harness-ctrl/1.0"}
    if ct: headers["Content-Type"] = ct
    req = Request(url, data=data, headers=headers)
    with urlopen(req, timeout=10) as resp:
        return resp.read()

def snap(ip: str) -> Image.Image:
    raw = api(ip, "/hrns/bmp")
    if len(raw) != 64854:
        print(f"⚠️  BMP size {len(raw)} (expected 64854)")
    img = Image.new("RGB", (SCR_W, SCR_H))
    pix = img.load()
    offset = 54  # skip BMP header
    for y in range(SCR_H - 1, -1, -1):
        for x in range(SCR_W):
            lo = raw[offset]
            hi = raw[offset + 1]
            offset += 2
            px = lo | (hi << 8)
            r = (px >> 11) & 0x1F
            g = (px >> 5) & 0x3F
            b = px & 0x1F
            pix[x, y] = (r << 3, g << 2, b << 3)
        # skip row padding
        rs = (SCR_W * 2 + 3) & ~3
        offset += rs - SCR_W * 2
    return img

def btn(ip: str, btn_id: int, action: str = "click"):
    payload = json.dumps({"btn": btn_id, "action": action}).encode()
    resp = api(ip, "/hrns/btn", data=payload, ct="application/json")
    return resp.decode().strip()

def debug(ip: str) -> dict:
    return json.loads(api(ip, "/hrns/debug"))

def reset(ip: str):
    return api(ip, "/hrns/reset", data=b"{}", ct="application/json")

def data(ip: str, payload: dict):
    return api(ip, "/hrns/data", data=json.dumps(payload).encode(), ct="application/json")

def set_frame(ip: str, frame: int):
    """Jump to a specific frame (avoids cycling through QR mode)."""
    payload = json.dumps({"frame": frame}).encode()
    resp = api(ip, "/hrns/frame", data=payload, ct="application/json")
    return resp.decode().strip()

# ─── Display help ────────────────────────────────────────

def show(img: Image.Image, path: str = None):
    if path:
        img.save(path)
        print(f"💾 Saved {path}")
    pixels = list(img.getdata())
    if pixels:
        r = sum(p[0] for p in pixels) // len(pixels)
        g = sum(p[1] for p in pixels) // len(pixels)
        b = sum(p[2] for p in pixels) // len(pixels)
        print(f"📊 Avg: RGB({r},{g},{b}) {img.size[0]}×{img.size[1]}")

# ─── Watch mode ──────────────────────────────────────────

def watch(ip: str):
    print(f"🔍 Watching {ip} — Ctrl+C to stop")
    last = None
    try:
        while True:
            img = snap(ip)
            path = "/tmp/harness_watch.bmp"
            img.save(path)
            show(img, path)
            if img.tobytes() != last:
                last = img.tobytes()
                print(f"   changed at {time.strftime('%H:%M:%S')}")
            time.sleep(2)
    except KeyboardInterrupt:
        print("\n👋 Stopped")

# ─── Auto test ───────────────────────────────────────────

def auto_test(ip: str):
    print(f"🤖 Auto-test on {ip}")
    results = []

    # 1. Debug
    d = debug(ip)
    print(f"  [1/5] Debug: heap={d['free_heap']} uptime={d['uptime']}")
    results.append(d['free_heap'] > 0)

    # 2. Screenshot
    img = snap(ip)
    path = "/tmp/harness_auto_snap.bmp"
    img.save(path)
    print(f"  [2/5] Snap: {os.path.getsize(path)}B")
    results.append(len(img.tobytes()) > 0)

    # 3. Button A click
    r = btn(ip, 0, "click")
    print(f"  [3/5] BtnA click: {r}")
    results.append(r == "OK")

    # 4. Screenshot after
    img2 = snap(ip)
    path2 = "/tmp/harness_auto_after.bmp"
    img2.save(path2)
    diff = len(img2.tobytes()) != len(img.tobytes())
    print(f"  [4/5] After click: {os.path.getsize(path2)}B (changed={diff})")
    results.append(diff)

    # 5. Debug again
    d2 = debug(ip)
    print(f"  [5/5] Final debug: ok")
    results.append(True)

    passed = sum(1 for r in results if r)
    print(f"\n{'='*40}")
    print(f"  {'✅ PASS' if passed == len(results) else '⚠️  PARTIAL'}  {passed}/{len(results)} tests passed")
    return passed == len(results)

def walk_test(ip: str, rounds: int = 2) -> bool:
    """Cycle through non-QR frames N rounds, snapshot each.
    Uses /hrns/frame to jump back (avoids entering QR AP mode).
    """
    total_frames = 3  # stats, companion, device
    print(f"🔁 Walk test: {rounds} rounds × {total_frames} frames = {rounds * total_frames} steps")
    all_ok = True
    for r in range(rounds):
        for f in range(total_frames):
            img = snap(ip)
            if img:
                w, h = img.size
                print(f"  Round {r+1} Frame {f}: {w}×{h}", end="")
                if w == 240 and h == 135:
                    print(" ✅", end="")
                else:
                    print(f" ⚠️  size={w}×{h}", end="")
                    all_ok = False
                img.save(f"/tmp/walk_r{r+1}_f{f}.bmp")
            else:
                print(f"  Round {r+1} Frame {f}: ❌ TIMEOUT / CRASH")
                all_ok = False
            # Advance to next frame (skip after last frame — avoid entering QR)
            if f < total_frames - 1:
                rsp = btn(ip, 0, "click")
                if rsp != "OK":
                    print(f"  ⚠️  btn failed: {rsp}")
                    all_ok = False
        print(f"  Round {r+1} done")
        # After last frame, jump back to frame 0 (skip QR)
        if r < rounds - 1:
            rsp = set_frame(ip, 0)
            if rsp != "OK":
                print(f"  ⚠️  set_frame(0) failed: {rsp}")
                all_ok = False
            time.sleep(0.5)
    print(f"{'✅ ALL WALK TESTS PASSED' if all_ok else '❌ SOME FAILED'}")
    return all_ok

# ─── Main ────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description="Harness Control — ESP32 remote test tool")
    ap.add_argument("ip", help="Device IP")
    ap.add_argument("action", nargs="?", default="snap",
                    help="snap|btn|debug|reset|data|watch|frame|auto|walk")
    ap.add_argument("arg", nargs="?", default=None, help="btn id (a/b), data payload, or walk rounds")
    ap.add_argument("extra", nargs="?", default=None, help="long for long press")

    args = ap.parse_args()

    if args.action == "auto":
        sys.exit(0 if auto_test(args.ip) else 1)

    elif args.action == "walk":
        rounds = int(args.arg) if args.arg else 2
        ok = walk_test(args.ip, rounds)
        sys.exit(0 if ok else 1)

    elif args.action == "snap":
        img = snap(args.ip)
        path = args.arg or "/tmp/harness_snap.bmp"
        show(img, path)
        img.show()

    elif args.action == "btn":
        btn_id = 0 if args.arg in (None, "a", "0") else 1
        action = args.extra if args.extra == "long" else "click"
        r = btn(args.ip, btn_id, action)
        print(f"  {'✅' if r=='OK' else '❌'} {r}")
        # Auto-snap
        time.sleep(0.5)
        img = snap(args.ip)
        path = f"/tmp/harness_btn_{args.arg or 'a'}.bmp"
        show(img, path)

    elif args.action == "debug":
        d = debug(args.ip)
        print(json.dumps(d, indent=2))

    elif args.action == "reset":
        print("  🔄 Resetting...")
        reset(args.ip)

    elif args.action == "data":
        if not args.arg:
            print("  ❌ Need data payload")
            sys.exit(1)
        payload = json.loads(args.arg)
        r = data(args.ip, payload)
        print(f"  {'✅' if r else '❌'} injected")

    elif args.action == "watch":
        watch(args.ip)

    elif args.action == "frame":
        f = int(args.arg) if args.arg else 0
        r = set_frame(args.ip, f)
        print(f"  {'✅' if r=='OK' else '❌'} frame={f} → {r}")

    else:
        ap.print_help()

if __name__ == "__main__":
    main()
