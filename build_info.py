# PlatformIO pre-script: the git commit this firmware is built from, as
# FW_BUILD (shown in the settings tab with the web UI's own, see
# tools/build_id.py).
Import("env")  # noqa: F821  (provided by PlatformIO)
import os
import sys

project = env.subst("$PROJECT_DIR")  # noqa: F821
sys.path.insert(0, os.path.join(project, "tools"))
from build_id import build_id, release_version  # noqa: E402

bid = build_id(project)
ver = release_version(project)
env.Append(CPPDEFINES=[("FW_BUILD", env.StringifyMacro(bid)),  # noqa: F821
                       ("FW_VERSION", env.StringifyMacro(ver))])  # noqa: F821
print("build_info.py: FW_BUILD = %s, FW_VERSION = %s" % (bid, ver))
