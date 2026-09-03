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
 * run applyV8HeapCap() as early as possible:
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
 * full-document re-render leaves the replaced DOM tree stranded in the V8 old
 * space — renderer RSS grows by tens to hundreds of MB per render with no
 * plateau, and only a renderer restart returns it. Capping the V8 old space
 * (`--js-flags=--max-old-space-size=N`) makes V8 collect under pressure, which
 * turns that unbounded growth into a bounded plateau. The cap is invisible
 * while a document's per-render working set stays under it (typical docs) and
 * only costs GC time when a render genuinely exceeds it (very large or
 * math-heavy documents) — which is exactly when the unbounded growth would
 * otherwise be worst.
 */
namespace kdxchromiumflags
{
// Merge the V8 old-space cap into QTWEBENGINE_CHROMIUM_FLAGS.
//
// Rules:
//   - capMb <= 0  -> nothing (the stored setting "off" leaves Chromium's own
//                    default, i.e. effectively unbounded).
//   - the environment already contains "--js-flags" -> nothing. An explicit
//     --js-flags (e.g. a user's --js-flags=--expose-gc) means the user owns
//     V8 tuning; appending a second --js-flags switch would be silently
//     dropped by Chromium (it reads the first one), so we must not pretend
//     the cap is active.
//   - otherwise append "--js-flags=--max-old-space-size=<cap>" (as one
//     argument; QTWEBENGINE_CHROMIUM_FLAGS is space-split).
inline void applyV8HeapCap(int capMb)
{
    if (capMb <= 0) {
        return;
    }
    QByteArray flags = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
    if (flags.contains("--js-flags")) {
        return; // explicit V8 flags win
    }
    QByteArray add = QByteArrayLiteral("--js-flags=--max-old-space-size=");
    add += QByteArray::number(capMb);
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
