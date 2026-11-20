"""The build's git commit, for the firmware (build_info.py) and the web UI
(webui_build.py): the settings tab shows both, so a timer still running an
older firmware or web UI is easy to spot."""
import os
import subprocess


def build_id(project):
    """Short commit, "+" if there are uncommitted changes; "?" without git."""
    def git(*args):
        return subprocess.run(["git", "-C", project] + list(args), capture_output=True, text=True,
                              timeout=10).stdout.strip()
    try:
        rev = git("rev-parse", "--short=7", "HEAD")
        if not rev:
            return "?"
        dirty = git("status", "--porcelain", "--untracked-files=no", "--", "src", "lib", "data")
        return rev + ("+" if dirty else "")
    except Exception:
        return "?"


def release_version(project):
    """The release this is: the release branch CI builds (GITHUB_REF_NAME
    v2.2.0), else the nearest release tag (v2.2.0, or v2.2.0-3-gabc1234 past
    it), else "dev"."""
    ref = os.environ.get("GITHUB_REF_NAME", "")
    if os.environ.get("GITHUB_REF_TYPE") == "branch" and ref[:1] == "v" and ref[1:2].isdigit():
        return ref
    try:
        out = subprocess.run(["git", "-C", project, "describe", "--tags", "--match", "v[0-9]*"],
                             capture_output=True, text=True, timeout=10).stdout.strip()
        return out or "dev"
    except Exception:
        return "dev"


if __name__ == "__main__":
    print(build_id(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
