// Verifies the optional, runtime-cached features: LaTeX rendering (KaTeX +
// markdown-it-texmath from the data dir) and user custom CSS. Both degrade to
// no-ops when the assets/settings are absent, which is what the earlier
// lifecycle tests rely on.

#include "katexdownpaths.h"
#include "previewwidget.h"
#include "settings.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QDebug>
#include <QDeadlineTimer>
#include <QFile>
#include <QImage>
#include <QKeyEvent>
#include <QStringList>
#include <QWidget>
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

bool waitForPageText(PreviewWidget *preview, const QString &needle)
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
    void codeCopyButtonCanBeDisabled();
    void codeCopyButtonCopiesViaHost();
    void selectionCopyShapesPlainText();
    void selectionCopyOfMathKeepsTheLatex();
    void ctrlCCopiesSelectionToClipboard();
    void previewClaimsItsKeysFromTheShortcutMap();
    void imageModesControlDecoding();
    void parkingPreservesTheImageBox();
    void rememberedImageBoxesShapeAFreshReload();
    void relativeCssResolvesAgainstDataDir();
    void outlineListsConfiguredHeadings();
    void fragmentLinksJumpToSections();
    void enginesAreLoadedOnlyWhenTheTextNeedsThem();
    void numberMathLoadsKatexEngine();
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

    // A document whose only formulas are numeric must render too: the engine
    // gate has to load KaTeX for "$2^{192} - 2^{32}$" and a bare "$2$"
    // exactly like for "$x^2$". Regression: the gate treated a "$" after a
    // digit as currency and required a LaTeX-ish letter inside the pair, so
    // such a document never loaded KaTeX and its dollars stayed literal.
    KTextEditor::Document *numDoc = openDocument(QStringLiteral(
        "# numerics\n\n这是 NVIDIA CUDA toolkit 的默认方案，其周期为 $2^{192} - 2^{32}$。\n\nA bare $2$ alone.\n"));
    auto numPreview = makePreview(numDoc);
    QVERIFY(waitForPageText(numPreview.get(), QLatin1String("numerics")));
    QCOMPARE(evalJs(numPreview.get(), QStringLiteral("typeof katex")), QStringLiteral("object"));
    const int numInline = evalJs(numPreview.get(), QStringLiteral("document.querySelectorAll('#content .katex').length")).toInt();
    QVERIFY2(numInline >= 2, qPrintable(QStringLiteral("numeric math not rendered (katex nodes: %1)").arg(numInline)));
    QCOMPARE(evalJs(numPreview.get(), QStringLiteral("document.querySelectorAll('.katex-error').length")).toInt(), 0);
    delete numDoc;

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

// Every code block carries a copy button (on by default); the setting
// removes/adds it live and, because the button is a DOM node that export
// serializes, decides whether an exported .html carries it too.
void RenderFeaturesTest::codeCopyButtonCanBeDisabled()
{
    QVERIFY(m_dir.isValid());
    Settings::self()->setCodeCopyButton(true);
    KTextEditor::Document *doc = openDocument(QStringLiteral("# copy\n\n```js\nvar x = 1;\n```\n\ntext after.\n"));
    auto preview = makePreview(doc);
    QVERIFY(waitForPageText(preview.get(), QLatin1String("text after")));

    QVERIFY(waitForCond(preview.get(),
                        QStringLiteral("document.querySelectorAll('#content pre .kdx-copy-btn').length"),
                        QStringLiteral("1")));
    // The button is a sibling of <code>, not part of it: copying the code
    // takes exactly the fence text.
    QCOMPARE(evalJs(preview.get(), QStringLiteral("document.querySelector('#content pre code').textContent")),
             QStringLiteral("var x = 1;\n"));

    // Export serializes the button. Parse the serialized HTML to count real
    // nodes — the embedded preview.js source mentions the class too.
    const QString serializedButtons = QStringLiteral(
        "(function () { var d = new DOMParser().parseFromString(window.__serializedHtml(), 'text/html'); "
        "return String(d.querySelectorAll('.kdx-copy-btn').length); })()");
    QCOMPARE(evalJs(preview.get(), serializedButtons), QStringLiteral("1"));

    // Disabled: the live page drops the button and export follows.
    Settings::self()->setCodeCopyButton(false);
    QVERIFY(waitForCond(preview.get(),
                        QStringLiteral("document.querySelectorAll('#content pre .kdx-copy-btn').length"),
                        QStringLiteral("0")));
    QCOMPARE(evalJs(preview.get(), serializedButtons), QStringLiteral("0"));
    delete doc;

    Settings::self()->setCodeCopyButton(true); // leave the default for later tests
}

// Clicking a copy button puts the block's text on the system clipboard through
// the host's Qt-clipboard bridge (QWebChannel), not through the renderer's own
// clipboard APIs — which need document focus / permission and silently do
// nothing in some sessions. This is the behavior the button exists for.
void RenderFeaturesTest::codeCopyButtonCopiesViaHost()
{
    QVERIFY(m_dir.isValid());
    Settings::self()->setCodeCopyButton(true);
    QApplication::clipboard()->clear();

    KTextEditor::Document *doc = openDocument(QStringLiteral("# copy\n\n```js\nvar copied = 42;\n```\n"));
    auto preview = makePreview(doc);
    QVERIFY(waitForPageText(preview.get(), QLatin1String("copied")));
    QVERIFY(waitForCond(preview.get(),
                        QStringLiteral("document.querySelectorAll('#content .kdx-copy-btn').length"),
                        QStringLiteral("1")));
    // The QWebChannel transport must be on the page for the bridge to exist.
    QVERIFY(waitForCond(preview.get(),
                        QStringLiteral("(window.qt && window.qt.webChannelTransport) ? 'transport' : 'no-transport'"),
                        QStringLiteral("transport")));

    evalJs(preview.get(), QStringLiteral("document.querySelector('#content .kdx-copy-btn').click(); 'clicked'"));

    const QString expected = QStringLiteral("var copied = 42;\n");
    QDeadlineTimer deadline(10000);
    while (!deadline.hasExpired() && QApplication::clipboard()->text() != expected) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    QCOMPARE(QApplication::clipboard()->text(), expected);

    delete doc;
}

// Ctrl+C on a selection hands the page's structured plain text to the host.
// The page (__kdxSelectionText) is what shapes it: block boundaries become
// blank lines, list items get markers and indentation, tables become
// tab-separated rows, code keeps its text verbatim, and interactive chrome
// (code copy button, outline control) never leaks into the clipboard.
void RenderFeaturesTest::selectionCopyShapesPlainText()
{
    KTextEditor::Document *doc = openDocument(QStringLiteral(
        "# Title 1\n"
        "\n"
        "Hello **world**, this is a paragraph.\n"
        "\n"
        "- alpha\n"
        "- beta\n"
        "  - nested\n"
        "\n"
        "1. one\n"
        "2. two\n"
        "\n"
        "- [x] done\n"
        "- [ ] todo\n"
        "\n"
        "```js\n"
        "var x = 1;\n"
        "```\n"
        "\n"
        "| a | b |\n"
        "| - | - |\n"
        "| 1 | 2 |\n"
        "\n"
        "after\n"));
    auto preview = makePreview(doc);
    QVERIFY(waitForPageText(preview.get(), QLatin1String("after")));

    // Select the whole body, like Ctrl+A does: this is also what proves the
    // interactive chrome (which lives outside #content) is skipped.
    const QString selectAll = QStringLiteral(
        "var r = document.createRange(); r.selectNodeContents(document.body);"
        "var s = getSelection(); s.removeAllRanges(); s.addRange(r); 'selected'");
    QCOMPARE(evalJs(preview.get(), selectAll), QStringLiteral("selected"));
    const QString all = evalJs(preview.get(), QStringLiteral("window.__kdxSelectionText()"));
    QCOMPARE(all,
             QStringLiteral("Title 1\n"
                            "\n"
                            "Hello world, this is a paragraph.\n"
                            "\n"
                            "- alpha\n"
                            "- beta\n"
                            "  - nested\n"
                            "\n"
                            "1. one\n"
                            "2. two\n"
                            "\n"
                            "- [x] done\n"
                            "- [ ] todo\n"
                            "\n"
                            "var x = 1;\n"
                            "\n"
                            "a\tb\n"
                            "1\t2\n"
                            "\n"
                            "after"));
    // The copy button and the outline control are chrome, not document text.
    QVERIFY(!all.contains(QLatin1String("Copy")));

    // A selection inside one block takes exactly the selected characters.
    const QString partial = QStringLiteral(
        "var t = document.querySelector('#content p strong').firstChild;"
        "var r = document.createRange(); r.setStart(t, 0); r.setEnd(t, 5);"
        "var s = getSelection(); s.removeAllRanges(); s.addRange(r); 'selected'");
    QCOMPARE(evalJs(preview.get(), partial), QStringLiteral("selected"));
    QCOMPARE(evalJs(preview.get(), QStringLiteral("window.__kdxSelectionText()")), QStringLiteral("world"));

    // Nothing selected: empty, so the host leaves the clipboard alone.
    evalJs(preview.get(), QStringLiteral("getSelection().removeAllRanges(); 'cleared'"));
    QCOMPARE(evalJs(preview.get(), QStringLiteral("window.__kdxSelectionText()")), QString());

    delete doc;
}

// A KaTeX formula renders twice (accessible MathML + visual HTML spans); a
// copied selection must contribute the LaTeX source exactly once, from the
// MathML annotation, not both DOM copies.
void RenderFeaturesTest::selectionCopyOfMathKeepsTheLatex()
{
    const QString dataDir = katexdownpaths::dataDir();
    if (!QFile::exists(dataDir + QStringLiteral("/katex.min.js")) || !QFile::exists(dataDir + QStringLiteral("/texmath.min.js"))) {
        QSKIP("KaTeX assets not found in the data dir; run tools/fetch-assets.py");
    }

    KTextEditor::Document *doc = openDocument(QStringLiteral(
        "# math\n\ninline $x^2 + y^2$ here.\n\n$$\n\\int_0^1 x\\,dx = \\frac{1}{2}\n$$\n"));
    auto preview = makePreview(doc);
    QVERIFY(waitForPageText(preview.get(), QLatin1String("math")));
    QVERIFY(waitForCond(preview.get(),
                        QStringLiteral("document.querySelectorAll('#content .katex').length"),
                        QStringLiteral("2")));

    const QString select = QStringLiteral(
        "var r = document.createRange(); r.selectNodeContents(document.getElementById('content'));"
        "var s = getSelection(); s.removeAllRanges(); s.addRange(r); 'selected'");
    QCOMPARE(evalJs(preview.get(), select), QStringLiteral("selected"));

    const QString text = evalJs(preview.get(), QStringLiteral("window.__kdxSelectionText()"));
    QCOMPARE(text.count(QLatin1String("x^2 + y^2")), 1);
    QCOMPARE(text.count(QLatin1String("\\int_0^1 x\\,dx = \\frac{1}{2}")), 1);
    QVERIFY2(text.contains(QLatin1String("inline x^2 + y^2 here.")), qPrintable(text));
    delete doc;
}

// Qt runs the shortcut map before the focus widget sees a key, so a
// window-context Kate action (Copy, Select All, cursor movement) would take
// Ctrl+C / Ctrl+A / the arrow keys first and the preview would never get them:
// Ctrl+C here would copy the *editor's* selection (wrong paragraph, wrong
// offsets, or nothing at all). The preview must accept ShortcutOverride for
// the keys it uses. A synthetic sendEvent() test alone cannot catch this,
// because it bypasses the shortcut map — so this asserts the claim directly.
void RenderFeaturesTest::previewClaimsItsKeysFromTheShortcutMap()
{
    KTextEditor::Document *doc = openDocument(QStringLiteral("# Title 1\n\nHello world.\n"));
    auto preview = makePreview(doc);
    QVERIFY(waitForPageText(preview.get(), QLatin1String("Hello world")));
    preview->show();

    auto *view = preview->findChild<QWebEngineView *>();
    QVERIFY(view);
    QWidget *proxy = nullptr;
    QDeadlineTimer deadline(10000);
    while (!deadline.hasExpired()) {
        if ((proxy = view->focusProxy())) {
            break;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    QVERIFY2(proxy, "the web view never created its focus proxy");

    auto overrideAccepted = [&](int key, Qt::KeyboardModifiers mods) {
        QKeyEvent ovr(QEvent::ShortcutOverride, key, mods);
        ovr.setAccepted(false);
        QCoreApplication::sendEvent(proxy, &ovr);
        return ovr.isAccepted();
    };
    // The keys the preview owns (copy, select all, reading keys) are claimed.
    QVERIFY(overrideAccepted(Qt::Key_C, Qt::ControlModifier));
    QVERIFY(overrideAccepted(Qt::Key_Insert, Qt::ControlModifier));
    QVERIFY(overrideAccepted(Qt::Key_A, Qt::ControlModifier));
    QVERIFY(overrideAccepted(Qt::Key_Down, Qt::NoModifier));
    QVERIFY(overrideAccepted(Qt::Key_PageDown, Qt::NoModifier));
    // A key the preview does not use stays unclaimed, so Kate's shortcut map
    // still gets it.
    QVERIFY(!overrideAccepted(Qt::Key_B, Qt::ControlModifier));

    delete doc;
}

// The keyboard path: Ctrl+C in the preview is intercepted on the web view's
// focus proxy and puts __kdxSelectionText() on Qt's clipboard (the renderer's
// own clipboard write is unreliable, see codeCopyButtonCopiesViaHost).
void RenderFeaturesTest::ctrlCCopiesSelectionToClipboard()
{
    QApplication::clipboard()->clear();
    KTextEditor::Document *doc = openDocument(QStringLiteral("# Title 1\n\nHello world.\n"));
    auto preview = makePreview(doc);
    QVERIFY(waitForPageText(preview.get(), QLatin1String("Hello world")));
    preview->show(); // the web view's render widget (and its focus proxy) needs a window

    auto *view = preview->findChild<QWebEngineView *>();
    QVERIFY(view);
    QVERIFY(waitForCond(preview.get(),
                        QStringLiteral("(function () { var r = document.createRange();"
                                       "r.selectNodeContents(document.getElementById('content'));"
                                       "var s = getSelection(); s.removeAllRanges(); s.addRange(r);"
                                       "return 'selected'; })()"),
                        QStringLiteral("selected")));

    // installInputFilter attaches the key filter to the web view's focus proxy.
    QWidget *proxy = nullptr;
    QDeadlineTimer deadline(10000);
    while (!deadline.hasExpired()) {
        if ((proxy = view->focusProxy())) {
            break;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    QVERIFY2(proxy, "the web view never created its focus proxy");

    QKeyEvent press(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
    QCoreApplication::sendEvent(proxy, &press);

    const QString expected = QStringLiteral("Title 1\n\nHello world.");
    QDeadlineTimer wait(10000);
    while (!wait.hasExpired() && QApplication::clipboard()->text() != expected) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    QCOMPARE(QApplication::clipboard()->text(), expected);

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

    // The maintenance charge counts only the image decodes the *reader* caused.
    // Eager mode decodes everything by itself, so not even a forced re-decode
    // after a scroll input is charged — a recycle there would only decode the
    // same set again (this is the accounting that kept an idle image-heavy
    // preview reloading itself forever).
    evalJs(preview.get(), QStringLiteral("window.dispatchEvent(new WheelEvent('wheel')); 'ok'"));
    evalJs(preview.get(), QStringLiteral(
        "(function () { var i = document.querySelector('#content img'); var s = i.getAttribute('src'); "
        "i.removeAttribute('src'); i.setAttribute('src', s); return 'ok'; })()"));
    {
        QDeadlineTimer settle(900);
        while (!settle.hasExpired()) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        }
    }
    QCOMPARE(evalJs(preview.get(), QStringLiteral("window.__kdxDecodeStats()")), QStringLiteral("0:0"));

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

    // Scrolling back to the top restores the image in front of the viewport —
    // and that decode *is* the reader's, so it is charged (unlike the eager
    // mode re-decode above). The wheel event stands for the reader's scroll
    // input: a programmatic scrollTo alone is not a viewport move the page can
    // attribute to the reader (and a re-render resets that attribution, see
    // rerender() in preview.js).
    evalJs(preview.get(), QStringLiteral("window.dispatchEvent(new WheelEvent('wheel')); 'ok'"));
    evalJs(preview.get(), QStringLiteral("window.scrollTo(0, 0); 'ok'"));
    QVERIFY(waitForCond(preview.get(),
                        QStringLiteral("(function () { var i = document.querySelector('#content img'); return ((i.dataset.kdx || '') === '') + '|' + (i.src.indexOf('data:image/png') === 0); })()"),
                        QStringLiteral("true|true")));
    QVERIFY(waitForCond(preview.get(),
                        QStringLiteral("String(parseInt(window.__kdxDecodeStats(), 10) > 0)"),
                        QStringLiteral("true")));

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

// A page reload (the maintenance recycle) starts from the markdown, which
// carries only an image's src: a fresh page would lay the document out as if
// every image were zero-height until it decodes, and the recycle's restored
// scroll position would clamp to that much shorter document — the "it
// refreshed and now I am at a different place" symptom. The host carries the
// boxes the previous page recorded across the reload (__kdxRecycleState →
// __setImageDims); this pins the page side of that: the boxes it reports, the
// box a fresh page lays out with even when nothing decodes, the author's own
// dimensions winning, and a remembered box being only provisional (the real
// decode is authoritative).
void RenderFeaturesTest::rememberedImageBoxesShapeAFreshReload()
{
    QVERIFY(m_dir.isValid());
    // A real 64x32 PNG on disk. "far.png" deliberately stays *unloaded* by the
    // page below: Chromium loads a lazy image immediately once it is in the
    // memory cache, so only a src the page has never fetched can be counted on
    // to sit there undecoded.
    for (const char *name : {"pic.png", "far.png"}) {
        QImage img(64, 32, QImage::Format_RGB32);
        img.fill(QColor(30, 80, 120));
        QVERIFY(img.save(m_dir.filePath(QLatin1String(name)), "PNG"));
    }
    // Tall enough that an image below it is far outside Chromium's own
    // lazy-loading distance (so it really never decodes while we look).
    QString filler;
    for (int i = 0; i < 1200; ++i) {
        filler += QStringLiteral("filler paragraph %1 above the image\n\n").arg(i);
    }
    const QString state = QStringLiteral("(function () { var i = document.querySelector('#content img'); "
                                         "return i.naturalWidth + '|' + i.getAttribute('width') + '|' + i.getAttribute('height') + '|' "
                                         "+ (document.body.scrollHeight > 1000); })()");

    Settings::self()->setImageMode(Settings::Adaptive);
    // The document lives next to the image (a relative src resolves against
    // the document's folder, exactly like in Kate).
    const QString docPath = m_dir.filePath(QStringLiteral("doc.md"));
    {
        QFile f(docPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QVERIFY(f.write("# imgs\n\n![pic](pic.png)\n") > 0);
    }
    KTextEditor::Document *doc = KTextEditor::Editor::instance()->createDocument(nullptr);
    QVERIFY(doc->openUrl(QUrl::fromLocalFile(docPath)));
    auto preview = makePreview(doc);
    preview->resize(800, 600);
    preview->show();
    QVERIFY(waitForPageText(preview.get(), QLatin1String("imgs")));
    // Near the viewport: it decodes and its real box is what a capture reports.
    QVERIFY(waitForCond(preview.get(), state, QStringLiteral("64|null|null|false")));
    const QString recycled = evalJs(preview.get(), QStringLiteral("window.__kdxRecycleState()"));
    QVERIFY2(recycled.contains(QStringLiteral("[64,32]")), qPrintable(recycled));
    QVERIFY2(recycled.contains(QStringLiteral("\"y\"")), qPrintable(recycled));

    // A fresh page for the same document, now long: "pic" sits at the top and
    // decodes, while "far" is ~48000 px down and has never been fetched, so it
    // stays undecoded. Both are shaped by a deliberately wrong remembered box,
    // which makes clear where each size came from: a decode says 64x32.
    evalJs(preview.get(), QStringLiteral("window.__setImageDims({\"pic.png\": [11,7], \"far.png\": [13,5]}); 'ok'"));
    doc->setText(QStringLiteral("![pic](pic.png)\n\n%1\n![far](far.png)\n\nend of document\n").arg(filler));
    QVERIFY(waitForPageText(preview.get(), QLatin1String("end of document")));

    const QString boxOf = QStringLiteral(
        "(function () { var i = document.images[%1]; "
        "return i.naturalWidth + '|' + i.getAttribute('width') + '|' + i.getAttribute('height') + '|' "
        "+ (i.dataset.kdxRemembered || 'no'); })()");
    // The real decode is authoritative and corrects the remembered box (11x7 ->
    // the real 64x32) — for *this* image, not for whichever one the loop
    // happened to visit last.
    QVERIFY(waitForCond(preview.get(), boxOf.arg(0), QStringLiteral("64|64|32|no")));
    // Nothing ever decoded the other one, so the only size it can have is the
    // remembered box: exactly what keeps a reload's layout intact.
    QCOMPARE(evalJs(preview.get(), boxOf.arg(1)), QStringLiteral("0|13|5|1"));

    // Author-pinned dimensions are never overwritten by a remembered box.
    const QString pinned = evalJs(preview.get(), QStringLiteral(
        "(function () { window.__setImageDims({\"pic.png\": [11,7]}); "
        "window.__setMarkdown('<img src=\"pic.png\" width=\"10\" height=\"20\">'); "
        "var i = document.querySelector('#content img'); "
        "return i.getAttribute('width') + '|' + i.getAttribute('height'); })()"));
    QCOMPARE(pinned, QStringLiteral("10|20"));

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

// Math whose content starts with (or is entirely) digits is real math to
// markdown-it-texmath ("$2^{192} - 2^{32}$", a bare "$2$"), so it must pull
// the KaTeX engine in exactly like "$x^2$" does — while prose dollar amounts
// that texmath itself keeps literal ("价格 $5 元", "$10 and $20 are prices")
// must not. Regression: the engine gate used to treat a "$" right after a
// digit as currency and required a LaTeX-ish letter in the pair, so numeric
// math never loaded KaTeX and stayed literal forever — and because the
// one-time rebuild check used the same scan, typing such math into a plain
// document never self-healed either.
void RenderFeaturesTest::numberMathLoadsKatexEngine()
{
    QVERIFY(m_dir.isValid());
    const QByteArray oldDataDir = qgetenv("KATEXDOWN_DATA_DIR");
    const auto writeAsset = [this](const QString &name, const QByteArray &body) {
        QFile f(m_dir.filePath(name));
        return f.open(QIODevice::WriteOnly) && f.write(body) == body.size();
    };
    QVERIFY(writeAsset(QStringLiteral("katex.min.js"),
                       QByteArrayLiteral("window.katex = { renderToString: function () { return ''; } };\n")));
    QVERIFY(writeAsset(QStringLiteral("texmath.min.js"),
                       QByteArrayLiteral("window.texmath = function () { return function () {}; };\n")));
    qputenv("KATEXDOWN_DATA_DIR", m_dir.path().toUtf8());

    const QString katexMarker = QStringLiteral(
        "(function () { for (var i = 0; i < document.scripts.length; ++i) { "
        "if (document.scripts[i].textContent.indexOf('renderToString') >= 0) return 'loaded'; } return 'absent'; })()");

    // Currency-ish prose that texmath itself would keep literal must not pull
    // KaTeX in: a lone amount (no closing dollar) and a pair whose closing $
    // sits before a digit.
    KTextEditor::Document *cur = openDocument(QStringLiteral("价格 $5 元。\n\n$10 and $20 are prices.\n"));
    auto previewCur = makePreview(cur);
    QVERIFY(waitForPageText(previewCur.get(), QStringLiteral("价格")));
    QVERIFY(waitForCond(previewCur.get(), katexMarker, QStringLiteral("absent")));

    // Numeric math typed in later: the text grows into the KaTeX engine and
    // the page rebuilds itself once — the user's exact scenario.
    cur->setText(QStringLiteral("其周期为 $2^{192} - 2^{32}$。\n\nAnd a bare $2$ alone.\n"));
    QVERIFY(waitForCond(previewCur.get(), katexMarker, QStringLiteral("loaded")));
    QCOMPARE(evalJs(previewCur.get(), QStringLiteral("typeof window.texmath")), QStringLiteral("function"));

    // Opening a document whose only math is numeric loads KaTeX on first load.
    KTextEditor::Document *doc = openDocument(QStringLiteral("成本 $10^3$ 元？\n"));
    auto preview = makePreview(doc);
    QVERIFY(waitForPageText(preview.get(), QStringLiteral("成本")));
    QVERIFY(waitForCond(preview.get(), katexMarker, QStringLiteral("loaded")));

    delete doc;
    delete cur;
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
