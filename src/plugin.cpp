#include "plugin.h"
#include "configpage.h"
#include "pluginview.h"

#include <KPluginFactory>

K_PLUGIN_FACTORY_WITH_JSON(MarkdownPreviewPluginFactory, "markdownpreview.json", registerPlugin<MarkdownPreviewPlugin>();)

MarkdownPreviewPlugin::MarkdownPreviewPlugin(QObject *parent, const QVariantList &args)
    : KTextEditor::Plugin(parent)
{
    Q_UNUSED(args);
}

QObject *MarkdownPreviewPlugin::createView(KTextEditor::MainWindow *mainWindow)
{
    return new PluginView(this, mainWindow);
}

int MarkdownPreviewPlugin::configPages() const
{
    return 1;
}

KTextEditor::ConfigPage *MarkdownPreviewPlugin::configPage(int number, QWidget *parent)
{
    if (number != 0) {
        return nullptr;
    }
    return new ConfigPage(parent);
}

#include "plugin.moc"
