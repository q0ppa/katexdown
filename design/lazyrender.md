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
- **False negatives are self-healing:** a page that lacks an engine the text
  grows into is rebuilt *once* — `render()` (and `exportToFile()`) detect
  `enginesForText(m_text) & ~m_pageEngines != 0` and call `loadPage()`, a full
  `setHtml()` whose `loadFinished` re-applies mode/theme/content. A fully
  equipped page skips the scan entirely; a rebuild is stable (the rebuilt page
  carries the engines the same text needs).
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
- Images with `srcset` are left to native lazy loading only (`data-kdx-skip`),
  and images that genuinely failed (`data-kdx-failed`) are never parked.
- **Export is unaffected by all of this:** `__serializedHtml()` serializes a
  detached clone with real srcs restored and all machinery attributes
  stripped, so `Export HTML…` always writes plain, eager images (see Pitfalls
  for why the live page must not be serialized directly).


### 4. Renderer-memory maintenance (`previewwidget.cpp` idleTick / performMemoryRecycle)

The levers above decide what enters a renderer. This one bounds what a
long-lived renderer *retains* — because nothing inside Chromium gives it back.
Measurements that shaped the design (offscreen test environment, text-only
documents):

- A renderer that stays alive never returns dead memory: switching among three
  ~100 kB documents grew one renderer by ~70 MB **per switch**, linearly, to
  2.2 GB in 30 switches, and 48 full re-renders of one document grew it to
  2.75 GB with no plateau and no self-triggered GC. (Pure JS allocation churn
  barely leaks — the growth comes from full-document re-rendering.)
- QtWebEngine refuses to freeze or discard a page it considers **visible**
  ("page is visible"), and a frozen page only runs Chromium's memory purge
  when it was *never shown* — so neither freeze nor discard can reclaim an
  open preview's renderer.
- Discard is the reliable primitive: when the page is told it is hidden
  (`QWebEnginePage::setVisible(false)`), Qt allows `Discarded`, the renderer
  process **exits and returns every byte**, and a subsequent load starts a
  fresh process at the ~180 MB baseline.

The maintenance therefore **recycles the renderer** (lets it die and loads a
fresh page from the mirrored document state) instead of trying to purge it.
Two trigger points, both invisible by construction:

- **At a document switch** (`attachDocument`): a switch already reloads the
  page, so replacing that reload with discard + fresh load costs the user
  nothing extra — the switch just lands on a clean renderer. This is what
  bounds tab-switching sessions.
- **At an idle moment on the same document** (`idleTick`, once the estimated
  dead memory passes the budget and the page has been quiet for a while, not
  focused, visible): the page is recycled in place with the scroll position
  restored after the fresh load, so a paused reader sees nothing change.

The trigger is an **estimate**, not an RSS measurement (portable): every full
page load adds a fixed cost (re-executing the inlined engines), every
full-document markdown push that *replaces already-rendered content* adds
`text size × factor`, and every real image decode adds a charge (see below),
with the factors calibrated from the measurements above (defaults in
`readMemTuning()`; tunable per process via `KATEXDOWN_MEM_BUDGET_MB`,
`KATEXDOWN_MEM_IDLE_MS`, `KATEXDOWN_MEM_MAX_AGE_MS`, `KATEXDOWN_MEM_OFF=1`, and
for tests `KATEXDOWN_MEM_IMG_CHARGE_MB`). The estimate over- rather than
under-counts, so real memory stays under the budget even where the real leak
ratio is higher than measured. A discard (or a closed-panel freeze, which
*is* a purge) resets the estimate — that is the "since the last purge" book-
keeping. A 5 s safety timer aborts any recycle that does not complete, so the
preview can never stay hidden.

**A page's first render is not charged.** The dead memory the estimate counts
is what a render leaves behind when it *replaces* content that was already on
the page. The very first render of a page — first open, or the fresh page of
a maintenance recycle — replaces nothing, so charging it text × factor would
keep any document whose text charge exceeds the budget permanently "due";
the open preview would then recycle itself in an endless loop (measured:
a fresh renderer process every ~7 s for an idle 1 MB text document).
`m_renderedOnce` (reset when a discard kills the renderer) is the gate.

**Image decodes are charged too.** Scrolling past an image decodes it (at
viewport quality) and the renderer keeps a share of that decoded frame it
will never return on its own — measured at ~8-10 MB per photo scrolled past,
in *every* image mode, sticky over 50+ s of idle, invisible to a text-only
estimate because it arrives without any render or navigation. The page
counts every real decode (`window.__kdxDecodeStats` in preview.js — a load
with `naturalWidth > 1`, so the 1x1 parking placeholder and failed loads
excluded) and the host polls it once per second while an image-bearing
document is open (`idleTick`/`handleDecodeStats`, gated by a cheap `![`/`<img`
text scan). Each new decode is charged min(intrinsic bytes, 8 MiB) — capped
so a grid of small images stays cheap — or a flat amount when
`KATEXDOWN_MEM_IMG_CHARGE_MB` is set. A photo-browsing session therefore
recycles at the next quiet moment instead of accreting without bound.

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
7. **Reclaiming an open preview's renderer means letting the process die.** Qt
   refuses `Frozen`/`Discarded` on a visible page; freeze purges only
   never-shown pages. The maintenance recycle therefore hides the page
   (`setVisible(false)`), discards it, and loads a fresh page from the
   mirrored document state — every recycle is a full discard-and-reload, and
   nothing else may be substituted for it.
8. **A recycle is a reload, so it must obey the load pipeline invariants.** It
   only starts when the page is loaded/active and the panel is open; the fresh
   load clears and re-sets `m_loaded` through the normal `loadFinished`
   pipeline, which re-applies theme, image mode, content and outline from the
   mirrored state (`m_text`/`m_doc`). `render()` and friends never push into a
   page that is mid-recycle; they wait for the pipeline like after any load.
9. **A recycle is only ever user-invisible.** It happens either as part of a
   document switch (which reloads anyway) or after the preview has been quiet
   (no text change, input or pending work) for the idle window, is visible,
   and is not focused; the same-document variant restores the scroll position
   after the fresh load. A safety timer aborts any recycle that gets stuck, so
   the preview can never remain hidden.
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
    recycle the open preview in an endless loop. `m_renderedOnce` is the gate,
    reset together with the estimate when a discard kills the renderer.
13. **Every real image decode is charged exactly once.** The page counts a
    decode as an `<img>` load with `naturalWidth > 1` (the 1×1 parking
    placeholder and failed loads never count), and the host polls the counter
    once per second while the open document can contain images; the delta is
    charged against the estimate, so an image-heavy reading session recycles
    instead of accreting decoded frames the renderer never returns.

## Pitfalls (each already cost a debugging session)

- **Image browsing accretes memory the text estimate cannot see.** Scrolling
  past a photo decodes it, and Chromium keeps a share of every decoded frame:
  measured ~8-10 MB per photo scrolled past, in every image mode, sticky for
  50+ s of idle, and produced without any render or navigation — so a
  text-only estimator never fires. The decode poll (Invariant 13) exists for
  this; do not "fix" it by removing the charge (back to unbounded accretion)
  or by parking harder (parking already caps the *simultaneously decoded* set;
  the retention is the renderer's, not the `<img>` elements').

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
- **A giant fence is a span factory.** Tokenizing turns every token into a
  `<span>`; a single pasted minified bundle can mean tens of thousands of DOM
  elements, re-built on every keystroke. The fence-size cap above is what
  stops it — do not "fix" this by silently dropping oversized fences (the
  text must always render) or by removing the cap.
- **"The renderer never returns memory" is the default, not a bug report.**
  QtWebEngine keeps a long-lived renderer's dead memory forever (measured:
  tens of MB per full re-render of a large document, unbounded growth, no
  self-limiting GC). `performance.memory` is useless for tracking this — its
  numbers only update when a GC runs, which is exactly what never happens.
  Renderer RSS (the process' resident set) is the only honest signal.
- **Visible pages cannot be frozen or discarded, and "hidden" is Qt's call.**
  `setLifecycleState(Frozen/Discarded)` on a shown page logs "page is visible"
  and does nothing. Telling the *page* it is hidden
  (`QWebEnginePage::setVisible(false)`, which leaves the widget on screen
  showing its last frame) is what makes the discard legal. Early prototypes
  froze unshown widgets and "proved" freeze purges memory — that purge only
  runs for pages that were never shown, so it must not be relied on for an
  open panel.
- **A discard that is part of a recycle must load the mirrored document, not
  wait for the committed URL to reload.** Loading the old document first and
  then replacing it doubles the work and can apply stale engines/guard roots.
  `setHtml` on a discarded page restores it and navigates straight to the new
  content (verified empirically), which is what the recycle's discard handler
  does.
- **An idle recycle must not run while the user is touching the preview.** The
  quiet gate watches text changes, input events, focus and pending work; a
  same-document recycle restores the scroll *after* the fresh render. Without
  the scroll restore a reload would visibly jump a reading user to the top.
- **The recycle estimate must reset when the memory is actually gone.** A
  discard (process death) and a closed-panel freeze (which *does* purge)
  both reset the estimate; otherwise the budget would fire immediately after
  the very recycle that just cleaned the renderer.

## Test seams

- Env hooks (named like the existing `KATEXDOWN_DEBUG`):
  `KATEXDOWN_IDLE_DISCARD_MS` shortens the LazyKeep discard delay;
  `KATEXDOWN_DATA_DIR` points at a fake asset dir (used to fake KaTeX assets
  so engine-gating tests need no downloaded data);
  `KATEXDOWN_MEM_BUDGET_MB`/`KATEXDOWN_MEM_IDLE_MS`/`KATEXDOWN_MEM_MAX_AGE_MS`
  shorten the recycle budget and quiet window, `KATEXDOWN_MEM_IMG_CHARGE_MB`
  sets a flat per-decode image charge (deterministic for tests), and
  `KATEXDOWN_MEM_OFF=1`
  disables the policy ticker so tests can drive `performMemoryRecycle()`
  directly. RSS is measured in tests by walking `/proc` for the test process'
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
  - `renderfeaturestest::oversizedFenceRendersPlain` — Invariant 11: the cap
    is read from `window.kdxHljsMaxChars` (the page is the single source of
    truth); an oversized fence keeps its exact text, its `<pre>` box and its
    HTML escaping but renders zero token spans, while a normal fence in the
    same document still highlights.
  - `renderfeaturestest::imageModesControlDecoding` — the three modes, the
    park/restore cycle, and export normalization (Invariant 5).
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
    Invariant 9: a same-document recycle reclaims hundreds of MB, keeps the
    content, and restores the exact scroll position.
  - `rendermemorytest::imagesSurviveMaintenanceRecycle` — a relative image
    still decodes after a recycle (the fresh load re-applies the local-file
    guard) with no stranded failure state.
  - `rendermemorytest::idleOpenDocumentDoesNotRecycleInALoop` — Invariant 12:
    an idle open preview of a document whose text charge exceeds the budget
    stays put (no churn, no loop); one edit recycles exactly once and the
    fresh page settles again.
  - `rendermemorytest::imageDecodesChargeTheRecycleBudget` — Invariant 13:
    real image decodes (flat charge via `KATEXDOWN_MEM_IMG_CHARGE_MB`) push
    the estimate over the budget and trigger the recycle, and images decode
    again on the fresh page without a stranded failure state.
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
- Renderer-memory maintenance: `src/previewwidget.{h,cpp}` (`idleTick` open-
  panel branch incl. the 1 Hz decode poll, `handleDecodeStats`,
  `performMemoryRecycle`/`beginRecycle`/`finalizeRecycle`/
  `abortRecycle`, the lifecycle-state handler's discard/frozen branches
  (resets `m_renderedOnce` and the decode bookkeeping),
  `noteRenderWork`/`noteActivity`/`recycleDue`/`recycleIdle`,
  `readMemTuning`, the `attachDocument` recycle + same-folder fast path),
  the first-render gate (`m_renderedOnce`) and the image-charge constants in
  `render()`/`handleDecodeStats`; the decode counter lives in
  `data/js/preview.js` (`__kdxDecodeStats`);
  tests in `tests/rendermemorytest.cpp`.
