# Fragment links — preview jumps to the section a "#slug" names

Markdown links come in three flavors, and the preview now answers all three
with a section jump: in-page anchors (`[install](#install)`), links to another
document with a fragment (`[install](../README.md#installation)`, the gap this
doc covers), and self-file fragment links (`[top](README.md#installation)` on
`README.md` itself). The editor only ever opens the document (its current
behavior is untouched); the jump exists in the preview only.

## Goal / problem

A click on a cross-document fragment link used to *open the document* in Kate
— and the preview, following the now-active editor document, rendered it from
the top, silently dropping the `#section` part. Fragment links are "slugified"
against GitHub's heading anchors, which adds a second, subtler failure: the
preview generated heading ids only for the headings its floating outline
lists (configurable levels, none when the outline is off), and its slug rules
(duplicates numbered `-2`…, punctuation collapsed to dashes) did not match the
slugs document authors write links against.

## Design

### 1. Heading anchors (`data/js/preview.js`)

`rebuildOutline()` assigns an anchor id to **every** heading with text (h1-h6,
whatever the outline-level configuration — the outline list still only shows
configured levels). Ids follow GitHub's slug rules, which is what authors link
against: lowercase (Unicode-aware), punctuation dropped outright, each space
mapped to one hyphen without collapsing runs (`Chapter 2 — Getting Started` →
`chapter-2--getting-started`), and repeated headings numbered `-1`, `-2`, …
from the second occurrence (`outlineUniqueId` keeps clear of ids already in
the document, e.g. raw-HTML headings). An authored id on a heading is kept.

### 2. Resolving a fragment (`window.__scrollToFragment`, in preview.js)

All jumps funnel through one page entry point: the C++ side calls it after
rendering a document that a fragment link named, and the pure in-page click
fallback (below) calls it too. Resolution order:

1. `getElementById(frag)` — an anchor id the preview generated, or a raw id.
2. A tolerant heading scan (`headingForFragment`) for slugs written against
   slightly different rules: case- and punctuation-insensitive comparison
   (letters/digits only) over the GitHub-numbered slug of each heading's
   visible text.

On success the heading is scrolled into view (instant, like a native anchor)
and flashed with the outline's `kdx-outline-jumped` ring (`flashHeading`, the
same helper the outline's `jumpToSection` uses).

### 3. The click paths

- **In-page `#slug` clicks** keep Chromium's native fragment navigation when an
  element with that id exists (the common case now that every heading carries
  a GitHub-flavored id). `onFragmentClick` only intercepts a click whose
  fragment names **no** element and retries it through the tolerant match —
  so a heading the preview left id-less or a foreign slug still lands, and
  nothing that worked before changes.
- **Document links with a fragment** (`../README.md#installation`, or the
  self-file form) are intercepted by `PreviewPage` as before and reach
  `PreviewWidget::openLink()`, which stores the pending anchor
  (`m_pendingFragmentDoc` = target URL minus fragment, `m_pendingFragment`)
  and opens the document through Kate exactly as a fragmentless link would.
  `render()` applies the pending anchor whenever it renders the target
  document — the anchor is **one-shot** and is dropped once the page reports
  the section was found, or when any *other* document renders instead.
  `openLink()` applies it immediately when the preview is already showing the
  target document *live* and nothing is about to re-render it (the self-file
  case, where Kate only re-focuses the same editor view), because no render
  would ever consume it.

## Invariants

1. **The anchor is one-shot and target-scoped.** It fires only on a render of
   the document the click named (compared by absolute local path), is cleared
   once the page found the section (`__scrollToFragment` returns true), and is
   cleared without firing when any other document renders. A stale anchor can
   never jump inside an unrelated document.
2. **An anchor survives renders that ran too early.** Kate opens a clicked
   file asynchronously; the first render of the freshly attached document can
   run on empty text. Because the anchor is only dropped on `found == true`,
   the render that finally carries the real content performs the jump. Without
   this, the click would be swallowed by the empty render.
3. **Fragment links never navigate the preview page.** As before, a
   non-anchor link click is rejected by `PreviewPage` and handed to the host;
   the preview only ever re-renders documents the editor activates.
4. **Heading ids exist independent of the outline configuration** (any level,
   or none enabled): fragment links must not depend on a UI setting. The
   outline *list* still honors its levels.
5. **A click that worked before still works natively.** Found-element in-page
   anchors keep Chromium's fragment navigation; the interception only fires
   where native navigation would do nothing.
6. **Exports and the live image machinery are untouched** — anchors are plain
   ids in the serialized DOM; no new attributes or state are added to content.
7. **The immediate jump in `openLink` requires a live document.** For the
   frozen snapshot of a closed editor tab the click opens the file afresh, so
   the anchor is left pending for the new document's render instead of being
   applied to content that may be stale.

## Pitfalls (each cost a debugging session here or upstream)

- **Duplicate numbering was off by one vs. GitHub.** The old id generator
  made the *second* repeated heading `install-2`; GitHub (and every TOC
  generator authors copy from) makes it `install-1`. Any jump to a repeated
  heading silently missed. The numbering now matches GitHub's.
- **Outline-gated ids broke fragment links.** Ids used to be assigned only to
  headings the outline listed — with the outline switched off, *no* heading
  had an id and every fragment link (including GitHub-style in-page ones)
  failed. Ids are now assigned before the level filter.
- **Slug punctuation rules differ between renderers.** The old slugger
  collapsed punctuation runs to dashes (`What's next?` → `what-s-next`);
  GitHub drops the punctuation (`whats-next`). Authors write GitHub slugs, so
  ids follow GitHub rules; the tolerant match in `headingForFragment` is the
  safety net for any *other* slug scheme the author may have used (compare
  letters/digits only, honor `-1`/`-2` numbering).
- **The empty render race (async open).** An anchor consumed at the first
  render after a link click would fire on an empty document, because Kate
  loads the opened file asynchronously. Anchors are dropped only on a *found*
  report (Invariant 2).
- **Jumping a frozen snapshot vs. the re-opened file.** The preview can keep
  showing a closed document's snapshot; clicking a fragment link to that same
  file re-opens it, and the editor may load *newer* content than the
  snapshot. The immediate-jump shortcut is therefore restricted to a live
  document (Invariant 7).
- **Scroll drift from late images.** Images above the target that decode
  after the jump push the heading down. This is inherent to lazy images and
  applies to native anchors and GitHub alike; the preview's parked images keep
  their boxes only after a first decode, so a jump into never-decoded content
  can land a bit high until the images around it load.
- **Kate drops the fragment when opening the file.** Real Kate and the
  follow-mode test host both open `doc.md` when handed `doc.md#section`; the
  C++ side must capture the fragment *before* handing the URL over.
- **A debounced edit or an in-flight recycle must not swallow the anchor.**
  The immediate-jump path checks that no re-render is queued (debounce idle,
  no pending export, not mid-recycle) and otherwise leaves the anchor to the
  next `render()` — which also re-applies it after a recycle's fresh load.

## Test seams

- Which test pins which invariant:
  - `renderfeaturestest::fragmentLinksJumpToSections` — Invariant 4 and the
    slug rules: ids on every level incl. an H6 while the outline lists H1-H3
    only; `whats-next` (punctuation dropped) and `install`/`install-1`
    (GitHub numbering); exact and tolerant `__scrollToFragment` jumps flash
    and scroll; unknown fragments report false; outline off still anchors.
  - `followmodetest::fragmentLinksFollowAndScroll` — Invariants 1-3, 7 end to
    end against the fake Kate host: clicking a real markdown link to a file in
    another folder opens it through `openUrl` (async — the empty-render race
    is live here), the preview follows and jumps to the flashed, scrolled
    section; a self-file fragment link then jumps in place with no second
    editor view (`openUrlCalls` unchanged by the re-focus).
- Env hooks: none new. Tests run shown + resized because jumps scroll the real
  page (same requirement as the image/scroll tests in lazyrender.md).
- The follow-mode test host strips URL fragments in `openUrl`, mirroring Kate.

## Where it lives

- C++ pending anchor: `src/previewwidget.{h,cpp}` (`openLink`,
  `attemptFragmentJump`, `sameDocumentAsCurrent`, the `render()` hook, the
  `m_pendingFragment*` members).
- Page: `data/js/preview.js` (`outlineSlug`, `outlineUniqueId`,
  `rebuildOutline` id assignment, `flashHeading`, `__scrollToFragment`,
  `headingForFragment`, `onFragmentClick`).
- Tests: `tests/renderfeaturestest.cpp`, `tests/followmodetest.cpp`.
