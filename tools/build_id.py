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


if __name__ == "__main__":
    print(build_id(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
