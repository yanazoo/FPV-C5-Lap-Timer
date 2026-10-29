#!/usr/bin/env python3
"""Mock of the S3 firmware's HTTP API, for working on the web UI on a PC.

    python3 tools/mock_server.py [port]      (default 8000)

Serves data/ and answers /api/* from memory. There is no WebSocket:
the page keeps retrying it; push messages from the browser console with
onMsg({...}) (see ws.js for the message types).
"""
import json
import os
import sys
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data")

state = {
    "pilots": [],
    "active": [-1] * 8,
    "settings": {"lapMode": 0, "cooldownMs": 3000, "sdLogMode": 0, "totalLaps": 0, "gain": 30},
    "elrs": {
        "enabled": False, "transport": 0, "host": "auto", "port": 8080, "rdBind": "", "eventName": "",
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
}


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
            return self.reply({"raceRunning": False, "lapCount": 0, "rosterCount": len(state["pilots"])})
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
        if p == "/api/auth/password":
            return self.reply({"password": state["password"]})
        return self.reply({"error": "not found"}, 404)

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
            return self.reply({"ok": True, "id": pid})
        if p == "/api/pilots/delete":
            pid = d["id"]
            del state["pilots"][pid]
            state["active"] = [-1 if a == pid else (a - 1 if a > pid else a) for a in state["active"]]
            return self.reply({"ok": True})
        if p == "/api/active":
            state["active"] = list(d["slots"])[:8]
            return self.reply({"ok": True})
        if p == "/api/calib":
            state["pilots"][d["id"]].update(enter=d["enter"], exit=d["exit"])
            return self.reply({"ok": True})
        if p == "/api/settings":
            state["settings"].update(d)
            return self.reply({"ok": True})
        if p == "/api/net":
            state["net"].update(mode=d.get("mode", 0), apSsid=d.get("apSsid"), apPass=d.get("apPass"),
                                staSsid=d.get("staSsid", ""), fallbackSec=d.get("fallbackSec", 30))
            if d.get("staPass"):
                state["net"]["staPassSet"] = True
            return self.reply({"ok": True})
        if p == "/api/elrs":
            state["elrs"].update(d)
            return self.reply({"ok": True})
        if p == "/api/auth/login":
            ok = d.get("password") == state["password"]
            return self.reply({"ok": ok}, 200 if ok else 401)
        if p == "/api/race/save":
            return self.reply({"ok": True, "saved": False, "reason": "empty"})
        if p == "/api/sd/pilots/restore":
            return self.reply({"ok": True, "pilots": len(state["pilots"])})
        return self.reply({"ok": True})


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    print(f"mock FPV C5 Lap Timer on http://localhost:{port}")
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
