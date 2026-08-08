#include "pluginview.h"
#include "previewwidget.h"

#include <QAction>
#include <QFile>
#include <QIcon>
#include <QKeySequence>
#include <QSet>
#include <QTimer>
#include <QUrl>

#include <utility>

#include <KActionCollection>
#include <KConfigGroup>
#include <KLocalizedString>
#include <KXMLGUIFactory>
#include <KTextEditor/Application>
#include <KTextEditor/Document>
#include <KTextEditor/Editor>
#include <KTextEditor/MainWindow>
#include <KTextEditor/View>

namespace
{
QString readUiRc()
{
    QFile f(QStringLiteral(":/katdown/ui.rc"));
    if (!f.open(QIODevice::ReadOnly)) {
        return QString();
    }
    return QString::fromUtf8(f.readAll());
}
} // namespace

PluginView::PluginView(QObject *plugin, KTextEditor::MainWindow *mainWindow)
    : QObject(plugin)
    , KXMLGUIClient()
    , m_mainWindow(mainWindow)
{
    setComponentName(QStringLiteral("katdown"), i18n("Katdown"));

    m_action = actionCollection()->addAction(QStringLiteral("katdown_show"));
    m_action->setText(i18n("Preview"));
    m_action->setToolTip(i18n("Open a GitHub-styled preview of this Markdown document in a new tab"));
    m_action->setIcon(QIcon::fromTheme(QStringLiteral("text-markdown"), QIcon::fromTheme(QStringLiteral("view-preview"))));
    actionCollection()->setDefaultShortcut(m_action, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M));
    connect(m_action, &QAction::triggered, this, &PluginView::showPreview);

    setXML(readUiRc());

    m_mainWindow->guiFactory()->addClient(this);

    connect(m_mainWindow, &KTextEditor::MainWindow::viewChanged, this, &PluginView::updateActionState);
    connect(m_mainWindow, &KTextEditor::MainWindow::widgetRemoved, this, &PluginView::onWidgetRemoved);
    watchApplication();

    updateActionState();
}

void PluginView::watchApplication()
{
    KTextEditor::Application *app = KTextEditor::Editor::instance()->application();
    if (!app) {
        return;
    }
    connect(app, &KTextEditor::Application::documentCreated, this, &PluginView::onDocumentCreated, Qt::UniqueConnection);
    connect(app, &KTextEditor::Application::documentWillBeDeleted, this, &PluginView::onDocumentWillBeDeleted, Qt::UniqueConnection);
}

PluginView::~PluginView()
{
    m_mainWindow->guiFactory()->removeClient(this);
}

bool PluginView::currentIsMarkdown() const
{
    KTextEditor::View *view = m_mainWindow->activeView();
    if (!view) {
        return false;
    }
    KTextEditor::Document *doc = view->document();
    if (doc->highlightingMode().compare(QLatin1String("Markdown"), Qt::CaseInsensitive) == 0) {
        return true;
    }
    const QString path = doc->url().path();
    return path.endsWith(QLatin1String(".md"), Qt::CaseInsensitive) || path.endsWith(QLatin1String(".markdown"), Qt::CaseInsensitive)
        || path.endsWith(QLatin1String(".mkd"), Qt::CaseInsensitive);
}

void PluginView::updateActionState()
{
    m_action->setEnabled(currentIsMarkdown());
}

void PluginView::showPreview()
{
    KTextEditor::View *view = m_mainWindow->activeView();
    if (!view) {
        return;
    }
    if (PreviewWidget *preview = openPreview(view->document(), view)) {
        m_mainWindow->activateWidget(preview);
    }
}

PreviewWidget *PluginView::openPreview(KTextEditor::Document *doc, KTextEditor::View *view)
{
    if (!doc) {
        return nullptr;
    }
    if (PreviewWidget *existing = m_previews.value(doc)) {
        return existing;
    }

    auto *preview = new PreviewWidget(m_mainWindow, view, doc);
    if (!m_mainWindow->addWidget(preview)) {
        delete preview;
        return nullptr;
    }
    trackPreview(doc, preview);
    return preview;
}

void PluginView::trackPreview(KTextEditor::Document *doc, PreviewWidget *preview)
{
    m_previews.insert(doc, preview);
    // documentWillBeDeleted is the signal that carries the preview over to m_detached;
    // this only keeps a dangling key out of the hash if a host never emits it.
    connect(doc, &QObject::destroyed, this, [this, doc]() {
        m_previews.remove(doc);
    });
}

KTextEditor::View *PluginView::viewForDocument(KTextEditor::Document *doc) const
{
    const auto views = m_mainWindow->views();
    for (KTextEditor::View *view : views) {
        if (view->document() == doc) {
            return view;
        }
    }
    return nullptr;
}

void PluginView::writeSessionConfig(KConfigGroup &config)
{
    QStringList urls;
    // Frozen previews count too: they are still open tabs, and their document is
    // already gone by the time a session is saved on shutdown.
    auto collect = [&urls](const QPointer<PreviewWidget> &preview) {
        if (!preview) {
            return;
        }
        const QString url = preview->documentUrl().toString();
        if (!url.isEmpty() && !urls.contains(url)) {
            urls << url;
        }
    };
    for (const auto &preview : std::as_const(m_previews)) {
        collect(preview);
    }
    for (const auto &preview : std::as_const(m_detached)) {
        collect(preview);
    }
    config.writeEntry("previews", urls);
}

void PluginView::readSessionConfig(const KConfigGroup &config)
{
    const QStringList urls = config.readEntry("previews", QStringList());
    m_pendingPreviews = QSet<QString>(urls.cbegin(), urls.cend());
    if (m_pendingPreviews.isEmpty()) {
        return;
    }
    // Kate restores plugin session config before (and around) its documents, so the
    // documents we want previews for usually don't exist yet at this point. Defer the
    // lookup to the next event-loop turn; documents created later are picked up by the
    // documentCreated watch.
    watchApplication();
    QTimer::singleShot(0, this, &PluginView::rescanDocuments);
}

void PluginView::onDocumentCreated()
{
    // A freshly created document may not have its URL set yet; re-scan next turn.
    QTimer::singleShot(0, this, &PluginView::rescanDocuments);
}

// Match documents against previews waiting on them: previews frozen by an editor tab
// closing, and previews the session restore has not placed yet.
void PluginView::rescanDocuments()
{
    KTextEditor::Application *app = KTextEditor::Editor::instance()->application();
    if (!app) {
        return;
    }
    const auto docs = app->documents();
    for (KTextEditor::Document *doc : docs) {
        const QString url = doc->url().toString();
        if (url.isEmpty()) {
            continue;
        }
        if (PreviewWidget *preview = m_detached.take(url)) {
            if (!m_previews.contains(doc)) {
                preview->attachDocument(doc, viewForDocument(doc));
                trackPreview(doc, preview);
            }
        } else if (m_pendingPreviews.remove(url)) {
            openPreview(doc, viewForDocument(doc));
        }
    }
}

// The document dies with its editor tab; hand the preview its frozen copy and file it
// under the url so reopening the file re-attaches the same tab.
void PluginView::onDocumentWillBeDeleted(KTextEditor::Document *doc)
{
    PreviewWidget *preview = m_previews.take(doc);
    if (!preview) {
        return;
    }
    preview->detachDocument();
    const QString url = preview->documentUrl().toString();
    if (url.isEmpty()) {
        return;
    }
    m_detached.insert(url, preview);
}

void PluginView::onWidgetRemoved(QWidget *widget)
{
    for (auto it = m_previews.begin(); it != m_previews.end(); ++it) {
        if (it.value() == widget) {
            m_previews.erase(it);
            return;
        }
    }
    for (auto it = m_detached.begin(); it != m_detached.end(); ++it) {
        if (it.value() == widget) {
            m_detached.erase(it);
            return;
        }
    }
}
