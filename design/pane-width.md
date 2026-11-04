# pane-width — opt-in programmatic width for the preview pane

A narrow, off-by-default workaround for Kate windows whose splitter handles
cannot be dragged; it sets the width of the tool-view pane from the plugin
instead of relying on the mouse.

## Goal / problem

Kate gives every plugin tool view a pane inside Kate's private MDI splitters.
Normally the user drags the divider between editor and panel; Kate stores that
width in its session (`Kate-MDI-H-Splitter`, `Kate-MDI-Sidebar-<pos>-LastSize`)
and restores it.

Some environments (observed on an Omarchy/Hyprland setup, but reproduced in
Dolphin as well) end up with **QSplitter handles that are not interactive**:
the cursor never changes over the divider and dragging does nothing. The
preview pane then stays at `KateMDI::ToolView::minimumSizeHint()` = 160 px,
which makes the Markdown preview unusable. Fusion style, XCB vs Wayland, a
clean restart, and unsetting the GTK platform theme did not help there, so the
plugin cannot fix the root cause — but it can bypass the dead handle.

## Design

`Settings::paneWidthOverride()` (off by default) + `paneWidthPercent()`
(10..80, default 35) drive `PluginView::applyPaneWidth()`:

- `PaneWidth::resolve()` walks the ancestry of the tool view:
  `ToolView -> tab-stack widget -> ownSplit -> outer splitter`. Only apply when
  the outer splitter is horizontal (Left/Right); Top/Bottom is a height, out of
  scope. This is deliberately the only place that knows Kate's widget shape.
- Target = `percent` of the outer splitter's current total pane sizes.
- `PaneWidth::apply()` sets the pane to the target and takes the delta from
  the largest sibling (normally the central editor pane), so the other panes
  keep their sizes; it clamps to leave at least 200 px on both sides.
- Re-apply on: tool-view `Show`, a debounced `Resize` of the outer splitter
  (so a percentage stays proportional when Hyprland tiles/maximizes the
  window), and `Settings::changed`. A one-shot re-apply 600 ms after `Show`
  covers Kate's session restore, which calls `updateSidebar()` ~400 ms after
  startup and would otherwise overwrite the width with the saved 160 px.
- The outer splitter is watched with an event filter; the pointer is a
  `QPointer` and the filter is removed in the destructor and when the
  ancestry changes (the user can move the tool view to another side).

## Invariants

- **Off by default.** With the setting off, `applyPaneWidth()` returns
  immediately and the plugin never touches splitter sizes.
- **No permanent constraints.** Never use `setMinimumWidth`/`setMaximumWidth`
  or a fixed width on the pane: that breaks Kate's drag-to-collapse and
  would make the pane unresizable even in a healthy environment.
- **Only Katexdown's pane.** The code resolves the splitter by walking up from
  its own tool view; it never enumerates Kate's other tool views.
- **Shared width is expected.** The pane holds every tool view in that
  sidebar/tab stack (e.g. snippets), so enabling this changes their width too.
- **Fail soft.** If the widget chain is not the expected KateMDI shape, do
  nothing. This is private structure; a Kate refactor must not crash us.

## Pitfalls

- **160 px is a symptom, not a width.** It is exactly `ToolView`'s minimum.
  If the divider is draggable, dragging once is the correct fix; do not turn
  this setting on and then wonder why the panel snaps back on resize.
- **Kate overwrites on restore.** `finishRestore()` re-applies saved sizes
  from a ~400 ms timer; a single apply at `Show` can lose that race. Hence
  the delayed second apply.
- **Percentage vs pixels.** The window size itself is unreliable on the
  affected setup (floating windows may open at a small default), so a
  percentage tracks the editor area; a fixed pixel width would squeeze the
  editor on a small window.
- **Do not fight a working handle.** In a healthy environment, a window
  resize will re-apply the percentage and undo a manual drag; that is the
  documented cost of the override, and why it is opt-in.

## Test seams

`PaneWidth::resolve()`, `target()` and `apply()` are free functions in
`pluginview.h`, exercised by
`FollowModeTest::paneWidthTargetsAndAppliesToSplitter()` in
`tests/followmodetest.cpp` against a plain `QSplitter` hierarchy (shape
resolution, refusal of an unrelated widget, percentage clamping, no-op, donor
distribution, editor minimum). `resolve()` is the single adapter to Kate's
MDI shape: if Kate changes it, that function is the only code to update, and
until then the setting simply logs once and does nothing. The manual check is:
enable the setting, open the preview, resize the window, and move the tool view
to the other side.

## Where it lives

`src/pluginview.{h,cpp}` (resolution + re-apply hooks), `src/settings.*`,
`src/configpage.*` (the "Set the preview panel width manually" rows).
