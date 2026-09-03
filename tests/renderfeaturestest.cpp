// Verifies the optional, runtime-cached features: LaTeX rendering (KaTeX +
// markdown-it-texmath from the data dir) and user custom CSS. Both degrade to
// no-ops when the assets/settings are absent, which is what the earlier
// lifecycle tests rely on.

#include "katexdownpaths.h"
#include "previewwidget.h"
#include "settings.h"

#include <QDeadlineTimer>
#include <QFile>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <QWebEnginePage>
#include <QWebEngineView>

#include <memory>

#include <KTextEditor/Document>
#include <KTextEditor/Editor>

namespace
{
// Evaluate one expression in the page and hand back its value as a string.
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
    QDeadlineTimer deadline(5000);
    while (!slot->second && !deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return slot->first;
}

// Wait until path exists and is non-empty.
QString readFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return QString();
    }
    return QString::fromUtf8(f.readAll());
}

bool waitForFile(const QString &path)
{
    QDeadlineTimer deadline(15000);
    while (!deadline.hasExpired()) {
        QFile f(path);
        if (f.exists() && f.size() > 0) {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    return false;
}

bool waitForPageText(PreviewWidget *preview, QLatin1String needle)
{
    QDeadlineTimer deadline(20000);
    while (!deadline.hasExpired()) {
        const QString text = evalJs(preview, QStringLiteral("document.getElementById('content').innerText"));
        if (text.contains(needle)) {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    return false;
}

// Poll a JS expression until it evaluates to the expected string.
bool waitForCond(PreviewWidget *preview, const QString &code, const QString &expected)
{
    QDeadlineTimer deadline(20000);
    while (!deadline.hasExpired()) {
        if (evalJs(preview, code) == expected) {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    return false;
}
} // namespace

class RenderFeaturesTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void mathRendersWhenAssetsPresent();
    void customCssApplies();
    void githubCssCanBeDisabled();
    void exportsStandaloneHtml();
    void imageModesControlDecoding();
    void parkingPreservesTheImageBox();
    void relativeCssResolvesAgainstDataDir();
    void outlineListsConfiguredHeadings();
    void fragmentLinksJumpToSections();
    void enginesAreLoadedOnlyWhenTheTextNeedsThem();
    void oversizedFenceRendersPlain();

private:
    KTextEditor::Document *openDocument(const QString &text);
    std::unique_ptr<PreviewWidget> makePreview(KTextEditor::Document *doc);

    QTemporaryDir m_dir;
};

KTextEditor::Document *RenderFeaturesTest::openDocument(const QString &text)
{
    KTextEditor::Document *doc = KTextEditor::Editor::instance()->createDocument(nullptr);
    doc->setText(text);
    return doc;
}

std::unique_ptr<PreviewWidget> RenderFeaturesTest::makePreview(KTextEditor::Document *doc)
{
    auto preview = std::make_unique<PreviewWidget>(nullptr, nullptr, doc);
    return preview;
}

// Needs the assets downloaded by tools/fetch-assets.py into $KATEXDOWN_DATA_DIR
// (set by the test environment); without them the test is skipped.
void RenderFeaturesTest::mathRendersWhenAssetsPresent()
{
    // Resolve the data dir exactly like the running plugin does (env override
    // or per-platform default) — this test doubles as proof that no
    // environment variable is needed.
    const QString dataDir = katexdownpaths::dataDir();
    if (!QFile::exists(dataDir + QStringLiteral("/katex.min.js")) || !QFile::exists(dataDir + QStringLiteral("/texmath.min.js"))) {
        QSKIP("KaTeX assets not found in the data dir; run tools/fetch-assets.py");
    }

    QVERIFY(m_dir.isValid());
    KTextEditor::Document *doc = openDocument(QStringLiteral("# math\n\ninline $x^2 + y^2$ here.\n\n$$\n\\int_0^1 x\\,dx = \\frac{1}{2}\n$$\n"));
    auto preview = makePreview(doc);
    QVERIFY(waitForPageText(preview.get(), QLatin1String("math")));

    // texmath is wired up and katex is available in the page (katex is an
    // object exposing render/renderToString).
    QCOMPARE(evalJs(preview.get(), QStringLiteral("typeof katex")), QStringLiteral("object"));
    QCOMPARE(evalJs(preview.get(), QStringLiteral("typeof katex.renderToString")), QStringLiteral("function"));

    // Inline math produced a .katex element and display math a .katex-display.
    const int inlineCount = evalJs(preview.get(), QStringLiteral("document.querySelectorAll('#content .katex').length")).toInt();
    const int displayCount = evalJs(preview.get(), QStringLiteral("document.querySelectorAll('#content .katex-display').length")).toInt();
    QVERIFY2(inlineCount >= 1, qPrintable(QStringLiteral("no inline math rendered (katex nodes: %1)").arg(inlineCount)));
    QVERIFY2(displayCount >= 1, qPrintable(QStringLiteral("no display math rendered (katex-display nodes: %1)").arg(displayCount)));

    // No parse errors: throwOnError is off, but the formulas above are valid.
    const int errorCount = evalJs(preview.get(), QStringLiteral("document.querySelectorAll('.katex-error').length")).toInt();
    QCOMPARE(errorCount, 0);

    delete doc;
}

void RenderFeaturesTest::customCssApplies()
{
    QVERIFY(m_dir.isValid());
    const QString cssPath = m_dir.filePath(QStringLiteral("wide.css"));
    QFile f(cssPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QVERIFY(f.write(QByteArrayLiteral(".markdown-body { max-width: 471px !important; }\n")) > 0);
    f.close();

    Settings::self()->setCustomCssFiles({cssPath});

    KTextEditor::Document *doc = openDocument(QStringLiteral("# css\n\nsome body.\n"));
    auto preview = makePreview(doc);
    QVERIFY(waitForPageText(preview.get(), QLatin1String("some body")));

    const QString maxWidth = evalJs(preview.get(), QStringLiteral("getComputedStyle(document.getElementById('content')).maxWidth"));
    QCOMPARE(maxWidth, QStringLiteral("471px"));

    delete doc;
}

// The bundled GitHub stylesheet is on by default and can be switched off so a
// custom stylesheet owns the whole layout.
void RenderFeaturesTest::githubCssCanBeDisabled()
{
    QVERIFY(m_dir.isValid());

    // Default: the github-markdown.css is inlined into the page.
    Settings::self()->setUseGithubCss(true);
    KTextEditor::Document *on = openDocument(QStringLiteral("# gh\n\nthe github css body.\n"));
    auto previewOn = makePreview(on);
    QVERIFY(waitForPageText(previewOn.get(), QLatin1String("the github css body")));
    const int cssLen = evalJs(previewOn.get(), QStringLiteral("document.getElementById('ghmd').textContent.length")).toInt();
    QVERIFY2(cssLen > 1000, qPrintable(QStringLiteral("github css not inlined, %1 chars").arg(cssLen)));
    delete on;

    // Disabled: the style element is left empty.
    Settings::self()->setUseGithubCss(false);
    KTextEditor::Document *off = openDocument(QStringLiteral("# gh\n\nthe github css body.\n"));
    auto previewOff = makePreview(off);
    QVERIFY(waitForPageText(previewOff.get(), QLatin1String("the github css body")));
    const int offLen = evalJs(previewOff.get(), QStringLiteral("document.getElementById('ghmd').textContent.length")).toInt();
    QCOMPARE(offLen, 0);
    delete off;

    Settings::self()->setUseGithubCss(true); // leave defaults for later tests
}

// ExportToFile serializes the live page: rendered content plus every included
// stylesheet (github css, user css) land in a standalone .html file.
void RenderFeaturesTest::exportsStandaloneHtml()
{
    QVERIFY(m_dir.isValid());

    Settings::self()->setUseGithubCss(true);
    KTextEditor::Document *doc = openDocument(QStringLiteral("# exported\n\nsome exportable body.\n\n|a|b|\n|-|-|\n|1|2|\n"));
    auto preview = makePreview(doc);
    QVERIFY(waitForPageText(preview.get(), QLatin1String("some exportable body")));

    const QString outPath = m_dir.filePath(QStringLiteral("exported.html"));
    QVERIFY(preview->exportToFile(outPath));
    QVERIFY(waitForFile(outPath));

    const QString html = readFile(outPath);
    QVERIFY2(html.contains(QLatin1String("some exportable body")), qPrintable(QStringLiteral("export misses content")));
    QVERIFY2(html.contains(QLatin1String("id=\"ghmd\"")), qPrintable(QStringLiteral("export misses the github stylesheet")));
    QVERIFY2(html.contains(QLatin1String("markdown-body")), qPrintable(QStringLiteral("export misses the rendered article")));
    delete doc;
}

// The image memory mode decides how images behave once rendered: eager leaves
// them alone (decode all at once), memory-saver lazy-loads and additionally
// parks far off-screen images on a placeholder (releasing decoded memory),
// and auto lazy-loads while arming the parking only for image-heavy
// documents. __serializedHtml() must always hand export a plain, eager copy
// regardless of the live mode.
void RenderFeaturesTest::imageModesControlDecoding()
{
    QVERIFY(m_dir.isValid());
    // A real 64x32 PNG, inline so the test carries no fixture file. It must not
    // be 1x1: the parking placeholder itself decodes as 1x1, and the manager
    // must never record the placeholder's box as the image's (1x1 images need
    // no recorded box at all). kdxSized "1" below is meaningful only for a
    // real-sized image.
    const QString png = QStringLiteral(
        "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAEAAAAAgCAIAAAAt/+nTAAAATklEQVR4nO3PUQkAIBTAwBfNaEYzmiH8OITBAtxmr/N1wwUNaEEDWtCAFjSgBQ1oQQNa0IAWNKAFDWhBA1rQgBY0oAUNaEEDWtCAFjx2AZO6ALV3naQ3AAAAAElFTkSuQmCC");

    Settings::self()->setImageMode(Settings::DecodeAll);
    QString filler;
    for (int i = 0; i < 600; ++i) {
        filler += QStringLiteral("filler line %1 making the document tall\n").arg(i);
    }
    KTextEditor::Document *doc = openDocument(QStringLiteral("# imgs\n\n![pic](%1) inline tail\n\n%2").arg(png, filler));
    auto preview = makePreview(doc);
    // IntersectionObserver callbacks are driven by compositor frames: show the
    // preview (offscreen platform still renders) so scrolls deliver them.
    preview->resize(800, 600);
    preview->show();
    QVERIFY(waitForPageText(preview.get(), QLatin1String("imgs")));

    // Eager: images carry no lazy attributes and keep their real src.
    QVERIFY(waitForCond(preview.get(),
                        QStringLiteral("(function () { var i = document.querySelector('#content img'); return (i.getAttribute('loading') === null) + '|' + (i.src.indexOf('data:image/png') === 0); })()"),
                        QStringLiteral("true|true")));

    // __serializedHtml() hands export a plain image no matter the live mode.
    const QString serializeImg = QStringLiteral(
        "(function () { var d = new DOMParser().parseFromString(window.__serializedHtml(), 'text/html'); "
        "var im = d.querySelector('img'); "
        "return im.getAttribute('src') + '|' + im.getAttribute('loading') + '|' + im.getAttribute('data-kdx') + '|' + im.getAttribute('width'); })()");
    QCOMPARE(evalJs(preview.get(), serializeImg), png + QStringLiteral("|null|null|null"));

    // Memory-saver: lazy attributes; the image decodes near the top (its box
    // gets recorded for stable placeholders) ...
    Settings::self()->setImageMode(Settings::MemorySaver);
    QVERIFY(waitForCond(preview.get(),
                        QStringLiteral("document.querySelector('#content img').getAttribute('loading')"),
                        QStringLiteral("lazy")));
    QVERIFY(waitForCond(preview.get(),
                        QStringLiteral("(document.querySelector('#content img').dataset.kdxSized || '')"),
                        QStringLiteral("1")));

    // ... and once scrolled far away it is parked on a 1x1 placeholder.
    evalJs(preview.get(), QStringLiteral("window.scrollTo(0, document.body.scrollHeight); 'ok'"));
    QVERIFY(waitForCond(preview.get(),
                        QStringLiteral("(function () { var i = document.querySelector('#content img'); return ((i.dataset.kdx || '') !== '') + '|' + (i.src.indexOf('data:image/gif') === 0); })()"),
                        QStringLiteral("true|true")));
    // Export serialization still carries the real src, machinery stripped.
    QCOMPARE(evalJs(preview.get(), serializeImg), png + QStringLiteral("|null|null|null"));

    // Scrolling back to the top restores the image in front of the viewport.
    evalJs(preview.get(), QStringLiteral("window.scrollTo(0, 0); 'ok'"));
    QVERIFY(waitForCond(preview.get(),
                        QStringLiteral("(function () { var i = document.querySelector('#content img'); return ((i.dataset.kdx || '') === '') + '|' + (i.src.indexOf('data:image/png') === 0); })()"),
                        QStringLiteral("true|true")));

    // Auto with a one-image document: lazy attributes only, never parked.
    Settings::self()->setImageMode(Settings::Adaptive);
    QVERIFY(waitForCond(preview.get(),
                        QStringLiteral("document.querySelector('#content img').getAttribute('loading')"),
                        QStringLiteral("lazy")));
    evalJs(preview.get(), QStringLiteral("window.scrollTo(0, document.body.scrollHeight); 'ok'"));
    QVERIFY(waitForCond(preview.get(),
                        QStringLiteral("(function () { var i = document.querySelector('#content img'); return (i.dataset.kdx || '') === ''; })()"),
                        QStringLiteral("true")));

    delete doc;
    Settings::self()->setImageMode(Settings::Adaptive); // leave the default for later tests
}

// Parking (replacing the displayed src of an image that scrolled far away
// with the invisible 1x1 placeholder, releasing its decoded bitmap) must
// never change the document layout: swapOut() only parks an image whose box
// is known — it has decoded (width/height/aspect-ratio recorded) or the
// author sized it — so the placeholder swap leaves the document height
// untouched. An image that never decoded is never parked (nothing decoded to
// free, no known box): it keeps its real src and loads like any lazy image
// once it approaches the viewport. The regression this guards: images below
// the fold used to be parked while still undecoded, collapsing the document
// height below them (or, earlier, baking the 1x1 placeholder box into them
// so they stayed ~1px dots).
void RenderFeaturesTest::parkingPreservesTheImageBox()
{
    QVERIFY(m_dir.isValid());
    // A real 64x32 PNG, inline so the test carries no fixture file. It must
    // not be 1x1: the parking placeholder decodes as 1x1 too, and the manager
    // must never treat that as the image's size.
    const QString png = QStringLiteral(
        "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAEAAAAAgCAIAAAAt/+nTAAAATklEQVR4nO3PUQkAIBTAwBfNaEYzmiH8OITBAtxmr/N1wwUNaEEDWtCAFjSgBQ1oQQNa0IAWNKAFDWhBA1rQgBY0oAUNaEEDWtCAFjx2AZO6ALV3naQ3AAAAAElFTkSuQmCC");

    Settings::self()->setImageMode(Settings::MemorySaver);
    QString fillerA;
    for (int i = 0; i < 500; ++i) {
        fillerA += QStringLiteral("filler line %1 above the image\n").arg(i);
    }
    QString fillerB;
    for (int i = 0; i < 500; ++i) {
        fillerB += QStringLiteral("filler line %1 below the image\n").arg(i);
    }
    KTextEditor::Document *doc = openDocument(QStringLiteral("# imgs\n\n%1\n\n![pic](%2)\n\n%3\nend of document\n").arg(fillerA, png, fillerB));
    auto preview = makePreview(doc);
    preview->resize(800, 600);
    preview->show();
    QVERIFY(waitForPageText(preview.get(), QLatin1String("end of document")));

    const QString imgJs = QStringLiteral("document.querySelector('#content img')");
    // The image must sit far below the fold (beyond the keep zone of ~1.3
    // viewport heights and Chromium's own lazy-loading distance), or the test
    // below would describe an in-zone image instead of a below-the-fold one.
    QVERIFY(waitForCond(preview.get(),
                        QStringLiteral("Math.round(%1.getBoundingClientRect().top) > 2000").arg(imgJs),
                        QStringLiteral("true")));

    // Far below the fold and never decoded: it is NOT parked. It keeps its
    // real src and no box is recorded (parking it now would collapse the
    // document height below it; the old code parked it immediately).
    QDeadlineTimer settle(700);
    while (!settle.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    QCOMPARE(evalJs(preview.get(),
                    QStringLiteral("(function () { var i = %1; return ((i.dataset.kdx || '') === '') + '|' + (i.src.indexOf('data:image/png') === 0) + '|' + (i.getAttribute('width') || '') + '|' + (i.dataset.kdxSized || '') + '|' + (i.naturalWidth === 0); })()").arg(imgJs)),
             QStringLiteral("true|true|||true"));

    // Scroll it into the viewport: it loads at its natural size and its box is
    // recorded from the real decode (64x32).
    evalJs(preview.get(),
           QStringLiteral("(function () { var i = %1; window.scrollTo(0, i.getBoundingClientRect().top + window.scrollY - 200); })()").arg(imgJs));
    QVERIFY(waitForCond(preview.get(),
                        QStringLiteral("String(%1.naturalWidth)").arg(imgJs),
                        QStringLiteral("64")));
    QVERIFY(waitForCond(preview.get(),
                        QStringLiteral("(function () { var i = %1; return i.getAttribute('width') + '|' + i.getBoundingClientRect().width; })()").arg(imgJs),
                        QStringLiteral("64|64")));
    const qlonglong heightAfterDecode = evalJs(preview.get(), QStringLiteral("document.body.scrollHeight")).toLongLong();
    QVERIFY2(heightAfterDecode > 3000, qPrintable(QStringLiteral("document unexpectedly short: %1").arg(heightAfterDecode)));

    // Scroll far past it (to the bottom of the document): the decoded image is
    // now parked on the placeholder (its decoded bitmap is released) — with
    // the box preserved, so the document height does not change at all.
    evalJs(preview.get(), QStringLiteral("window.scrollTo(0, document.body.scrollHeight); 'ok'"));
    QVERIFY(waitForCond(preview.get(),
                        QStringLiteral("(function () { var i = %1; return ((i.dataset.kdx || '') !== '') + '|' + (i.src.indexOf('data:image/gif') === 0); })()").arg(imgJs),
                        QStringLiteral("true|true")));
    QVERIFY(waitForCond(preview.get(),
                        QStringLiteral("(function () { var i = %1; return i.getAttribute('width') + '|' + i.getBoundingClientRect().width; })()").arg(imgJs),
                        QStringLiteral("64|64")));
    QCOMPARE(evalJs(preview.get(), QStringLiteral("document.body.scrollHeight")).toLongLong(), heightAfterDecode);

    // Scrolling back to it swaps the real src in again; the box and the
    // document height stay untouched the whole way.
    evalJs(preview.get(),
           QStringLiteral("(function () { var i = %1; window.scrollTo(0, i.getBoundingClientRect().top + window.scrollY - 200); })()").arg(imgJs));
    QVERIFY(waitForCond(preview.get(),
                        QStringLiteral("(function () { var i = %1; return ((i.dataset.kdx || '') === '') + '|' + String(i.naturalWidth); })()").arg(imgJs),
                        QStringLiteral("true|64")));
    QVERIFY(waitForCond(preview.get(),
                        QStringLiteral("(function () { var i = %1; return i.getAttribute('width') + '|' + i.getBoundingClientRect().width; })()").arg(imgJs),
                        QStringLiteral("64|64")));
    QCOMPARE(evalJs(preview.get(), QStringLiteral("document.body.scrollHeight")).toLongLong(), heightAfterDecode);

    delete doc;
    Settings::self()->setImageMode(Settings::Adaptive); // leave the default for later tests
}

void RenderFeaturesTest::relativeCssResolvesAgainstDataDir()
{
    QVERIFY(m_dir.isValid());
    // Relative paths in the settings resolve against $KATEXDOWN_DATA_DIR.
    qputenv("KATEXDOWN_DATA_DIR", m_dir.path().toUtf8());
    QFile f(m_dir.filePath(QStringLiteral("narrow.css")));
    QVERIFY(f.open(QIODevice::WriteOnly));
    QVERIFY(f.write(QByteArrayLiteral(".markdown-body { max-width: 333px !important; }\n")) > 0);
    f.close();

    Settings::self()->setCustomCssFiles({QStringLiteral("narrow.css")});

    KTextEditor::Document *doc = openDocument(QStringLiteral("# rel\n\nsome body.\n"));
    auto preview = makePreview(doc);
    QVERIFY(waitForPageText(preview.get(), QLatin1String("some body")));

    const QString maxWidth = evalJs(preview.get(), QStringLiteral("getComputedStyle(document.getElementById('content')).maxWidth"));
    QCOMPARE(maxWidth, QStringLiteral("333px"));

    delete doc;
}

// The floating section-outline control lists exactly the heading levels the
// settings select (default H1-H5), assigns anchor ids, and clicking an entry
// jumps to (and flashes) that heading.
void RenderFeaturesTest::outlineListsConfiguredHeadings()
{
    QVERIFY(m_dir.isValid());
    Settings::self()->setTocLevels({1, 2, 3});
    KTextEditor::Document *doc = openDocument(QStringLiteral(
        "# Intro\n\nsome intro text.\n\n"
        "## Details\n\nmore text.\n\n"
        "### Deep dive\n\ndeeper text.\n\n"
        "#### Skipped\n\nlevel four is off.\n"));
    auto preview = makePreview(doc);
    QVERIFY(waitForPageText(preview.get(), QLatin1String("some intro text")));

    // The control appears only once the document has listable headings.
    QVERIFY(waitForCond(preview.get(),
                        QStringLiteral("var b = document.getElementById('kdx-outline-btn'); b !== null && getComputedStyle(b).display !== 'none'"),
                        QStringLiteral("true")));
    // H1-H3 are listed with their levels, in document order.
    QVERIFY(waitForCond(preview.get(),
                        QStringLiteral("document.querySelectorAll('#kdx-outline-list .kdx-outline-item').length"),
                        QStringLiteral("3")));
    QCOMPARE(evalJs(preview.get(), QStringLiteral("Array.prototype.map.call(document.querySelectorAll('#kdx-outline-list .kdx-outline-item'), function (li) { return li.getAttribute('data-level'); }).join(',')")),
             QStringLiteral("1,2,3"));
    const QString titles = evalJs(preview.get(),
                                  QStringLiteral("Array.prototype.map.call(document.querySelectorAll('#kdx-outline-list .kdx-outline-text'), function (s) { return s.textContent; }).join('|')"));
    QVERIFY2(titles.contains(QLatin1String("Intro")) && titles.contains(QLatin1String("Deep dive")) && !titles.contains(QLatin1String("Skipped")),
             qPrintable(QStringLiteral("unexpected outline entries: %1").arg(titles)));

    // Headings get GitHub-style anchor ids ("Intro" -> "intro").
    QCOMPARE(evalJs(preview.get(), QStringLiteral("document.getElementById('intro') !== null && document.getElementById('intro').tagName === 'H1'")),
             QStringLiteral("true"));

    // Clicking the first entry closes the panel and flashes the heading.
    QCOMPARE(evalJs(preview.get(),
                    QStringLiteral("document.querySelector('#kdx-outline-list .kdx-outline-item').click(); "
                                   "document.querySelector('#content h1').classList.contains('kdx-outline-jumped') && "
                                   "document.getElementById('kdx-outline-panel').hidden")),
             QStringLiteral("true"));
    delete doc;

    // Default levels H1-H5: an H6 drops out of the list.
    Settings::self()->setTocLevels({1, 2, 3, 4, 5});
    KTextEditor::Document *docAll = openDocument(QStringLiteral("# A\n\n## B\n\n### C\n\n#### D\n\n##### E\n\n###### F\n"));
    auto previewAll = makePreview(docAll);
    QVERIFY(waitForPageText(previewAll.get(), QLatin1String("F")));
    QVERIFY(waitForCond(previewAll.get(),
                        QStringLiteral("document.querySelectorAll('#kdx-outline-list .kdx-outline-item').length"),
                        QStringLiteral("5")));
    QVERIFY(!evalJs(previewAll.get(), QStringLiteral("Array.prototype.map.call(document.querySelectorAll('#kdx-outline-list .kdx-outline-item'), function (li) { return li.getAttribute('data-level'); }).join(',')"))
                 .contains(QLatin1String("6")));
    delete docAll;

    // No enabled levels: the button stays hidden.
    Settings::self()->setTocLevels({});
    KTextEditor::Document *docNone = openDocument(QStringLiteral("# Lone\n\nsome text.\n"));
    auto previewNone = makePreview(docNone);
    QVERIFY(waitForPageText(previewNone.get(), QLatin1String("some text")));
    QVERIFY(waitForCond(previewNone.get(),
                        QStringLiteral("var b = document.getElementById('kdx-outline-btn'); b !== null && getComputedStyle(b).display === 'none' && document.querySelectorAll('#kdx-outline-list .kdx-outline-item').length === 0"),
                        QStringLiteral("true")));
    delete docNone;

    Settings::self()->setTocLevels({1, 2, 3, 4, 5}); // leave the default for later tests
}

// Fragment links ("[install](#install)" in the page, "doc.md#section" across
// documents) resolve to the right section. Every heading — at any depth and
// whatever the configured outline levels — carries a GitHub-style anchor id
// ("What's next?" -> "whats-next", repeated headings numbered "-1", "-2", ...),
// and __scrollToFragment lands on the exact id, or on a tolerant match when
// the slug was written against slightly different rules.
void RenderFeaturesTest::fragmentLinksJumpToSections()
{
    QVERIFY(m_dir.isValid());
    Settings::self()->setTocLevels({1, 2, 3}); // the outline lists H1-H3 only

    QString text = QStringLiteral("# Intro\n\nSome introduction prose.\n\n");
    for (int i = 0; i < 40; ++i) {
        text += QStringLiteral("Filler paragraph %1 to make the document taller than the viewport.\n\n").arg(i);
    }
    text += QStringLiteral(
        "## What's next?\n\nQuick note.\n\n"
        "## Install\n\nFirst time.\n\n"
        "## Install\n\nAgain.\n\n"
        "###### Deep six\n\nfinally the bottom.\n");
    KTextEditor::Document *doc = openDocument(text);
    auto preview = makePreview(doc);
    QVERIFY(waitForPageText(preview.get(), QLatin1String("finally the bottom")));
    preview->resize(800, 600);
    preview->show();
    QTest::qWait(300);

    // Anchor ids follow GitHub's rules at every level, independent of the
    // outline configuration ("What's next?" drops the apostrophe; the second
    // "Install" gets "-1").
    const QString ids = evalJs(preview.get(),
                               QStringLiteral("['intro','whats-next','install','install-1','deep-six'].map(function (id) { "
                                              "var el = document.getElementById(id); return el ? el.tagName : ''; }).join(',')"));
    QCOMPARE(ids, QStringLiteral("H1,H2,H2,H2,H6"));
    // The outline list itself still honors the configured levels (H1-H3 only).
    const QString listed = evalJs(preview.get(),
                                  QStringLiteral("Array.prototype.map.call(document.querySelectorAll('#kdx-outline-list .kdx-outline-item'), "
                                                 "function (li) { return li.getAttribute('data-level'); }).join(',')"));
    QCOMPARE(listed, QStringLiteral("1,2,2,2"));

    // Jump to a section by exact id: the preview scrolls and flashes it.
    QCOMPARE(evalJs(preview.get(), QStringLiteral("window.__scrollToFragment('deep-six')")), QStringLiteral("true"));
    QTest::qWait(300);
    QCOMPARE(evalJs(preview.get(), QStringLiteral("window.scrollY > 600 ? 'scrolled' : 'no'")), QStringLiteral("scrolled"));
    QCOMPARE(evalJs(preview.get(), QStringLiteral("document.getElementById('deep-six').classList.contains('kdx-outline-jumped')")),
             QStringLiteral("true"));

    // A slug that no element carries still lands through the tolerant match
    // ("what's next?" as "what-s-next", as a punctuation-to-dash slugger
    // would write it).
    QCOMPARE(evalJs(preview.get(), QStringLiteral("window.__scrollToFragment('what-s-next')")), QStringLiteral("true"));
    QTest::qWait(300);
    QCOMPARE(evalJs(preview.get(), QStringLiteral("document.getElementById('whats-next').classList.contains('kdx-outline-jumped')")),
             QStringLiteral("true"));

    // A fragment naming nothing changes nothing and reports failure.
    QCOMPARE(evalJs(preview.get(), QStringLiteral("window.__scrollToFragment('does-not-exist')")), QStringLiteral("false"));
    delete doc;

    // With the outline switched off entirely, headings keep their anchor ids
    // (only the list disappears), so fragment links keep working.
    Settings::self()->setTocLevels({});
    KTextEditor::Document *docOff = openDocument(QStringLiteral("# Still anchored\n\nbody text.\n"));
    auto previewOff = makePreview(docOff);
    QVERIFY(waitForPageText(previewOff.get(), QLatin1String("body text")));
    QVERIFY(waitForCond(previewOff.get(),
                        QStringLiteral("var b = document.getElementById('kdx-outline-btn'); "
                                       "b !== null && getComputedStyle(b).display === 'none' && document.getElementById('still-anchored') !== null"),
                        QStringLiteral("true")));
    delete docOff;

    Settings::self()->setTocLevels({1, 2, 3, 4, 5}); // leave the default
}

// The heavy engines (KaTeX, highlight.js, js-yaml) are inlined into a page
// only when the mirrored text can use them. Introducing such content while the
// preview is open triggers a one-time page rebuild; switching to a document
// that no longer needs an engine drops it again. KaTeX assets are faked in a
// temporary data dir so the test does not depend on tools/fetch-assets.py
// having been run.
void RenderFeaturesTest::enginesAreLoadedOnlyWhenTheTextNeedsThem()
{
    QVERIFY(m_dir.isValid());
    const QByteArray oldDataDir = qgetenv("KATEXDOWN_DATA_DIR");
    const auto writeAsset = [this](const QString &name, const QByteArray &body) {
        QFile f(m_dir.filePath(name));
        return f.open(QIODevice::WriteOnly) && f.write(body) == body.size();
    };
    QVERIFY(writeAsset(QStringLiteral("katex.min.js"),
                       QByteArrayLiteral("window.katex = { renderToString: function () { return ''; } };\n")));
    // A no-op plugin: enough for markdown-it to accept the engine wiring.
    QVERIFY(writeAsset(QStringLiteral("texmath.min.js"),
                       QByteArrayLiteral("window.texmath = function () { return function () {}; };\n")));
    qputenv("KATEXDOWN_DATA_DIR", m_dir.path().toUtf8());

    // Plain prose: no engine at all is loaded into the page.
    KTextEditor::Document *doc = openDocument(QStringLiteral("# plain\n\nsome prose.\n"));
    auto preview = makePreview(doc);
    QVERIFY(waitForPageText(preview.get(), QLatin1String("some prose")));
    // typeof window.katex is useless here: the page always carries a
    // <style id="katex"> element, and element ids become window globals.
    // Check whether the engine script itself made it into the page instead.
    const QString katexMarker = QStringLiteral(
        "(function () { for (var i = 0; i < document.scripts.length; ++i) { "
        "if (document.scripts[i].textContent.indexOf('renderToString') >= 0) return 'loaded'; } return 'absent'; })()");
    QCOMPARE(evalJs(preview.get(), katexMarker), QStringLiteral("absent"));
    QCOMPARE(evalJs(preview.get(), QStringLiteral("typeof window.hljs")), QStringLiteral("undefined"));
    QCOMPARE(evalJs(preview.get(), QStringLiteral("typeof window.jsyaml")), QStringLiteral("undefined"));

    // Math typed in later: the page rebuilds itself once and KaTeX arrives.
    doc->setText(QStringLiteral("# math\n\ninline $x^2$ now.\n"));
    QVERIFY(waitForCond(preview.get(), katexMarker, QStringLiteral("loaded")));
    QVERIFY(waitForCond(preview.get(), QStringLiteral("typeof window.texmath"), QStringLiteral("function")));
    QCOMPARE(evalJs(preview.get(), QStringLiteral("typeof window.hljs")), QStringLiteral("undefined"));

    // A fenced code block brings the syntax highlighter along (KaTeX stays).
    doc->setText(QStringLiteral("# code\n\n```js\nvar a = 1;\n```\n\nand $x$ again.\n"));
    QVERIFY(waitForCond(preview.get(), QStringLiteral("typeof window.hljs"), QStringLiteral("object")));
    QVERIFY(waitForCond(preview.get(), katexMarker, QStringLiteral("loaded")));

    // Front matter pulls in js-yaml — and a document that needs none of the
    // engines drops them all on the rebuild.
    doc->setText(QStringLiteral("---\ntitle: t\n---\n\nbody text\n"));
    QVERIFY(waitForCond(preview.get(), QStringLiteral("typeof window.jsyaml"), QStringLiteral("object")));
    QVERIFY(waitForCond(preview.get(), katexMarker, QStringLiteral("absent")));
    QVERIFY(waitForCond(preview.get(), QStringLiteral("typeof window.hljs"), QStringLiteral("undefined")));

    delete doc;
    if (oldDataDir.isNull()) {
        qunsetenv("KATEXDOWN_DATA_DIR");
    } else {
        qputenv("KATEXDOWN_DATA_DIR", oldDataDir);
    }
}

// A fence larger than the highlight cap (kdxHljsMaxChars in preview.js) is
// rendered as plain escaped text instead of being tokenized into thousands of
// hljs spans: a single oversized block (a pasted bundle or dump) must not be
// able to blow the DOM up. The text, the <pre> box and HTML escaping stay
// intact; only the syntax colors are lost, while normal-sized fences keep
// their highlighting.
void RenderFeaturesTest::oversizedFenceRendersPlain()
{
    // Read the cap from the loaded page (single source of truth is preview.js).
    KTextEditor::Document *probe = openDocument(QStringLiteral("# probe\n"));
    auto preview = makePreview(probe);
    QVERIFY(waitForPageText(preview.get(), QLatin1String("probe")));
    const int cap = evalJs(preview.get(), QStringLiteral("Number(window.kdxHljsMaxChars)")).toInt();
    QVERIFY2(cap > 1000, qPrintable(QStringLiteral("highlight cap missing/too small: %1").arg(cap)));

    // One ordinary fence plus one fence comfortably above the cap. Both use a
    // language highlight.js knows and tokens that would normally highlight
    // (var/function/string/number); the giant one also carries raw HTML that
    // must stay inert text.
    const QString giantUnit = QStringLiteral("var function omega2 = \"<script>alert(1)</script>&amp;\";\n");
    const QString giantBody = giantUnit.repeated(cap / giantUnit.size() + 40)
        + QStringLiteral("var tailToken = true; // zzend\n");
    QVERIFY2(giantBody.size() > cap + 200, qPrintable(QStringLiteral("giant fence only %1 chars").arg(giantBody.size())));

    KTextEditor::Document *doc = openDocument(QStringLiteral("# fences\n\n```js\nvar smallToken = 1;\n```\n\n```js\n%1```\n")
                                                  .arg(giantBody));
    preview->attachDocument(doc, nullptr);
    QVERIFY(waitForPageText(preview.get(), QLatin1String("fences")));
    // The fenced content brings highlight.js in (one-time rebuild), then the
    // giant body renders as plain text.
    QVERIFY(waitForCond(preview.get(), QStringLiteral("typeof window.hljs"), QStringLiteral("object")));
    QVERIFY(waitForCond(preview.get(),
                        QStringLiteral("(function () { var t = document.getElementById('content').innerText; "
                                       "return t.indexOf('smallToken') >= 0 && t.indexOf('tailToken') >= 0 ? 'done' : 'wait'; })()"),
                        QStringLiteral("done")));

    // Exactly two <pre> blocks (the fences did not merge or vanish) and the
    // giant body arrived verbatim, HTML entities decoded back by the DOM.
    QCOMPARE(evalJs(preview.get(), QStringLiteral("document.querySelectorAll('#content pre').length")), QStringLiteral("2"));
    QCOMPARE(evalJs(preview.get(),
                    QStringLiteral("(function () { var ps = document.querySelectorAll('#content pre code'); "
                                   "return ps.length === 2 && ps[1].textContent.indexOf('var tailToken = true; // zzend') >= 0 "
                                   "&& ps[1].textContent.indexOf('<script>alert(1)</script>&amp;') >= 0 ? 'ok' : 'bad'; })()")),
             QStringLiteral("ok"));

    // The giant fence must not have introduced a live <script> or any hljs
    // token span; the small fence must still be highlighted.
    QCOMPARE(evalJs(preview.get(), QStringLiteral("document.querySelectorAll('#content script').length")), QStringLiteral("0"));
    QCOMPARE(evalJs(preview.get(),
                    QStringLiteral("(function () { var ps = document.querySelectorAll('#content pre'); "
                                   "return [ps[0].querySelectorAll('[class*=hljs-]').length > 0, "
                                   "ps[1].querySelectorAll('[class*=hljs-]').length === 0].join(','); })()")),
             QStringLiteral("true,true"));

    // Character-for-character round trip through the giant block: the cap only
    // drops colors, never content.
    QCOMPARE(evalJs(preview.get(), QStringLiteral("document.querySelectorAll('#content pre code')[1].textContent.length")),
             QString::number(giantBody.size()));

    delete doc;
    delete probe;
}

QTEST_MAIN(RenderFeaturesTest)

#include "renderfeaturestest.moc"
