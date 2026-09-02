#include "plugin.h"
#include "configpage.h"
#include "pluginview.h"

#include <KPluginFactory>

K_PLUGIN_FACTORY_WITH_JSON(KatexdownPluginFactory, "katexdown.json", registerPlugin<KatexdownPlugin>();)

KatexdownPlugin::KatexdownPlugin(QObject *parent, const QVariantList &args)
    : KTextEditor::Plugin(parent)
{
    Q_UNUSED(args);
}

QObject *KatexdownPlugin::createView(KTextEditor::MainWindow *mainWindow)
{
    return new PluginView(this, mainWindow);
}

int KatexdownPlugin::configPages() const
{
    return 1;
}

KTextEditor::ConfigPage *KatexdownPlugin::configPage(int number, QWidget *parent)
{
    if (number != 0) {
        return nullptr;
    }
    return new ConfigPage(parent);
}

#include "plugin.moc"
