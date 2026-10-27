# PlatformIO pre-script: the build needs the FPVGate C5MK submodule (c5host).
# A plain `git clone` or a ZIP download leaves third_party/ empty, and the
# build then fails on a confusing "c5host.h: No such file or directory".
#   * git checkout: run `git submodule update --init --recursive`
#   * ZIP download (no .git): clone FPVGateC5MK at the pinned commit
# Otherwise stop with what to do.
import os
import subprocess

Import("env")  # noqa: F821  (provided by PlatformIO)

# Keep in step with the submodule pointer (`git ls-tree HEAD third_party/FPVGateC5MK`);
# test/run.sh checks they match.
C5MK_URL = "https://github.com/yanazoo/FPVGateC5MK.git"
C5MK_COMMIT = "e62a63595994e583e6b12c7703fa1d71b858fb4e"

project = env.subst("$PROJECT_DIR")  # noqa: F821
root = os.path.normpath(project)
c5mk = os.path.join(root, "third_party", "FPVGateC5MK")
header = os.path.join(c5mk, "host", "c5host", "c5host.h")


def run(cmd, cwd):
    try:
        subprocess.check_call(cmd, cwd=cwd)
        return True
    except (OSError, subprocess.CalledProcessError) as e:
        print("  failed: %s" % e)
        return False


def in_git_checkout():
    try:
        out = subprocess.check_output(["git", "rev-parse", "--show-toplevel"], cwd=root,
                                      stderr=subprocess.DEVNULL)
        return os.path.normcase(os.path.normpath(out.decode().strip())) == os.path.normcase(root)
    except (OSError, subprocess.CalledProcessError):
        return False


if not os.path.isfile(header):
    if in_git_checkout():
        print("FPVGate C5MK submodule missing: running git submodule update --init --recursive")
        run(["git", "submodule", "update", "--init", "--recursive"], root)
    else:
        print("Not a git checkout (ZIP download?): fetching FPVGate C5MK %s" % C5MK_COMMIT[:7])
        if os.path.isdir(c5mk) and not os.listdir(c5mk):
            os.rmdir(c5mk)
        if not os.path.isdir(c5mk):
            os.makedirs(os.path.dirname(c5mk), exist_ok=True)
            if run(["git", "clone", "-q", C5MK_URL, c5mk], root):
                run(["git", "-c", "advice.detachedHead=false", "checkout", "-q", C5MK_COMMIT], c5mk)
    if not os.path.isfile(header):
        raise SystemExit(
            "\n*** third_party/FPVGateC5MK is empty. In the repository root run:\n"
            "***     git submodule update --init --recursive\n"
            "*** or clone the repository with: git clone --recurse-submodules ...\n"
        )
