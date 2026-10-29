#!/usr/bin/env python3
"""Cross-check the firmware's ELRS OSD bytes against vrxc_elrs itself.

Loads the real plugin source from third_party/vrxc_elrs (msp.py and the
methods of ELRSBackpack that build packets), runs them without RotorHazard,
and compares their bytes with the C++ timer_core output printed by
test_core. Covers bind-phrase UIDs and one full OSD burst built from the
plugin's own packet builders (SET_SEND_UID, rows, display, reset). Unlike
the plugin, the firmware writes every row at its full width and doesn't
start the burst with "clear" (see osd_engine.h), so the rows are composed
here the same way.

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
              "reset_send_uid", "send_clear_osd", "send_osd_text", "send_clear_osd_row", "send_display_osd"}
    with open(os.path.join(PLUGIN, "elrs_backpack.py"), encoding="utf-8") as f:
        tree = ast.parse(f.read())
    cls = next(n for n in tree.body if isinstance(n, ast.ClassDef) and n.name == "ELRSBackpack")
    funcs = [n for n in cls.body if isinstance(n, ast.FunctionDef) and n.name in wanted]
    missing = wanted - {f.name for f in funcs}
    if missing:
        raise SystemExit(f"vrxc_elrs changed: missing {sorted(missing)}")
    import hashlib
    mod = ast.Module(body=funcs, type_ignores=[])
    fns = {"hashlib": hashlib, "MSPPacket": ns["MSPPacket"], "MSPTypes": ns["MSPTypes"]}
    exec(compile(mod, "elrs_backpack.py", "exec"), fns)

    class Backpack:
        _backpack_connected = True

        def __init__(self):
            self.sent = []
            self._osd_frames = {}
            self._send_uid = None
            self._send_queue = types.SimpleNamespace(put=self.sent.append)

    for f in funcs:
        setattr(Backpack, f.name, fns[f.name])
    return Backpack


def run(binary, *args):
    return subprocess.run([binary, *args], check=True, capture_output=True, text=True).stdout.strip()


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
    lines = {}
    for row, col, text in cells:
        line = lines.setdefault(row, [" "] * 50)
        for i, ch in enumerate(text[: 50 - col]):
            line[col + i] = ch
    for row in sorted(lines):
        bp._send_osd_packet(bytearray((0x03, row, 0, 0)) + "".join(lines[row]).encode("ascii"))
    bp._send_osd_packet(bytearray((0x04,)))
    bp.reset_send_uid()
    want = b"".join(bytes(p.get_packet()) for p in bp.sent).hex()
    args = ["frame", "pilot-a"]
    for row, col, text in cells:
        args += [str(row), str(col), text]
    got = run(binary, *args)
    if want != got:
        print(f"FAIL OSD burst\n  vrxc     {want}\n  firmware {got}")
        failures += 1

    for n in (0, 5, 11, 30, 49, 50, 60):
        if Backpack().center_osd(n) != int(run(binary, "center", str(n))):
            print(f"FAIL center_osd({n})")
            failures += 1

    print(f"{failures} FAILED" if failures else "vrxc_elrs cross-check passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
