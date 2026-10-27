#!/usr/bin/env bash
# PC tests for the FPV C5 Lap Timer (no hardware needed).
#   bash fpv_c5/test/run.sh
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
core="$here/../lib/timer_core"
c5host="$here/../../third_party/FPVGateC5MK/host/c5host"
out="${TMPDIR:-/tmp}/fpv_c5_test"
mkdir -p "$out"

echo "== timer_core unit tests"
g++ -std=c++17 -O1 -Wall -Wextra -Werror -I"$core" \
    "$here/test_core.cpp" "$core"/msp_v2.cpp "$core"/md5.cpp "$core"/race_core.cpp "$core"/osd_engine.cpp \
    -o "$out/test_core"
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
root="$here/../.."
if git -C "$root" rev-parse --git-dir >/dev/null 2>&1; then
    want=$(git -C "$root" ls-tree HEAD third_party/FPVGateC5MK | awk '{print $3}')
    got=$(sed -n 's/^C5MK_COMMIT = "\([0-9a-f]*\)"/\1/p' "$here/../check_submodules.py")
    if [ "$want" != "$got" ]; then
        echo "FAIL: submodule is $want but check_submodules.py pins $got"; exit 1
    fi
    echo "ok ($got)"
fi
