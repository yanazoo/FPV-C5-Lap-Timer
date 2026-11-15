"""Build the web UI image from data/.

The page in data/ loads its nine scripts one after another (handy for
editing and for tools/mock_server.py). On the timer every request costs
time, so the built page carries those scripts inline: one request opens
the whole UI, and a repeat visit is one 304. Everything is gzip-compressed
(ESPAsyncWebServer serves "x.gz" for "x" and answers repeat visits from the
gzip CRC as ETag).

    python3 tools/webui_build.py OUT_DIR [--plain]    # --plain: no gzip
"""
import gzip
import os
import re
import shutil
import sys

BEGIN, END = "<!--APP-JS-->", "<!--/APP-JS-->"


def inline_scripts(html, src):
    """Replace the script loader between the markers with the scripts."""
    a, b = html.index(BEGIN), html.index(END)
    loader = html[a:b]
    names = re.search(r"var files=\[([^\]]*)\]", loader).group(1)
    names = [n.strip().strip("'\"") for n in names.split(",") if n.strip()]
    parts, inlined = [], []
    for n in names:
        rel = os.path.join("js", n + ".js")
        with open(os.path.join(src, rel), encoding="utf-8") as f:
            js = f.read()
        # the HTML parser must not see the end of the script inside it
        js = js.replace("</script", "<\\/script").replace("<!--", "<\\!--")
        parts.append("<script>\n" + js + "\n</script>")
        inlined.append(rel)
    # A script that did not run (a cut-off page): say so, as the loader did.
    parts.append("<script>if(typeof wsConnect!=='function')"
                 "document.getElementById('loadErr').style.display='block';</script>")
    return html[:a] + "\n".join(parts) + html[b + len(END):], inlined


def build(src, dst, gz=True):
    shutil.rmtree(dst, ignore_errors=True)
    with open(os.path.join(src, "index.html"), encoding="utf-8") as f:
        html, inlined = inline_scripts(f.read(), src)
    skip = {os.path.normpath(p) for p in inlined} | {"index.html"}
    out = {"index.html": html.encode("utf-8")}
    for root, _, files in os.walk(src):
        for name in files:
            rel = os.path.normpath(os.path.relpath(os.path.join(root, name), src))
            if rel in skip:
                continue
            with open(os.path.join(root, name), "rb") as f:
                out[rel] = f.read()
    for rel, raw in out.items():
        path = os.path.join(dst, rel + (".gz" if gz else ""))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            # mtime=0: the same input always gives the same bytes (and ETag)
            f.write(gzip.compress(raw, compresslevel=9, mtime=0) if gz else raw)
    return out


if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    files = build(os.path.join(here, "..", "data"), sys.argv[1], "--plain" not in sys.argv)
    for rel, raw in sorted(files.items()):
        print(f"{rel}: {len(raw)} bytes, gzip {len(gzip.compress(raw, 9))}")
