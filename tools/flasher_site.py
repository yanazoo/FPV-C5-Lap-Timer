"""Builds the web flasher site (GitHub Pages) from a release's build outputs:
docs/flasher/index.html, plus per device a folder of .bin files and an
ESP Web Tools manifest whose offsets come from that build's partitions.bin.

    python3 tools/flasher_site.py <site dir> <release tag>

Run from the project root after building everything (publish-release.yaml).
Only the regions below are written, never the NVS: a plain install keeps the
timer's settings and pilots (ESP Web Tools asks whether to erase first).
"""
import glob
import json
import os
import shutil
import struct
import sys

# Second-stage bootloader offset per chip.
BOOTLOADER_AT = {"ESP32": 0x1000, "ESP32-S3": 0x0, "ESP32-C3": 0x0, "ESP32-C5": 0x2000}


def partitions(path):
    """(type, subtype, offset, size, label) of each entry of a partitions.bin."""
    out = []
    data = open(path, "rb").read()
    for i in range(0, len(data), 32):
        e = data[i:i + 32]
        if len(e) < 32 or e[:2] != b"\xaa\x50":
            break
        ptype, sub, off, size = struct.unpack("<BBII", e[2:12])
        out.append((ptype, sub, off, size, e[12:28].split(b"\0")[0].decode()))
    return out


def find(parts, ptype, subtypes):
    for p in parts:
        if p[0] == ptype and p[1] in subtypes:
            return p[2]
    raise SystemExit("partition type %d subtype %s not found" % (ptype, subtypes))


def boot_app0():
    """Arduino's otadata image that selects app0 (the slot firmware.bin goes
    to): after an online update the timer may run from app1."""
    hits = glob.glob(os.path.expanduser("~/.platformio/packages/framework-arduinoespressif32*/tools/partitions/boot_app0.bin"))
    if not hits:
        raise SystemExit("boot_app0.bin not found")
    return hits[0]


def device(site, key, name, chip, build, otadata_file, extra=None):
    """Copies a device's files into site/<key>/ and writes manifest-<key>.json."""
    parts = partitions(os.path.join(build, "partitions.bin"))
    app = find(parts, 0, (0x00, 0x10))           # factory or ota_0
    ota = find(parts, 1, (0x00,))                 # otadata
    files = [("bootloader.bin", os.path.join(build, "bootloader.bin"), BOOTLOADER_AT[chip]),
             ("partitions.bin", os.path.join(build, "partitions.bin"), 0x8000),
             ("otadata.bin", otadata_file, ota),
             ("firmware.bin", os.path.join(build, "firmware.bin"), app)]
    if extra:
        fname, src, ptype, subs = extra
        files.append((fname, src, find(parts, ptype, subs)))
    os.makedirs(os.path.join(site, key), exist_ok=True)
    out_parts = []
    for fname, src, off in files:
        shutil.copy(src, os.path.join(site, key, fname))
        out_parts.append({"path": "%s/%s" % (key, fname), "offset": off})
    return {"key": key, "name": name, "chip": chip, "parts": out_parts}


def main():
    site, tag = sys.argv[1], sys.argv[2]
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.makedirs(site, exist_ok=True)
    shutil.copy(os.path.join(root, "docs", "flasher", "index.html"), os.path.join(site, "index.html"))
    s3 = os.path.join(root, ".pio", "build", "xiao_esp32s3")
    c5 = os.path.join(root, "third_party", "FPVGateC5MK", "multipilot", ".pio", "build", "xiaoc5")
    br = os.path.join(root, "third_party", "elrs-netpack", "elrs-espnow-bridge", ".pio", "build")
    devices = [
        device(site, "timer-s3", "FPV C5 Lap Timer（XIAO ESP32-S3）", "ESP32-S3", s3, boot_app0(),
               ("littlefs.bin", os.path.join(s3, "littlefs.bin"), 1, (0x82,))),
        device(site, "receiver-c5", "FPVGate C5MK 受信機（XIAO ESP32-C5）", "ESP32-C5", c5,
               os.path.join(c5, "ota_data_initial.bin")),
        device(site, "bridge-c3", "ESP-NOW ブリッジ（XIAO ESP32-C3）", "ESP32-C3",
               os.path.join(br, "xiao_esp32c3"), boot_app0()),
        device(site, "bridge-esp32", "ESP-NOW ブリッジ（ESP32 Wrover-E）", "ESP32",
               os.path.join(br, "esp32dev"), boot_app0()),
    ]
    for d in devices:
        manifest = {
            "name": d["name"],
            "version": tag,
            "new_install_prompt_erase": True,   # asks; a plain install keeps the settings
            "new_install_improv_wait_time": 0,
            "builds": [{"chipFamily": d["chip"], "parts": d["parts"]}],
        }
        with open(os.path.join(site, "manifest-%s.json" % d["key"]), "w") as f:
            json.dump(manifest, f, ensure_ascii=False, indent=1)
    with open(os.path.join(site, "version.json"), "w") as f:
        json.dump({"version": tag}, f)
    print("flasher site for %s in %s: %s" % (tag, site, ", ".join(d["key"] for d in devices)))


if __name__ == "__main__":
    main()
