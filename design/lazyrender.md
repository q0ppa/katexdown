# lazyrender — preview loading & renderer memory

One design idea, applied at three scales: **nothing should be loaded, decoded,
or alive that the user is not looking at.** This doc covers the levers the
`lazyrender` work added — page lifetime, per-document engine gating, the image
decode policy, and the renderer-memory maintenance that keeps a long-session
renderer from growing without bound — their cross-cutting invariants, and the
regressions they caused that must not happen again.

## Goal / problem

The preview is a QWebEngineView with its own Chromium renderer process. Four
things scale badly with naive "render everything, keep everything":

1. **The process itself** — an idle renderer resident in a closed panel.
2. **Heavy engines** — KaTeX is hundreds of KB of JS/CSS plus fonts; highlight.js
   and js-yaml are not free either. Inlining all of them into every page
   wastes memory and parse time on documents that never use them.
3. **Decoded images** — a decoded photo is width × height × 4 bytes in the
   renderer. A document with hundreds of images decodes them all eagerly if
   nothing stops it.
4. **The long-lived renderer's dead memory** — a Chromium renderer never gives
   memory back to the OS on its own, and in QtWebEngine a *visible* page can
   neither be frozen nor discarded. Every full re-render of a large document
   (or every document switch) leaves a chunk of dead memory behind — measured
   at roughly 0.5-0.9 kB per byte of markdown pushed, per generation, with no
   natural plateau (a text-only switching session grew to 2.2 GB in testing
   with the panel open the whole time). Without maintenance, "leave it open"
   is a slow leak to gigabytes.

## Design

### 1. Page lifetime (Settings::LoadingMode, `pluginview.cpp` + `previewwidget.cpp`)

The tool-view *shell* (a plain QWidget) always exists so Kate's sidebar,
View menu and session handling work. The expensive web view follows the mode:

| Mode | When the web view exists | While the panel is closed |
|------|--------------------------|---------------------------|
| `LazyKeep` (default) | first panel show | frozen immediately; discarded (renderer exits) after the panel has stayed closed for `m_idleDiscardMs` (default 60 s) |
| `LazyUnload` | first panel show | destroyed on hide; re-opening recreates it |
| `Eager` | plugin load | frozen, never released |

Mechanics that matter:

- **Freeze is instant; discard is delayed.** `panelClosed()` requests `Frozen`
  right away, but Qt only freezes *loaded, hidden* pages and can refuse the
  request while the hide event is still being delivered — so a 1 s `idleTick`
  retries until the freeze sticks. Only once the page is frozen *and* the
  panel has stayed closed past the idle delay does `LazyKeep` request
  `Discarded`. Eager freezes but never discards.
- **A discarded page comes back as a reload.** `panelOpened()` requests
  `Active`; Qt reloads a discarded page, which re-runs the whole load
  pipeline (`loadFinished` → theme, image mode, render, outline, pending
  export). The widget itself survives, so content returns without recreating
  anything.
- **`m_loaded` means "page loaded and its renderer is alive".** It is cleared
  while a page is loading and whenever the lifecycle callback reports
  `Discarded`. Every script push (`render`, themes, image mode, outline) is
  guarded by it; a discarded page simply waits for its reload to re-apply
  everything. This invariant is what makes freeze/discard safe mid-session.
- **Session restore** creates the preview early (`readSessionConfig`) when the
  panel was open in the previous session, at the same timing Eager uses —
  creating it mid-restore was what used to duplicate the panel.
- A *frozen* page keeps its renderer and content and still accepts script
  pushes, so toggling the panel back is instant.

### 2. Engine gating (`enginesForText` in `previewwidget.cpp`, `buildHtml(engines)`)

Each page inlines only the optional engines its document text can use:
`kEngineKatex` (math), `kEngineHljs` (fenced code), `kEngineYaml` (front
matter). `enginesForText()` is a cheap line-oriented scan that skips the
inside of fences (so `$5` amounts or `$`-using code do not pull KaTeX in);
any fenced-code line (a triple-backtick or a tilde fence) — opening or
closing — marks the document as using the highlighter, a very first "---"
line marks front matter, and `$`-math outside fences marks KaTeX.

Rules:

- markdown-it and preview.js are always inlined; the heavy engines are gated.
  KaTeX's stylesheet is gated together with its scripts; the (small) hljs
  stylesheets are always present so a theme switch can never leave a gap.
- **The dollar scan mirrors the renderer, it never invents stricter rules.**
  A line counts as math exactly when markdown-it-texmath (the `dollars` set)
  would turn its `$...$` into a math token: opener not after a digit or
  backslash (`$_pre`), non-space content after the opener, closer not
  preceded by space/backslash and not followed by a digit (`$_post`). There
  is deliberately **no LaTeX-ish content requirement** — `$2^{192} - 2^{32}$`
  and even a bare `$2$` render like `$x$` does. Over-detecting (a prose pair
  texmath would keep literal) only costs an inlined engine; under-detecting
  strands the dollars as literal text with no way to self-heal.
- **False negatives are self-healing:** a page that lacks an engine the text
  grows into is rebuilt *once* — `render()` (and `exportToFile()`) detect
  `enginesForText(m_text) & ~m_pageEngines != 0` and call `loadPage()`, a full
  `setHtml()` whose `loadFinished` re-applies mode/theme/content. A fully
  equipped page skips the scan entirely; a rebuild is stable (the rebuilt page
  carries the engines the same text needs). Self-healing is only as good as
  the scan: it cannot repair a detector false negative (see the digit-math
  pitfall).
- An engine a document *stops* needing only leaves on the next full load
  (document switch or rebuild), never mid-page.

### Fence-size cap (highlight output)

Engine gating decides *whether* highlight.js is on the page; the cap decides
*how much of one fence it may tokenize*. highlight.js turns each token into a
`<span>`, so a single oversized fence (a minified bundle or a dump pasted
wholesale — extremely rare) would otherwise build tens of thousands of DOM
elements in one block and re-tokenize them on every keystroke. `preview.js`
therefore treats any fence longer than `kdxHljsMaxChars` (50 000, exposed on
`window` for tests) as plain code: same `<pre>` box, same text (HTML-
escaped), no token spans — only the syntax colors are lost. Normal fences are
unaffected, and engine gating is unchanged (a document still loads highlight.js
when any fence exists, because a doc can outgrow an oversized fence into a
normal one on the next edit).

### 3. Image decode policy (Settings::ImageMode, `data/js/preview.js`, `data/css/base.css`)

Mirrored in the page through `__setImageMode('eager' | 'auto' | 'saver')`
(JS default `auto`; the C++ side applies the mode on every load *before* the
first render and on every setting change, re-rendering only when the mode
actually changes):

| Mode | Behavior |
|------|----------|
| `eager` | markdown-it output untouched; every image decodes at once (classic) |
| `auto` (default) | images get `loading="lazy"` + `decoding="async"`; the parking machinery arms only once a document passes `IMG_AUTO_ARM_AT` (12) images |
| `saver` | the same machinery, always on, with a tighter keep zone |

The machinery per managed image is a small state machine:

```
real src, decoded  ──leave keep zone──▶  parked (src = 1x1 placeholder,
  (dims recorded)                         real src in data-kdx, bitmap freed)
        ▲                                          │
        └─────────── enter keep zone ──────────────┘
                     (swapIn: restore real src, decode again)
```

- **Keep zone:** viewport ± `max(400px, viewportHeight × factor)` — × 2.5 for
  auto, × 1.3 for saver — delivered as the `rootMargin` of one
  IntersectionObserver. Images outside the zone are parked; images entering it
  are swapped back in. The zone (and the observer) is rebuilt on every
  re-render and on resize.
- **Parking preserves the box** (see Invariants): before an image can be
  parked its size must be known, so every image carries `width`/`height`
  attributes plus an inline `aspect-ratio` recorded from its first *real*
  decode (`recordDims`, marker `data-kdx-sized`); base.css adds
  `html[data-pv-imgmode="auto|saver"] img { height: auto }` so the recorded
  box scales like the real image under `max-width: 100%`. Author-pinned
  dimensions are left alone (`data-kdx-sized="0"`).
- **Never-decoded images are never parked.** They have no decoded memory to
  free and no known box; parking them would collapse the document height. They
  keep their real src and stay native-lazy: the browser loads them near the
  viewport, their box appears once, and *from that decode on* they join the
  parking machinery like everyone else. The document height therefore only
  ever grows once per image, off-screen — never shrinks, never re-collapses.
  (A box *remembered* from a previous page of the same document counts as
  known — see Section 6 — so it may be parked, box intact.)
- Images with `srcset` are left to native lazy loading only (`data-kdx-skip`),
  and images that genuinely failed (`data-kdx-failed`) are never parked.
- **Export is unaffected by all of this:** `__serializedHtml()` serializes a
  detached clone with real srcs restored and all machinery attributes
  stripped, so `Export HTML…` always writes plain, eager images (see Pitfalls
  for why the live page must not be serialized directly).


### 4. Renderer-memory maintenance (`previewwidget.cpp` idleTick / reclaimInPlace / reclaimAtIdle)

The levers above decide what enters a renderer. This one bounds what a
long-lived renderer *retains*. Measurements that shaped the design (offscreen
test environment):

- A renderer that stays alive never collects, by itself, the dead DOM/V8 memory
  a full-document re-render strands: switching among three ~100 kB documents
  grew one renderer by ~70 MB **per switch**, linearly, to 2.2 GB in 30
  switches, and 48 full re-renders of one document grew it to 2.75 GB with no
  plateau and no self-triggered GC.
- An explicit forced GC returns that memory **in place**: on the real preview,
  `window.gc()` took a 90 kB text document from 1053 MB back to 458 MB
  (400–600 MB reclaimed; ~40–150 ms for text, up to ~640 ms on a 660 MB KaTeX
  document), repeatably, with no navigation. With a 32 MB cap + GC the total
  engine RSS returns to the *loaded* footprint (~319 MB, helper processes
  included) — the same steady state a discard+reload converges to after it
  re-renders the document, minus the reload.
- Decoded-image memory is the one part a GC does **not** return: parking
  (`src = 1×1 placeholder`), `window.gc()` and
  `QWebEngineProfile::clearHttpCache()` barely move it. Only process death does.
- QtWebEngine refuses to freeze or discard a page it considers **visible**
  ("page is visible"), and a frozen page only runs Chromium's memory purge when
  it was *never shown* — so neither freeze nor discard can reclaim a visible
  page's renderer.

The maintenance therefore has two primitives, and **neither ever reloads a page
the user can see**:

- **In place (the primary): a forced GC.** `--expose-gc` is merged into the
  engine's `--js-flags` (see the JS heap guard below), exposing `window.gc()`.
  At an idle moment over budget, `reclaimInPlace()` calls it; the estimate
  drops to the image charge (what a GC cannot return). No navigation, no scroll
  change, no flicker.
- **Discard+reload (the image residue only, and only when invisible).**
  `performMemoryRecycle()` hides the page, discards it (the renderer process
  exits and returns every byte) and loads a fresh page from the mirrored
  document state, restoring the reader's scroll position and image geometry
  (Section 6). `reclaimAtIdle()` uses it only when the decoded-image charge
  alone is over budget **and** the preview's top-level window is minimized
  (`previewWindowMinimized()`), i.e. the user cannot see the page.
- **At a document switch**, the same discard+reload is free: the switch already
  reloads the page, so replacing that reload with a recycle costs the user
  nothing extra. This is what bounds tab-switching sessions.

The trigger is an **estimate**, not an RSS measurement (portable): every full
page load adds a fixed cost (re-executing the inlined engines), every
full-document markdown push that *replaces already-rendered content* adds
`text size × factor`, and every image decode the reader caused adds a charge
(see below), with the factors calibrated from the measurements above (defaults
in `readMemTuning()`; tunable per process via `KATEXDOWN_MEM_BUDGET_MB`,
`KATEXDOWN_MEM_IDLE_MS`, `KATEXDOWN_MEM_MAX_AGE_MS`, `KATEXDOWN_MEM_OFF=1`, and
for tests `KATEXDOWN_MEM_IMG_CHARGE_MB` and `KATEXDOWN_MEM_MINIMIZED`). The
estimate over- rather than under-counts, so real memory stays under the budget
even where the real leak ratio is higher than measured. A discard (or a
closed-panel freeze, which *is* a purge) resets the estimate; a forced GC
resets its reclaimable part and keeps `m_imgCharge` — that is the "since the
last reclaim" bookkeeping. A 5 s safety timer aborts any recycle that does not
complete, so the preview can never stay hidden.

**A page's first render is not charged.** The dead memory the estimate counts
is what a render leaves behind when it *replaces* content that was already on
the page. The very first render of a page — first open, or the fresh page of a
recycle — replaces nothing, so charging it text × factor would keep any
document whose text charge exceeds the budget permanently "due"; the open
preview would then reclaim on every idle window (measured: a fresh renderer
process every ~7 s for an idle 1 MB text document before this gate existed).
`m_renderedOnce` (reset when a discard kills the renderer) is the gate.

**Image decodes are charged too — but only the ones the reader caused.**
Scrolling past an image decodes it (at viewport quality) and the renderer keeps
a share of that decoded frame it will never return on its own — sticky over
50+ s of idle, invisible to a text-only estimate because it arrives without any
render or navigation. The page counts those decodes
(`window.__kdxDecodeStats` in preview.js — a load with `naturalWidth > 1`, so
the 1×1 parking placeholder and failed loads excluded) and the host polls it
once per second while an image-bearing document is open
(`idleTick`/`handleDecodeStats`, gated by a cheap `![`/`<img` text scan). Each
new decode is charged min(intrinsic bytes, 8 MiB) — capped so a grid of small
images stays cheap — or a flat amount when `KATEXDOWN_MEM_IMG_CHARGE_MB` is
set. The charge accumulates in `m_imgCharge` as well as `m_memEstimate`,
because a forced GC cannot return decoded images; only a discard can.

The page counts a decode only once the reader has moved the viewport since the
current DOM was built, and in `eager` mode it never counts. What a page decodes
*by itself* — the images around the position it was loaded at, the whole
document in `eager` mode, and the images a re-render rebuilds around the
viewport — is exactly the live set the next page reproduces, so charging it
reclaims nothing and keeps the estimate over budget forever (the self-refresh
loop, see Pitfalls). Decodes a reader's scrolling pulls in are the ones a
reclaim can actually take back. "Since the current DOM was built" is why
`rerender()` resets the gate: a keystroke rebuilds the image DOM, so its
decodes are page-caused even though the reader scrolled earlier in the same
page generation.

**A reclaim never runs in a loop on what it cannot reclaim.** After
`reclaimInPlace()` the estimate is exactly `m_imgCharge`, so if only images
keep it over budget, `reclaimAtIdle()` does **not** GC again (no reclaimable
charge) and does not reload (the page is visible): it simply does nothing until
the reader minimizes the window, at which point one discard+reload returns the
images. This is what makes an image-heavy reading session safe without a
self-refresh.

### JS heap guard (`Settings::V8HeapCapMb` + `--expose-gc`, `src/kdxchromiumflags.h`)

Two renderer JS flags travel together in one `--js-flags` switch (Chromium
reads only the first such switch, so they must be comma-joined):

- `--expose-gc` — always, even when the cap is off. It exposes `window.gc()`,
  the maintenance's in-place reclamation primitive (Section 4).
- `--max-old-space-size=N` — when the configured cap is > 0. It bounds the V8
  old space **between** those GCs so the growth per render stays small; it
  never returns the freed pages itself. Cap and GC are complementary: cap =
  peak between collections, GC = return to floor.

Default cap **64 MB** (measured sweet spot): on a heavy KaTeX document it
renders in ~2.9–3.2 s versus ~2.45 s uncapped, but peaks ~710 MB instead of
~1.44 GB; with the idle GC the text floor is ~320–365 MB depending on cap, and
a 32 MB cap reaches the loaded baseline (~319 MB). The cap is invisible while a
document's per-render working set stays under it and only costs GC time when it
does not. "Off" (0) drops the cap; the forced GC still runs.

- **Delivery is the constraint.** QtWebEngine has no runtime API for flags; it
  reads `QTWEBENGINE_CHROMIUM_FLAGS` once, when the engine starts (first
  profile/page/view — effectively the first renderer process). The flags are
  merged into that env var before the engine can start, from two idempotent
  points: the plugin constructor (plugins load before kate creates any tool
  view) and the `PreviewWidget` constructor, *before* it creates its own
  profile (covers the tests, which drive the widget without the plugin, and the
  lazy modes where the engine starts on first open). Best effort: if another
  plugin already started the engine, the flags arrive too late.
- **Explicit `--js-flags` in the user's env wins.** Chromium reads the first
  `--js-flags` switch, so appending a second one would be silently dropped and
  the flags would look active without being. `applyJsFlags()` therefore refuses
  to touch the env when it already contains `--js-flags`. Then `window.gc` may
  be missing: `reclaimInPlace()` detects that once (`m_gcUnavailable`) and
  disables itself, and no visible page is ever reloaded as a fallback.
- **Reading happens once per process:** a setting change needs a Kate restart
  (config page says so), unless the engine has not started yet.

Measured behavior of the cap alone (Qt 6.11 / Chromium 140, offscreen probe,
renderer RSS alone — the two static ~88 MB helper processes are constant and
excluded; fresh renderer baseline ~172 MB):

| Document | No cap | Cap 128 | Cap 64 |
|---|---|---|---|
| typical (10 kB, 80 math), 8 re-renders | 172→443 MB, still +29 MB/render; ~100 ms | plateau ~420 MB; ~100 ms (free) | plateau ~291 MB; ~110 ms |
| stress (90 kB, 700 math), 8 re-renders | 172→2231 MB, +256 MB/render; ~0.8 s/render | plateau ~600 MB; ~3.0 s/render | plateau ~570 MB; ~3.0 s/render |
| text-only 90 kB churn | unbounded until recycle | ≈ no cap (see below) | — |

The cap alone never hands the freed pages back (it plateaus); the idle forced
GC is what returns RSS to the loaded footprint. Two measured consequences shape
the rules above:

- **A low cap costs render time when it binds.** A document whose per-render
  working set exceeds the cap makes V8 collect *during* the build — the stress
  row above goes from 0.8 s to ~3.0 s per render. That is the trade-off the
  configurable setting (and "off") exists for.
- **The cap is not a substitute for reclamation.** On the text-only churn the
  maintenance was calibrated on, a 128 MB cap changes almost nothing (that
  growth is not old-space-dominated), and neither the cap nor a GC returns
  decoded images — those need the discard. A binding cap merely makes real
  growth smaller than the estimate, so a reclaim may fire a little early —
  harmless, never late.
- `--enable-features=PartitionAllocMemoryReclaimer` and
  `PartitionAllocLazyCommit` measured no effect on this workload in Chromium
  140 (already the defaults there); do not resurrect them without re-measuring.

### 5. Reloadless document switches (`attachDocument` fast path)

Every document switch used to be a full `setHtml` navigation — re-executing
every inlined engine and paying the fixed per-load renderer cost even for a
plain text swap. When the incoming document lives in the **same folder as the
page that is currently loaded** (same relative-image base, same local-file
guard) and needs no engine the page lacks, the switch now just pushes the text
(`render()` in place) and resets the scroll. Same-folder tab switching — the
common "a few markdown files in one project" case — is therefore much cheaper
than before *and* dirties the renderer less between recycles. Any folder or
engine change, a loading page, or a closed panel falls back to the full load.
A doc that outgrows the page's engine set still triggers the one-time rebuild
(Invariant 4).

### 6. Remembered image geometry (`__kdxRecycleState`/`__setImageDims`, preview.js)

A page reload is a new document built from the markdown, and the markdown
carries only an image's `src`: the fresh page knows no image sizes, so it lays
the document out as if every image were zero-height until each one decodes.
That is fine when the reader arrives at the top, and wrong for the one reload
that is supposed to be invisible: the maintenance recycle restores the reader's
scroll position immediately after the fresh render, when the document is still
that much shorter, so `scrollTo` clamps to the collapsed height and the reader
lands somewhere else entirely (measured: 640 px off on a six-image document,
before the layout even finished growing back).

The host therefore carries the geometry across the reload with the scroll
position:

- Before discarding the page, the same-document recycle asks it for one JSON
  snapshot — `window.__kdxRecycleState()` returns `{y: scrollY, dims: {...}}`.
  One call on purpose: a `runJavaScript` callback cannot survive the discard,
  so everything the fresh page needs must be captured before it, and both
  values belong to the same moment (a position is only meaningful against the
  layout it was measured in).
- `dims` maps an image's real `src` to `[width, height]`, taken from a real
  decode (`naturalWidth > 1`, which also covers `eager` mode, where no boxes
  are ever recorded) or from the recorded box of a parked image. srcset images
  stay out (they are native-lazy only) and the 1×1 parking placeholder is never
  a key.
- The box set is **per document**: `attachDocument` clears it, and so does a
  URL change (save-as/rename), so the same relative image name in another
  folder can never be sized by the wrong picture.
- `loadFinished` pushes it into the fresh page (`applyImageDims` →
  `__setImageDims`) *before* the first render, so `rerender()` gives every
  image its box (`width`/`height` + inline `aspect-ratio`, marker
  `data-kdx-remembered`) before the layout — and with it the restored scroll
  position — is decided.
- A remembered box never overwrites author-pinned dimensions, and it is only
  provisional: the image's first real decode on the fresh page corrects it
  (`recordDims` treats `data-kdx-remembered` as "ours, re-record", so a file
  that changed on disk wins over the memory), and it is stripped from exports
  like any other machinery box (`data-kdx-sized="1"`).

Because the boxes exist from the first frame, the recycle's scroll restore
lands on exactly the content the reader was looking at, and nothing shifts as
images decode afterwards. It also means the fresh page parks far-off images
right away (their box is known), which is friendlier to memory than
re-decoding them.

## Invariants (rules a change must not break)

1. **Parking never moves the layout.** A parked image's box is byte-identical
   to its real box: same `scrollHeight`, same position of everything below.
   Consequence: `swapOut()` refuses to park an image whose box is unknown
   (never decoded, no author width+height attributes).
2. **The 1×1 placeholder is never observed as an image.** Its `load` must not
   reach `recordDims` (the dot bug, below), a parked image ignores its load
   and error events (they are placeholder decodes / parking aborts), and
   `recordDims()` itself refuses a decoded 1×1 size. Dimensions are recorded
   only from real decodes.
3. **`m_loaded` == page loaded and renderer alive.** Discard clears it; every
   script push is guarded by it; the load pipeline re-applies everything after
   a discard-induced reload.
4. **A page only ever carries the engines its text can use, and grows into a
   new engine through a one-time full rebuild** — never by scripting assets in
   afterwards. Every path that can push fresh text (`render`, export) must
   check the engine set first.
5. **Exported HTML is always plain and eager**, regardless of the live image
   mode, and is serialized from the freshest pushed text.
6. **The closed panel costs no CPU.** Kept modes freeze on close; only a
   frozen, long-closed page may be discarded. Opening returns the page to
   Active and lets the load pipeline refresh it.
7. **Reclaim in place first; discard only for what a GC cannot return — and
   never a visible page.** A forced GC (`window.gc`, exposed by the merged
   `--expose-gc`) returns the dead DOM/V8 memory a re-render stranded without
   touching the page: that is the primary reclaim. Decoded-image memory is the
   one part it cannot return, so the maintenance may fall back to the
   discard+reload (`performMemoryRecycle`) only for the image charge and only
   while the preview's window is minimized (`previewWindowMinimized()`), i.e.
   invisible; a page the user can see is never reloaded, and when `window.gc`
   is unavailable (the user's own `--js-flags` won the merge) the maintenance
   does nothing rather than reload. Discard is the only wholesale renderer
   release QtWebEngine allows: it hides the page (`setVisible(false)`), which
   makes the `Discarded` transition legal, and then loads a fresh page from
   the mirrored document state. The retry for a refused discard only ever
   re-discards a page whose discard never happened (`m_recycleDiscarded`):
   once the discard stuck, the page in front of the ticker is the recycle's
   *fresh* page, and discarding that one kills the load the cycle is waiting
   for (see Pitfalls).
8. **A discard+reload is a reload, so it must obey the load pipeline
   invariants.** It only starts when the page is loaded/active and the panel
   is open; the fresh load clears and re-sets `m_loaded` through the normal
   `loadFinished` pipeline, which re-applies theme, image mode, content and
   outline from the mirrored state (`m_text`/`m_doc`). `render()` and friends
   never push into a page that is mid-recycle; they wait for the pipeline like
   after any load.
9. **A reclaim is only ever user-invisible.** The in-place GC runs only after
   the preview has been quiet (no text change, input or pending work) for the
   idle window, is visible, and is not focused; a charged image decode counts
   as activity, so a scrolling reader is neither GC'd nor reloaded mid-scroll.
   The image-residue discard additionally requires the preview's window to be
   minimized; it restores the scroll position *and the image geometry* that
   position was measured against (Section 6), so the reload lands on the same
   content when the reader returns. A safety timer aborts any recycle that
   gets stuck, so the preview can never remain hidden.
10. **A page only ever re-renders a same-folder switch in place** (Section 5)
    when it is loaded, the panel is open, nothing is pending, and the new text
    needs no engine the page lacks; any other switch is a full load.
11. **A fence above `kdxHljsMaxChars` renders plain.** It is never fed to
    highlight.js; its content is HTML-escaped into the same `<pre>` it would
    otherwise get, so the block's box, text and export output are identical
    except for the missing token spans/colors. Raising the cap (or removing
    it) without re-checking renderer memory re-opens the span-factory pitfall
    below.
12. **A page's first render is never charged to the estimate.** Only renders
    that replace already-rendered content leave the dead memory the estimate
    counts; charging a fresh page's own first render (first open, or a
    recycle's fresh load) would keep any large document permanently "due" and
    reclaim on every idle window. `m_renderedOnce` is the gate, reset together
    with the estimate when a discard kills the renderer.
13. **Only the image decodes the reader caused are charged, and they are
    tracked separately.** The page counts an `<img>` load with
    `naturalWidth > 1` (the 1×1 parking placeholder and failed loads never
    count) *and* only once the reader has moved the viewport since the current
    DOM was built; `rerender()` resets that gate (a re-render rebuilds the
    image DOM, so the decodes it triggers are page-caused), and in `eager`
    mode no decode counts at all. The host polls the counter once per second
    while the open document can contain images and charges the delta against
    the estimate **and** against `m_imgCharge`. What a page decodes *by
    itself* is the live set the next page reproduces, so charging it would
    reclaim nothing and keep the estimate over budget in every generation (the
    self-refresh loop, see Pitfalls). The split matters: a forced GC clears
    the render part but not `m_imgCharge`, which is what keeps an image-heavy
    reading session from GCing in a loop, and what the invisible discard is
    for.
14. **The JS heap cap is additive and only ever lowers memory.** It must never
    be promoted into a substitute for reclamation (it neither bounds text-only
    churn nor decoded images), so the estimate, its calibration and the
    reclaim triggers stay as they are. A binding cap makes real growth smaller
    than the estimate charges; a slightly-early reclaim is fine, a late one is
    not.
15. **The JS flags reach Chromium only through the env, before the engine
    starts, and never over an explicit `--js-flags`.** Merging happens from
    the plugin and the `PreviewWidget` constructor before its own profile
    exists; both are idempotent and skip when the env already carries
    `--js-flags` (a second switch would be silently dropped — the flags must
    not look active without being). The merged switch always carries
    `--expose-gc` (the reclaim primitive) and the cap when configured.
    `KATEXDOWN_V8_CAP_MB` (an env int, 0 disables the cap but not
    `--expose-gc`) overrides the stored setting for scripts and tests.
16. **The geometry a reload is laid out with is per document, a fingerprint of
    the fresh page's layout, and never an author's dimension.** `m_imageDims`
    is captured from the live page before a same-document recycle discards it,
    pushed into the fresh page before its first render, cleared when another
    document is attached or its URL changes (a save-as can move the document to
    a folder where the same relative names mean other pictures), never
    overwrites width/height the author wrote, and is provisional — the image's
    first real decode on the fresh page corrects it. Exports never see it
    (`data-kdx-sized="1"` boxes are stripped, like every other recorded one).

## Pitfalls (each already cost a debugging session)

- **Image browsing accretes memory the text estimate cannot see.** Scrolling
  past a photo decodes it, and Chromium keeps a share of every decoded frame:
  measured ~8-10 MB per photo scrolled past, sticky for 50+ s of idle, and
  produced without any render or navigation — so a text-only estimator never
  fires. The decode poll (Invariant 13) exists for this; do not "fix" it by
  removing the charge (back to unbounded accretion) or by parking harder
  (parking already caps the *simultaneously decoded* set; the retention is the
  renderer's, not the `<img>` elements'), and do not extend it to decodes the
  reader did not cause (see the next pitfall).

- **An image-heavy preview recycled itself forever (the reported bug).** The
  decode charge counted *every* real decode, including the ones a page
  performs by itself at load — and a fresh page reproduces exactly that set. A
  document whose own decode set alone exceeded the budget therefore crossed the
  budget again in every generation, with no user action: measured, an idle
  six-photo document discarded and reloaded the page every ~3 s indefinitely
  (and each reload also moved the reader, see below). Neither the JS heap cap
  nor `eager` mode helps, because the loop is in the charge policy, not in the
  renderer: with `eager` the page decodes everything at load, so it loops
  hardest there. Fixed by Invariant 13 — the page's own set is never charged,
  in any image mode. `idleImageHeavyDocumentDoesNotRecycleInALoop` pins it (a
  fresh renderer every few seconds is the failure).

- **A re-render after a scroll re-charged the page's own decodes (the
  *residual* self-refresh).** The first fix gated the decode charge on "the
  reader has moved the viewport in this page generation", but the gate stayed
  open for the rest of the generation once a single scroll opened it. Every
  later `rerender()` rebuilds the image DOM (the host pushes new text on every
  debounced keystroke), so the images around the viewport decode again — and
  with the gate still open each of those page-caused decodes was charged
  (measured: 7 decodes after one scroll growing to 42 over five ordinary edits,
  ~150 MB of pure fiction against an 8 MB budget). The estimate then stayed
  over budget, and the maintenance recycled an idle document nobody was
  touching — the "it still sometimes refreshes by itself" report. `rerender()`
  now resets the gate (Invariant 13): a decode counts only while the reader has
  moved the viewport since the DOM was last built.
  `editorRendersAfterAScrollDoNotRechargeThePageOwnDecodes` pins it (the charged
  set must not grow across edits, and the renderer must stay put).

- **"Only process death returns renderer memory" was wrong, and it cost a
  reload.** The design originally treated the discard+reload as the only
  reclamation primitive, so the same-document recycle navigated (and flickered)
  purely for memory. Measured: an explicit `window.gc()` returns 400-600 MB of
  stranded DOM/V8 memory **in place**, repeatably, in ~40-150 ms, down to the
  loaded footprint. The maintenance therefore reclaims with a forced GC
  (merged `--expose-gc`), and the discard survives only for decoded images —
  which a GC does not return — and only while the window is minimized. Do not
  simplify this back to a visible reload, and do not treat the heap cap as the
  reclaim: it bounds the peak between GCs and never returns the pages.
  `idleTextChurnReclaimsInPlaceWithoutReload` pins the in-place reclaim (no
  renderer process change, RSS actually comes down).

- **A GC cannot return decoded images, so it must not loop on them.** After a
  forced GC the estimate is exactly `m_imgCharge`. If images alone keep it over
  budget, a policy that simply retries "over budget -> reclaim" would GC every
  idle window forever for no gain. `reclaimAtIdle()` therefore GCs only when
  there is render charge (`m_memEstimate > m_imgCharge`); the image residue
  waits for the minimized-window discard. `imageDecodeResidueRecyclesOnlyWhenMinimized`
  pins the visible half (no reload) and the minimized half (the discard).

- **The "1×1 dot" bug.** Images parked *before their first decode* (below the
  fold) had the parking placeholder's own dimensions recorded as their box:
  the placeholder is itself a 1×1 image, its `load` fired `recordDims`, and
  `data-kdx-sized="1"` then locked the image into a 1×1 box forever — a dot
  where the image should be, even though the real src decoded fine. Fixed by
  Invariant 2. The old test passed because its PNG was genuinely 1×1; the
  regression test now uses a 64×32 PNG and a 2×1 PNG (lifecycle tests) whose
  natural sizes cannot be confused with the placeholder's.
- **Parking undecoded images collapses the document.** Even after the dot fix,
  parking a never-decoded below-the-fold image on the 1×1 placeholder removed
  its height from the document, so reading a long document kept re-inflating
  the layout image by image. Fixed by Invariant 1: only box-known images are
  parked; the rest stay native-lazy. `parkingPreservesTheImageBox` pins this
  with exact `scrollHeight` equality across park/unpark cycles.
- **`src` must not be emptied to "park" an image.** `src=""` is a request for
  the document URL in Chromium; the placeholder is a 1×1 transparent data-URI
  GIF for this reason (and `swapIn` re-sets `src` explicitly so the real
  resource always restarts its load).
- **"Eager" is overloaded.** `Settings::LoadingMode::Eager` means *the web
  view is created at plugin load and never destroyed while closed*, while the
  image mode's JS value `'eager'` means *decode every image immediately*. Both
  appear in settings/UI prose; when touching one, be explicit about which.
- **Freeze requests can be refused silently.** Qt delivers the panel hide
  asynchronously and will not freeze a page that still counts as visible; the
  idle ticker's retry loop is what makes the closed-panel policy reliable.
- **Discard is not unload.** A discarded page has no renderer, so anything the
  C++ side would push (theme, mode, content) is dropped until Qt reloads it on
  the next open — which is exactly why the reload re-runs the whole pipeline
  instead of the widget being recreated (LazyKeep's "kept" promise).
- **Serializing the live DOM would leak machinery into exports.** A direct
  `outerHTML` would carry `data-kdx*`, placeholders, lazy attributes and
  recorded sizes into the standalone file, and would capture whatever image
  state the current scroll position left behind. Normalizing a detached clone
  keeps the live page untouched (nothing re-fetches or re-decodes).
- **Engine scan vs. live text:** engine decisions must always be made against
  the freshest text (document switches can arrive before the first render),
  which is why `render()`/`loadPage()`/`exportToFile()` refresh `m_text` from
  the document before scanning.
- **A reload that moves the reader is not "just a reload".** The image-residue
  discard restores the reader's scroll position after the fresh page renders,
  and it used to restore it against a page that knew no image geometry at all:
  markdown carries only an image's `src`, so the fresh document laid out as if
  every image were zero-height, `scrollTo` clamped to that much shorter
  document, and the reader landed hundreds of pixels away (measured: 640 px on
  a six-image document — and the layout then grew back under them as images
  decoded). Do not "fix" this by scrolling later or repeatedly: images the
  reader has already passed are never re-loaded by a lazy document, so their
  height is unknown no matter how long the restore waits. Capture the boxes
  with the position and hand both to the fresh page (Section 6).

- **The recycle's discard retry could kill its own fresh load.** `idleTick`
  retries a discard that Qt refused while its visibility flag was still
  catching up — but a page whose discard already stuck looks exactly like one
  that was refused: it is Active again (Qt re-activates it for the fresh
  `setHtml`). The retry therefore discarded the recycle's fresh page before it
  finished loading, which restarted the cycle, so the recycle never reached
  `finalizeRecycle`, its safety timer aborted it, and the reader's position was
  thrown away (measured: a flaky failure of the scroll restore in
  `maintenanceRecycleKeepsTheReaderPlaceWithImages`, roughly one run in three).
  `m_recycleDiscarded` marks the point after which the page in front of the
  ticker is the fresh one (Invariant 7).

- **A cached image is never "lazy".** Chromium loads a `loading="lazy"` image
  immediately once its bytes are in the memory cache, so an image can be far
  below the fold, keep `loading="lazy"`, and still decode the moment it is
  re-rendered (measured while writing the geometry test: an image 48 000 px
  down had already decoded). Only a src the page has never fetched is
  reliably deferred — a test that needs "an image that has not decoded yet"
  must give it a fresh URL (and the geometry test's far-away image is a
  *second* file for exactly this reason).

- **A giant fence is a span factory.** Tokenizing turns every token into a
  `<span>`; a single pasted minified bundle can mean tens of thousands of DOM
  elements, re-built on every keystroke. The fence-size cap above is what
  stops it — do not "fix" this by silently dropping oversized fences (the
  text must always render) or by removing the cap.
- **A second `--js-flags` is silently ignored.** Chromium keeps the first
  `--js-flags` switch, so merging a second one (e.g. appending our
  `--js-flags=--max-old-space-size=N,--expose-gc` to an env that already has
  `--js-flags`) would *look* applied without ever taking effect. The merge
  refuses instead (Invariant 15); `envflagstest` pins the refusal, and the
  maintenance then finds no `window.gc` and disables in-place reclamation
  rather than reloading a visible page (Pitfall above).
- **Flags are read at engine start, not at widget creation.** QtWebEngine
  consumes `QTWEBENGINE_CHROMIUM_FLAGS` once per process, at its first
  profile/page/view. Applying the cap in the plugin constructor is
  best-effort: it only works when nothing else started the engine first, and
  changing the setting never affects an already-running session — the config
  page tells the user to restart Kate.
- **"The renderer never returns memory" is the default, not a bug report.**
  Left alone, QtWebEngine keeps a long-lived renderer's dead memory (measured:
  tens of MB per full re-render of a large document, unbounded growth, no
  self-limiting GC). `performance.memory` is useless for tracking this — its
  numbers only update when a GC runs, which is exactly what never happens on
  its own. Renderer RSS (the process' resident set) is the only honest signal,
  and an explicit `window.gc()` is the reclaim (see the pitfall above).
- **Visible pages cannot be frozen or discarded, and "hidden" is Qt's call.**
  `setLifecycleState(Frozen/Discarded)` on a shown page logs "page is visible"
  and does nothing. Telling the *page* it is hidden
  (`QWebEnginePage::setVisible(false)`, which leaves the widget on screen
  showing its last frame) is what makes the discard legal. That is why the only
  same-document reload left is the image-residue discard, and why it is gated
  on the **minimized** window (`previewWindowMinimized()`): the page is then
  genuinely invisible to the user. Early prototypes froze unshown widgets and
  "proved" freeze purges memory — that purge only runs for pages that were
  never shown, so it must not be relied on for an open panel.
- **A discard that is part of a recycle must load the mirrored document, not
  wait for the committed URL to reload.** Loading the old document first and
  then replacing it doubles the work and can apply stale engines/guard roots.
  `setHtml` on a discarded page restores it and navigates straight to the new
  content (verified empirically), which is what the recycle's discard handler
  does.
- **An idle reclaim must not run while the user is touching the preview.** The
  quiet gate watches text changes, input events, focus and pending work (and a
  charged image decode counts as activity, so a scrolling reader is neither
  GC'd nor reloaded mid-scroll); the image-residue discard restores the scroll
  *after* the fresh render. Without the scroll restore a reload would visibly
  jump a reading user to the top.
- **The estimate must reset when the memory is actually gone.** A discard
  (process death), a closed-panel freeze (which *does* purge) and an in-place
  forced GC all reset the reclaimable part; otherwise the budget would fire
  immediately after the very reclaim that just cleaned the renderer. The GC
  keeps `m_imgCharge`, because decoded images survive it and must still be
  accounted for.

- **A gate heuristic stricter than the renderer strands real math.**
  `lineLooksLikeMath` used to treat a `$` right after a digit as currency
  ("$5") and demanded a LaTeX-ish letter inside the pair, so numeric math
  (`$2^{192} - 2^{32}$`, a bare `$2$` — both of which texmath parses happily)
  never set `kEngineKatex`. The page was built without KaTeX, the dollars
  rendered as literal text, and the "self-healing" rebuild used the same
  scan, so typing such math into a plain document never recovered either.
  The scan now mirrors texmath's `$_pre`/`$_post`/regex acceptance exactly
  (see the rules above); `numberMathLoadsKatexEngine` pins both the initial
  load and the typed-in rebuild, and keeps currency prose ("价格 $5 元",
  "$10 and $20 are prices") engine-free.

## Test seams

- Env hooks (named like the existing `KATEXDOWN_DEBUG`):
  `KATEXDOWN_IDLE_DISCARD_MS` shortens the LazyKeep discard delay;
  `KATEXDOWN_DATA_DIR` points at a fake asset dir (used to fake KaTeX assets
  so engine-gating tests need no downloaded data);
  `KATEXDOWN_MEM_BUDGET_MB`/`KATEXDOWN_MEM_IDLE_MS`/`KATEXDOWN_MEM_MAX_AGE_MS`
  shorten the reclaim budget and quiet window, `KATEXDOWN_MEM_IMG_CHARGE_MB`
  sets a flat per-decode image charge (deterministic for tests),
  `KATEXDOWN_MEM_MINIMIZED=1` makes `previewWindowMinimized()` report true (the
  offscreen harness cannot always minimize, and the image-residue discard needs
  to be drivable deterministically), and `KATEXDOWN_MEM_OFF=1` disables the
  policy ticker so tests can drive `performMemoryRecycle()` directly (it also
  disables the in-place GC, since nothing is ever "due");
  `KATEXDOWN_V8_CAP_MB` overrides the JS heap cap setting (an int in MB, 0 =
  cap off — `--expose-gc` is still merged). The tests' env also sets
  `QTWEBENGINE_CHROMIUM_FLAGS`, and the `PreviewWidget` constructor merges the
  production flags on top of it (the 64 MB default cap plus `--expose-gc`), so
  every engine test can call `window.gc` unless its own env already carries
  `--js-flags`. RSS is measured in tests
  by walking `/proc` for the test process'
  QtWebEngine descendants (Linux; the RSS tests skip elsewhere). Note that
  the policy refuses to recycle a *focused* preview, and the offscreen test
  harness focuses the shown widget — tests that exercise `idleTick` drop the
  focus again (`clearFocus` on the view and its focus proxy) to mirror Kate,
  where the editor holds focus.
- Which test pins which invariant:
  - `followmodetest::keptModesFreezeAndReleaseWhileClosed` — lifecycle state
    machine (Active → Frozen → Discarded → reload → Active; Eager never
    discards).
  - `renderfeaturestest::enginesAreLoadedOnlyWhenTheTextNeedsThem` —
    Invariant 4, including the one-time rebuild on growing text.
  - `renderfeaturestest::numberMathLoadsKatexEngine` — the gate's parity with
    texmath: numeric math (`$2^{192} - 2^{32}$`, `$2$`, `$10^3$`) loads the
    KaTeX engine both on first load and when typed into a plain document,
    while currency prose texmath keeps literal never does.
  - `renderfeaturestest::oversizedFenceRendersPlain` — Invariant 11: the cap
    is read from `window.kdxHljsMaxChars` (the page is the single source of
    truth); an oversized fence keeps its exact text, its `<pre>` box and its
    HTML escaping but renders zero token spans, while a normal fence in the
    same document still highlights.
  - `renderfeaturestest::imageModesControlDecoding` — the three modes, the
    park/restore cycle, export normalization (Invariant 5), and the
    decode-charge gating of Invariant 13 (a forced re-decode after reader
    input is charged in a managing mode, and never in `eager`; a programmatic
    `scrollTo` alone is not reader input).
  - `renderfeaturestest::parkingPreservesTheImageBox` — Invariant 1 (below-the-
    fold image is not parked until decoded; park/unpark leaves `scrollHeight`
    and the rendered box untouched).
  - `previewlifecycletest::loadsImageBesideTheDocument` — relative-image guard
    via a 2×1 PNG whose natural width distinguishes a real decode (2) from a
    failed (0) or placeholder (1) state.
  - `rendermemorytest::memoryStaysBoundedAcrossDocumentSwitches` — Invariants
    7-9: switching among three large documents with a shortened budget keeps
    total engine RSS within a couple of hundred MB of the baseline (an
    unrecycled run grows past a gigabyte), a fresh renderer process appears,
    and the preview stays functional.
  - `rendermemorytest::maintenanceRecycleFreesMemoryAndKeepsThePage` —
    Invariants 7/9: the discard+reload reclaims the churn, keeps the content,
    and restores the exact scroll position.
  - `rendermemorytest::imagesSurviveMaintenanceRecycle` — a relative image
    still decodes after a recycle (the fresh load re-applies the local-file
    guard) with no stranded failure state.
  - `rendermemorytest::idleTextChurnReclaimsInPlaceWithoutReload` — Invariants
    7/12: an idle open preview of a large document stays put; one edit crosses
    the budget and is reclaimed **in place** (a forced GC — the renderer
    process never changes, RSS actually comes down), then it settles. Before
    the redesign this reloaded the renderer, which was the intrusive
    self-refresh.
  - `rendermemorytest::imageDecodeResidueRecyclesOnlyWhenMinimized` —
    Invariants 7/13: decodes the *reader* caused (a wheel event + a scroll
    that pulls far-below-the-fold images in; flat charge via
    `KATEXDOWN_MEM_IMG_CHARGE_MB`) push the estimate over the budget; the
    **visible** page must not reload (the image residue is not GC-reclaimable),
    and the same page recycles as soon as `KATEXDOWN_MEM_MINIMIZED` reports the
    window minimized — with images decoding again on the fresh page and no
    stranded failure state.
  - `rendermemorytest::idleImageHeavyDocumentDoesNotRecycleInALoop` —
    Invariants 12/13 for images: an idle open document whose *own* decode set
    crosses the budget (2048×2048 photos, three of them charge the 8 MiB cap)
    stays on one renderer process and charges nothing (no GC, no reload).
    Before the fix a fresh renderer appeared every ~3 s.
  - `rendermemorytest::maintenanceRecycleKeepsTheReaderPlaceWithImages` —
    Invariants 7/9/16: an image-residue discard of an image document keeps the
    exact scroll position *and* the content under the viewport top (the
    geometry the recycled page knew survives into the fresh one), and the
    remembered boxes never leak into an export. Also the regression test for
    the discard-retry livelock (it failed ~1 run in 3 before
    `m_recycleDiscarded`).
  - `rendermemorytest::editorRendersAfterAScrollDoNotRechargeThePageOwnDecodes`
    — Invariant 13's re-render reset: after one reader scroll, five ordinary
    edits (each a re-render that rebuilds the images around the viewport) must
    not charge a single extra decode, and the renderer must not be reloaded.
    Before the reset the charged set grew 7 → 42 and the estimate crossed the
    budget with no reader action left to blame (the residual self-refresh).
  - `renderfeaturestest::rememberedImageBoxesShapeAFreshReload` — the page side
    of Invariant 16: `__kdxRecycleState()` reports the boxes the page recorded,
    a fresh page lays an *undecoded* image out with the remembered box it was
    given, the author's own `width`/`height` win over the memory, and a real
    decode corrects a (deliberately wrong) remembered box — on the image that
    decoded, not on whichever image a loop visited last (the bad closure this
    test caught). Note for anyone extending it: Chromium loads a
    `loading="lazy"` image immediately when it is already in the memory cache,
    so "an image that never decodes" needs a src the page has not fetched yet.
  - `envflagstest` (no web engine) — Invariant 15 and the delivery rules of
    the renderer JS flags: the flags are appended when the env is empty or
    carries non-js-flags, `--expose-gc` is always included (even with the cap
    off), an explicit `--js-flags` in the env always wins, and
    `KATEXDOWN_V8_CAP_MB` overrides the stored cap while an unparsable override
    falls back to it.
- Tests run headless: `QT_QPA_PLATFORM=offscreen`, `--disable-gpu
  --no-sandbox`, and they show + resize the preview because IntersectionObserver
  callbacks are driven by compositor frames.

## Where it lives

- Lifecycle: `src/previewwidget.{h,cpp}` (`panelClosed`/`panelOpened`/
  `idleTick`, lifecycle-state callback), `src/pluginview.{h,cpp}` (mode
  decisions), `src/settings.{h,cpp}` (the two mode enums).
- Engine gating: `src/previewwidget.cpp` (`enginesForText`, `buildHtml(int)`,
  the rebuild checks in `render()`/`exportToFile()`); fence-size cap:
  `data/js/preview.js` (the `highlight` option of the markdown-it instance,
  `kdxHljsMaxChars`).
- Image policy: `data/js/preview.js` (image section: `armImages`, `swapOut`,
  `swapIn`, `recordDims`, `onImgLoad/onImgError`, `__setImageMode`,
  `__serializedHtml`), `data/css/base.css` (the `data-pv-imgmode` height rule).
- Remembered image geometry: `data/js/preview.js` (`imageGeometry`,
  `applyRememberedDims`, `__setImageDims`, `__kdxRecycleState`, the
  `data-kdx-remembered` branch of `recordDims`), `src/previewwidget.{h,cpp}`
  (`m_imageDims`, the capture in `performMemoryRecycle`, `applyImageDims` from
  `loadFinished`, the clear in `attachDocument`).
- Renderer-memory maintenance: `src/previewwidget.{h,cpp}` (`idleTick` open-
  panel branch incl. the 1 Hz decode poll, `handleDecodeStats`, the in-place
  `reclaimInPlace`/`reclaimAtIdle` and `m_imgCharge`/`m_gcUnavailable`, the
  image-residue `performMemoryRecycle`/`beginRecycle`/`finalizeRecycle`/
  `abortRecycle` and `previewWindowMinimized`, the lifecycle-state handler's
  discard/frozen branches (resets `m_renderedOnce`, `m_imgCharge` and the
  decode bookkeeping), `noteRenderWork`/`noteActivity`/`recycleDue`/
  `recycleIdle`, `readMemTuning`, the `attachDocument` recycle + same-folder
  fast path), the first-render gate (`m_renderedOnce`) and the image-charge
  constants in `render()`/`handleDecodeStats`; the decode counter lives in
  `data/js/preview.js` (`__kdxDecodeStats`, gated on reader input and reset by
  `rerender()`); tests in `tests/rendermemorytest.cpp`.
- Renderer JS flags (`--expose-gc` + the V8 heap cap): `src/kdxchromiumflags.h`
  (`applyJsFlags` env merge + `effectiveV8HeapCapMb` override helper), applied
  from `src/plugin.cpp` (plugin ctor) and `src/previewwidget.cpp` (ctor,
  before its profile); setting + config UI in `src/settings.{h,cpp}`
  (`V8HeapCapMb`) and `src/configpage.{h,cpp}` ("JS memory cap", 0–1024 MB,
  default 64); tests in `tests/envflagstest.cpp`.
