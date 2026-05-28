#pragma once

#include <KTextEditor/Plugin>

/**
 * Entry point for the Katdown plugin. Creates a per-main-window view and
 * exposes one configuration page.
 */
class KatdownPlugin : public KTextEditor::Plugin
{
    Q_OBJECT
public:
    explicit KatdownPlugin(QObject *parent, const QVariantList &args = QVariantList());

    QObject *createView(KTextEditor::MainWindow *mainWindow) override;

    int configPages() const override;
    KTextEditor::ConfigPage *configPage(int number, QWidget *parent) override;
};
