// Pins the renderer-memory maintenance (see lazyrender.md, "renderer-memory
// maintenance"): a long-lived QWebEngineView renderer never reclaims the dead
// memory it accumulates while rendering document after document — measured at
// ~0.5-0.9 kB per byte of markdown pushed, per full render, with no natural
// plateau — and Qt refuses to freeze or discard a page it considers visible.
// The only reliable reclamation is letting the renderer process die, which the
// maintenance recycle does by telling the page it is hidden, discarding it,
// and loading a fresh page from the mirrored document state.
//
// These tests drive the real widget:
//   - switching between documents with the (env-shortened) maintenance budget
//     keeps the renderer processes' total RSS bounded, where an unrecycled run
//     grows by hundreds of MB per round;
//   - a same-document maintenance recycle returns memory to the fresh-renderer
//     baseline while keeping the content and the scroll position;
//   - images survive a recycle (the reload re-applies the same local-file
//     guard and the images decode again, with no stranded failure state).
//
// RSS is read from /proc (Linux, which is what CI runs); on other platforms
// the RSS-based tests skip and only the image test runs.

#include "previewwidget.h"
#include "settings.h"

#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <QWebEnginePage>
#include <QWebEngineView>

#include <unistd.h>

#include <memory>

#include <KTextEditor/Document>
#include <KTextEditor/Editor>

namespace
{
constexpr int ImageWidth = 2;
// A 2x1 red PNG, inline so the test carries no fixture file. Two pixels wide because a
// failed load reports naturalWidth 0 and a placeholder reports 1.
const QByteArray RedPng =
    QByteArray::fromHex("89504e470d0a1a0a0000000d494844520000000200000001080200000"
                        "07b40e8dd0000000d4944415478da63f8cfc000440008fe01ff19c06be"
                        "70000000049454e44ae426082");

// ---- Renderer RSS plumbing (Linux /proc) ----

bool procAvailable()
{
    return QFile::exists(QStringLiteral("/proc/self/statm"));
}

QString procComm(long pid)
{
    QFile f(QStringLiteral("/proc/%1/comm").arg(pid));
    if (!f.open(QIODevice::ReadOnly)) {
        return QString();
    }
    return QString::fromLatin1(f.readAll().trimmed());
}

long procRssKb(long pid)
{
    QFile f(QStringLiteral("/proc/%1/statm").arg(pid));
    if (!f.open(QIODevice::ReadOnly)) {
        return -1;
    }
    const QList<QByteArray> parts = f.readAll().trimmed().split(' ');
    if (parts.size() < 2) {
        return -1;
    }
    return parts[1].toLong() * (sysconf(_SC_PAGESIZE) / 1024);
}

void collectChildren(long pid, QSet<long> &out)
{
    QFile f(QStringLiteral("/proc/%1/task/%1/children").arg(pid));
    if (!f.open(QIODevice::ReadOnly)) {
        return;
    }
    const QList<QByteArray> parts = f.readAll().split(' ');
    for (const QByteArray &p : parts) {
        bool ok = false;
        const long c = p.toLong(&ok);
        if (ok && c > 1 && !out.contains(c)) {
            out.insert(c);
            collectChildren(c, out);
        }
    }
}

// (pid, rssKb) of every descendant QtWebEngine process.
QList<QPair<long, long>> engineChildren()
{
    QList<QPair<long, long>> out;
    QSet<long> kids;
    collectChildren(QCoreApplication::applicationPid(), kids);
    for (long pid : kids) {
        if (procComm(pid).contains(QLatin1String("WebEngine"), Qt::CaseInsensitive)) {
            out.append({pid, procRssKb(pid)});
        }
    }
    return out;
}

long totalEngineKb()
{
    long total = 0;
    const auto children = engineChildren();
    for (const auto &c : children) {
        if (c.second > 0) {
            total += c.second;
        }
    }
    return total;
}

// ---- page helpers (same shape as the other preview tests) ----

QString evalJs(PreviewWidget *preview, const QString &code)
{
    auto *view = preview->findChild<QWebEngineView *>();
    if (!view) {
        return QString();
    }
    auto slot = std::make_shared<std::pair<QString, bool>>(QString(), false);
    view->page()->runJavaScript(code, [slot](const QVariant &result) {
        slot->first = result.toString();
        slot->second = true;
    });
    QDeadlineTimer deadline(6000);
    while (!slot->second && !deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return slot->first;
}

QString pageText(PreviewWidget *preview)
{
    return evalJs(preview, QStringLiteral("document.getElementById('content').innerText"));
}

bool waitForText(PreviewWidget *preview, QLatin1String needle)
{
    QDeadlineTimer deadline(30000);
    while (!deadline.hasExpired()) {
        if (pageText(preview).contains(needle)) {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    return false;
}

int imageWidth(PreviewWidget *preview)
{
    return evalJs(preview, QStringLiteral("document.images.length ? document.images[0].naturalWidth : 0")).toInt();
}

bool waitForImageWidth(PreviewWidget *preview, int width)
{
    QDeadlineTimer deadline(20000);
    while (!deadline.hasExpired()) {
        if (imageWidth(preview) == width) {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    return false;
}

// A text-only markdown document large enough that a render measurably dirties
// the renderer (~90 kB of source).
QString bigText(const QString &tag)
{
    QString s;
    s += QStringLiteral("# %1\n\n").arg(tag);
    for (int i = 0; i < 350; ++i) {
        s += QStringLiteral("## Section %1 of %2\n\n").arg(i).arg(tag);
        s += QStringLiteral("Some **bold** and *italic* text with `inline code`, a [link](https://example.com/%1/%2), and a list:\n\n").arg(tag).arg(i);
        s += QStringLiteral("- item one\n- item two\n- item three\n\n");
        s += QStringLiteral("> a quoted line with more words to fill the paragraph out\n\n");
        s += QStringLiteral("| a | b |\n|---|---|\n| 1 | 2 |\n\n");
    }
    return s;
}
} // namespace

class RenderMemoryTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void memoryStaysBoundedAcrossDocumentSwitches();
    void maintenanceRecycleFreesMemoryAndKeepsThePage();
    void imagesSurviveMaintenanceRecycle();
    void idleOpenDocumentDoesNotRecycleInALoop();
    void imageDecodesChargeTheRecycleBudget();
};

// Switching among several markdown tabs must not grow the renderer without
// bound: the maintenance recycle resets it (a fresh renderer process appears)
// and total engine RSS stays near the baseline instead of climbing by hundreds
// of MB per round. KATEXDOWN_MEM_BUDGET_MB is shortened so the budget is
// crossed every round or two of large documents.
void RenderMemoryTest::memoryStaysBoundedAcrossDocumentSwitches()
{
    if (!procAvailable()) {
        QSKIP("renderer RSS is read from /proc (Linux only)");
    }
    qputenv("KATEXDOWN_MEM_BUDGET_MB", "32");
    qputenv("KATEXDOWN_MEM_IDLE_MS", "1000");
    qunsetenv("KATEXDOWN_MEM_OFF");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QStringList paths;
    QStringList tags;
    for (int d = 0; d < 3; ++d) {
        const QString path = dir.filePath(QStringLiteral("doc%1.md").arg(d));
        const QString tag = QStringLiteral("alpha%1").arg(d);
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QVERIFY(f.write(bigText(tag).toUtf8()) > 0);
        f.close();
        paths << path;
        tags << tag;
    }

    QList<KTextEditor::Document *> docs;
    for (const QString &p : paths) {
        KTextEditor::Document *doc = KTextEditor::Editor::instance()->createDocument(nullptr);
        QVERIFY(doc->openUrl(QUrl::fromLocalFile(p)));
        QTRY_VERIFY(doc->text().size() > 1000);
        docs << doc;
    }

    auto preview = std::make_unique<PreviewWidget>(nullptr, nullptr, docs[0]);
    preview->resize(700, 500);
    preview->show(); // like kate's open panel
    QVERIFY(waitForText(preview.get(), QLatin1String("alpha0")));
    QTest::qWait(2000); // first load + settle

    const auto rendererPids = [&]() {
        QSet<long> pids;
        for (const auto &c : engineChildren()) {
            pids.insert(c.first);
        }
        return pids;
    };

    const long baselineTotal = totalEngineKb();
    QVERIFY2(baselineTotal > 100 * 1024, qPrintable(QStringLiteral("no engine processes measured, total=%1 kB").arg(baselineTotal)));
    const QSet<long> baselinePids = rendererPids();

    // A few rounds of switching, each round visiting all three documents.
    QSet<long> seenPids = baselinePids;
    for (int round = 1; round <= 5; ++round) {
        for (int d = 0; d < 3; ++d) {
            preview->attachDocument(docs[d], nullptr);
            QVERIFY2(waitForText(preview.get(), QLatin1String(tags.at(d).toLatin1().constData())),
                     qPrintable(QStringLiteral("round %1 doc %2 did not render").arg(round).arg(d)));
        }
        QTest::qWait(2000); // let any pending maintenance settle
        const auto pids = rendererPids();
        seenPids.unite(pids);
    }

    // The maintenance ran at least once: a *new* renderer process served the
    // preview by the end of the run (the widget keeps working throughout).
    const long finalTotal = totalEngineKb();
    qInfo("baseline engine RSS %ld kB, final engine RSS %ld kB", baselineTotal, finalTotal);
    QVERIFY2(seenPids.size() > baselinePids.size(),
             "no renderer process was ever recycled during the switching run");
    // Bounded: an unrecycled run of this size grows to well over a gigabyte
    // of engine RSS; recycling keeps it within a couple of hundred MB of the
    // baseline. The bound is deliberately generous for slow CI machines.
    QVERIFY2(finalTotal < baselineTotal + 700 * 1024,
             qPrintable(QStringLiteral("engine RSS grew unbounded: baseline %1 kB -> final %2 kB").arg(baselineTotal).arg(finalTotal)));

    // The preview is still fully functional: re-attach and edit.
    preview->attachDocument(docs[0], nullptr);
    QVERIFY(waitForText(preview.get(), QLatin1String("alpha0")));
    docs[0]->setText(bigText(QStringLiteral("alpha0-final")));
    QVERIFY(waitForText(preview.get(), QLatin1String("alpha0-final")));

    for (KTextEditor::Document *d : docs) {
        delete d;
    }
}

// A long same-document session (edits re-render the whole document, which
// dirties the renderer each time) is reclaimed by the maintenance recycle, and
// the recycle keeps the reader's place: content and scroll survive.
void RenderMemoryTest::maintenanceRecycleFreesMemoryAndKeepsThePage()
{
    if (!procAvailable()) {
        QSKIP("renderer RSS is read from /proc (Linux only)");
    }
    qputenv("KATEXDOWN_MEM_OFF", "1"); // drive the recycle API directly, no policy ticker

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("doc.md"));
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QVERIFY(f.write(bigText(QStringLiteral("alpha0")).toUtf8()) > 0);
    f.close();
    KTextEditor::Document *doc = KTextEditor::Editor::instance()->createDocument(nullptr);
    QVERIFY(doc->openUrl(QUrl::fromLocalFile(path)));
    QTRY_VERIFY(doc->text().size() > 1000);

    auto preview = std::make_unique<PreviewWidget>(nullptr, nullptr, doc);
    preview->resize(700, 500);
    preview->show();
    QVERIFY(waitForText(preview.get(), QLatin1String("alpha0")));

    // Churn: repeated full-document re-renders pile up dead renderer memory.
    for (int i = 0; i < 7; ++i) {
        doc->setText(bigText(QStringLiteral("alpha0-ed%1").arg(i)));
        QVERIFY(waitForText(preview.get(), QLatin1String(QStringLiteral("alpha0-ed%1").arg(i).toLatin1().constData())));
    }
    QTest::qWait(800);
    const long beforeKb = totalEngineKb();
    QVERIFY2(beforeKb > 300 * 1024, qPrintable(QStringLiteral("churn did not build up memory, total=%1 kB").arg(beforeKb)));

    // Scroll into the document, then recycle in place.
    evalJs(preview.get(), QStringLiteral("window.scrollTo(0, 1200);"));
    QTest::qWait(300);
    const double scrollBefore = evalJs(preview.get(), QStringLiteral("String(window.scrollY)")).toDouble();
    QVERIFY(scrollBefore > 500);

    QVERIFY2(preview->performMemoryRecycle(true), "maintenance recycle refused to start");
    QVERIFY(waitForText(preview.get(), QLatin1String("alpha0-ed6"))); // content returns via the fresh page
    QTest::qWait(1500); // let the post-load scroll restore land

    const long afterKb = totalEngineKb();
    qInfo("engine RSS before recycle %ld kB, after %ld kB", beforeKb, afterKb);
    // Two static helper processes (~140 MB) never die; the renderer itself is
    // back at its fresh-process baseline (~180 MB), so the total lands around
    // 320 MB no matter how much dead memory was reclaimed.
    QVERIFY2(afterKb < beforeKb - 150 * 1024 && afterKb < 400 * 1024,
             qPrintable(QStringLiteral("recycle did not reclaim memory: %1 kB -> %2 kB").arg(beforeKb).arg(afterKb)));

    const double scrollAfter = evalJs(preview.get(), QStringLiteral("String(window.scrollY)")).toDouble();
    QVERIFY2(qAbs(scrollAfter - scrollBefore) < 2.0,
             qPrintable(QStringLiteral("scroll position lost across recycle: %1 -> %2").arg(scrollBefore).arg(scrollAfter)));

    // Still live: one more edit renders.
    doc->setText(bigText(QStringLiteral("alpha0-post")));
    QVERIFY(waitForText(preview.get(), QLatin1String("alpha0-post")));
    delete doc;
}

// A relative image must keep working across a recycle: the fresh page is
// loaded under the same local-file guard, the image decodes again, and no
// stranded "failed" state survives.
void RenderMemoryTest::imagesSurviveMaintenanceRecycle()
{
    qputenv("KATEXDOWN_MEM_OFF", "1");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QFile png(dir.filePath(QStringLiteral("red.png")));
    QVERIFY(png.open(QIODevice::WriteOnly));
    QCOMPARE(png.write(RedPng), RedPng.size());
    png.close();

    const QString path = dir.filePath(QStringLiteral("img.md"));
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QVERIFY(f.write(QStringLiteral("# images\n\n![red](red.png)\n\nsome body.\n").toUtf8()) > 0);
    f.close();
    KTextEditor::Document *doc = KTextEditor::Editor::instance()->createDocument(nullptr);
    QVERIFY(doc->openUrl(QUrl::fromLocalFile(path)));
    QTRY_VERIFY(doc->text().size() > 0);

    auto preview = std::make_unique<PreviewWidget>(nullptr, nullptr, doc);
    preview->resize(700, 500);
    preview->show();
    QVERIFY(waitForText(preview.get(), QLatin1String("images")));
    QVERIFY(waitForImageWidth(preview.get(), ImageWidth));

    QVERIFY(preview->performMemoryRecycle(true));
    QVERIFY(waitForText(preview.get(), QLatin1String("images")));
    QVERIFY2(waitForImageWidth(preview.get(), ImageWidth),
             qPrintable(QStringLiteral("image did not decode again after the recycle, naturalWidth=%1").arg(imageWidth(preview.get()))));

    const QString failed = evalJs(preview.get(), QStringLiteral("document.images[0].dataset.kdxFailed || 'ok'"));
    QCOMPARE(failed, QStringLiteral("ok"));

    delete doc;
}

// The maintenance estimate counts the dead memory a render leaves behind when
// it *replaces* already-rendered content. A fresh page's own first render
// replaces nothing, so it must not be charged — otherwise any document whose
// text charge exceeds the budget would be "due" forever and the open preview
// would recycle itself in an endless loop (~every 7 s, measured). This pins:
// first open of a large document stays put while idle; one edit recycles once;
// and the fresh page after that recycle stays put again (no loop).
void RenderMemoryTest::idleOpenDocumentDoesNotRecycleInALoop()
{
    if (!procAvailable()) {
        QSKIP("renderer RSS is read from /proc (Linux only)");
    }
    qputenv("KATEXDOWN_MEM_BUDGET_MB", "16"); // below the ~46 MB text charge of bigText
    qputenv("KATEXDOWN_MEM_IDLE_MS", "800");
    qunsetenv("KATEXDOWN_MEM_OFF");
    qunsetenv("KATEXDOWN_MEM_IMG_CHARGE_MB");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("doc.md"));
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QVERIFY(f.write(bigText(QStringLiteral("loop0")).toUtf8()) > 0);
    f.close();
    KTextEditor::Document *doc = KTextEditor::Editor::instance()->createDocument(nullptr);
    QVERIFY(doc->openUrl(QUrl::fromLocalFile(path)));
    QTRY_VERIFY(doc->text().size() > 1000);

    auto preview = std::make_unique<PreviewWidget>(nullptr, nullptr, doc);
    preview->resize(700, 500);
    preview->show();
    QVERIFY(waitForText(preview.get(), QLatin1String("loop0")));
    // Production-like: in Kate the editor holds focus, never the preview. The
    // offscreen test harness activates the shown window and focuses the web
    // view (and its internal render widget), so drop the focus again — the
    // maintenance policy refuses to recycle a focused preview.
    {
        auto *view = preview->findChild<QWebEngineView *>();
        view->clearFocus();
        if (view->focusProxy()) {
            view->focusProxy()->clearFocus();
        }
        QTRY_VERIFY_WITH_TIMEOUT(QApplication::focusWidget() == nullptr, 5000);
    }
    const QSet<long> baselinePids = [&]() {
        QSet<long> pids;
        for (const auto &c : engineChildren()) {
            pids.insert(c.first);
        }
        return pids;
    }();

    // Phase 1: an idle open preview of a large document must NOT recycle (the
    // first render is uncharged). The old code recycled roughly every 7 s here.
    QTest::qWait(12000);
    const QSet<long> pids1 = [&]() {
        QSet<long> pids;
        for (const auto &c : engineChildren()) {
            pids.insert(c.first);
        }
        return pids;
    }();
    QVERIFY2(pids1 == baselinePids,
             "idle open preview of a large document recycled without any churn (loop?)");

    // Phase 2: one real edit dirties the renderer (content replacement), the
    // budget is crossed, and the maintenance recycles exactly once...
    doc->setText(bigText(QStringLiteral("loop1")));
    QVERIFY(waitForText(preview.get(), QLatin1String("loop1")));
    bool recycled = false;
    {
        QDeadlineTimer deadline(20000);
        while (!deadline.hasExpired()) {
            const QSet<long> now = [&]() {
                QSet<long> pids;
                for (const auto &c : engineChildren()) {
                    pids.insert(c.first);
                }
                return pids;
            }();
            if (now != baselinePids) {
                recycled = true;
                break;
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        }
    }
    QVERIFY2(recycled, "the edit did not trigger the maintenance recycle");
    QVERIFY(waitForText(preview.get(), QLatin1String("loop1")));

    // ...and then it stops: the recycle's own fresh render is uncharged, so no
    // endless loop follows. The old code changed renderer pids again within a
    // few seconds of the previous recycle.
    const QSet<long> afterRecycle = [&]() {
        QSet<long> pids;
        for (const auto &c : engineChildren()) {
            pids.insert(c.first);
        }
        return pids;
    }();
    QTest::qWait(12000);
    const QSet<long> pids2 = [&]() {
        QSet<long> pids;
        for (const auto &c : engineChildren()) {
            pids.insert(c.first);
        }
        return pids;
    }();
    QVERIFY2(pids2 == afterRecycle,
             "the preview kept recycling after the maintenance cycle (estimate not reset?)");

    delete doc;
}

// Image decodes are invisible to the text-based estimate, yet Chromium keeps
// a share of every decoded frame it will never return (measured ~8-10 MB per
// photo scrolled past, in every image mode). The 1 Hz decode poll must charge
// those decodes so an image-heavy session recycles instead of accreting
// without bound. KATEXDOWN_MEM_IMG_CHARGE_MB makes the charge deterministic.
void RenderMemoryTest::imageDecodesChargeTheRecycleBudget()
{
    if (!procAvailable()) {
        QSKIP("renderer RSS is read from /proc (Linux only)");
    }
    qputenv("KATEXDOWN_MEM_BUDGET_MB", "16"); // 4 (fixed load) + 3 x 4 (flat charge) crosses it
    qputenv("KATEXDOWN_MEM_IDLE_MS", "800");
    qputenv("KATEXDOWN_MEM_IMG_CHARGE_MB", "4");
    qunsetenv("KATEXDOWN_MEM_OFF");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QString md = QStringLiteral("# imgs\n\n");
    for (int i = 0; i < 4; ++i) {
        QFile png(dir.filePath(QStringLiteral("r%1.png").arg(i)));
        QVERIFY(png.open(QIODevice::WriteOnly));
        QCOMPARE(png.write(RedPng), RedPng.size());
        png.close();
        md += QStringLiteral("![img %1](r%1.png)\n\n").arg(i);
    }
    md += QStringLiteral("some body.\n");
    const QString path = dir.filePath(QStringLiteral("doc.md"));
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QVERIFY(f.write(md.toUtf8()) > 0);
    f.close();
    KTextEditor::Document *doc = KTextEditor::Editor::instance()->createDocument(nullptr);
    QVERIFY(doc->openUrl(QUrl::fromLocalFile(path)));
    QTRY_VERIFY(doc->text().size() > 0);

    auto preview = std::make_unique<PreviewWidget>(nullptr, nullptr, doc);
    preview->resize(700, 500);
    preview->show();
    QVERIFY(waitForText(preview.get(), QLatin1String("imgs")));
    QVERIFY(waitForImageWidth(preview.get(), ImageWidth));
    {
        auto *view = preview->findChild<QWebEngineView *>();
        view->clearFocus();
        if (view->focusProxy()) {
            view->focusProxy()->clearFocus();
        }
        QTRY_VERIFY_WITH_TIMEOUT(QApplication::focusWidget() == nullptr, 5000);
    }

    // The images at the top decode right away; wait until the page reports at
    // least three real decodes (naturalWidth > 1, placeholders excluded).
    {
        QDeadlineTimer deadline(15000);
        bool counted = false;
        while (!deadline.hasExpired()) {
            const QString stats = evalJs(preview.get(), QStringLiteral("window.__kdxDecodeStats ? window.__kdxDecodeStats() : '0:0'"));
            const int colon = stats.indexOf(QLatin1Char(':'));
            if (colon > 0 && stats.left(colon).toInt() >= 3) {
                counted = true;
                break;
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        }
        QVERIFY2(counted, "the page never reported the image decodes");
    }

    // The decode charges push the estimate over the budget; the maintenance
    // recycles (a new renderer process appears) and the images decode again
    // on the fresh page, with no stranded failure state.
    const QSet<long> baselinePids = [&]() {
        QSet<long> pids;
        for (const auto &c : engineChildren()) {
            pids.insert(c.first);
        }
        return pids;
    }();
    {
        QDeadlineTimer deadline(20000);
        bool recycled = false;
        while (!deadline.hasExpired()) {
            const QSet<long> now = [&]() {
                QSet<long> pids;
                for (const auto &c : engineChildren()) {
                    pids.insert(c.first);
                }
                return pids;
            }();
            if (now != baselinePids) {
                recycled = true;
                break;
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        }
        QVERIFY2(recycled, "image decodes never triggered the maintenance recycle");
    }

    QVERIFY(waitForText(preview.get(), QLatin1String("imgs")));
    QVERIFY(waitForImageWidth(preview.get(), ImageWidth));
    const QString failed = evalJs(preview.get(), QStringLiteral("document.images[0].dataset.kdxFailed || 'ok'"));
    QCOMPARE(failed, QStringLiteral("ok"));

    delete doc;
}

QTEST_MAIN(RenderMemoryTest)
#include "rendermemorytest.moc"
