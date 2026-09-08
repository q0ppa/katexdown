# Export & print — interactive chrome never reaches paper

## Goal / problem

Exporting the preview saves a standalone `.html` file that carries the
floating section-outline control (jump button + panel) as part of the
document body. When that file is printed from a browser, browsers repeat
`position: fixed` elements on every printed page — so the jump button UI
ended up stamped over the document content on each sheet.

The requirement: printing the exported file must yield clean document
content, with no jump-button UI. Whether the exported file itself keeps the
on-screen UI is a free choice; we keep it (it is a deliberate feature of the
standalone file — see the re-wire comment in `preview.js`).

## Design (the mechanism, and why this shape)

Print rules live in `data/css/base.css` inside an `@media print` block:

- `#kdx-outline-btn` and `#kdx-outline-panel` get `display: none !important`
  (the `!important` is required: `preview.js` may have set an inline
  `style.display` on the button, and inline styles would otherwise beat a
  plain author rule).
- `.markdown-body` bottom padding is reduced from the screen value (96px
  desktop / 64px mobile) to the regular 32px gutter. That padding exists
  only to keep content clear of the floating button on screen; in print it
  would leave a tall empty band after the last line.

The exported file embeds `base.css` inside its `<style id="base">`
(`__serializedHtml()` serializes the live DOM, head included), so the rule
travels into every export produced after this change with no further wiring.
The live preview page is governed by the same rule for free, which keeps
screen and paper consistent if a print path is ever added to the widget.

Why not strip the UI out of the exported DOM instead (`__serializedHtml`
deleting the button/panel)? `preview.js` is embedded in the export and
rebuilds the outline on `DOMContentLoaded`, so the UI would reappear in the
file anyway unless export detection logic were added to the page script —
a bigger blast radius (page boot logic, `lazyrender.md` invariants) for a
print-only problem. A stylesheet rule is the smallest correct fix and also
covers any other future print source.

## Invariants

- Printed output of an exported `.html` (or the live page) contains no
  `#kdx-outline-btn` / `#kdx-outline-panel` — i.e. no floating interactive
  chrome on paper.
- Any new interactive chrome added to the preview must be screen-only too:
  it either gets hidden in the same `@media print` block, or never becomes
  part of printed output by construction.
- The on-screen outline control (live page and exported file) is unchanged.
- The print block must not force a light palette: a doc exported from a dark
  editor theme prints dark (browser "background graphics" on) just as it
  looked on screen; remapping only some vars while `hljs` token colors stay
  light would make code unreadable on white paper with no way to fix it.

## Pitfalls

- **Fixed elements print on every page.** Any `position: fixed` UI in the
  preview is a print hazard by default; hiding it in a print media query is
  the fix, not removing the element (screen behavior must stay).
- **Inline styles beat stylesheet rules.** `ensureOutlineUi()` sets
  `btn.style.display` directly, so a plain `display: none` in `@media print`
  would silently lose to the inline style on some states; `!important` is
  load-bearing.
- **Reserved padding is screen-only.** The 96px/64px bottom padding on
  `.markdown-body` is clearance for the floating button; copying it into
  print output leaves a blank band on the final page.

## Test seams

No automated test covers printing (headless QWebEngine has no paper
layout); `renderfeaturestest.cpp` exercises the on-screen outline
(`outlineListsConfiguredHeadings`, "outline off" case) via
`getComputedStyle(...).display` — screen media, unaffected by the print
rule. The print behavior is verified by opening an exported `.html` in a
browser and printing.

## Where it lives

- `data/css/base.css` — `@media print` block at the end of the file.
- `data/js/preview.js` — the outline control (`ensureOutlineUi`,
  `rebuildOutline`) that the print rule hides.
- `src/previewwidget.cpp` — `__serializedHtml()` export path that embeds
  `base.css` into the standalone file.
