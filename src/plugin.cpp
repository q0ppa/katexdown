#include "plugin.h"
#include "configpage.h"
#include "pluginview.h"

#include <KPluginFactory>

K_PLUGIN_FACTORY_WITH_JSON(KatdownPluginFactory, "katdown.json", registerPlugin<KatdownPlugin>();)

KatdownPlugin::KatdownPlugin(QObject *parent, const QVariantList &args)
    : KTextEditor::Plugin(parent)
{
    Q_UNUSED(args);
}

QObject *KatdownPlugin::createView(KTextEditor::MainWindow *mainWindow)
{
    return new PluginView(this, mainWindow);
}

int KatdownPlugin::configPages() const
{
    return 1;
}

KTextEditor::ConfigPage *KatdownPlugin::configPage(int number, QWidget *parent)
{
    if (number != 0) {
        return nullptr;
    }
    return new ConfigPage(parent);
}

#include "plugin.moc"
