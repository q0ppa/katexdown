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
    void relativeCssResolvesAgainstDataDir();
    void outlineListsConfiguredHeadings();

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

QTEST_MAIN(RenderFeaturesTest)

#include "renderfeaturestest.moc"
