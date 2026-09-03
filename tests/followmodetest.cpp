// Drives the real PluginView against a fake Kate main-window host to verify the
// follow-mode behavior end to end, in one continuous session with a single
// preview panel (exactly how the plugin is used in kate):
//   - the action creates a tool view (a panel, not a covering tab)
//   - focusing another Markdown document re-targets the preview
//   - non-Markdown documents leave the last content on screen
//   - opening a file (as a link click does) makes the preview follow it
//   - closing the previewed document freezes its last content
//   - the action toggles the panel
//
// The fake mirrors how Kate's MainWindow wrapper relays calls: every
// KTextEditor::MainWindow method invokeMethod()s the host object by name, and
// kate emits the wrapper's viewChanged signal when the active view changes.

#include "pluginview.h"
#include "previewwidget.h"

#include <QAction>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <QVBoxLayout>
#include <QWebEnginePage>
#include <QWebEngineView>

#include <KTextEditor/Document>
#include <KTextEditor/Editor>
#include <KTextEditor/MainWindow>
#include <KTextEditor/Plugin>
#include <KTextEditor/View>
#include <KConfig>
#include <KConfigGroup>
#include <KXMLGUIBuilder>
#include <KXMLGUIFactory>

namespace
{
// Dummy plugin: PluginView only needs it for ownership/tool-view registration.
class DummyPlugin : public KTextEditor::Plugin
{
    Q_OBJECT
public:
    using KTextEditor::Plugin::Plugin;
    QObject *createView(KTextEditor::MainWindow *) override
    {
        return nullptr;
    }
};

// Fake counterpart of Kate's main window: implements the slots KTextEditor's
// MainWindow wrapper relays to, and emits viewChanged like kate does. A QWidget
// so it can anchor a KXMLGUIBuilder for the factory.
class FakeHost : public QWidget
{
    Q_OBJECT
public:
    explicit FakeHost(QWidget *parent = nullptr)
        : QWidget(parent)
        , m_builder(new KXMLGUIBuilder(this))
        , factory(new KXMLGUIFactory(m_builder, this))
    {
    }

    KXMLGUIBuilder *m_builder = nullptr;
    KXMLGUIFactory *factory = nullptr;
    KTextEditor::View *active = nullptr;
    QList<KTextEditor::View *> allViews;
    QWidget *toolView = nullptr;
    bool toolShown = false;
    int openUrlCalls = 0;

    void setActive(KTextEditor::View *view)
    {
        active = view;
        Q_EMIT viewChanged(view);
    }

Q_SIGNALS:
    void viewChanged(KTextEditor::View *view);

public Q_SLOTS:
    QWidget *window()
    {
        return this;
    }
    KXMLGUIFactory *guiFactory()
    {
        return factory;
    }
    QList<KTextEditor::View *> views()
    {
        return allViews;
    }
    KTextEditor::View *activeView()
    {
        return active;
    }
    KTextEditor::View *activateView(KTextEditor::Document *document)
    {
        for (KTextEditor::View *view : std::as_const(allViews)) {
            if (view->document() == document) {
                setActive(view);
                return view;
            }
        }
        return nullptr;
    }
    // Opens (or focuses) the document, like Kate's openUrl: the fragment of a
    // "doc.md#section" link is dropped (the file itself opens), the document
    // becomes the active view, and viewChanged fires.
    KTextEditor::View *openUrl(const QUrl &url, const QString &encoding)
    {
        Q_UNUSED(encoding);
        ++openUrlCalls;
        const QUrl docUrl = url.adjusted(QUrl::RemoveFragment);
        for (KTextEditor::View *view : std::as_const(allViews)) {
            if (view->document()->url() == docUrl) {
                setActive(view);
                return view;
            }
        }
        KTextEditor::Document *doc = KTextEditor::Editor::instance()->createDocument(nullptr);
        doc->openUrl(docUrl);
        KTextEditor::View *view = doc->createView(nullptr);
        allViews << view;
        setActive(view);
        return view;
    }
    QWidget *createToolView(KTextEditor::Plugin *plugin,
                            const QString &identifier,
                            KTextEditor::MainWindow::ToolViewPosition pos,
                            const QIcon &icon,
                            const QString &text)
    {
        Q_UNUSED(plugin);
        Q_UNUSED(pos);
        Q_UNUSED(icon);
        Q_UNUSED(text);
        // Like kate's ToolView: the returned frame already owns a layout (it
        // hosts a hidden toolbar). Katexdown must plug into this layout; a second
        // one would never be installed and the preview would collapse.
        auto *w = new QWidget(this); // owned by the host, like kate's MDI
        w->setObjectName(identifier);
        auto *layout = new QVBoxLayout(w);
        layout->setContentsMargins(0, 0, 0, 0);
        toolView = w;
        return w;
    }
    bool showToolView(QWidget *widget)
    {
        toolShown = (widget == toolView);
        if (toolShown) {
            widget->show(); // like kate's MDI; drives the plugin's show events
        }
        return toolShown;
    }
    bool hideToolView(QWidget *widget)
    {
        if (widget == toolView) {
            toolShown = false;
            widget->hide();
        }
        return !toolShown;
    }
};
} // namespace

// Evaluate one expression in the preview's page and hand back its value.
static QString evalJs(PreviewWidget *preview, const QString &code)
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

// Poll for the preview widget: lazy modes create it one event-loop turn after
// the panel becomes visible.
static PreviewWidget *previewIn(QWidget *toolView)
{
    if (!toolView) {
        return nullptr;
    }
    QDeadlineTimer deadline(10000);
    while (!deadline.hasExpired()) {
        if (auto *p = toolView->findChild<PreviewWidget *>()) {
            return p;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return nullptr;
}

// The web engine page behind a preview (lifecycle state checks).
static QWebEnginePage *previewPage(PreviewWidget *preview)
{
    auto *view = preview->findChild<QWebEngineView *>();
    return view ? view->page() : nullptr;
}

static QString pageText(PreviewWidget *preview)
{
    return evalJs(preview, QStringLiteral("document.getElementById('content').innerText"));
}

static bool waitForText(PreviewWidget *preview, QLatin1String needle)
{
    QDeadlineTimer deadline(20000);
    while (!deadline.hasExpired()) {
        if (pageText(preview).contains(needle)) {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    return false;
}

// Poll a JS expression until it evaluates to the expected string.
static bool waitForCond(PreviewWidget *preview, const QString &code, const QString &expected)
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

class FollowModeTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void onePanelFollowsTheActiveDocumentEndToEnd();
    void fragmentLinksFollowAndScroll();
    void loadingModesGovernPreviewLifetime();
    void sessionRestoreCreatesPreviewEarly();
    void keptModesFreezeAndReleaseWhileClosed();

private:
    QTemporaryDir m_dir;
    QString m_alphaPath;
    QString m_betaPath;
    QString m_plainPath;
};

void FollowModeTest::initTestCase()
{
    qRegisterMetaType<KTextEditor::MainWindow::ToolViewPosition>("KTextEditor::MainWindow::ToolViewPosition");
    QVERIFY(m_dir.isValid());

    const auto writeFile = [this](const QString &name, const QString &body) -> QString {
        const QString path = m_dir.filePath(name);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly) || f.write(body.toUtf8()) <= 0) {
            return QString();
        }
        f.close();
        return path;
    };
    m_alphaPath = writeFile(QStringLiteral("alpha.md"), QStringLiteral("# alpha\n\nthe alpha body.\n"));
    QVERIFY(!m_alphaPath.isEmpty());
    m_betaPath = writeFile(QStringLiteral("beta.md"), QStringLiteral("# beta\n\nthe beta body.\n"));
    QVERIFY(!m_betaPath.isEmpty());
    m_plainPath = writeFile(QStringLiteral("notes.txt"), QStringLiteral("just text\n"));
    QVERIFY(!m_plainPath.isEmpty());
}

void FollowModeTest::onePanelFollowsTheActiveDocumentEndToEnd()
{
    Settings::self()->setLoadingMode(Settings::LazyKeep);
    auto host = std::make_unique<FakeHost>();
    host->show(); // the "kate window" must be visible for show/hide logic to run
    QTest::qWait(50);
    KTextEditor::MainWindow wrapper(host.get());
    connect(host.get(), &FakeHost::viewChanged, &wrapper, &KTextEditor::MainWindow::viewChanged);

    // The plugin session lives in an inner scope: PluginView (a GUI client of
    // the host's factory) must be destroyed before the host so the factory
    // never tears down containers while its window is dying.
    {
        DummyPlugin plugin(nullptr);
        PluginView pluginView(&plugin, &wrapper);

        KTextEditor::Document *alpha = KTextEditor::Editor::instance()->createDocument(nullptr);
        QVERIFY(alpha->openUrl(QUrl::fromLocalFile(m_alphaPath)));
        QTRY_VERIFY(alpha->text().contains(QLatin1String("alpha body")));
        KTextEditor::View *alphaView = alpha->createView(nullptr);
        host->allViews << alphaView;

        KTextEditor::Document *beta = KTextEditor::Editor::instance()->createDocument(nullptr);
        QVERIFY(beta->openUrl(QUrl::fromLocalFile(m_betaPath)));
        QTRY_VERIFY(beta->text().contains(QLatin1String("beta body")));
        KTextEditor::View *betaView = beta->createView(nullptr);
        host->allViews << betaView;

        // The panel shell exists from the start (cheap), but the heavy preview
        // widget is created lazily on the first open (default LazyKeep mode).
        QVERIFY(host->toolView);
        QVERIFY(!host->toolShown);
        QVERIFY(!host->toolView->findChild<PreviewWidget *>());
        host->setActive(alphaView);
        QTest::qWait(300);
        QVERIFY(!host->toolView->findChild<PreviewWidget *>()); // still nothing: never opened

        QVERIFY(QMetaObject::invokeMethod(&pluginView, "togglePreview"));
        QVERIFY(host->toolShown);
        PreviewWidget *preview = previewIn(host->toolView);
        QVERIFY(preview);
        QVERIFY(waitForText(preview, QLatin1String("alpha body")));

        // The preview must fill the panel: when the tool view is resized the
        // preview widget follows (regression: a competing layout on the
        // tool-view frame left the preview without managed geometry).
        host->toolView->show();
        host->toolView->resize(700, 500);
        QTest::qWait(200);
        QVERIFY2(preview->width() >= 600,
                 qPrintable(QStringLiteral("preview does not fill the panel, width is %1").arg(preview->width())));

        // Focus beta: the single preview follows the active document.
        host->setActive(betaView);
        QVERIFY(waitForText(preview, QLatin1String("beta body")));

        // And back to alpha.
        host->setActive(alphaView);
        QVERIFY(waitForText(preview, QLatin1String("alpha body")));

        // A non-Markdown document leaves the last content frozen.
        KTextEditor::Document *plain = KTextEditor::Editor::instance()->createDocument(nullptr);
        QVERIFY(plain->openUrl(QUrl::fromLocalFile(m_plainPath)));
        QTRY_VERIFY(plain->text().contains(QLatin1String("just text")));
        KTextEditor::View *plainView = plain->createView(nullptr);
        host->allViews << plainView;
        host->setActive(plainView);
        QTest::qWait(600);
        QVERIFY(pageText(preview).contains(QLatin1String("alpha body")));

        // A link click opens the target through MainWindow::openUrl(), and because
        // the target becomes the active document the preview follows it.
        wrapper.openUrl(QUrl::fromLocalFile(m_betaPath));
        QCOMPARE(host->openUrlCalls, 1);
        QVERIFY(waitForText(preview, QLatin1String("beta body")));

        // The action toggles the panel off and back on; LazyKeep keeps the
        // preview alive across the hide, so the same widget serves both times.
        QVERIFY(QMetaObject::invokeMethod(&pluginView, "togglePreview"));
        QVERIFY(!host->toolShown);
        QVERIFY(host->toolView->findChild<PreviewWidget *>() != nullptr);
        QVERIFY(QMetaObject::invokeMethod(&pluginView, "togglePreview"));
        QVERIFY(host->toolShown);
        QVERIFY(waitForText(preview, QLatin1String("beta body")));

        // Closing the editor tab of the previewed document freezes the preview on
        // the last content instead of going blank or crashing.
        beta->closeUrl();
        delete betaView;
        delete beta;
        QTest::qWait(800);
        QVERIFY(pageText(preview).contains(QLatin1String("beta body")));

        // Opening the same file again re-attaches and it goes live.
        KTextEditor::Document *betaAgain = KTextEditor::Editor::instance()->createDocument(nullptr);
        QVERIFY(betaAgain->openUrl(QUrl::fromLocalFile(m_betaPath)));
        QTRY_VERIFY(betaAgain->text().contains(QLatin1String("beta body")));
        KTextEditor::View *betaAgainView = betaAgain->createView(nullptr);
        host->allViews << betaAgainView;
        host->setActive(betaAgainView);
        betaAgain->setText(QStringLiteral("# beta\n\nthe beta body v2.\n"));
        QVERIFY(waitForText(preview, QLatin1String("beta body v2")));

        // PluginView's destructor deletes the tool view (and with it the
        // preview + web view) at the end of this scope, while the event loop
        // is still alive.
        delete betaAgain;
        delete plain;
        delete alpha;
    }

    // Let the tool-view deletion (triggered by ~PluginView above) settle, then
    // drop the host. Host teardown is deliberately skipped: destroying the
    // wrapper/factory chain crashes QtWebEngine's offscreen renderer on some
    // setups. The remaining plain widgets are reclaimed by the OS at exit.
    QTest::qWait(300);
    host.release();
}

// Fragment links ("[section](sub/doc.md#deep-section)") open the target in the
// editor like any link — and the preview, which follows the document the link
// just activated, then lands on the section the fragment names instead of the
// top of the file. A fragment link to the document the preview is already
// showing jumps in place. The editor side never scrolls: the section jump
// exists in the preview only.
void FollowModeTest::fragmentLinksFollowAndScroll()
{
    Settings::self()->setLoadingMode(Settings::LazyKeep);

    // The target lives in a subdirectory so the document switch goes through a
    // full page load (different folder, different base URL), and it is opened
    // by the click itself — Kate loads it asynchronously, which is exactly the
    // race the pending-anchor machinery must survive (a render can run before
    // the file's text has arrived).
    const QString fromPath = m_dir.filePath(QStringLiteral("frag-from.md"));
    {
        QFile f(fromPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QVERIFY(f.write(QStringLiteral("# Frag from\n\n[Deep section](sub/sectioned.md#deep-section)\n\nfrom body text.\n").toUtf8()) > 0);
    }
    QVERIFY(QDir(m_dir.path()).mkpath(QStringLiteral("sub")));
    QString sectioned = QStringLiteral("# Sectioned\n\nintroduction paragraph.\n\n");
    for (int i = 0; i < 60; ++i) {
        sectioned += QStringLiteral("Filler paragraph %1 giving the document enough height to scroll.\n\n").arg(i);
    }
    sectioned += QStringLiteral("## Deep Section\n\nsection body text.\n\n[Back to the top](sectioned.md#sectioned)\n");
    const QString sectionedPath = m_dir.filePath(QStringLiteral("sub/sectioned.md"));
    {
        QFile f(sectionedPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QCOMPARE(f.write(sectioned.toUtf8()), sectioned.size());
    }

    auto host = std::make_unique<FakeHost>();
    host->show();
    QTest::qWait(50);
    KTextEditor::MainWindow wrapper(host.get());
    connect(host.get(), &FakeHost::viewChanged, &wrapper, &KTextEditor::MainWindow::viewChanged);

    {
        DummyPlugin plugin(nullptr);
        PluginView pluginView(&plugin, &wrapper);

        KTextEditor::Document *from = KTextEditor::Editor::instance()->createDocument(nullptr);
        QVERIFY(from->openUrl(QUrl::fromLocalFile(fromPath)));
        QTRY_VERIFY(from->text().contains(QLatin1String("from body")));
        KTextEditor::View *fromView = from->createView(nullptr);
        host->allViews << fromView;
        host->setActive(fromView);

        QVERIFY(QMetaObject::invokeMethod(&pluginView, "togglePreview"));
        QVERIFY(host->toolShown);
        PreviewWidget *preview = previewIn(host->toolView);
        QVERIFY(preview);
        host->toolView->resize(700, 500);
        QTest::qWait(200);
        QVERIFY(waitForText(preview, QLatin1String("from body")));

        // Click the markdown link inside the preview. The click is handled like
        // a real navigation: the target opens through the host and the preview
        // follows it; once sectioned.md has rendered, the preview scrolls the
        // "Deep Section" heading into view and flashes it.
        QCOMPARE(evalJs(preview,
                        QStringLiteral("var a = document.querySelector('#content a[href=\"sub/sectioned.md#deep-section\"]'); "
                                       "a ? (a.click(), true) : false")),
                 QStringLiteral("true"));
        QVERIFY(waitForText(preview, QLatin1String("section body")));
        QCOMPARE(host->openUrlCalls, 1);
        QVERIFY(waitForCond(preview,
                            QStringLiteral("var h = document.getElementById('deep-section'); "
                                           "h !== null && h.classList.contains('kdx-outline-jumped')"),
                            QStringLiteral("true")));
        QVERIFY(waitForCond(preview,
                            QStringLiteral("window.scrollY > 300 ? 'jumped' : 'still-top'"),
                            QStringLiteral("jumped")));

        // A fragment link to the document the preview already shows (the
        // self-file form "sectioned.md#sectioned") jumps in place — the same
        // editor view is merely re-focused, no re-render — back to the top
        // heading, which flashes again.
        QTest::qWait(300); // let any trailing render of the switch settle
        QCOMPARE(evalJs(preview,
                        QStringLiteral("var a = document.querySelector('#content a[href=\"sectioned.md#sectioned\"]'); "
                                       "a ? (a.click(), true) : false")),
                 QStringLiteral("true"));
        QVERIFY(waitForCond(preview,
                            QStringLiteral("var h = document.getElementById('sectioned'); "
                                           "h !== null && h.classList.contains('kdx-outline-jumped')"),
                            QStringLiteral("true")));
        QVERIFY(waitForCond(preview,
                            QStringLiteral("window.scrollY < 50 ? 'back-at-top' : 'not-top'"),
                            QStringLiteral("back-at-top")));
        QCOMPARE(host->openUrlCalls, 2); // same document again: just re-focused

        delete from;
    }
    host.release();
}

// The loading modes decide when the heavy preview (web view) exists:
// LazyUnload destroys it whenever the panel hides; Eager creates it at plugin
// load (and on switching to Eager); the default LazyKeep creates on first show
// and keeps it across hides.
void FollowModeTest::loadingModesGovernPreviewLifetime()
{
    Settings::self()->setLoadingMode(Settings::LazyUnload);
    auto host = std::make_unique<FakeHost>();
    host->show();
    QTest::qWait(50);
    KTextEditor::MainWindow wrapper(host.get());
    connect(host.get(), &FakeHost::viewChanged, &wrapper, &KTextEditor::MainWindow::viewChanged);

    {
        DummyPlugin plugin(nullptr);
        PluginView pluginView(&plugin, &wrapper);

        KTextEditor::Document *alpha = KTextEditor::Editor::instance()->createDocument(nullptr);
        QVERIFY(alpha->openUrl(QUrl::fromLocalFile(m_alphaPath)));
        QTRY_VERIFY(alpha->text().contains(QLatin1String("alpha body")));
        KTextEditor::View *alphaView = alpha->createView(nullptr);
        host->allViews << alphaView;
        host->setActive(alphaView);

        // LazyUnload: nothing exists until the first open.
        QVERIFY(host->toolView);
        QVERIFY(!host->toolView->findChild<PreviewWidget *>());

        QVERIFY(QMetaObject::invokeMethod(&pluginView, "togglePreview"));
        QVERIFY(host->toolShown);
        QVERIFY(waitForText(previewIn(host->toolView), QLatin1String("alpha body")));

        // Closing the panel destroys the preview entirely (minimal memory).
        QVERIFY(QMetaObject::invokeMethod(&pluginView, "togglePreview"));
        QVERIFY(!host->toolShown);
        QTest::qWait(300);
        QVERIFY(!host->toolView->findChild<PreviewWidget *>());

        // kate's own "Show Preview" toggle (not the Katexdown action) also
        // recreates it lazily.
        host->showToolView(host->toolView);
        QVERIFY(host->toolShown);
        PreviewWidget *again = previewIn(host->toolView);
        QVERIFY(again);
        QVERIFY(waitForText(again, QLatin1String("alpha body")));
        host->hideToolView(host->toolView);
        QTest::qWait(300);
        QVERIFY(!host->toolView->findChild<PreviewWidget *>());

        // Switching to Eager while the panel is closed creates the preview
        // immediately (hidden), like starting kate in Eager mode would.
        Settings::self()->setLoadingMode(Settings::Eager);
        QVERIFY(host->toolView->findChild<PreviewWidget *>() != nullptr);

        QVERIFY(QMetaObject::invokeMethod(&pluginView, "togglePreview"));
        QVERIFY(host->toolShown);
        QVERIFY(waitForText(previewIn(host->toolView), QLatin1String("alpha body")));

        // Eager keeps the preview across a close.
        QVERIFY(QMetaObject::invokeMethod(&pluginView, "togglePreview"));
        QVERIFY(!host->toolShown);
        QTest::qWait(300);
        QVERIFY(host->toolView->findChild<PreviewWidget *>() != nullptr);

        Settings::self()->setLoadingMode(Settings::LazyKeep); // restore default
        delete alpha;
    }
    host.release();
}

// When the panel was open in the previous session, kate calls the plugin
// view's readSessionConfig() during startup. Lazy modes must use that early,
// pre-restore moment to create the preview (same timing as Eager) instead of
// creating it mid-restore — the mid-restore creation is what produced the
// duplicated preview on window re-open.
void FollowModeTest::sessionRestoreCreatesPreviewEarly()
{
    Settings::self()->setLoadingMode(Settings::LazyKeep);
    auto host = std::make_unique<FakeHost>();
    host->show();
    QTest::qWait(50);
    KTextEditor::MainWindow wrapper(host.get());
    connect(host.get(), &FakeHost::viewChanged, &wrapper, &KTextEditor::MainWindow::viewChanged);

    {
        DummyPlugin plugin(nullptr);
        PluginView pluginView(&plugin, &wrapper);

        KTextEditor::Document *alpha = KTextEditor::Editor::instance()->createDocument(nullptr);
        QVERIFY(alpha->openUrl(QUrl::fromLocalFile(m_alphaPath)));
        QTRY_VERIFY(alpha->text().contains(QLatin1String("alpha body")));
        KTextEditor::View *alphaView = alpha->createView(nullptr);
        host->allViews << alphaView;
        host->setActive(alphaView);

        // Nothing yet: panel never shown, no session restore.
        QVERIFY(!host->toolView->findChild<PreviewWidget *>());

        // Simulate kate restoring the session: readSessionConfig() reports the
        // panel was open, and the preview must be created right away.
        const QString cfgPath = m_dir.filePath(QStringLiteral("session.ini"));
        QFile::remove(cfgPath);
        KConfig cfg(cfgPath, KConfig::SimpleConfig);
        KConfigGroup grp(&cfg, QStringLiteral("Plugin:katexdown:MainWindow:0"));
        grp.writeEntry(QStringLiteral("PanelVisible"), true);
        pluginView.readSessionConfig(grp);

        QVERIFY2(host->toolView->findChild<PreviewWidget *>() != nullptr,
                 "lazy mode must create the preview early when the session had it open");

        // And the session flag is persisted back on shutdown.
        KConfigGroup out(&cfg, QStringLiteral("Plugin:katexdown:MainWindow:0"));
        pluginView.writeSessionConfig(out);
        QCOMPARE(out.readEntry(QStringLiteral("PanelVisible"), false), true);

        delete alpha;
    }
    host.release();
}

// Kept modes freeze the closed preview (no CPU; toggling back stays instant),
// and LazyKeep additionally releases the page once the panel has stayed
// closed past the idle delay: Qt discards the page (its renderer process
// exits) and reloads it on the next open, which re-runs the normal load
// pipeline — the widget survives, so content comes back without recreating
// anything. Eager freezes too but never releases while closed.
// KATEXDOWN_IDLE_DISCARD_MS shortens the release delay for this test.
void FollowModeTest::keptModesFreezeAndReleaseWhileClosed()
{
    qputenv("KATEXDOWN_IDLE_DISCARD_MS", "700");

    Settings::self()->setLoadingMode(Settings::LazyKeep);
    auto host = std::make_unique<FakeHost>();
    host->show();
    QTest::qWait(50);
    KTextEditor::MainWindow wrapper(host.get());
    connect(host.get(), &FakeHost::viewChanged, &wrapper, &KTextEditor::MainWindow::viewChanged);

    {
        DummyPlugin plugin(nullptr);
        PluginView pluginView(&plugin, &wrapper);

        KTextEditor::Document *alpha = KTextEditor::Editor::instance()->createDocument(nullptr);
        QVERIFY(alpha->openUrl(QUrl::fromLocalFile(m_alphaPath)));
        QTRY_VERIFY(alpha->text().contains(QLatin1String("alpha body")));
        KTextEditor::View *alphaView = alpha->createView(nullptr);
        host->allViews << alphaView;
        host->setActive(alphaView);

        QVERIFY(QMetaObject::invokeMethod(&pluginView, "togglePreview"));
        QVERIFY(host->toolShown);
        PreviewWidget *preview = previewIn(host->toolView);
        QVERIFY(preview);
        QVERIFY(waitForText(preview, QLatin1String("alpha body")));
        auto *page = previewPage(preview);
        QVERIFY(page);
        QTRY_VERIFY(int(page->lifecycleState()) == int(QWebEnginePage::LifecycleState::Active));

        // Closing the panel freezes the page: the renderer stays (toggling
        // back is instant) but the page stops doing work.
        QVERIFY(QMetaObject::invokeMethod(&pluginView, "togglePreview"));
        QVERIFY(!host->toolShown);
        QTRY_VERIFY(int(page->lifecycleState()) == int(QWebEnginePage::LifecycleState::Frozen));

        // The panel stays closed past the (shortened) idle delay: the page is
        // released — Qt shuts its renderer process down (Discarded).
        QTRY_VERIFY(int(page->lifecycleState()) == int(QWebEnginePage::LifecycleState::Discarded));

        // Re-opening restores the page through an automatic reload; the same
        // widget serves it and the mirrored content comes back via loadFinished.
        QVERIFY(QMetaObject::invokeMethod(&pluginView, "togglePreview"));
        QVERIFY(host->toolShown);
        QVERIFY(waitForText(preview, QLatin1String("alpha body")));
        QTRY_VERIFY(int(page->lifecycleState()) == int(QWebEnginePage::LifecycleState::Active));

        delete alpha;
    }
    host.release();

    Settings::self()->setLoadingMode(Settings::Eager);
    auto hostEager = std::make_unique<FakeHost>();
    hostEager->show();
    QTest::qWait(50);
    KTextEditor::MainWindow wrapperEager(hostEager.get());
    connect(hostEager.get(), &FakeHost::viewChanged, &wrapperEager, &KTextEditor::MainWindow::viewChanged);

    {
        DummyPlugin plugin(nullptr);
        PluginView pluginView(&plugin, &wrapperEager);

        KTextEditor::Document *alpha = KTextEditor::Editor::instance()->createDocument(nullptr);
        QVERIFY(alpha->openUrl(QUrl::fromLocalFile(m_alphaPath)));
        QTRY_VERIFY(alpha->text().contains(QLatin1String("alpha body")));
        KTextEditor::View *alphaView = alpha->createView(nullptr);
        hostEager->allViews << alphaView;
        hostEager->setActive(alphaView);

        // Eager created the preview at plugin load (hidden); show and use it.
        PreviewWidget *preview = previewIn(hostEager->toolView);
        QVERIFY(preview);
        QVERIFY(QMetaObject::invokeMethod(&pluginView, "togglePreview"));
        QVERIFY(hostEager->toolShown);
        QVERIFY(waitForText(preview, QLatin1String("alpha body")));
        auto *page = previewPage(preview);
        QVERIFY(page);
        QTRY_VERIFY(int(page->lifecycleState()) == int(QWebEnginePage::LifecycleState::Active));

        QVERIFY(QMetaObject::invokeMethod(&pluginView, "togglePreview"));
        QVERIFY(!hostEager->toolShown);
        QTRY_VERIFY(int(page->lifecycleState()) == int(QWebEnginePage::LifecycleState::Frozen));

        // Eager never releases while closed: the page stays frozen even well
        // past the delay LazyKeep would have used to discard it.
        QTest::qWait(2500);
        QVERIFY(int(page->lifecycleState()) != int(QWebEnginePage::LifecycleState::Discarded));

        QVERIFY(QMetaObject::invokeMethod(&pluginView, "togglePreview"));
        QVERIFY(hostEager->toolShown);
        QVERIFY(waitForText(preview, QLatin1String("alpha body")));

        delete alpha;
    }
    hostEager.release();
    Settings::self()->setLoadingMode(Settings::LazyKeep); // restore the default
}

QTEST_MAIN(FollowModeTest)

#include "followmodetest.moc"
