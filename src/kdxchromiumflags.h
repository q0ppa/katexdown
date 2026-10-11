#pragma once

#include <QByteArray>
#include <QtGlobal>

/**
 * Chromium-side switches Katexdown wants applied to its QtWebEngine renderer.
 *
 * Qt WebEngine reads QTWEBENGINE_CHROMIUM_FLAGS exactly once, when the web
 * engine initializes inside this process (the first profile/page/view — which
 * in practice is the moment the first renderer process would spawn). There is
 * no runtime API, so the only way to influence the renderer's V8 is to make
 * sure the flag is in the environment before that happens. Callers therefore
 * run applyJsFlags() as early as possible:
 *
 *   - the plugin constructor (Kate loads plugins before it creates any tool
 *     view, so this is normally before any QWebEngine object exists), and
 *   - the PreviewWidget constructor, before it creates its own profile
 *     (covers direct use — the tests drive the widget without the plugin — and
 *     the lazy modes, where the preview — and with it the engine — may not
 *     start until the panel is first opened).
 *
 * Both calls are idempotent and best-effort: if anything else already touched
 * the web engine, or the user set QTWEBENGINE_CHROMIUM_FLAGS with --js-flags
 * of their own, the flag is not applied (see below).
 *
 * Why a cap at all (measured, Qt 6.11 / Chromium 140, see design/lazyrender.md
 * "JS heap guard"): a Chromium renderer never major-GCs on its own, so every
 * full-document re-render leaves the replaced DOM tree stranded — renderer RSS
 * grows by tens to hundreds of MB per render with no plateau. Capping the V8
 * old space (`--js-flags=--max-old-space-size=N`) makes V8 collect under
 * pressure, which turns unbounded growth into a plateau. The cap is invisible
 * while a document's per-render working set stays under it and only costs GC
 * time when a render genuinely exceeds it (very large or math-heavy documents).
 *
 * Why --expose-gc (measured, see the maintenance design): the cap bounds the
 * growth between collections but never hands the freed pages back. The
 * maintenance therefore also needs `window.gc()`, which an explicit forced GC
 * exposes in the page; calling it at an idle moment returns 400-600 MB in
 * place, repeatably, with no reload (the dominant DOM/V8 churn; decoded images
 * are the part a GC cannot return). --expose-gc is therefore always merged,
 * even when the cap is off, because it is the reclamation primitive itself.
 * If the user set their own --js-flags, neither is merged: window.gc() may be
 * missing, and the maintenance then simply does not reclaim in place — it must
 * never fall back to reloading a page the user is looking at.
 */
namespace kdxchromiumflags
{
// Merge the renderer JS flags into QTWEBENGINE_CHROMIUM_FLAGS.
//
// Both flags travel in one --js-flags switch (Chromium reads only the first
// --js-flags switch, so they have to be comma-joined):
//   - --expose-gc always: exposes window.gc(), the maintenance's in-place
//     reclamation primitive.
//   - --max-old-space-size=<cap> only when capMb > 0: bounds the V8 old space
//     between those GCs.
//
// Rules:
//   - the environment already contains "--js-flags" -> nothing. An explicit
//     --js-flags (e.g. a user's --js-flags=--expose-gc) means the user owns V8
//     tuning; appending a second --js-flags switch would be silently dropped by
//     Chromium (it reads the first one), so we must not pretend either flag is
//     active.
//   - capMb <= 0  -> append "--js-flags=--expose-gc".
//   - otherwise    -> append "--js-flags=--max-old-space-size=<cap>,--expose-gc"
//     (as one argument; QTWEBENGINE_CHROMIUM_FLAGS is space-split).
inline void applyJsFlags(int capMb)
{
    QByteArray flags = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
    if (flags.contains("--js-flags")) {
        return; // explicit V8 flags win
    }
    QByteArray add = QByteArrayLiteral("--js-flags=");
    if (capMb > 0) {
        add += QByteArrayLiteral("--max-old-space-size=");
        add += QByteArray::number(capMb);
        add += ',';
    }
    add += QByteArrayLiteral("--expose-gc");
    if (!flags.isEmpty()) {
        flags += ' ';
    }
    flags += add;
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", flags);
}

// The cap actually to apply: the stored setting, unless $KATEXDOWN_V8_CAP_MB
// overrides it (a non-negative integer; tests and scripts can force a cap, or
// disable it entirely with KATEXDOWN_V8_CAP_MB=0, without touching config).
// An unparsable value falls back to the configured cap.
inline int effectiveV8HeapCapMb(int configuredMb)
{
    const QByteArray env = qgetenv("KATEXDOWN_V8_CAP_MB");
    if (!env.isEmpty()) {
        bool ok = false;
        const int value = env.toInt(&ok);
        if (ok) {
            return qMax(0, value);
        }
    }
    return qMax(0, configuredMb);
}
} // namespace kdxchromiumflags
