# Reading navigation — opt-in Vim keys for the preview

## Goal / problem

Markdown previews are often read without a mouse. An independent, default-off
setting adds small-step (`j`/`k`), overlapping-page (`n`/`p`), and horizontal
(`h`/`l`) reading navigation without changing editor behavior. `Ctrl+h/j/k/l`
also moves focus between the editor and preview panes.

## Design

The setting is persisted as `VimReadingNavigation` and is applied live to all
loaded previews. Qt claims the unmodified reading keys from Kate's shortcut
map only when the preview has focus; the embedded page performs scrolling so it
can inspect the DOM. Auto-repeat is the browser's normal repeated key-press
stream, not a timer.

The page ignores input, textarea, select, button, and contenteditable targets.
For horizontal movement it first chooses the hovered or focused ancestor with
real horizontal overflow, then the page itself; a nested container at an edge
is not bypassed and a document without overflow is a no-op.

Pane focus movement is implemented in Qt. The plugin filters the main
application event stream for editor-originated keys and the preview emits a
request for preview-originated keys. The application filter only accepts a
verified editor pane as its source; preview descendants remain on the
PreviewWidget/WebEngine path. Ctrl pane keys are claimed from Qt's shortcut
map, then allowed through the WebEngine page; preview.js checks the actual DOM
event target synchronously and leaves editable controls native.
Normal page targets call the `kdxNavigation` QWebChannel bridge. Candidate panes
are every visible editor view exposed by `MainWindow::views()` plus the visible
preview; their centers are mapped into the main-window coordinate system and
the nearest candidate with positive distance in the requested half-plane wins.
No left/right layout is assumed.

## Invariants

- The setting defaults to off, is independent of theme/loading/image settings,
  persists, and changes existing previews without a reload.
- Native arrows, selection/copy, links, and unrelated keys remain native.
- Reading keys never act on the editor. Pane focus shortcuts are limited to
  this setting and the plugin's main window.
- Editable preview controls keep their text-entry behavior; they are not
  scrolled or pane-switched by reading navigation.
- The application-wide pane router handles only verified editor panes. All
  preview descendants, including WebEngine focus proxies and editable DOM
  controls, continue to PreviewWidget and the page.
- Horizontal scrolling never falls through from an overflowing nested target
  to the page merely because the nested target is already at an edge.
- Focus movement uses actual widget geometry and does nothing when no pane lies
  in the requested direction.
- The preview pane's geometry container is not treated as the focused content:
  editor-to-preview movement resolves the current WebEngine view/focus proxy,
  and the handoff is useful only when subsequent page keys reach that child.
- DOM focus ownership is decided for each key event; no asynchronous Qt cache
  can survive a focus transition, reload, or discarded page.
- A render clears the remembered hover node, and horizontal navigation rejects
  detached nodes before falling back to live focus and the document root.

## Pitfalls

- JavaScript-only key interception is too late: Kate's shortcut map can consume
  a key before the WebEngine page sees it. `ShortcutOverride` ownership belongs
  to the Qt preview filter.
- WebEngine's focus proxy is created and replaced lazily. The preview keeps its
  input filter attached to the current proxy. Ctrl directional ownership must
  remain in the page's DOM keydown path; polling activeElement from a Qt event
  filter races the browser's default focus processing and can steal controls.
- `PreviewWidget` is deliberately retained as the pane geometry object. Calling
  `setFocus()` on that outer widget can leave focus outside the WebEngine
  render widget; use its focus-content helper, which resolves the live proxy
  on every handoff.
- `Ctrl+h/j/k/l` intentionally conflicts with Kate actions using those
  sequences while the option is enabled. The configuration page calls this
  out; turning the option off restores those shortcuts.

## Test seams

The setting's getter/setter and config persistence can be tested without
WebEngine. Preview tests exercise `ShortcutOverride` ownership and page scroll
offsets with the real embedded page; JavaScript tests cover programmatic
editable focus, normal-page bridge requests across a render, stale hover
discard, and nested overflow. Pane focus tests use deliberately positioned
widgets and verify editor→preview and preview→editor movement. The plugin
also uses the public `KTextEditor::MainWindow::views()` API, so visible editor
splits are covered by the same geometry seam. The end-to-end plugin test also
keeps a focused preview input while a destination editor exists and verifies
that both Qt key-event phases reach the preview path instead of the global
router.

## Where it lives

The preference is in `src/settings.*` and `src/configpage.*`; Qt ownership and
focus arbitration are in `src/previewwidget.*` and `src/pluginview.*`; page
scroll targeting is in `data/js/preview.js`.
