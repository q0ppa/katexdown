// Unit tests for the Chromium-flag plumbing (src/kdxchromiumflags.h) that
// carries the configured V8 heap cap and the --expose-gc flag (the maintenance's
// in-place reclamation primitive) into QTWEBENGINE_CHROMIUM_FLAGS. Pure
// environment logic — deliberately no QtWebEngine anywhere in this binary, so
// it runs instantly and cannot perturb engine state.
//
// Pins: the flags are appended when the env is empty or carries non-js-flags;
// --expose-gc is included even when the cap is off; an explicit --js-flags in
// the env always wins (a second --js-flags switch would be silently dropped by
// Chromium); KATEXDOWN_V8_CAP_MB=0 disables only the cap, not --expose-gc; the
// env override wins over the configured value and an unparsable override falls
// back to it.

#include "kdxchromiumflags.h"

#include <QByteArray>
#include <QTest>

class EnvFlagsTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void appliesWhenEnvEmpty();
    void appendsToExistingNonJsFlags();
    void offCapStillExposesGc();
    void leavesExplicitJsFlagsAlone();
    void envOverrideWins();
    void invalidEnvOverrideFallsBack();
};

static QByteArray env()
{
    return qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
}

void EnvFlagsTest::appliesWhenEnvEmpty()
{
    qunsetenv("QTWEBENGINE_CHROMIUM_FLAGS");
    kdxchromiumflags::applyJsFlags(128);
    QCOMPARE(env(), QByteArray("--js-flags=--max-old-space-size=128,--expose-gc"));
    qunsetenv("QTWEBENGINE_CHROMIUM_FLAGS");
}

void EnvFlagsTest::appendsToExistingNonJsFlags()
{
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--disable-gpu --no-sandbox");
    kdxchromiumflags::applyJsFlags(128);
    QCOMPARE(env(), QByteArray("--disable-gpu --no-sandbox --js-flags=--max-old-space-size=128,--expose-gc"));
    qunsetenv("QTWEBENGINE_CHROMIUM_FLAGS");
}

void EnvFlagsTest::offCapStillExposesGc()
{
    // 0 (the setting's "off") drops only the V8 cap. --expose-gc is the
    // maintenance's reclamation primitive, so it is merged regardless.
    qunsetenv("QTWEBENGINE_CHROMIUM_FLAGS");
    kdxchromiumflags::applyJsFlags(0);
    QCOMPARE(env(), QByteArray("--js-flags=--expose-gc"));
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--disable-gpu");
    kdxchromiumflags::applyJsFlags(-1);
    QCOMPARE(env(), QByteArray("--disable-gpu --js-flags=--expose-gc"));
    qunsetenv("QTWEBENGINE_CHROMIUM_FLAGS");
}

void EnvFlagsTest::leavesExplicitJsFlagsAlone()
{
    // Any --js-flags means the user owns V8 tuning; appending a second
    // --js-flags switch would be dropped by Chromium (first one wins), so the
    // helper must not pretend the cap or --expose-gc is active.
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--js-flags=--expose-gc");
    kdxchromiumflags::applyJsFlags(128);
    QCOMPARE(env(), QByteArray("--js-flags=--expose-gc"));
    // Same for a user-set cap: theirs stands, ours never doubles it.
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--disable-gpu --js-flags=--max-old-space-size=512");
    kdxchromiumflags::applyJsFlags(128);
    QCOMPARE(env(), QByteArray("--disable-gpu --js-flags=--max-old-space-size=512"));
    qunsetenv("QTWEBENGINE_CHROMIUM_FLAGS");
}

void EnvFlagsTest::envOverrideWins()
{
    qputenv("KATEXDOWN_V8_CAP_MB", "64");
    QCOMPARE(kdxchromiumflags::effectiveV8HeapCapMb(128), 64);
    qputenv("KATEXDOWN_V8_CAP_MB", "0");
    QCOMPARE(kdxchromiumflags::effectiveV8HeapCapMb(128), 0);
    qunsetenv("KATEXDOWN_V8_CAP_MB");
    QCOMPARE(kdxchromiumflags::effectiveV8HeapCapMb(128), 128);
    QCOMPARE(kdxchromiumflags::effectiveV8HeapCapMb(-5), 0);
}

void EnvFlagsTest::invalidEnvOverrideFallsBack()
{
    qputenv("KATEXDOWN_V8_CAP_MB", "banana");
    QCOMPARE(kdxchromiumflags::effectiveV8HeapCapMb(128), 128);
    qunsetenv("KATEXDOWN_V8_CAP_MB");
}

QTEST_GUILESS_MAIN(EnvFlagsTest)
#include "envflagstest.moc"
