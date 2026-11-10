#!/usr/bin/env python3
"""Mock of the S3 firmware's HTTP API, for working on the web UI on a PC.

    python3 tools/mock_server.py [port]      (default 8000)

Serves data/ and answers /api/* from memory, with a WebSocket at /ws that
broadcasts what the firmware does (init, race_*, lap, settings, calib,
active_update, config_changed), so several browsers can be checked
against each other. GET /mock/lap?slot=N records a pass for slot N.
GET /mock/seed loads sample pilots and a stopped race (for screenshots).
"""
import base64
import random
import hashlib
import json
import re
import os
import sys
import threading
import time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data")

# Sample race files on the "SD card": the newest with the #FPVC5 line,
# race_001 in the old format (no date, no rules).
def _race_csv(no, pilots, laps, meta=True, rank=0, holeshot=True):
    import random
    rnd = random.Random(no)
    out = "\ufeff"
    if meta:
        out += (f"#FPVC5,v=1,start=2026-11-{1 + no:02d}T14:{10 + no:02d}:05+09:00,mode="
                f"{'holeshot' if holeshot else 'immediate'},laps={laps if rank != 2 else 0},rank={rank},"
                f"limit_s={120 if rank == 2 else 0},lastlap=1,elapsed_ms=60000,event=サンプル大会\n")
    out += "Slot,Name,FreqMHz,Lap,LapTime_ms,PeakRSSI,Timestamp_ms\n"
    ts = 0
    for slot, (name, base) in enumerate(pilots):
        n = laps - (1 if slot == len(pilots) - 1 else 0)
        rows = [(0, 1800 + slot * 250)] if holeshot else []
        rows += [(i + 1, int(base - no * 120 + rnd.uniform(-600, 900))) for i in range(n)]
        for lap, ms in rows:
            ts += ms
            out += f"{slot},{name},{5658 + slot * 37},{lap},{ms},{rnd.randint(500, 800)},{ts}\n"
    return out.encode()


_PILOTS = [("たろう", 19200), ("はなこ", 19600), ("じろう", 20100), ("さくら", 20800)]
SD_FILES = {"pilots.csv": ("\ufeffname,yomi,freq\n".encode(), 0)}
SD_FILES["race_001.csv"] = (_race_csv(1, _PILOTS[:3], 3, meta=False), 0)
for _no in range(2, 15):
    SD_FILES[f"race_{_no:03d}_202611{1 + _no:02d}_14{10 + _no:02d}.csv"] = (_race_csv(_no, _PILOTS, 3 if _no % 4 else 5, rank=2 if _no % 5 == 0 else 0),
                                       1793000000 + _no * 900)

# Six pilots: one with a single lap, one with only the holeshot.
SD_FILES["race_015_20261116_1500.csv"] = ((
    "\ufeff#FPVC5,v=1,start=2026-11-16T15:00:05+09:00,mode=holeshot,laps=3,rank=0,limit_s=0,lastlap=1,"
    "elapsed_ms=60000,event=サンプル大会\nSlot,Name,FreqMHz,Lap,LapTime_ms,PeakRSSI,Timestamp_ms\n"
    + "".join(f"{s},{n},{5658 + s * 37},{lap},{ms},700,0\n"
              for s, (n, laps) in enumerate([("たろう", [17800, 18100, 17600]), ("はなこ", [18300, 18000, 18400]),
                                             ("じろう", [19000, 18700, 18900]), ("さくら", [19500, 19800, 19200]),
                                             ("けんた", [21000]), ("ゆき", [])])
              for lap, ms in [(0, 2000 + s * 100)] + list(enumerate(laps, 1)))).encode(), 1793020000)


# Like a field test: two pilots recorded the same passes (one quad).
SD_FILES["race_016_20261116_1639.csv"] = ((
    "\ufeff#FPVC5,v=1,start=2026-11-16T16:39:05+09:00,mode=holeshot,laps=0,rank=1,limit_s=0,lastlap=1,"
    "elapsed_ms=60000,event=\nSlot,Name,FreqMHz,Lap,LapTime_ms,PeakRSSI,Timestamp_ms\n"
    + "".join(f"{s},{n},{5658 + s * 37},{lap},{ms},700,0\n"
              for s, (n, laps) in enumerate([("そうた", [11600, 18100, 8420]), ("みお", []),
                                             ("れん", [13300, 11000, 9500]), ("ひな", [13300, 11000, 9500, 8900])])
              for lap, ms in [(0, 2000)] + list(enumerate(laps, 1)))).encode(), 1793030000)
START = time.monotonic()
BOOT_ID = random.randint(1, 2**31)
state = {
    "pilots": [],
    "active": [-1] * 8,
    "settings": {"lapMode": 0, "cooldownMs": 3000, "sdLogMode": 0, "totalLaps": 0, "gain": 30,
                 "rankMode": 0, "timeLimitSec": 120, "countLastLap": True, "startDelayOn": False, "startDelaySec": 10},
    "elrs": {
        "enabled": False, "transport": 1, "host": "auto", "port": 8080, "rdBind": "", "rdSwitch": 0, "eventName": "",
        "showRaceClock": True, "showBestLap": True, "positionMode": True, "showTotalTime": False,
        "resultsMode": True, "stageMsg": "w ARM NOW x", "startMsg": "w   GO!   x",
        "pilotDoneMsg": "w FINISHED! x", "raceStopMsg": "w  LAND NOW!  x", "startUptime": 5,
        "finishUptime": 100, "resultsUptime": 50, "announceUptime": 50,
        "pos": {"eventName": "0,-1", "announcement": "3,-1", "status": "5,-1", "currentLap": "1,-1",
                "lapResults": "8,-1", "bestLap": "12,0", "totalTime": "3,-1", "results": "10,-1",
                "raceClock": "2,-1"},
    },
    "net": {"mode": 0, "apSsid": "FPV-C5-LT", "apPass": "fpv-c5-lt", "staSsid": "", "staPassSet": False, "fallbackSec": 30,
            "ipFixed": False, "ip": "", "mask": "", "gw": "", "dns": "",
            "status": {"ap": True, "connected": False, "fallback": False, "ip": "20.0.0.1", "rssi": 0}},
    "password": "admin",
    "posts": [],   # every POST, for tests
    "race": {"running": False, "paused": False, "t0": 0.0, "elapsed": 0.0, "laps": [], "epoch": 0},
}

ws_clients = []
ws_lock = threading.Lock()
# As the firmware (ws_handler.cpp): browsers are viewers until their hello
# carries an admin token; admins get rssi_all; the oldest viewers over the
# limit are closed with code 4001; admin logins are limited.
ws_role = {}        # wfile -> {"since": s, "admin": bool}
sessions = {}       # token -> last seen (s)
SESSION_GRACE = 60.0   # "holdSec" in CONN
CONN = {"adminMax": 2, "viewHost": 3, "viewClient": 6, "holdSec": 60}
CONN_LIMITS = {"adminLimit": 4, "viewHostLimit": 5, "viewClientLimit": 8}


def live_sessions():
    now = time.monotonic()
    held = {r.get("token") for r in ws_role.values() if r.get("admin")}
    return [t for t, seen in sessions.items() if t in held or now - seen < CONN["holdSec"]]


def ws_close(wfile, code):
    try:
        wfile.write(bytes([0x88, 2]) + code.to_bytes(2, "big"))
        wfile.flush()
    except OSError:
        pass


def enforce_viewers():
    now = time.monotonic()
    with ws_lock:
        viewers = sorted((r["since"], w) for w, r in ws_role.items()
                         if not r.get("admin") and now - r["since"] >= 3 and w in ws_clients)
        extra = len(viewers) - CONN["viewHost"]
        for _, w in viewers[:max(0, extra)]:
            ws_close(w, 4001)
            ws_clients.remove(w)


# The timer's clock is millis(): it starts again from 0 when the timer
# restarts. So does this mock's (time.monotonic from this process's start).
_mono, _BOOT = time.monotonic, time.monotonic()
time.monotonic = lambda: _mono() - _BOOT + 1.0


def now_ms():
    return int(time.monotonic() * 1000)


def ws_frame(text):
    b = text.encode()
    n = len(b)
    if n < 126:
        head = bytes([0x81, n])
    elif n < 65536:
        head = bytes([0x81, 126]) + n.to_bytes(2, "big")
    else:
        head = bytes([0x81, 127]) + n.to_bytes(8, "big")
    return head + b


def ws_send(wfile, obj):
    try:
        wfile.write(ws_frame(json.dumps(obj)))
        wfile.flush()
        return True
    except OSError:
        return False


def broadcast(obj, admins_only=False):
    with ws_lock:
        for w in list(ws_clients):
            if admins_only and not ws_role.get(w, {}).get("admin"):
                continue
            if not ws_send(w, obj):
                ws_clients.remove(w)


def race_elapsed():
    r = state["race"]
    return r["elapsed"] + (time.monotonic() - r["t0"] if r["running"] else 0.0)


def init_msg():
    r = state["race"]
    counts = [0] * 8
    for l in r["laps"]:
        counts[l["slot"]] += 1
    pilots = []
    for o in active_json():
        o = dict(o, lapCount=counts[o["slot"]], bestLap=0, rssi=0, crossing=False)
        pilots.append(o)
    return dict(state["settings"], type="init", raceRunning=r["running"], racePaused=r["paused"],
                boot=BOOT_ID, raceElapsedMs=int(race_elapsed() * 1000), raceStartMs=int((time.monotonic() - race_elapsed()) * 1000),
                now=now_ms(), sdPresent=True, pilots=pilots)


SAMPLE = [  # name, yomi, freq, holeshot ms, lap times ms
    ("Yana", "やな", 5658, 2100, [21340, 20480, 19870, 20110]),
    ("Taro", "たろう", 5695, 2350, [22150, 21020, 21460, 20890]),
    ("Hana", "はな", 5732, 2600, [23010, 22340, 21980]),
    ("Ken", "けん", 5769, 2450, [21870, 22760, 21550, 21330]),
    ("Mio", "みお", 5806, 2900, [24120, 23380, 22910]),
    ("Sora", "そら", 5843, 3100, [25410, 24020]),
]


def seed():
    """Sample pilots on Ch1-6 and a stopped race with their laps."""
    state["pilots"] = [{"name": n, "yomi": y, "freq": f, "bind": "", "osd": True, "enter": 350, "exit": 280}
                       for n, y, f, _, _ in SAMPLE]
    state["active"] = list(range(len(SAMPLE))) + [-1] * (8 - len(SAMPLE))
    state["elrs"]["enabled"] = True
    t0 = now_ms() - 100000
    laps = []
    for slot, (_, _, _, hs, times) in enumerate(SAMPLE):
        ts = t0 + hs
        laps.append({"slot": slot, "lapTime": hs, "ts": ts})
        for t in times:
            ts += t
            laps.append({"slot": slot, "lapTime": t, "ts": ts})
    laps.sort(key=lambda l: l["ts"])
    r = state["race"]
    r.update(running=False, paused=True, t0=time.monotonic() - 96.4, elapsed=96.4, laps=laps, epoch=r["epoch"] + 1)
    broadcast({"type": "active_update", "pilots": active_json()})


def slot_of(pid):
    return state["active"].index(pid) if pid in state["active"] else -1


def active_json():
    out = []
    for s, ri in enumerate(state["active"]):
        p = state["pilots"][ri] if 0 <= ri < len(state["pilots"]) else None
        o = {"slot": s, "rosterIdx": ri, "name": p["name"] if p else "---", "yomi": p["yomi"] if p else "",
             "freq": p["freq"] if p else 0}
        if p:
            o.update(enter=p["enter"], exit=p["exit"])
        out.append(o)
    return out


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=DATA, **kw)

    def log_message(self, *_):
        pass

    def reply(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urlparse(self.path)
        p = u.path
        if p == "/ws":
            return self.websocket()
        if p == "/mock/lap":
            slot = int(parse_qs(u.query).get("slot", ["0"])[0])
            r = state["race"]
            mine = [l for l in r["laps"] if l["slot"] == slot]
            ts = now_ms()
            lap_ms = ts - (mine[-1]["ts"] if mine else int(r["t0"] * 1000))
            r["laps"].append({"slot": slot, "lapTime": lap_ms, "ts": ts})
            broadcast({"type": "lap", "pilot": slot, "lapTime": lap_ms, "lapCount": len(mine) + 1, "ts": ts})
            return self.reply({"ok": True})
        if p == "/mock/seed":
            seed()
            return self.reply({"ok": True})
        if p == "/api/laps":
            return self.reply(state["race"]["laps"])
        if not p.startswith("/api/"):
            return super().do_GET()
        if p == "/api/pilots":
            return self.reply([dict(x, id=i, activeSlot=slot_of(i)) for i, x in enumerate(state["pilots"])])
        if p == "/api/active":
            return self.reply(active_json())
        if p == "/api/settings":
            return self.reply(state["settings"])
        if p == "/api/elrs":
            return self.reply(state["elrs"])
        if p == "/api/net":
            return self.reply(state["net"])
        if p == "/api/conn":
            with ws_lock:
                admins = sum(1 for w, r in ws_role.items() if r.get("admin"))
                viewers = sum(1 for w, r in ws_role.items() if not r.get("admin"))
                logins = len(live_sessions())
            return self.reply({**CONN, **CONN_LIMITS, "ap": True, "admins": admins, "logins": logins,
                               "viewers": viewers, "viewerLimit": CONN["viewHost"]})
        if p == "/api/sys":
            return self.reply({"uptime": int(time.monotonic()), "reason": "restart", "boots": 12,
                               "crashes": 1, "lastCrash": "panic", "lastCrashAgo": 3, "heap": 182000,
                               "heapMin": 151000, "heapMax": 110000, "psram": 0})
        if p == "/api/sd/status":
            return self.reply({"present": True})
        if p == "/api/sd/files":
            return self.reply([{"name": n, "size": len(b), **({"t": t} if t else {})}
                               for n, (b, t) in SD_FILES.items()])
        if p == "/api/sd/file":
            q = parse_qs(u.query)
            path = q.get("path", [""])[0]
            base = path.rsplit("/", 1)[-1].lower()
            if ((base.startswith("settings") and base.endswith(".json")) or (base.startswith("qr_") and base.endswith(".png"))) \
                    and q.get("token", [""])[0] not in live_sessions():
                return self.reply({"error": "login"}, 403)
            if path.lstrip("/") not in SD_FILES:
                return self.reply({"error": "not found"}, 404)
            time.sleep(0.15)   # a real card over SPI is not instant
            body = SD_FILES[path.lstrip("/")][0]
            self.send_response(200)
            self.send_header("Content-Type", "image/png" if base.endswith(".png") else
                             "application/json" if base.endswith(".json") else "text/csv; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        return self.reply({"error": "not found"}, 404)

    def websocket(self):
        key = self.headers.get("Sec-WebSocket-Key", "")
        self.protocol_version = "HTTP/1.1"   # a 101 must be HTTP/1.1
        acc = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
        self.send_response(101)
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", acc.decode())
        self.end_headers()
        self.wfile.flush()
        with ws_lock:
            ws_send(self.wfile, init_msg())
            ws_clients.append(self.wfile)
            ws_role[self.wfile] = {"since": time.monotonic(), "admin": False}
        try:
            while True:   # client frames (masked): hello, or close
                h = self.rfile.read(2)
                if len(h) < 2:
                    break
                op, n = h[0] & 0x0F, h[1] & 0x7F
                if n == 126:
                    n = int.from_bytes(self.rfile.read(2), "big")
                elif n == 127:
                    n = int.from_bytes(self.rfile.read(8), "big")
                mask = self.rfile.read(4) if h[1] & 0x80 else b"\0\0\0\0"
                data = bytes(b ^ mask[i % 4] for i, b in enumerate(self.rfile.read(n)))
                if op == 8:
                    break
                if op == 1:
                    msg = json.loads(data or b"{}")
                    if msg.get("type") == "hello":
                        tok = msg.get("token", "")
                        admin = tok in live_sessions()
                        with ws_lock:
                            ws_role[self.wfile].update(admin=admin, token=tok)
                            if admin:
                                sessions[tok] = time.monotonic()
                                state["clock"] = [msg.get("epoch"), msg.get("tz")]
                            ws_send(self.wfile, {"type": "hello", "admin": admin})
        except OSError:
            pass
        with ws_lock:
            r = ws_role.pop(self.wfile, {})
            if r.get("admin"):
                sessions[r["token"]] = time.monotonic()
            if self.wfile in ws_clients:
                ws_clients.remove(self.wfile)
        self.close_connection = True

    def do_POST(self):
        p = urlparse(self.path).path
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b""
        if p == "/api/sd/upload":   # a QR image (PNG body)
            q = parse_qs(urlparse(self.path).query)
            path = q.get("path", [""])[0]
            if q.get("token", [""])[0] not in live_sessions():
                return self.reply({"ok": False, "reason": "login"}, 403)
            if not re.fullmatch(r"/qr_[A-Za-z0-9_-]+\.png", path) or not raw or len(raw) > 160 * 1024:
                return self.reply({"ok": False, "reason": "path" if raw else "size"}, 400)
            SD_FILES[path[1:]] = (raw, int(time.time()))
            state["posts"].append([p, {"path": path, "size": len(raw)}])
            return self.reply({"ok": True})
        d = json.loads(raw) if raw else {}
        state["posts"].append([p, d])
        if p == "/api/reload-all":
            if d.get("token", "") not in live_sessions():
                return self.reply({"ok": False, "reason": "login"}, 403)
            broadcast({"type": "reload"})
            return self.reply({"ok": True})
        if p == "/api/sd/settings/backup":
            if d.get("token", "") not in live_sessions():
                return self.reply({"ok": False, "reason": "login"}, 403)
            net = {k: v for k, v in state["net"].items() if k not in ("status", "staPassSet")}
            net.update(staPass="router-pass" if state["net"].get("staSsid") else "", staPassClear=not state["net"].get("staSsid"))
            doc = {"kind": "fpv-c5-lt-settings", "ver": 1, "saved": int(time.time()),
                   "settings": state["settings"], "elrs": state["elrs"],
                   "conn": dict(CONN),
                   "net": net, "auth": {"password": state["password"]}}
            name = time.strftime("settings_%Y%m%d_%H%M.json")
            SD_FILES[name] = (json.dumps(doc, ensure_ascii=False, indent=2).encode(), int(time.time()))
            return self.reply({"ok": True, "file": name})
        if p == "/api/sd/settings/restore":
            if d.get("token", "") not in live_sessions():
                return self.reply({"ok": False, "reason": "login"}, 403)
            if state["race"]["running"] or state["race"]["paused"]:
                return self.reply({"ok": False, "reason": "race"}, 409)
            name = d.get("path", "/settings.json").lstrip("/")
            if not re.fullmatch(r"settings(_\d{8}_\d{4})?\.json", name) or name not in SD_FILES:
                return self.reply({"ok": False, "reason": "nofile"}, 404)
            try:
                b = json.loads(SD_FILES[name][0])
                assert b.get("kind") == "fpv-c5-lt-settings"
            except Exception:
                return self.reply({"ok": False, "reason": "badfile"}, 422)
            state["settings"].update(b.get("settings", {}))
            broadcast(dict(state["settings"], type="settings"))
            state["elrs"].update(b.get("elrs", {}))
            broadcast({"type": "config_changed", "what": "elrs"})
            n = b.get("net", {})
            state["net"].update({k: v for k, v in n.items() if k in state["net"] and k != "status"})
            broadcast({"type": "config_changed", "what": "net"})
            CONN.update({k: v for k, v in b.get("conn", {}).items() if k in CONN})
            if b.get("auth", {}).get("password"):
                state["password"] = b["auth"]["password"]
            return self.reply({"ok": True, "net": True, "saved": b.get("saved")})
        if p == "/api/pilots":
            pid = d.get("id", -1)
            rec = {k: d.get(k) for k in ("name", "yomi", "freq", "bind", "osd")}
            rec["enter"] = d.get("enter", 350)
            rec["exit"] = d.get("exit", 280)
            if pid == -1:
                state["pilots"].append(rec)
                pid = len(state["pilots"]) - 1
            else:
                state["pilots"][pid].update(rec)
            broadcast({"type": "active_update", "pilots": active_json()})
            return self.reply({"ok": True, "id": pid})
        if p == "/api/pilots/delete":
            pid = d["id"]
            del state["pilots"][pid]
            state["active"] = [-1 if a == pid else (a - 1 if a > pid else a) for a in state["active"]]
            return self.reply({"ok": True})
        if p == "/api/active":
            state["active"] = list(d["slots"])[:8]
            broadcast({"type": "active_update", "pilots": active_json()})
            return self.reply({"ok": True})
        if p == "/api/calib":
            state["pilots"][d["id"]].update(enter=d["enter"], exit=d["exit"])
            broadcast({"type": "calib", "id": d["id"], "enter": d["enter"], "exit": d["exit"]})
            return self.reply({"ok": True})
        if p == "/api/settings":
            state["settings"].update(d)
            broadcast(dict(state["settings"], type="settings"))
            return self.reply({"ok": True})
        r = state["race"]
        # Start (web button or /api/mock/rd = the race director's switch): the
        # timer runs ready -> stage -> start, or resumes a stopped race.
        if p in ("/api/race/start", "/api/mock/rd"):
            if r["running"] or r.get("seq"):
                return self.reply({"ok": True})
            if r["paused"]:
                r.update(running=True, paused=False, t0=time.monotonic())
                broadcast({"type": "race_resume", "ts": int(r["t0"] * 1000 - r["elapsed"] * 1000)})
                return self.reply({"ok": True})
            token = object()
            r["seq"] = token
            def seq():
                wait = int(state["settings"].get("startDelaySec", 0)) if state["settings"].get("startDelayOn") else 0
                if wait > 0:
                    r["waitEnd"] = time.monotonic() + wait
                    broadcast({"type": "race_wait", "sec": wait, "now": now_ms()})
                    while time.monotonic() < r["waitEnd"]:
                        time.sleep(0.05)
                        if r.get("seq") is not token: r["waitEnd"] = 0; return
                    r["waitEnd"] = 0
                broadcast({"type": "race_ready"}); time.sleep(1.5)
                if r.get("seq") is not token: return
                broadcast({"type": "race_stage"}); time.sleep(3.3)
                if r.get("seq") is not token: return
                r["seq"] = None
                r.update(running=True, paused=False, t0=time.monotonic(), elapsed=0.0, laps=[], epoch=r["epoch"] + 1)
                broadcast({"type": "race_start", "ts": int(r["t0"] * 1000)})
            threading.Thread(target=seq, daemon=True).start()
            return self.reply({"ok": True})
        if p == "/api/race/stop":
            if not r["running"] and not r.get("seq"):
                return self.reply({"ok": True})
            r["seq"] = None
            if r["running"]:
                r.update(running=False, paused=True, elapsed=race_elapsed())
            broadcast({"type": "race_stop", "elapsed": int(race_elapsed() * 1000)})
            return self.reply({"ok": True})
        if p == "/api/net":
            state["net"].update({k: d[k] for k in ("ipFixed", "ip", "mask", "gw", "dns") if k in d})
            state["net"].update(mode=d.get("mode", 0), apSsid=d.get("apSsid"), apPass=d.get("apPass"),
                                staSsid=d.get("staSsid", ""), fallbackSec=d.get("fallbackSec", 30))
            if d.get("staPass"):
                state["net"]["staPassSet"] = True
            return self.reply({"ok": True})
        if p == "/api/elrs":
            state["elrs"].update(d)
            broadcast({"type": "config_changed", "what": "elrs"})
            return self.reply({"ok": True})
        if p == "/api/auth/password":
            state["password"] = d.get("password", state["password"])
            return self.reply({"ok": True})
        if p == "/api/auth/login":
            if d.get("password") != state["password"]:
                return self.reply({"ok": False, "error": "bad password"}, 401)
            with ws_lock:
                if len(live_sessions()) >= CONN["adminMax"]:
                    return self.reply({"ok": False, "reason": "full", "max": CONN["adminMax"]}, 409)
                tok = "%016x" % random.getrandbits(64)
                sessions[tok] = time.monotonic()
            return self.reply({"ok": True, "token": tok})
        if p == "/api/auth/logout":
            with ws_lock:
                sessions.pop(d.get("token", ""), None)
                for w, r in ws_role.items():
                    if r.get("token") == d.get("token") and r.get("admin"):
                        r["admin"] = False
                        ws_send(w, {"type": "hello", "admin": False})
            return self.reply({"ok": True})
        if p == "/api/conn":
            for k in CONN:
                if k in d:
                    CONN[k] = int(d[k])
            return self.reply({"ok": True})
        if p == "/api/laps/delete":   # the timer's race::Core::deleteLap
            mine = [l for l in r["laps"] if l["slot"] == d.get("slot")]
            i = d.get("index", -1)
            if not 0 <= i < len(mine) or (d.get("lapTime") and mine[i]["lapTime"] != d["lapTime"]):
                return self.reply({"ok": False, "reason": "no such lap"}, 409)
            if i + 1 < len(mine):
                mine[i + 1]["lapTime"] += mine[i]["lapTime"]
            r["laps"].remove(mine[i])
            broadcast({"type": "laps_changed", "pilot": d["slot"]})
            return self.reply({"ok": True})
        if p == "/api/race/save":
            had = bool(r["laps"])
            r.update(running=False, paused=False, elapsed=0.0, laps=[], epoch=r["epoch"] + 1)
            broadcast({"type": "race_clear"})
            return self.reply({"ok": True, "saved": False, "reason": "nosd" if had else "empty"})
        if p == "/api/sd/files/delete":
            gone = SD_FILES.pop(d.get("path", "").lstrip("/"), None)
            return self.reply({"ok": gone is not None})
        if p == "/api/laps/manual":   # the admin's lap button: a pass now
            r = state["race"]
            slot = int(d.get("slot", -1))
            if not r["running"] or not 0 <= slot < 8:
                return self.reply({"ok": False, "reason": "not racing"}, 409)
            mine = [l for l in r["laps"] if l["slot"] == slot]
            ts = now_ms()
            lap_ms = ts - (mine[-1]["ts"] if mine else int(r["t0"] * 1000))
            r["laps"].append({"slot": slot, "lapTime": lap_ms, "ts": ts})
            broadcast({"type": "lap", "pilot": slot, "lapTime": lap_ms, "lapCount": len(mine) + 1, "ts": ts})
            return self.reply({"ok": True})
        if p == "/api/net/wifi-qr":
            if d.get("token", "") not in live_sessions():
                return self.reply({"ok": False, "reason": "login"}, 403)
            return self.reply({"ssid": state["net"].get("staSsid") or "MyRouter", "pass": "router-pass"})
        if p == "/api/sd/erase":
            if d.get("password") != state["password"]:
                return self.reply({"ok": False, "reason": "password"}, 401)
            n = len(SD_FILES)
            SD_FILES.clear()
            return self.reply({"ok": True, "removed": n})
        if p == "/api/clock":
            return self.reply({"ok": True})
        if p == "/api/sd/pilots/backup":
            name = time.strftime("pilots_%Y%m%d_%H%M.csv")
            csv = "\ufeffname,yomi,freq\n" + "".join(f"{x['name']},{x.get('yomi') or ''},{x.get('freq') or 0}\n" for x in state["pilots"])
            SD_FILES[name] = (csv.encode(), int(time.time()))
            return self.reply({"ok": True, "file": name})
        if p == "/api/sd/pilots/restore":
            name = d.get("path", "/pilots.csv").lstrip("/")
            if not re.fullmatch(r"pilots(_\d{8}_\d{4})?\.csv", name) or name not in SD_FILES:
                return self.reply({"error": "no such backup"}, 503)
            return self.reply({"ok": True, "pilots": len(state["pilots"])})
        return self.reply({"ok": True})


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    print(f"mock FPV C5 Lap Timer on http://localhost:{port}")
    def telemetry():   # the firmware's 10 Hz rssi_all and 1 Hz status
        n = 0
        while True:
            time.sleep(0.1)
            # Sample RSSI for the assigned slots: a floor with noise and a
            # gate pass every few seconds (for the race cards and calibration).
            t = time.monotonic()
            p = []
            for s, ri in enumerate(state["active"]):
                if ri < 0:
                    continue
                phase = (t + s * 1.7) % 6.0
                v = 120 + int(15 * ((n * 7 + s * 13) % 10) / 10)
                if phase < 0.8:
                    v += int(420 * (1 - abs(phase - 0.4) / 0.4))
                p.append({"pilot": s, "rssi": v, "crossing": v > 350, "signal": True})
            broadcast({"type": "rssi_all", "ts": now_ms(), "p": p}, admins_only=True)
            n += 1
            if n % 5 == 0:
                enforce_viewers()
            if n % 10 == 0:
                r = state["race"]
                act = sum(1 for a in state["active"] if a >= 0)
                broadcast({"type": "c5", "online": True, "scanning": True, "state": "SCAN", "fw": "1.0", "gain": 30,
                           "records": n * 100 * act, "gaps": 0, "bad": 0,
                           "rate": [1000 if a >= 0 else 0 for a in state["active"]], "elrs": {"enabled": state["elrs"]["enabled"], "connected": state["elrs"]["enabled"], "peer": "UART"},
                           "race": {"run": r["running"], "paused": r["paused"], "el": int(race_elapsed() * 1000),
                                    "now": now_ms(), "laps": len(r["laps"]), "epoch": r["epoch"],
                                    "wait": max(0, int((r.get("waitEnd", 0) - time.monotonic()) * 1000)) if r.get("waitEnd") else 0}})
    threading.Thread(target=telemetry, daemon=True).start()
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
