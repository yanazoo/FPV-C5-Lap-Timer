# PlatformIO pre-script: build the LittleFS image (buildfs / uploadfs) from a
# gzip-compressed copy of data/. ESPAsyncWebServer's serveStatic serves
# "x.js.gz" for "x.js" (Content-Encoding: gzip) and answers repeat visits
# with 304 from the gzip CRC (ETag), so the web UI loads about 3x less data.
# data/ itself stays plain, for editing and for tools/mock_server.py.
Import("env")  # noqa: F821  (provided by PlatformIO)
import gzip
import os
import shutil

src = os.path.join(env.subst("$PROJECT_DIR"), "data")  # noqa: F821
dst = os.path.join(env.subst("$BUILD_DIR"), "webui")   # noqa: F821

if os.path.isdir(src):
    shutil.rmtree(dst, ignore_errors=True)
    for root, _, files in os.walk(src):
        for name in files:
            path = os.path.join(root, name)
            rel = os.path.relpath(path, src)
            out = os.path.join(dst, rel + ".gz")
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with open(path, "rb") as f:
                raw = f.read()
            # mtime=0: the same input always gives the same bytes (and ETag)
            with open(out, "wb") as f:
                f.write(gzip.compress(raw, compresslevel=9, mtime=0))
    env.Replace(PROJECT_DATA_DIR=dst)  # noqa: F821
