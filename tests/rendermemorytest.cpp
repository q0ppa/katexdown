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

#include <QColor>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <QWebEnginePage>
#include <QWebEngineView>

// The /proc RSS reads need the page size, but unistd.h only exists on POSIX;
// MSVC (Windows, where the test still has to compile in the Windows CI build)
// has no unistd.h. The RSS tests skip at runtime off-Linux anyway, so on
// non-POSIX builds pageSizeKb() is a constant that is never reached.
#if defined(Q_OS_UNIX)
#include <unistd.h>
#endif

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

long pageSizeKb()
{
#if defined(Q_OS_UNIX)
    return sysconf(_SC_PAGESIZE) / 1024;
#else
    return 4; // 4 KiB page; only ever used where /proc exists, which is Linux.
#endif
}

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
    return parts[1].toLong() * pageSizeKb();
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

// Pids of the running renderer processes: a change means the maintenance
// recycled the page (the renderer process died and a fresh one appeared).
QSet<long> rendererPids()
{
    QSet<long> pids;
    for (const auto &c : engineChildren()) {
        pids.insert(c.first);
    }
    return pids;
}

// The offscreen test harness activates the shown window and focuses the web
// view, while in Kate the editor holds focus and the maintenance policy
// refuses to recycle a focused preview. Drop it again, like the other tests.
bool dropPreviewFocus(PreviewWidget *preview)
{
    auto *view = preview->findChild<QWebEngineView *>();
    if (!view) {
        return false;
    }
    view->clearFocus();
    if (view->focusProxy()) {
        view->focusProxy()->clearFocus();
    }
    QDeadlineTimer deadline(5000);
    while (!deadline.hasExpired()) {
        if (QApplication::focusWidget() == nullptr) {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    return false;
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

// (count, bytes) the page reports for the reader-caused image decodes it
// counts (see preview.js). The page's own decode set never appears here.
QPair<int, long> decodeStats(PreviewWidget *preview)
{
    const QString stats = evalJs(preview, QStringLiteral("window.__kdxDecodeStats ? window.__kdxDecodeStats() : '0:0'"));
    const int colon = stats.indexOf(QLatin1Char(':'));
    if (colon <= 0) {
        return {0, 0};
    }
    return {stats.left(colon).toInt(), stats.mid(colon + 1).toLong()};
}

// How many images the page really decoded (naturalWidth > 1: the 1x1 parking
// placeholder and failed loads never count).
int decodedImages(PreviewWidget *preview)
{
    return evalJs(preview, QStringLiteral("Array.prototype.filter.call(document.images, "
                                          "function (i) { return i.naturalWidth > 1; }).length"))
        .toInt();
}

bool waitForDecodedImages(PreviewWidget *preview, int count)
{
    QDeadlineTimer deadline(20000);
    while (!deadline.hasExpired()) {
        if (decodedImages(preview) >= count) {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    return false;
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

// How many images carry a layout box (width/height attributes) right now.
int imagesWithBox(PreviewWidget *preview)
{
    return evalJs(preview, QStringLiteral("Array.prototype.filter.call(document.images, "
                                          "function (i) { return i.getAttribute('width') !== null; }).length"))
        .toInt();
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

// A solid w x h PNG: small on disk, large when decoded. The maintenance
// charges a decode by its intrinsic size (capped at 8 MiB), so a document with
// a few of these decodes more bytes than the shortened test budget.
bool writePng(const QString &path, int w, int h)
{
    QImage img(w, h, QImage::Format_RGB32);
    img.fill(QColor(120, 30, 40));
    return img.save(path, "PNG");
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
    void idleTextChurnReclaimsInPlaceWithoutReload();
    void imageDecodeResidueRecyclesOnlyWhenMinimized();
    void idleImageHeavyDocumentDoesNotRecycleInALoop();
    void maintenanceRecycleKeepsTheReaderPlaceWithImages();
    void editorRendersAfterAScrollDoNotRechargeThePageOwnDecodes();
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
    // 320 MB no matter how much dead memory was reclaimed. (With the 64 MB
    // default cap the churn builds less before the recycle than it did at 128,
    // so the absolute drop is smaller — what matters is that it lands at the
    // baseline.)
    QVERIFY2(afterKb + 50 * 1024 < beforeKb && afterKb < 400 * 1024,
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
// replaces nothing, so it must not be charged. This pins the maintenance's
// central promise: churn is reclaimed *in place* by an idle forced GC
// (window.gc, exposed by the --expose-gc flag), with no navigation — the
// renderer process never changes. The old code discarded and reloaded the
// renderer here, which was the intrusive self-refresh the design set out to
// remove.
void RenderMemoryTest::idleTextChurnReclaimsInPlaceWithoutReload()
{
    if (!procAvailable()) {
        QSKIP("renderer RSS is read from /proc (Linux only)");
    }
    qputenv("KATEXDOWN_MEM_BUDGET_MB", "16"); // below the ~46 MB text charge of bigText
    qputenv("KATEXDOWN_MEM_IDLE_MS", "800");
    qunsetenv("KATEXDOWN_MEM_OFF");
    qunsetenv("KATEXDOWN_MEM_IMG_CHARGE_MB");
    qunsetenv("KATEXDOWN_MEM_MINIMIZED");

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
    QVERIFY(dropPreviewFocus(preview.get()));

    // The reclamation primitive must be present, or nothing below can work.
    QCOMPARE(evalJs(preview.get(), QStringLiteral("String(typeof window.gc)")), QStringLiteral("function"));

    const QSet<long> baselinePids = rendererPids();
    const long baselineKb = totalEngineKb();

    // Phase 1: an idle open preview of a large document reclaims nothing (the
    // first render is uncharged) and never reloads.
    QTest::qWait(8000);
    QVERIFY2(rendererPids() == baselinePids, "an idle open preview reloaded itself");

    // Phase 2: one real edit dirties the renderer and crosses the budget. The
    // maintenance must reclaim it in place: no renderer process may change, and
    // RSS must actually come down.
    doc->setText(bigText(QStringLiteral("loop1")));
    QVERIFY(waitForText(preview.get(), QLatin1String("loop1")));
    const long peakKb = totalEngineKb();
    bool reclaimed = false;
    {
        QDeadlineTimer deadline(20000);
        while (!deadline.hasExpired()) {
            QVERIFY2(rendererPids() == baselinePids,
                     "text churn reloaded the renderer instead of reclaiming in place");
            if (totalEngineKb() + 20 * 1024 < peakKb) {
                reclaimed = true;
                break;
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        }
    }
    QVERIFY2(reclaimed,
             qPrintable(QStringLiteral("the idle forced GC did not return the churn "
                                       "(baseline %1 kB, peak %2 kB, now %3 kB)")
                            .arg(baselineKb).arg(peakKb).arg(totalEngineKb())));

    // Phase 3: it settles — no loop of GCs or reloads follows.
    const QSet<long> afterReclaim = rendererPids();
    QTest::qWait(8000);
    QVERIFY2(rendererPids() == afterReclaim, "the preview kept reloading after the reclaim");

    delete doc;
}

// Decoded-image memory is the one part a forced GC cannot return (measured:
// parking, window.gc and clearHttpCache barely move it), so it alone may still
// trigger the discard+reload — and only while the preview's window is
// minimized, because a visible page must never be reloaded. This pins both
// halves: the reader's decodes push the estimate over the budget, the visible
// page stays put through the idle windows (the in-place GC may run for the load
// charge, but the renderer process never changes), and the same page recycles
// as soon as the window is minimized. KATEXDOWN_MEM_IMG_CHARGE_MB makes the
// charge deterministic; KATEXDOWN_MEM_MINIMIZED stands in for the minimized
// window (the offscreen harness cannot always minimize).
void RenderMemoryTest::imageDecodeResidueRecyclesOnlyWhenMinimized()
{
    if (!procAvailable()) {
        QSKIP("renderer RSS is read from /proc (Linux only)");
    }
    qputenv("KATEXDOWN_MEM_BUDGET_MB", "16"); // 4 x 4 MB flat charge crosses it
    qputenv("KATEXDOWN_MEM_IDLE_MS", "800");
    qputenv("KATEXDOWN_MEM_IMG_CHARGE_MB", "4");
    qunsetenv("KATEXDOWN_MEM_OFF");
    qunsetenv("KATEXDOWN_MEM_MINIMIZED");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // The images sit far below the fold, after a body long enough that native
    // lazy loading keeps them out until the reader actually scrolls there:
    // their decodes are then unmistakably reader-caused (the page's own
    // load-time set never counts, see preview.js).
    QString md = QStringLiteral("# imgs\n\n");
    for (int i = 0; i < 400; ++i) {
        md += QStringLiteral("- filler line %1 of a long body of text\n").arg(i);
    }
    md += QStringLiteral("\n");
    for (int i = 0; i < 8; ++i) {
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
    QVERIFY(dropPreviewFocus(preview.get()));

    // The reader browses: a wheel event marks the viewport as reader-moved and
    // the scroll brings the far-away images into the lazy-loading zone.
    evalJs(preview.get(), QStringLiteral("(function(){window.dispatchEvent(new WheelEvent('wheel'));"
                                        "window.scrollTo(0, document.body.scrollHeight);return 'ok';})()"));
    {
        QDeadlineTimer deadline(15000);
        bool counted = false;
        while (!deadline.hasExpired()) {
            if (decodeStats(preview.get()).first >= 5) {
                counted = true;
                break;
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        }
        QVERIFY2(counted, "the page never charged the reader-caused image decodes");
    }

    // Visible: the image residue must NOT reload the page, however many idle
    // windows pass.
    const QSet<long> baselinePids = rendererPids();
    QTest::qWait(8000);
    QVERIFY2(rendererPids() == baselinePids,
             "an image-heavy page was reloaded while the user could see it");

    // Minimized: the reload is invisible, so the discard+reload is allowed and
    // the image memory is actually returned.
    qputenv("KATEXDOWN_MEM_MINIMIZED", "1");
    bool recycled = false;
    {
        QDeadlineTimer deadline(20000);
        while (!deadline.hasExpired()) {
            if (rendererPids() != baselinePids) {
                recycled = true;
                break;
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        }
    }
    qunsetenv("KATEXDOWN_MEM_MINIMIZED");
    QVERIFY2(recycled, "the image residue did not recycle even while minimized");

    QVERIFY(waitForText(preview.get(), QLatin1String("imgs")));
    QVERIFY(waitForDecodedImages(preview.get(), 1));
    const QString failed = evalJs(preview.get(), QStringLiteral("document.images[0].dataset.kdxFailed || 'ok'"));
    QCOMPARE(failed, QStringLiteral("ok"));

    delete doc;
}


// Opening a preview of a document with many images used to make the preview
// reload itself over and over (and every reload also moved the reader): the
// page's own decode set — the images it decodes by itself at load — is
// reproduced by every fresh page, so charging it against the budget kept the
// estimate over the budget in every generation. This pins the loop-free
// behavior: an idle open image-heavy document stays on one renderer process.
void RenderMemoryTest::idleImageHeavyDocumentDoesNotRecycleInALoop()
{
    if (!procAvailable()) {
        QSKIP("renderer RSS is read from /proc (Linux only)");
    }
    qputenv("KATEXDOWN_MEM_BUDGET_MB", "16"); // three 2048x2048 decodes cross it
    qputenv("KATEXDOWN_MEM_IDLE_MS", "800");
    qunsetenv("KATEXDOWN_MEM_OFF");
    qunsetenv("KATEXDOWN_MEM_IMG_CHARGE_MB");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QString md = QStringLiteral("# gallery\n\n");
    for (int i = 0; i < 6; ++i) {
        QVERIFY(writePng(dir.filePath(QStringLiteral("p%1.png").arg(i)), 2048, 2048));
        md += QStringLiteral("![photo %1](p%1.png)\n\n").arg(i);
    }
    const QString path = dir.filePath(QStringLiteral("doc.md"));
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QVERIFY(f.write(md.toUtf8()) > 0);
    f.close();
    KTextEditor::Document *doc = KTextEditor::Editor::instance()->createDocument(nullptr);
    QVERIFY(doc->openUrl(QUrl::fromLocalFile(path)));
    QTRY_VERIFY(doc->text().size() > 0);

    auto preview = std::make_unique<PreviewWidget>(nullptr, nullptr, doc);
    preview->resize(800, 600);
    preview->show();
    QVERIFY(waitForText(preview.get(), QLatin1String("gallery")));
    // The document's own decodes are what this test is about: each 2048x2048
    // decode charges the 8 MiB cap, so three of them alone would cross the
    // budget if the page's own set were charged (it is not — see preview.js).
    QVERIFY2(waitForDecodedImages(preview.get(), 3), "the document images never decoded");
    QVERIFY(dropPreviewFocus(preview.get()));

    const QSet<long> baselinePids = rendererPids();
    QTest::qWait(15000);
    QVERIFY2(decodeStats(preview.get()).first == 0,
             "the page charged its own decode set against the maintenance budget");
    QVERIFY2(rendererPids() == baselinePids,
             "an idle image-heavy preview kept recycling itself (self-refresh loop)");

    delete doc;
}

// A maintenance recycle must leave the reader exactly where they were. A fresh
// page starts from the markdown source, so it knows no image geometry at all:
// without the boxes the recycled page had recorded, the document is laid out
// as if every image were zero-height, the restored scroll position clamps to
// that much shorter document, and the reader lands somewhere else entirely —
// the "it refreshed and now I am at a different place" symptom.
void RenderMemoryTest::maintenanceRecycleKeepsTheReaderPlaceWithImages()
{
    qputenv("KATEXDOWN_MEM_OFF", "1"); // drive the recycle API directly, no policy ticker

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QString md = QStringLiteral("# gallery\n\n");
    for (int i = 0; i < 8; ++i) {
        QVERIFY(writePng(dir.filePath(QStringLiteral("p%1.png").arg(i)), 640, 480));
        md += QStringLiteral("## marker %1\n\n![photo %1](p%1.png)\n\n").arg(i);
        md += QStringLiteral("Body text under marker %1 so the images have room to scroll.\n\n").arg(i);
    }
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
    QVERIFY(waitForText(preview.get(), QLatin1String("gallery")));
    QVERIFY(waitForDecodedImages(preview.get(), 1));

    // Read deep into the document: park the viewport on a marker near the end.
    const QString marker = QStringLiteral("marker-6");
    const QString placed = evalJs(preview.get(),
                                  QStringLiteral("document.getElementById('%1') ? "
                                                 "(document.getElementById('%1').scrollIntoView(), 'ok') : 'missing'")
                                      .arg(marker));
    QCOMPARE(placed, QStringLiteral("ok"));
    QTest::qWait(1500); // let the images around the new position decode
    const double scrollBefore = evalJs(preview.get(), QStringLiteral("String(window.scrollY)")).toDouble();
    const double topBefore = evalJs(preview.get(), QStringLiteral("String(document.getElementById('%1').getBoundingClientRect().top)").arg(marker)).toDouble();
    const int sizedBefore = imagesWithBox(preview.get());
    QVERIFY2(scrollBefore > 2000, qPrintable(QStringLiteral("the test did not scroll deep enough: %1").arg(scrollBefore)));

    QVERIFY(preview->performMemoryRecycle(true));
    QVERIFY(waitForText(preview.get(), QLatin1String("gallery")));
    QTest::qWait(2500); // the fresh page re-lays out and the scroll restore lands

    const double scrollAfter = evalJs(preview.get(), QStringLiteral("String(window.scrollY)")).toDouble();
    const double topAfter = evalJs(preview.get(), QStringLiteral("String(document.getElementById('%1').getBoundingClientRect().top)").arg(marker)).toDouble();
    // The mechanism behind the restored place: the boxes the recycled page had
    // learned survive into the fresh page (its images would otherwise be
    // zero-height until they decode, which is what moved the reader).
    const int sizedAfter = imagesWithBox(preview.get());
    QVERIFY2(sizedAfter >= sizedBefore,
             qPrintable(QStringLiteral("the fresh page lost image boxes: %1 -> %2").arg(sizedBefore).arg(sizedAfter)));
    QVERIFY2(qAbs(scrollAfter - scrollBefore) < 8,
             qPrintable(QStringLiteral("scroll position lost across the recycle: %1 -> %2").arg(scrollBefore).arg(scrollAfter)));
    QVERIFY2(qAbs(topAfter - topBefore) < 8,
             qPrintable(QStringLiteral("the reader's content moved across the recycle: viewport top %1 -> %2").arg(topBefore).arg(topAfter)));

    // Exports stay plain and eager: the remembered boxes are preview machinery
    // and must not leak into a standalone file (see __serializedHtml). The
    // exported document is parsed back so the audit sees its images, not the
    // preview.js source text inlined next to them.
    const QString exportAudit = evalJs(preview.get(), QStringLiteral(
        "(function(){var d=new DOMParser().parseFromString(window.__serializedHtml(),'text/html');"
        "var imgs=d.querySelectorAll('img');var sized=0,real=0,machinery=0;"
        "for(var i=0;i<imgs.length;i++){var im=imgs[i];"
        "if(im.getAttribute('width')!==null||im.getAttribute('height')!==null)sized++;"
        "if((im.getAttribute('src')||'').indexOf('.png')>=0)real++;"
        "for(var j=0;j<im.attributes.length;j++)if(im.attributes[j].name.indexOf('data-kdx')===0)machinery++;}"
        "return imgs.length+':'+sized+':'+real+':'+machinery;})()"));
    QCOMPARE(exportAudit, QStringLiteral("8:0:8:0"));

    delete doc;
}

// A decode the page performs as a consequence of *re-rendering* is not a
// decode the reader caused: the render rebuilds the image DOM, so the images
// around the viewport decode again — exactly the live set a fresh page
// reproduces. The reader-moved gate behind the decode charge therefore resets
// on every re-render. Without that reset it stayed open for the rest of the
// page generation, so scrolling once and then editing re-charged the same
// images on every keystroke (measured: 7 decodes after one scroll growing to
// 42 over five edits), which kept the estimate over budget and let the
// maintenance recycle a document nobody was touching — the residual "it
// refreshed itself" report. This test scrolls once, edits five times, and
// asserts the charged set stops growing and the recycler stays put.
void RenderMemoryTest::editorRendersAfterAScrollDoNotRechargeThePageOwnDecodes()
{
    if (!procAvailable()) {
        QSKIP("renderer RSS is read from /proc (Linux only)");
    }
    qunsetenv("KATEXDOWN_MEM_OFF");
    qputenv("KATEXDOWN_MEM_BUDGET_MB", "64"); // above the reader's own ~35 MB of decodes
    qputenv("KATEXDOWN_MEM_IDLE_MS", "1000");
    qputenv("KATEXDOWN_MEM_MAX_AGE_MS", "600000"); // the idle recycle is what this pins
    qunsetenv("KATEXDOWN_MEM_IMG_CHARGE_MB");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QString md = QStringLiteral("# gallery\n\n");
    for (int i = 0; i < 8; ++i) {
        QVERIFY(writePng(dir.filePath(QStringLiteral("p%1.png").arg(i)), 1200, 900));
        md += QStringLiteral("## marker %1\n\n![photo %1](p%1.png)\n\n").arg(i);
        for (int j = 0; j < 20; ++j) {
            md += QStringLiteral("filler line %1-%2 under marker %1 to give the images room.\n\n").arg(i).arg(j);
        }
    }
    const QString path = dir.filePath(QStringLiteral("doc.md"));
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QVERIFY(f.write(md.toUtf8()) > 0);
    f.close();
    KTextEditor::Document *doc = KTextEditor::Editor::instance()->createDocument(nullptr);
    QVERIFY(doc->openUrl(QUrl::fromLocalFile(path)));
    QTRY_VERIFY(doc->text().size() > 0);

    auto preview = std::make_unique<PreviewWidget>(nullptr, nullptr, doc);
    preview->resize(800, 600);
    preview->show();
    QVERIFY(waitForText(preview.get(), QLatin1String("gallery")));
    QVERIFY(waitForDecodedImages(preview.get(), 1));
    QVERIFY(dropPreviewFocus(preview.get()));

    // The reader scrolls: the wheel event opens the reader-moved gate and the
    // images the scroll pulls in are charged (that part is by design). Wait for
    // that decode set to settle before measuring it — lazy decoding is a bit
    // asynchronous — so the only thing the edits could add is their own.
    evalJs(preview.get(), QStringLiteral("window.dispatchEvent(new WheelEvent('wheel')); 'ok'"));
    evalJs(preview.get(), QStringLiteral("window.scrollTo(0, document.body.scrollHeight); 'ok'"));
    QVERIFY(waitForDecodedImages(preview.get(), 6));
    int previous = -1;
    for (int i = 0; i < 30; ++i) {
        QTest::qWait(500);
        const int now = decodeStats(preview.get()).first;
        if (now == previous) {
            break; // no reader-caused decode for 500 ms: settled
        }
        previous = now;
    }
    const QPair<int, long> afterScroll = decodeStats(preview.get());
    QVERIFY2(afterScroll.first >= 3, qPrintable(QStringLiteral("the reader's scroll charged only %1 image decodes").arg(afterScroll.first)));

    // Now the reader types: each debounced re-render rebuilds the images around
    // the viewport. Those decodes belong to the page, not to a viewport move,
    // so the charged set must not grow (one stray late decode is tolerated;
    // the regression adds the whole live set, ~7 per edit, five times).
    const QSet<long> baselinePids = rendererPids();
    for (int i = 0; i < 5; ++i) {
        doc->setText(doc->text() + QStringLiteral("\nedit %1\n").arg(i));
        QTest::qWait(1200);
    }
    const QPair<int, long> afterEdits = decodeStats(preview.get());
    QVERIFY2(afterEdits.first - afterScroll.first <= 1,
             qPrintable(QStringLiteral("re-renders re-charged the page's own decodes: %1 -> %2")
                            .arg(afterScroll.first).arg(afterEdits.first)));

    // And the renderer stays put: before the gate reset, the re-charged decodes
    // pushed the estimate past the budget and the preview recycled itself with
    // no user action left to cause it.
    QTest::qWait(8000);
    QVERIFY2(rendererPids() == baselinePids, "editing after a scroll recycled the preview");

    delete doc;
}

QTEST_MAIN(RenderMemoryTest)

#include "rendermemorytest.moc"
