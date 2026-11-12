# PlatformIO pre-script: build the LittleFS image (buildfs / uploadfs) from
# data/ with tools/webui_build.py: the page with its scripts inline (one
# request opens the web UI), everything gzip-compressed. ESPAsyncWebServer's
# serveStatic serves "x.gz" for "x" (Content-Encoding: gzip) and answers
# repeat visits with 304 from the gzip CRC (ETag).
# data/ itself stays plain, for editing and for tools/mock_server.py.
Import("env")  # noqa: F821  (provided by PlatformIO)
import os
import sys

project = env.subst("$PROJECT_DIR")  # noqa: F821
src = os.path.join(project, "data")
dst = os.path.join(env.subst("$BUILD_DIR"), "webui")  # noqa: F821

if os.path.isdir(src):
    sys.path.insert(0, os.path.join(project, "tools"))
    import webui_build
    webui_build.build(src, dst)
    env.Replace(PROJECT_DATA_DIR=dst)  # noqa: F821

# The firmware's version for the page footer and the settings: the release
# tag, plus commits since and the commit (git describe), e.g. v2.0.1-12-gce8ecdf.
# Shows which firmware a timer really runs (and tells two timers apart).
import subprocess
try:
    ver = subprocess.check_output(["git", "describe", "--tags", "--always", "--dirty"], cwd=project,
                                  stderr=subprocess.DEVNULL).decode().strip()
except Exception:
    ver = "dev"
env.Append(CPPDEFINES=[("FW_VERSION", env.StringifyMacro(ver))])  # noqa: F821
