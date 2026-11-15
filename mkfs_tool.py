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

if any(t in ("buildfs", "uploadfs", "uploadfsota") for t in COMMAND_LINE_TARGETS):  # noqa: F821
    exe = "mklittlefs.exe" if sys.platform.startswith("win") else "mklittlefs"
    dirs = []
    try:
        d = env.PioPlatform().get_package_dir("tool-mklittlefs")  # noqa: F821
        if d:
            dirs.append(d)
    except Exception:
        pass
    for base in (env.subst("$PROJECT_PACKAGES_DIR"),  # noqa: F821
                 os.path.join(os.path.expanduser("~"), ".platformio", "packages")):
        dirs.append(os.path.join(base, "tool-mklittlefs"))
    found = None
    for d in dirs:
        if not os.path.isdir(d):
            continue
        for root, _, files in os.walk(d):   # the binary may sit in a subfolder
            if exe in files:
                found = os.path.join(root, exe)
                break
        if found:
            break
    if found:
        env.Replace(MKFSTOOL=found)  # noqa: F821
        print("mkfs_tool.py: mklittlefs = %s" % found)
    else:
        print("mkfs_tool.py: %s not found in %s (using PATH)" % (exe, ", ".join(dirs)))
