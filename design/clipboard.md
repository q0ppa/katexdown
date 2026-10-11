# Clipboard — copying from the preview (code blocks and selections)

## Goal / problem

The preview must be copyable like a browsed page. Two paths exist:

- **Code blocks**: a one-click **Copy** button (setting
  `Settings::codeCopyButton`, on by default) that copies the block's contents;
  because the button is a DOM node it also travels into an exported `.html`.
- **Selections**: select any rendered content and press <kbd>Ctrl</kbd>+<kbd>C</kbd>
  (or <kbd>Ctrl</kbd>+<kbd>Insert</kbd>) to copy it as structured plain text —
  a heading gives its words, a list keeps its markers, a table stays a table.

Neither may rely on the renderer's own clipboard: in an embedded
`QWebEngineView` that path needs document focus / clipboard permission and
silently does nothing in some sessions (the renderer writes, no error, no
system clipboard), so the page falls back to it only in an exported `.html`.
Both paths therefore write through the host. The selection path has a second,
independent obstacle: Qt resolves window-context shortcuts before the focus
widget sees a key, so Kate's own Copy would take Ctrl+C first (see the
shortcut-map design note and pitfall below).

## Design (the mechanism, and why this shape)

### One transport: the host bridge

`PreviewWidget` registers a `ClipboardBridge` (a one-slot `QObject`) on a
`QWebChannel` set on the page, exposed to the page as `kdxClipboard`. The slot
is `QGuiApplication::clipboard()->setText()` in the plugin process, which has
no focus or permission requirement. `qwebchannel.js` is inlined into every
page (`buildHtml` fills the `/*__QWEBCHANNEL_JS__*/` slot from Qt's
`:/qtwebchannel/qwebchannel.js` resource); an exported standalone `.html`
carries the same script but no `qt.webChannelTransport`, so it falls back to
the browser APIs.

The page has one writer, `kdxWriteClipboard(text, done)` in `preview.js`:
host bridge → `navigator.clipboard.writeText` → hidden-textarea
`execCommand("copy")`. The code button calls it and returns the `done(ok)`
result as its "Copied"/"Copy failed" feedback. In the live preview the bridge
is always the path taken.

### Code blocks: a DOM button inside its `<pre>`

`decorateCodeBlocks()` appends `<button class="kdx-copy-btn">` to every
`<pre>` under `#content` after each render; `base.css` positions it in the
block's corner and hides it in `@media print` (see `print.md`). It is a real
DOM node, so `__serializedHtml()` exports it exactly when the setting is on —
and never at page init, because an exported file made with the setting off
must not have buttons re-added on load. The click reads the `<code>` child's
`textContent`, never `pre.textContent` (which would include the button label).
The click listener is delegated at document level so the serialized buttons
work again in an export.

### Selections: the host interrupts Ctrl+C, the page shapes the text

`PreviewWidget::forwardKeyEvent()` runs on the web view's focus proxy and
intercepts <kbd>Ctrl</kbd>+<kbd>C</kbd> / <kbd>Ctrl</kbd>+<kbd>Insert</kbd>
when the page is loaded: it calls `copySelection()`, which asks the page for
`window.__kdxSelectionText()` and puts the (non-empty) result on Qt's
clipboard. <kbd>Ctrl</kbd>+<kbd>A</kbd> deliberately stays with the web view —
it selects the document natively, and the host then reads that selection.

The event filter must also **claim these keys from Qt's shortcut map**: for a
real key event, Qt sends `ShortcutOverride` to the focus widget first, and only
the keys the filter accepts survive to become a `KeyPress`. The web view's own
widget accepts none of them, so without this Kate's window-context actions
(Copy, Select All, cursor movement) consume the key before the preview sees it
— `Ctrl+C` then copies the *editor's* selection (wrong paragraph, wrong
offsets, or nothing when the editor has none), and the arrow keys move the
editor cursor instead of scrolling the preview. `previewHandlesKey()` is the
single set used both to claim the override and to decide `forwardKeyEvent`'s
behavior; everything outside it still reaches Kate's shortcut map normally.

The page does the shaping because a selection is DOM, not layout, and the
native `getSelection().toString()` would drag the preview's own chrome (the
code **Copy** button, the outline control) into the clipboard. The walker in
`preview.js` (`__kdxSelectionText`, `selWalk`) emits:

- inline text collapsed to one logical line (HTML source newlines/spaces);
  whitespace-only text nodes between block siblings are dropped, so blocks do
  not start with a stray space
- block boundaries (paragraphs, headings, quotes, pre, lists, tables) as a
  blank line; `<br>` as a line break
- `<pre>` verbatim (indentation and newlines preserved)
- `<ul>`/`<ol>` items as `- ` / `N. ` plus two spaces of indent per nesting
  level; a task-list checkbox as `[x] ` / `[ ] `
- a table as one line per row of `\t`-joined cells, internal cell
  newlines/tabs flattened to spaces (same shape as browser/table paste)
- images skipped; a KaTeX formula contributes its LaTeX source once — read
  from `data-kdx-tex` on the `.katex` element (KaTeX is rendered HTML-only,
  see lazyrender.md Invariant 17), with a MathML `<annotation
  encoding="application/x-tex">` still read as a fallback for a page that
  carries one. The visible `.katex-html` copy is skipped so the formula cannot
  appear twice
- interactive chrome (`.kdx-copy-btn`, `#kdx-outline-btn`,
  `#kdx-outline-panel`) skipped

`base.css` additionally marks that chrome `user-select: none`, so the
on-screen selection agrees with what is copied (a select-all must not
highlight "Copy" or the outline entries).

Plain text only: no `text/html` flavor, no pictures. There is no context-menu
work; the web view's default menu is untouched.

## Invariants

- **The live preview never copies through the renderer.** Both paths reach
  `QGuiApplication::clipboard()` (directly, via `ClipboardBridge::copy`, or via
  `copySelection()`); `navigator.clipboard`/`execCommand` are the
  export-only fallback.
- The code button copies the `<code>` child's `textContent`, never the
  `<pre>`'s; the selection walker skips `.kdx-copy-btn` explicitly.
- `__kdxSelectionText()` returns `""` for a collapsed/absent selection; the
  host then leaves the clipboard alone (browser behavior) and touches nothing.
- The selection shape is defined by the walker, not by
  `getSelection().toString()`: list markers, table tabs, blank-line block
  separation and chrome exclusion are requirements, pinned by tests.
- **The preview claims its keys from Qt's shortcut map.** `ShortcutOverride`
  must be accepted for every key in `previewHandlesKey()` (copy, select all,
  the reading keys); otherwise a window-context Kate action takes the key
  before the web view and the preview never receives it. The claim and
  `forwardKeyEvent` must use the same key set.
- An empty selection must not clobber the clipboard; a non-empty one must
  reach Qt's clipboard even though the page holds no OS focus.
- A `__serializedHtml()` export carries exactly the copy buttons the live DOM
  has: on when the setting is on, none when it is off, and never decorated
  merely by loading.
- The `QWebChannel` is owned by the page and set before any document load;
  re-inlining `qwebchannel.js` on every `buildHtml()` is what keeps a reloaded
  page able to reach the bridge.
- The button is interactive chrome: hidden by `@media print` in `base.css`,
  and `user-select: none` together with the outline control.

## Pitfalls

- **The renderer's clipboard is not reliable.** `navigator.clipboard` needs
  document focus and permission, and `execCommand("copy")` returns false when
  the document is unfocused; in those sessions a renderer-side copy appeared
  to do nothing. This is the reason both paths exist in this shape.
- **`pre.textContent` is contaminated by the button.** A naive "copy the
  block" reads the button label back too. The same trap applies to any future
  per-block DOM added inside the `pre`, and is why the selection walker has a
  skip list rather than trusting a DOM range's text.
- **The class name is also in the embedded script.** Searching exported HTML
  for `kdx-copy-btn` always matches the inlined `preview.js` source; count real
  nodes by parsing the serialized HTML (what `renderfeaturestest.cpp` does).
- **Native selection stringification is not the target.** `Selection.toString()`
  is layout-blind: it includes the copy button and outline text, and its
  block/newline rules are undocumented across Chromium versions. Do not
  "simplify" the walker down to it.
- **Qt's shortcut map runs before the focus widget — and `sendEvent()` tests
  skip it.** This is the bug that made the feature look broken in real Kate
  while every headless test passed: the web view's widget accepts no
  `ShortcutOverride`, so Kate's window-context Copy consumed Ctrl+C and copied
  the *editor's* selection (wrong paragraph / offsets / raw markdown / empty),
  and arrow keys moved the editor. A test that pushes a `QKeyEvent` straight
  to the focus widget bypasses the shortcut map entirely; assert the
  `ShortcutOverride` claim explicitly (`previewClaimsItsKeysFromTheShortcutMap`)
  and keep the KeyPress test as the second half.
- **Whitespace between block elements is source formatting.** markdown-it
  pretty-prints (`</h1>\n<p>`), and treating that text node as content indents
  every block with a stray space; the walker drops whitespace-only text nodes
  at a block boundary but keeps significant ones between inline siblings.
- **Indentation is not inline text.** Marker strings carrying list indent must
  bypass the inline whitespace collapse, or `"  - "` collapses to `"- "`.
- **A formula must contribute its LaTeX exactly once.** KaTeX is rendered
  HTML-only now, so the source rides on `data-kdx-tex` (the `.katex-html`
  glyph copy is skipped); the MathML annotation remains a fallback. Copying
  both the visual tree and the source would duplicate the math.
- **The host callback can outlive the widget** (a document switch during the
  JS round-trip), so `copySelection()` holds the widget weakly.
- **An exported file must not require the channel.** `window.qt` does not
  exist in a plain browser, so `ensureHostClipboard()` guards on it.

## Test seams

`renderfeaturestest.cpp` pins the behavior:

- `codeCopyButtonCanBeDisabled` — decoration on by default, the copy source is
  exactly the fence text, the parsed export contains one button, toggling the
  setting removes it from the live DOM and the export.
- `codeCopyButtonCopiesViaHost` — clicking the button puts the fence text on
  `QGuiApplication::clipboard()` through the QWebChannel bridge.
- `selectionCopyShapesPlainText` — a whole-body selection (headings, wrapped
  paragraph, nested/task lists, fence, table) yields the exact structured
  string, chrome is absent, a partial selection is exact, a collapsed one is
  empty.
- `selectionCopyOfMathKeepsTheLatex` — an inline and a display formula each
  contribute their LaTeX exactly once (from `data-kdx-tex`); skipped without
  the KaTeX data-dir assets.
- `ctrlCCopiesSelectionToClipboard` — a synthesized `Ctrl+C` on the web view's
  focus proxy puts `__kdxSelectionText()` on the clipboard.
- `previewClaimsItsKeysFromTheShortcutMap` — the filter accepts
  `ShortcutOverride` for the copy/select-all/reading keys and leaves other keys
  to Qt's shortcut map (the regression that only shows up with a real
  key event, not with `sendEvent()` alone).

## Where it lives

- `data/js/preview.js` — shared `kdxWriteClipboard` / bridge handshake /
  `legacyCopyText`; the code-copy section (`decorateCodeBlocks`, delegated
  click listener, `__setCodeCopy`); the selection section
  (`__kdxSelectionText`, `selWalk`, `selTable`).
- `data/css/base.css` — `.kdx-copy-btn` styling and `user-select: none` for it
  and the outline control, plus the `@media print` hiding.
- `src/previewwidget.{h,cpp}` — `ClipboardBridge` + `QWebChannel`
  registration; `applyCodeCopy()` pushes the setting; `previewHandlesKey()`,
  the `ShortcutOverride` claim, `forwardKeyEvent()` / `copySelection()` are the
  selection path.
- `src/settings.{h,cpp}`, `src/configpage.cpp` — the persisted code-copy
  setting and its checkbox.
- `data/preview.html` — the `/*__QWEBCHANNEL_JS__*/` slot filled by
  `buildHtml()`.
- `CMakeLists.txt` / `tests/CMakeLists.txt` — the `Qt6::WebChannel` link.
