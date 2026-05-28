#include "pluginview.h"
#include "previewwidget.h"

#include <QAction>
#include <QFile>
#include <QIcon>
#include <QKeySequence>
#include <QUrl>

#include <KActionCollection>
#include <KLocalizedString>
#include <KXMLGUIFactory>
#include <KTextEditor/Document>
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
    KTextEditor::Document *doc = view->document();

    auto existing = m_previews.value(doc);
    if (existing) {
        m_mainWindow->activateWidget(existing);
        return;
    }

    auto *preview = new PreviewWidget(m_mainWindow, view, doc);
    if (!m_mainWindow->addWidget(preview)) {
        delete preview;
        return;
    }
    m_mainWindow->activateWidget(preview);
    m_previews.insert(doc, preview);

    connect(doc, &QObject::destroyed, this, [this, doc]() {
        m_previews.remove(doc);
    });
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
