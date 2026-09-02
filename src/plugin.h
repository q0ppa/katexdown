#pragma once

#include <KTextEditor/Plugin>

/**
 * Entry point for the Katexdown plugin. Creates a per-main-window view and
 * exposes one configuration page.
 */
class KatexdownPlugin : public KTextEditor::Plugin
{
    Q_OBJECT
public:
    explicit KatexdownPlugin(QObject *parent, const QVariantList &args = QVariantList());

    QObject *createView(KTextEditor::MainWindow *mainWindow) override;

    int configPages() const override;
    KTextEditor::ConfigPage *configPage(int number, QWidget *parent) override;
};
