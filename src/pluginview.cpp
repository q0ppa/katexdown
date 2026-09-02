#include "pluginview.h"
#include "previewwidget.h"

#include <QAction>
#include <QDebug>
#include <QDesktopServices>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QIcon>
#include <QKeySequence>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <KActionCollection>
#include <KConfigGroup>
#include <KLocalizedString>
#include <KXMLGUIFactory>
#include <KTextEditor/Document>
#include <KTextEditor/MainWindow>
#include <KTextEditor/View>

namespace
{
QString readUiRc()
{
    QFile f(QStringLiteral(":/katexdown/ui.rc"));
    if (!f.open(QIODevice::ReadOnly)) {
        return QString();
    }
    return QString::fromUtf8(f.readAll());
}
} // namespace

PluginView::PluginView(KTextEditor::Plugin *plugin, KTextEditor::MainWindow *mainWindow)
    : QObject(plugin)
    , KXMLGUIClient()
    , m_mainWindow(mainWindow)
    , m_plugin(plugin)
{
    setComponentName(QStringLiteral("katexdown"), i18n("Katexdown"));
    // Always visible when kate is started from a terminal: proves the plugin
    // loaded and which version is running. Set KATEXDOWN_DEBUG=1 for details.
#ifdef KATEXDOWN_VERSION
    qInfo().noquote() << QStringLiteral("[katexdown] version %1 loaded (KATEXDOWN_DEBUG=1 for verbose logging)").arg(QStringLiteral(KATEXDOWN_VERSION));
#else
    qInfo() << "[katexdown] plugin view created (dev build without version macro; KATEXDOWN_DEBUG=1 for verbose logging)";
#endif

    m_action = actionCollection()->addAction(QStringLiteral("katexdown_show"));
    m_action->setText(i18n("Preview"));
    m_action->setToolTip(i18n("Toggle the GitHub-styled preview panel of the active Markdown document"));
    m_action->setIcon(QIcon::fromTheme(QStringLiteral("text-markdown"), QIcon::fromTheme(QStringLiteral("view-preview"))));
    actionCollection()->setDefaultShortcut(m_action, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M));
    connect(m_action, &QAction::triggered, this, &PluginView::togglePreview);

    m_exportAction = actionCollection()->addAction(QStringLiteral("katexdown_export"));
    m_exportAction->setText(i18n("Export HTML…"));
    m_exportAction->setToolTip(i18n("Save the rendered preview, with all included CSS, as a standalone HTML file"));
    m_exportAction->setIcon(QIcon::fromTheme(QStringLiteral("document-export")));
    connect(m_exportAction, &QAction::triggered, this, &PluginView::exportPreviewHtml);

    setXML(readUiRc());
    m_mainWindow->guiFactory()->addClient(this);

    connect(m_mainWindow, &KTextEditor::MainWindow::viewChanged, this, &PluginView::followActiveView);
    connect(m_mainWindow, &KTextEditor::MainWindow::viewChanged, this, &PluginView::updateActionState);
    connect(Settings::self(), &Settings::changed, this, &PluginView::onSettingsChanged);

    // The tool-view shell (plain QWidget, negligible cost) always exists so
    // kate's sidebar button, View menu entry and session restore work. The
    // heavy web view inside it obeys the loading mode.
    ensureToolView();

    if (loadingMode() == Settings::Eager) {
        ensurePreview();
    }
    followActiveView();
    updateActionState();
}

PluginView::~PluginView()
{
    m_mainWindow->guiFactory()->removeClient(this);
    // The tool view is ours to delete (kate's own preview plugin does the
    // same); this unregisters it from the MDI and destroys any preview +
    // web view while the event loop is still running.
    delete m_toolView;
}

Settings::LoadingMode PluginView::loadingMode() const
{
    return Settings::self()->loadingMode();
}

bool PluginView::currentIsMarkdown(KTextEditor::Document *doc) const
{
    if (!doc) {
        return false;
    }
    if (doc->highlightingMode().compare(QLatin1String("Markdown"), Qt::CaseInsensitive) == 0) {
        return true;
    }
    const QString path = doc->url().path();
    return path.endsWith(QLatin1String(".md"), Qt::CaseInsensitive) || path.endsWith(QLatin1String(".markdown"), Qt::CaseInsensitive)
        || path.endsWith(QLatin1String(".mkd"), Qt::CaseInsensitive);
}

// The panel shell: kate's "tool view" side area. Cheap; created once.
bool PluginView::ensureToolView()
{
    if (m_toolView) {
        return true;
    }
    m_toolView = m_mainWindow->createToolView(m_plugin,
                                              QStringLiteral("katexdown_preview"),
                                              KTextEditor::MainWindow::Right,
                                              QIcon::fromTheme(QStringLiteral("text-markdown"), QIcon::fromTheme(QStringLiteral("view-preview"))),
                                              i18n("Preview"));
    if (!m_toolView) {
        qWarning("katexdown: could not create the preview tool view");
        return false;
    }
    // kate's tool-view frames already own a layout (they host a hidden
    // toolbar). Plug into it instead of installing a competing one, or the
    // preview never gets managed geometry. Mirrors kate's own preview plugin.
    if (auto *layout = m_toolView->layout()) {
        layout->setContentsMargins(0, 0, 0, 0);
    }
    m_toolView->installEventFilter(this);
    m_panelVisible = m_toolView->isVisible();
    return true;
}

// The heavy part: a web view with its own renderer process.
bool PluginView::ensurePreview()
{
    if (m_preview) {
        return true;
    }
    if (!m_toolView) {
        return false;
    }
    KTextEditor::View *view = m_mainWindow->activeView();
    m_preview = new PreviewWidget(m_mainWindow, view, view ? view->document() : nullptr, m_toolView);
    if (qEnvironmentVariableIsSet("KATEXDOWN_DEBUG")) {
        qDebug() << "[katexdown] created preview widget" << m_preview
                 << "| children under tool view:" << m_toolView->findChildren<PreviewWidget *>().size()
                 << "| layout items:" << (m_toolView->layout() ? m_toolView->layout()->count() : -1);
    }
    // After an export, hand the standalone HTML file to the default browser.
    connect(m_preview, &PreviewWidget::exportFinished, this, [](const QString &path) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    });
    if (auto *layout = m_toolView->layout()) {
        layout->addWidget(m_preview);
    } else {
        auto *fallbackLayout = new QVBoxLayout(m_toolView);
        fallbackLayout->setContentsMargins(0, 0, 0, 0);
        fallbackLayout->addWidget(m_preview);
    }
    return true;
}

void PluginView::destroyPreview()
{
    if (qEnvironmentVariableIsSet("KATEXDOWN_DEBUG")) {
        qDebug() << "[katexdown] destroying preview widget" << m_preview.data();
    }
    delete m_preview; // QPointer clears itself; layout entry removed by QWidget
}

// Follow real visibility (our action, kate's View-menu toggle, the sidebar
// button and session restore all funnel through show/hide events).
bool PluginView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_toolView) {
        switch (event->type()) {
        case QEvent::Show:
            m_panelVisible = true;
            onPanelShown();
            return false;
        case QEvent::Hide:
            m_panelVisible = false;
            onPanelHidden();
            return false;
        default:
            break;
        }
    }
    return QObject::eventFilter(watched, event);
}

void PluginView::onPanelShown()
{
    m_sessionVisible = true;
    // Creating the web view in the middle of kate's session-restore/show
    // cascade produced a duplicated preview on window re-open (see the
    // LoadingMode notes). Defer creation to the next event-loop turn so it
    // always happens after the cascade settles. When the session says the
    // panel was open, readSessionConfig() already created it earlier at the
    // safe, pre-restore moment.
    activatePanel();
}

void PluginView::activatePanel()
{
    if (!m_panelVisible || m_preview) {
        followActiveView();
        return;
    }
    QTimer::singleShot(0, this, [this]() {
        if (!m_panelVisible || m_preview) {
            return;
        }
        ensurePreview();
        followActiveView();
    });
}

void PluginView::onPanelHidden()
{
    QWidget *win = m_mainWindow->window();
    if (!win || !win->isVisible()) {
        return; // kate window going away/minimized: not a panel close
    }
    m_sessionVisible = false;
    if (qEnvironmentVariableIsSet("KATEXDOWN_DEBUG")) {
        qDebug() << "[katexdown] panel HIDDEN, preview:" << m_preview.data()
                 << "| mode:" << int(loadingMode());
    }
    if (loadingMode() == Settings::LazyUnload) {
        destroyPreview();
    }
}

void PluginView::readSessionConfig(const KConfigGroup &config)
{
    m_sessionVisible = config.readEntry("PanelVisible", false);
    // Early creation at plugin-load time (before kate restores documents and
    // shows tool views) is the safe moment — identical timing to Eager mode.
    if (m_sessionVisible && !m_preview && m_toolView) {
        ensurePreview();
    }
}

void PluginView::writeSessionConfig(KConfigGroup &config)
{
    config.writeEntry("PanelVisible", m_sessionVisible);
}

void PluginView::togglePreview()
{
    if (!m_toolView) {
        return;
    }
    if (m_panelVisible) {
        m_mainWindow->hideToolView(m_toolView);
    } else {
        // The show event fires onPanelShown(); ensure it happens even if the
        // widget was already visible for some reason.
        m_mainWindow->showToolView(m_toolView);
        if (!m_preview) {
            ensurePreview();
        }
        followActiveView();
    }
}

// Re-target the preview at whatever Markdown document is active right now.
// Non-Markdown documents leave the previous content frozen on screen.
void PluginView::followActiveView()
{
    if (m_toolView && m_toolView->isVisible() && !m_preview) {
        // e.g. session restore showed the panel before any show event reached us
        ensurePreview();
    }
    if (!m_preview) {
        return;
    }
    KTextEditor::View *view = m_mainWindow->activeView();
    if (!view || !view->document()) {
        return;
    }
    if (currentIsMarkdown(view->document())) {
        m_preview->attachDocument(view->document(), view);
    }
}

void PluginView::onSettingsChanged()
{
    // Switching to Eager while the preview does not exist yet: create it now.
    // (Lazy modes need no immediate action; the next show/hide applies them.)
    if (loadingMode() == Settings::Eager && !m_preview) {
        ensurePreview();
    }
}

void PluginView::updateActionState()
{
    KTextEditor::View *view = m_mainWindow->activeView();
    const bool onMarkdown = view && currentIsMarkdown(view->document());
    m_exportAction->setEnabled(onMarkdown);
}

void PluginView::exportPreviewHtml()
{
    KTextEditor::View *view = m_mainWindow->activeView();
    if (!view || !view->document() || !currentIsMarkdown(view->document())) {
        return;
    }
    // Lazy modes may not have a preview yet; create one (it renders the
    // current document, and exportToFile waits for the page if needed).
    if (!m_preview) {
        ensurePreview();
        followActiveView();
    }
    const QUrl url = view->document()->url();
    QString defaultName = url.isValid() && !url.isEmpty() ? url.fileName() : QStringLiteral("preview.md");
    if (defaultName.endsWith(QLatin1String(".md"), Qt::CaseInsensitive) || defaultName.endsWith(QLatin1String(".markdown"), Qt::CaseInsensitive)) {
        defaultName.chop(defaultName.size() - defaultName.lastIndexOf(QLatin1Char('.')));
    }
    defaultName += QStringLiteral(".html");
    const QString startDir = url.isValid() && url.isLocalFile() ? QFileInfo(url.toLocalFile()).absolutePath() : QString();
    const QString path = QFileDialog::getSaveFileName(m_mainWindow->window(),
                                                      i18n("Export preview as HTML"),
                                                      startDir + QLatin1Char('/') + defaultName,
                                                      QStringLiteral("*.html"));
    if (path.isEmpty() || !m_preview) {
        return;
    }
    m_preview->exportToFile(path);
}
