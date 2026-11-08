#!/usr/bin/env bash
# Prepare pioarduino's Python environment (~/.platformio/penv) from PyPI.
#
# On the first build, the pioarduino platform (55.03.x) creates this
# environment itself and installs platformio-core from a GitHub source zip
# (pioarduino/platformio-core/archive/...). When that download fails, every
# ESP32-S3 / C5 build stops with "Failed to install Python dependencies".
# The platform accepts an environment that already has platformio 6.1.18
# (the version of that zip) and the packages below, so it is made here from
# PyPI and the zip is not needed. Run it before "pio run".
#
# The list mirrors python_deps in the platform's builder/penv_setup.py.
set -euo pipefail

penv="$HOME/.platformio/penv"
want="6.1.18"

if [ -x "$penv/bin/python" ] &&
   "$penv/bin/python" -c "import importlib.metadata as m, sys; sys.exit(m.version('platformio') != '$want')" 2>/dev/null; then
    echo "penv ready (platformio $want)"
    exit 0
fi

python3 -m venv --clear "$penv"
"$penv/bin/python" -m pip install -q --upgrade pip
"$penv/bin/python" -m pip install -q \
    "uv>=0.1.0" \
    "platformio==$want" \
    "pyyaml>=6.0.2" \
    "rich-click>=1.8.6" \
    "zopfli>=0.2.2" \
    "intelhex>=2.3.0" \
    "rich>=14.0.0" \
    "urllib3<2" \
    "cryptography>=45.0.3" \
    "certifi>=2025.8.3" \
    "ecdsa>=0.19.1" \
    "bitstring>=4.3.1" \
    "reedsolo>=1.5.3,<1.8" \
    "esp-idf-size>=2.0.0" \
    "esp-coredump>=1.14.0"
echo "penv created (platformio $want)"
