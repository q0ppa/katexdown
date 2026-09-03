// Unit tests for the Chromium-flag plumbing (src/kdxchromiumflags.h) that
// carries the configured V8 heap cap into QTWEBENGINE_CHROMIUM_FLAGS. Pure
// environment logic — deliberately no QtWebEngine anywhere in this binary, so
// it runs instantly and cannot perturb engine state.
//
// Pins: the cap is appended when the env is empty or carries non-js-flags;
// an explicit --js-flags in the env always wins (a second --js-flags switch
// would be silently dropped by Chromium); cap <= 0 and KATEXDOWN_V8_CAP_MB=0
// mean "off" and must leave the env untouched; the env override wins over the
// configured value and an unparsable override falls back to it.

#include "kdxchromiumflags.h"

#include <QByteArray>
#include <QTest>

class EnvFlagsTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void appliesWhenEnvEmpty();
    void appendsToExistingNonJsFlags();
    void leavesExplicitJsFlagsAlone();
    void offCapLeavesEnvAlone();
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
    kdxchromiumflags::applyV8HeapCap(128);
    QCOMPARE(env(), QByteArray("--js-flags=--max-old-space-size=128"));
    qunsetenv("QTWEBENGINE_CHROMIUM_FLAGS");
}

void EnvFlagsTest::appendsToExistingNonJsFlags()
{
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--disable-gpu --no-sandbox");
    kdxchromiumflags::applyV8HeapCap(128);
    QCOMPARE(env(), QByteArray("--disable-gpu --no-sandbox --js-flags=--max-old-space-size=128"));
    qunsetenv("QTWEBENGINE_CHROMIUM_FLAGS");
}

void EnvFlagsTest::leavesExplicitJsFlagsAlone()
{
    // Any --js-flags means the user owns V8 tuning; appending a second
    // --js-flags switch would be dropped by Chromium (first one wins), so the
    // helper must not pretend the cap is active.
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--js-flags=--expose-gc");
    kdxchromiumflags::applyV8HeapCap(128);
    QCOMPARE(env(), QByteArray("--js-flags=--expose-gc"));
    // Same for a user-set cap: theirs stands, ours never doubles it.
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--disable-gpu --js-flags=--max-old-space-size=512");
    kdxchromiumflags::applyV8HeapCap(128);
    QCOMPARE(env(), QByteArray("--disable-gpu --js-flags=--max-old-space-size=512"));
    qunsetenv("QTWEBENGINE_CHROMIUM_FLAGS");
}

void EnvFlagsTest::offCapLeavesEnvAlone()
{
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--disable-gpu");
    kdxchromiumflags::applyV8HeapCap(0);
    kdxchromiumflags::applyV8HeapCap(-1);
    QCOMPARE(env(), QByteArray("--disable-gpu"));
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
