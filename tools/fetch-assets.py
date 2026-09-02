#!/usr/bin/env python3
"""
fetch-assets.py — one-shot downloader for Katexdown's optional math assets.

Run it once, done — Katexdown then finds the files on its own. Re-run any time
to update. Nothing here touches the plugin build or the system, and the plugin
never downloads anything itself (the preview stays fully offline).

Layout written into the data directory:

    katex.min.js                 KaTeX renderer          (MIT)
    markdown-it-texmath.min.js   markdown-it math plugin (MIT)
    katex-standalone.min.css     katex.min.css with every font inlined
                                 as base64 data: URIs, plus a small
                                 theme-aware color/overflow tweak

Where it writes (same rules the plugin uses, no env vars needed):

    python3 tools/fetch-assets.py            # the platform default:
                                             #   Linux   ~/.config/katexdown
                                             #   Windows %APPDATA%\\katexdown
                                             #   macOS   ~/Library/Application Support/katexdown
    python3 tools/fetch-assets.py SOME_DIR   # explicit directory
    KATEXDOWN_DATA_DIR=... python3 tools/fetch-assets.py   # dev override

The active data folder is also shown in Kate: Configure Kate -> Katexdown.
"""

import base64
import os
import re
import sys
import urllib.request

KATEX_VERSION = "0.16.22"
TEXMATH_VERSION = "1.0.0"

BASE = f"https://cdn.jsdelivr.net/npm/katex@{KATEX_VERSION}/dist"
TEXMATH = f"https://cdn.jsdelivr.net/npm/markdown-it-texmath@{TEXMATH_VERSION}"

FILES = {
    "katex.min.js": f"{BASE}/katex.min.js",
    "katex.min.css": f"{BASE}/katex.min.css",
    "texmath.min.js": f"{TEXMATH}/texmath.min.js",
}

# Extra rules appended to the standalone css so math follows the active
# color scheme and long display equations scroll instead of overflowing.
CSS_TAIL = """
/* katexdown: follow the active theme and keep wide equations usable */
.katex { color: var(--fgColor-default, currentColor); }
.katex-display { overflow-x: auto; overflow-y: hidden; padding: 0.15em 0; }
"""


def default_data_dir():
    """Mirror src/katexdownpaths.h so a no-argument run lands where the plugin looks."""
    env = os.environ.get("KATEXDOWN_DATA_DIR")
    if env:
        return env
    if sys.platform == "darwin":
        return os.path.join(os.path.expanduser("~"), "Library", "Application Support", "katexdown")
    if os.name == "nt":
        return os.path.join(os.environ.get("APPDATA", os.path.expanduser("~")), "katexdown")
    return os.path.join(os.path.expanduser("~"), ".config", "katexdown")


def fetch(url):
    print(f"  fetch {url}")
    with urllib.request.urlopen(url, timeout=60) as r:
        return r.read()


def fetch_text(url):
    return fetch(url).decode("utf-8")


def write_atomic(path, data):
    """Write via a temp file + rename so a failed run never half-updates the cache."""
    tmp = path + ".tmp"
    mode = "wb" if isinstance(data, bytes) else "w"
    with open(tmp, mode) as f:
        f.write(data)
    os.replace(tmp, path)


def inline_fonts(css):
    """Replace url(fonts/KaTeX_*.woff2/woff) references with inlined base64 woff2."""

    def data_uri(font_name):
        raw = fetch(f"{BASE}/fonts/{font_name}")
        return "data:font/woff2;base64," + base64.b64encode(raw).decode("ascii")

    def one(m):
        try:
            return f"url({data_uri(m.group(1))}) format('woff2')"
        except Exception as e:
            print(f"  warning: could not inline font {m.group(1)}: {e}", file=sys.stderr)
            return m.group(0)

    # katex.min.css lists each face as: url(fonts/X.woff2) format("woff2"),
    # url(fonts/X.woff) format("woff"), url(fonts/X.ttf) format("truetype").
    # Collapse such groups into a single base64 data URI; woff/ttf are dropped,
    # Chromium only needs woff2.
    css = re.sub(
        "url\\(fonts/(KaTeX_[^)]+\\.woff2)\\)\\s*format\\([\"']woff2[\"']\\),?\\s*"
        "(?:url\\(fonts/[^)]*\\.woff\\)\\s*format\\([\"']woff[\"']\\),?\\s*)?"
        "(?:url\\(fonts/[^)]*\\.ttf\\)\\s*format\\([\"']truetype[\"']\\))?",
        one,
        css,
    )
    leftovers = re.findall(r"url\(fonts/[^)]*\)", css)
    if leftovers:
        print(f"  warning: {len(leftovers)} font url(s) not inlined: {leftovers[:3]}", file=sys.stderr)
    return css


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else default_data_dir()
    os.makedirs(out_dir, exist_ok=True)
    print(f"katexdown math assets -> {out_dir}")
    print(f"  KaTeX {KATEX_VERSION}, markdown-it-texmath {TEXMATH_VERSION}")

    blob = {}
    for name, url in FILES.items():
        blob[name] = fetch(url)

    print("  inlining fonts (base64)…")
    css = blob["katex.min.css"].decode("utf-8")
    standalone = inline_fonts(css) + CSS_TAIL

    write_atomic(os.path.join(out_dir, "katex.min.js"), blob["katex.min.js"])
    write_atomic(os.path.join(out_dir, "texmath.min.js"), blob["texmath.min.js"])
    write_atomic(os.path.join(out_dir, "katex-standalone.min.css"), standalone)
    write_atomic(
        os.path.join(out_dir, "ASSETS.txt"),
        "Katexdown math assets\n"
        f"KaTeX: {KATEX_VERSION}\n"
        f"markdown-it-texmath: {TEXMATH_VERSION}\n"
        "Source: https://cdn.jsdelivr.net\n"
        "Re-run tools/fetch-assets.py to update.\n",
    )
    print("  done. Remove this directory and re-run to reset.")


if __name__ == "__main__":
    main()
