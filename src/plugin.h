#pragma once

#include <KTextEditor/Plugin>

/**
 * Entry point for the Markdown Preview plugin. Creates a per-main-window view and
 * exposes one configuration page.
 */
class MarkdownPreviewPlugin : public KTextEditor::Plugin
{
    Q_OBJECT
public:
    explicit MarkdownPreviewPlugin(QObject *parent, const QVariantList &args = QVariantList());

    QObject *createView(KTextEditor::MainWindow *mainWindow) override;

    int configPages() const override;
    KTextEditor::ConfigPage *configPage(int number, QWidget *parent) override;
};
