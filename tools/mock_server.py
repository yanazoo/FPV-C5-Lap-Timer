#!/usr/bin/env python3
"""Mock of the S3 firmware's HTTP API, for working on the web UI on a PC.

    python3 tools/mock_server.py [port]      (default 8000)

Serves data/ and answers /api/* from memory, with a WebSocket at /ws that
broadcasts what the firmware does (init, race_*, lap, settings, calib,
active_update, config_changed), so several browsers can be checked
against each other. GET /mock/lap?slot=N records a pass for slot N.
"""
import base64
import hashlib
import json
import os
import sys
import threading
import time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data")

state = {
    "pilots": [],
    "active": [-1] * 8,
    "settings": {"lapMode": 0, "cooldownMs": 3000, "sdLogMode": 0, "totalLaps": 0, "gain": 30,
                 "rankMode": 0, "timeLimitSec": 120, "countLastLap": True},
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
            "status": {"ap": True, "connected": False, "fallback": False, "ip": "20.0.0.1", "rssi": 0}},
    "password": "admin",
    "posts": [],   # every POST, for tests
    "race": {"running": False, "paused": False, "t0": 0.0, "elapsed": 0.0, "laps": []},
}

ws_clients = []
ws_lock = threading.Lock()


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


def broadcast(obj):
    with ws_lock:
        for w in list(ws_clients):
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
                raceElapsedMs=int(race_elapsed() * 1000), sdPresent=True, pilots=pilots)


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
        if p == "/api/status":
            return self.reply({"raceRunning": state["race"]["running"], "lapCount": len(state["race"]["laps"]),
                               "rosterCount": len(state["pilots"])})
        if p == "/api/sd/status":
            return self.reply({"present": True})
        if p == "/api/sd/files":
            return self.reply([{"name": "race_001.csv", "size": 2048}, {"name": "pilots.csv", "size": 300}])
        if p == "/api/sd/file":
            path = parse_qs(u.query).get("path", [""])[0]
            body = f"﻿Slot,Name,FreqMHz,Lap,LapTime_ms,PeakRSSI,Timestamp_ms\n# {path}\n".encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/csv; charset=utf-8")
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
        try:
            while self.rfile.read(1):   # the page never sends; wait for it to go
                pass
        except OSError:
            pass
        with ws_lock:
            if self.wfile in ws_clients:
                ws_clients.remove(self.wfile)
        self.close_connection = True

    def do_POST(self):
        p = urlparse(self.path).path
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b""
        d = json.loads(raw) if raw else {}
        state["posts"].append([p, d])
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
        if p == "/api/race/stage":
            broadcast({"type": "race_stage"})
            return self.reply({"ok": True})
        if p == "/api/race/start":
            r.update(running=True, paused=False, t0=time.monotonic(), elapsed=0.0, laps=[])
            broadcast({"type": "race_start", "ts": now_ms()})
            return self.reply({"ok": True})
        if p == "/api/race/stop":
            if r["running"]:
                r.update(running=False, paused=True, elapsed=race_elapsed())
            broadcast({"type": "race_stop"})
            return self.reply({"ok": True})
        if p == "/api/race/resume":
            r.update(running=True, paused=False, t0=time.monotonic())
            broadcast({"type": "race_resume"})
            return self.reply({"ok": True})
        if p == "/api/net":
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
            ok = d.get("password") == state["password"]
            return self.reply({"ok": ok}, 200 if ok else 401)
        if p == "/api/race/save":
            had = bool(r["laps"])
            r.update(running=False, paused=False, elapsed=0.0, laps=[])
            broadcast({"type": "race_clear"})
            return self.reply({"ok": True, "saved": False, "reason": "nosd" if had else "empty"})
        if p == "/api/sd/pilots/restore":
            return self.reply({"ok": True, "pilots": len(state["pilots"])})
        return self.reply({"ok": True})


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    print(f"mock FPV C5 Lap Timer on http://localhost:{port}")
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
