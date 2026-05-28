#include "pluginview.h"
#include "previewwidget.h"

#include <QAction>
#include <QFile>
#include <QIcon>
#include <QKeySequence>
#include <QSet>
#include <QTimer>
#include <QUrl>

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
    QFile f(QStringLiteral(":/markdownpreview/ui.rc"));
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
    setComponentName(QStringLiteral("markdownpreview"), i18n("Markdown Preview"));

    m_action = actionCollection()->addAction(QStringLiteral("markdownpreview_show"));
    m_action->setText(i18n("Markdown Preview"));
    m_action->setToolTip(i18n("Open a GitHub-styled preview of this Markdown document in a new tab"));
    m_action->setIcon(QIcon::fromTheme(QStringLiteral("text-markdown"), QIcon::fromTheme(QStringLiteral("view-preview"))));
    actionCollection()->setDefaultShortcut(m_action, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M));
    connect(m_action, &QAction::triggered, this, &PluginView::showPreview);

    setXML(readUiRc());

    m_mainWindow->guiFactory()->addClient(this);

    connect(m_mainWindow, &KTextEditor::MainWindow::viewChanged, this, &PluginView::updateActionState);
    connect(m_mainWindow, &KTextEditor::MainWindow::widgetRemoved, this, &PluginView::onWidgetRemoved);

    updateActionState();
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
    m_previews.insert(doc, preview);

    connect(doc, &QObject::destroyed, this, [this, doc]() {
        m_previews.remove(doc);
    });
    return preview;
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
    for (auto it = m_previews.constBegin(); it != m_previews.constEnd(); ++it) {
        if (!it.value()) {
            continue;
        }
        const QUrl url = it.key()->url();
        if (!url.isEmpty()) {
            urls << url.toString();
        }
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
    // lookup to the next event-loop turn and keep watching for documents created later
    // until every saved preview has been matched.
    if (KTextEditor::Application *app = KTextEditor::Editor::instance()->application()) {
        connect(app, &KTextEditor::Application::documentCreated, this, &PluginView::onDocumentCreated, Qt::UniqueConnection);
    }
    QTimer::singleShot(0, this, &PluginView::restorePendingPreviews);
}

void PluginView::onDocumentCreated()
{
    // A freshly created document may not have its URL set yet; re-scan next turn.
    QTimer::singleShot(0, this, &PluginView::restorePendingPreviews);
}

void PluginView::restorePendingPreviews()
{
    KTextEditor::Application *app = KTextEditor::Editor::instance()->application();
    if (!app || m_pendingPreviews.isEmpty()) {
        return;
    }
    const auto docs = app->documents();
    for (KTextEditor::Document *doc : docs) {
        if (m_pendingPreviews.remove(doc->url().toString())) {
            openPreview(doc, viewForDocument(doc));
        }
    }
    if (m_pendingPreviews.isEmpty()) {
        disconnect(app, &KTextEditor::Application::documentCreated, this, &PluginView::onDocumentCreated);
    }
}

void PluginView::onWidgetRemoved(QWidget *widget)
{
    for (auto it = m_previews.begin(); it != m_previews.end(); ++it) {
        if (it.value() == widget) {
            m_previews.erase(it);
            return;
        }
    }
}
