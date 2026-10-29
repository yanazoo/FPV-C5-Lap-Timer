#!/usr/bin/env python3
"""Cross-check the firmware's ELRS OSD bytes against vrxc_elrs itself.

Loads the real plugin source from third_party/vrxc_elrs (msp.py and the
methods of ELRSBackpack that build packets), runs them without RotorHazard,
and compares their bytes with the C++ timer_core output printed by
test_core. Covers bind-phrase UIDs and one full OSD burst
(SET_SEND_UID, every row at full width, display, reset), plus a race of
displays (rows added, changed and removed) under packet loss: with HDZero's
canvas no row that should be up is ever blank.

    python3 crosscheck_vrxc.py <path to test_core binary>
"""
import ast
import os
import subprocess
import sys
import types

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGIN = os.path.join(HERE, "..", "third_party", "vrxc_elrs", "custom_plugins", "vrxc_elrs")


def load_plugin():
    if not os.path.isfile(os.path.join(PLUGIN, "msp.py")):
        print("skipped: run 'git submodule update --init' for third_party/vrxc_elrs")
        sys.exit(0)
    # msp.py only needs gevent's Queue for typing; stub it.
    gevent = types.ModuleType("gevent")
    gevent_queue = types.ModuleType("gevent.queue")
    gevent_queue.Queue = object
    sys.modules.setdefault("gevent", gevent)
    sys.modules.setdefault("gevent.queue", gevent_queue)
    ns = {"__name__": "vrxc_msp"}
    with open(os.path.join(PLUGIN, "msp.py"), encoding="utf-8") as f:
        exec(compile(f.read(), "msp.py", "exec"), ns)

    # Pull the packet-building methods out of ELRSBackpack without importing
    # RotorHazard: compile just those function definitions.
    wanted = {"hash_phrase", "center_osd", "send_msp", "_frame", "_send_osd_packet", "set_send_uid",
              "reset_send_uid", "send_clear_osd", "send_osd_text", "send_clear_osd_row", "send_display_osd",
              "_osd_row_bytes"}
    with open(os.path.join(PLUGIN, "elrs_backpack.py"), encoding="utf-8") as f:
        tree = ast.parse(f.read())
    cls = next(n for n in tree.body if isinstance(n, ast.ClassDef) and n.name == "ELRSBackpack")
    funcs = [n for n in cls.body if isinstance(n, ast.FunctionDef) and n.name in wanted]
    missing = wanted - {f.name for f in funcs}
    if missing:
        raise SystemExit(f"vrxc_elrs changed: missing {sorted(missing)}")
    import hashlib
    mod = ast.Module(body=funcs, type_ignores=[])
    consts = {}
    for n in tree.body:
        if isinstance(n, ast.Assign) and all(isinstance(t, ast.Name) and t.id.startswith("OSD_") for t in n.targets):
            exec(compile(ast.Module(body=[n], type_ignores=[]), "elrs_backpack.py", "exec"), consts)
    fns = {"hashlib": hashlib, "MSPPacket": ns["MSPPacket"], "MSPTypes": ns["MSPTypes"], **consts}
    exec(compile(mod, "elrs_backpack.py", "exec"), fns)

    class Backpack:
        _backpack_connected = True

        def __init__(self):
            self.sent = []
            self._osd_frames = {}
            self._send_uid = None
            self._osd_shown = {}
            self._osd_blank = {}
            self._osd_hard_clear = set()
            self._send_queue = types.SimpleNamespace(put=self.sent.append)

    for f in funcs:
        setattr(Backpack, f.name, fns[f.name])
    return Backpack


def run(binary, *args):
    return subprocess.run([binary, *args], check=True, capture_output=True, text=True).stdout.strip()


def lossy_race(Backpack):
    """Displays over a race with 25 % of OSD packets lost (HDZero canvas)."""
    import random
    bp = Backpack()
    uid = bp.hash_phrase("pilot-a")
    rng = random.Random(1)
    canvas = [[" "] * 50 for _ in range(18)]
    screen = [row[:] for row in canvas]

    def flush():
        bp.sent.clear()
        bp.send_display_osd()
        for pkt in bp.sent:
            data = bytes(pkt.get_packet())[8:-1]   # $X< flag func(2) size(2) ... crc
            if rng.random() < 0.25:
                continue
            if data[0] == 0x02:
                for r in canvas:
                    r[:] = [" "] * 50
            elif data[0] == 0x03:
                for i, b in enumerate(data[4:]):
                    canvas[data[1]][data[2] + i] = chr(b)
            elif data[0] == 0x04:
                screen[:] = [r[:] for r in canvas]

    bp.set_send_uid(uid)
    bp.send_clear_osd(hard=True)
    bp.send_osd_text(5, 19, "W ARM NOW X")
    flush()
    failures = 0
    best_seen = False
    for sec in range(1, 40):
        if sec == 2:
            bp.send_clear_osd_row(5)
        bp.send_osd_text(2, 23, f"00:{sec:02d}")
        if sec == 10:
            bp.send_osd_text(8, 22, "0:12.3")
            bp.send_osd_text(12, 0, "BEST: 0:12.3")
        if sec == 15:
            bp.send_clear_osd_row(8)
        flush()
        text = ["".join(r) for r in screen]
        # Once a row has been drawn it never blanks while it should be up.
        best_seen = best_seen or "BEST: 0:12.3" in text[12]
        if best_seen and "BEST: 0:12.3" not in text[12]:
            print(f"FAIL lossy race: best lap row blank at {sec} s")
            failures += 1
        if sec >= 20 and "0:12.3" in text[8]:
            print(f"FAIL lossy race: lap row still up at {sec} s")
            failures += 1
        if sec >= 6 and "ARM" in text[5]:
            print(f"FAIL lossy race: ARM NOW still up at {sec} s")
            failures += 1
    return failures


def main():
    binary = sys.argv[1]
    Backpack = load_plugin()
    failures = 0

    phrases = ["test", "pilot-a", "yanazoo FPV", "長い日本語のバインドフレーズ", "x" * 60]
    for p in phrases:
        want = bytes(Backpack().hash_phrase(p)).hex()
        got = run(binary, "uid", p)
        if want != got:
            print(f"FAIL uid {p!r}: vrxc {want} firmware {got}")
            failures += 1

    cells = [(1, 17, "POSN: 1 | LAP: 3"), (2, 23, "00:42"), (8, 22, "0:11.3"), (12, 0, "BEST: 0:10.9"),
             (5, 19, "w FINISHED! x".upper())]
    bp = Backpack()
    uid = bp.hash_phrase("pilot-a")
    bp.set_send_uid(uid)
    for row, col, text in cells:
        bp.send_osd_text(row, col, text)
    bp.send_display_osd()
    bp.reset_send_uid()
    want = b"".join(bytes(p.get_packet()) for p in bp.sent).hex()
    args = ["frame", "pilot-a"]
    for row, col, text in cells:
        args += [str(row), str(col), text]
    got = run(binary, *args)
    if want != got:
        print(f"FAIL OSD burst\n  vrxc     {want}\n  firmware {got}")
        failures += 1

    failures += lossy_race(Backpack)

    for n in (0, 5, 11, 30, 49, 50, 60):
        if Backpack().center_osd(n) != int(run(binary, "center", str(n))):
            print(f"FAIL center_osd({n})")
            failures += 1

    print(f"{failures} FAILED" if failures else "vrxc_elrs cross-check passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
