# PlatformIO pre-script: the build needs the FPVGate C5MK submodule (c5host).
# A plain `git clone` or a ZIP download leaves third_party/ empty, and the
# build then fails on a confusing "c5host.h: No such file or directory".
# Fetch the submodules when git can, otherwise stop with what to do.
import os
import subprocess

Import("env")  # noqa: F821  (provided by PlatformIO)

project = env.subst("$PROJECT_DIR")  # noqa: F821
root = os.path.normpath(os.path.join(project, ".."))
header = os.path.join(root, "third_party", "FPVGateC5MK", "host", "c5host", "c5host.h")

if not os.path.isfile(header):
    print("FPVGate C5MK submodule missing: running git submodule update --init --recursive")
    try:
        subprocess.check_call(["git", "submodule", "update", "--init", "--recursive"], cwd=root)
    except (OSError, subprocess.CalledProcessError) as e:
        print("  failed: %s" % e)
    if not os.path.isfile(header):
        raise SystemExit(
            "\n*** third_party/FPVGateC5MK is empty. In the repository root run:\n"
            "***     git submodule update --init --recursive\n"
            "*** (a ZIP download has no submodules: clone with --recurse-submodules)\n"
        )
