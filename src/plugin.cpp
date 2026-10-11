#include "plugin.h"
#include "configpage.h"
#include "kdxchromiumflags.h"
#include "pluginview.h"
#include "settings.h"

#include <KPluginFactory>

K_PLUGIN_FACTORY_WITH_JSON(KatexdownPluginFactory, "katexdown.json", registerPlugin<KatexdownPlugin>();)

KatexdownPlugin::KatexdownPlugin(QObject *parent, const QVariantList &args)
    : KTextEditor::Plugin(parent)
{
    Q_UNUSED(args);
    // Best effort, as early as possible: Kate loads plugins before creating
    // any tool view, so unless another plugin already started QtWebEngine
    // this is before any renderer process exists and the renderer JS flags
    // (heap cap + --expose-gc, see kdxchromiumflags.h) still reach Chromium.
    // PreviewWidget repeats this before creating its own profile. No-op when
    // the user's own QTWEBENGINE_CHROMIUM_FLAGS already carries --js-flags.
    kdxchromiumflags::applyJsFlags(kdxchromiumflags::effectiveV8HeapCapMb(Settings::self()->v8HeapCapMb()));
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
