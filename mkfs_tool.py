# PlatformIO post-script: run mklittlefs by its full path for buildfs / uploadfs.
#
# pioarduino keeps mklittlefs 3.x in packages/tool-mklittlefs and reinstalls
# it when another project left a different version there. On that run the
# package holds only its download description (package.json, tools.json): the
# binary is fetched by the platform's install_tool() on the *next* run, so
# buildfs failed with "'mklittlefs' is not recognized" and worked the second
# time. Here the install is finished at once (install_tool, as the platform
# itself does), and MKFSTOOL points at the binary by its full path.
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
    def find():
        for d in dirs:
            if not os.path.isdir(d):
                continue
            for root, _, files in os.walk(d):   # the binary may sit in a subfolder
                if exe in files:
                    return os.path.join(root, exe)
        return None

    found = find()
    if not found:
        # Only the download description so far: let the platform fetch the
        # binary now (the step it would otherwise leave for the next run).
        try:
            print("mkfs_tool.py: installing mklittlefs now ...")
            env.PioPlatform().install_tool("tool-mklittlefs")  # noqa: F821
        except Exception as e:
            print("mkfs_tool.py: install_tool failed: %s" % e)
        found = find()
    if found:
        env.Replace(MKFSTOOL=found)  # noqa: F821
        print("mkfs_tool.py: mklittlefs = %s" % found)
    else:
        print("mkfs_tool.py: %s not found in %s (using PATH)" % (exe, ", ".join(dirs)))
