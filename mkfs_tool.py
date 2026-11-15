# PlatformIO post-script: run mklittlefs by its full path for buildfs / uploadfs.
#
# pioarduino keeps mklittlefs 3.x in packages/tool-mklittlefs and reinstalls
# it when another project left a different version there. On that run the
# freshly installed tool is not on the build's PATH yet, and buildfs failed
# with "'mklittlefs' is not recognized" (it worked the next time). Pointing
# MKFSTOOL at the file itself works on the first run too.
Import("env")  # noqa: F821  (provided by PlatformIO)
import os
import sys

exe = "mklittlefs.exe" if sys.platform.startswith("win") else "mklittlefs"
dirs = []
try:
    d = env.PioPlatform().get_package_dir("tool-mklittlefs")  # noqa: F821
    if d:
        dirs.append(d)
except Exception:
    pass
dirs.append(os.path.join(env.subst("$PROJECT_PACKAGES_DIR"), "tool-mklittlefs"))  # noqa: F821
for d in dirs:
    path = os.path.join(d, exe)
    if os.path.isfile(path):
        env.Replace(MKFSTOOL=path)  # noqa: F821
        break
