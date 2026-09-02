# Agent guide — Katexdown

Katexdown is a Katdown fork: a Markdown preview plugin for Kate (KF6/Qt6,
QWebEngineView-based). Quick orientation:

- `src/` — plugin (preview widget, plugin view, settings, config page).
- `data/` — the rendered page: `preview.html`, `css/`, `js/preview.js` (the
  page-side logic, incl. all image memory handling). These ship inside the
  plugin binary via `data/resources.qrc` — changing them requires rebuilding
  test binaries to take effect.
- `tests/` — QTest binaries driving the real widget headless.
- `runtime/` — optional downloaded assets (KaTeX etc.), see `tools/`.

## Standing rule: design docs (do not skip)

**When you finish a major feature, a design change, or a bug fix that encodes
an invariant, create or update a design doc in `design/` — one focused doc per
major feature / design / pitfall — in the same change as the code.**

- New doc, or update of the existing one, is part of the work; a change that
  alters an invariant without updating its doc is incomplete.
- Read `design/README.md` for the format and the current index before writing.
- If your change touches anything covered by `design/lazyrender.md` (page
  lifecycle/freeze/discard, engine gating, image modes and parking) — check
  the doc's Invariants and Pitfalls sections against your change first, and
  keep them truthful.

## Testing

Headless (matches CI / the CTest environment):

```sh
cd build && cmake --build . --target renderfeaturestest previewlifecycletest followmodetest
QT_QPA_PLATFORM=offscreen QTWEBENGINE_CHROMIUM_FLAGS="--disable-gpu --no-sandbox" \
XDG_CONFIG_HOME=/tmp/kdxcfg XDG_CACHE_HOME=/tmp/kdxcache XDG_DATA_HOME=/tmp/kdxdata \
KATEXDOWN_DATA_DIR=$PWD/../runtime ./bin/renderfeaturestest        # or a single test name
```

After editing `data/` assets, rebuild the test targets (resources are compiled
into each binary). Run all three suites before finishing.
