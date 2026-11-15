#!/usr/bin/env bash
# PC tests for the FPV C5 Lap Timer (no hardware needed).
#   bash test/run.sh
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
core="$here/../lib/timer_core"
c5host="$here/../third_party/FPVGateC5MK/host/c5host"
out="${TMPDIR:-/tmp}/fpv_c5_test"
mkdir -p "$out"

echo "== timer_core unit tests"
g++ -std=c++17 -O1 -Wall -Wextra -Werror -I"$core" \
    "$here/test_core.cpp" "$core"/msp_v2.cpp "$core"/md5.cpp "$core"/race_core.cpp "$core"/osd_engine.cpp \
    "$core"/race_files.cpp -o "$out/test_core"
"$out/test_core"

echo "== C5MK link -> gate passes -> race core"
if [ -f "$c5host/c5host.cpp" ]; then
    g++ -std=c++17 -O1 -Wall -Wextra -I"$core" -I"$c5host" \
        "$here/test_c5_pipeline.cpp" "$c5host/c5host.cpp" "$core"/gate_tracker.cpp "$core"/race_core.cpp \
        -o "$out/test_c5_pipeline"
    "$out/test_c5_pipeline"
else
    echo "skipped: run 'git submodule update --init' for third_party/FPVGateC5MK"
fi

echo "== cross-check against vrxc_elrs (Python)"
python3 "$here/crosscheck_vrxc.py" "$out/test_core"

echo "== check_submodules.py pins the same FPVGate C5MK commit as the submodule"
root="$here/.."
if git -C "$root" rev-parse --git-dir >/dev/null 2>&1; then
    want=$(git -C "$root" ls-files -s third_party/FPVGateC5MK | awk '{print $2}')
    got=$(sed -n 's/^C5MK_COMMIT = "\([0-9a-f]*\)"/\1/p' "$here/../check_submodules.py")
    if [ "$want" != "$got" ]; then
        echo "FAIL: submodule is $want but check_submodules.py pins $got"; exit 1
    fi
    echo "ok ($got)"
fi

echo "== web UI build (tools/webui_build.py): the page carries its scripts"
python3 - "$here/.." "$out/webui" <<'PY'
import gzip, os, sys
root, dst = sys.argv[1], sys.argv[2]
sys.path.insert(0, os.path.join(root, "tools"))
import webui_build
webui_build.build(os.path.join(root, "data"), dst)
got = sorted(os.path.relpath(os.path.join(r, f), dst) for r, _, fs in os.walk(dst) for f in fs)
assert got == ["index.html.gz", os.path.join("js", "qrcode.js.gz")], got
html = gzip.open(os.path.join(dst, "index.html.gz")).read().decode()
assert "var files=[" not in html and "APP-JS" not in html, "the loader is still in the page"
for name in sorted(os.listdir(os.path.join(root, "data", "js"))):
    if name == "qrcode.js":
        continue
    js = open(os.path.join(root, "data", "js", name), encoding="utf-8").read()
    assert "</script" not in js.lower(), name + " would end its inline script"
    assert js.strip()[:200] in html, name + " is not in the built page (add it to the loader's list in index.html)"
print("ok (%d bytes gzip)" % os.path.getsize(os.path.join(dst, "index.html.gz")))
PY
