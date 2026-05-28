#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>

#include <KXMLGUIClient>
#include <KTextEditor/SessionConfigInterface>

class QAction;
class PreviewWidget;

namespace KTextEditor
{
class Document;
class MainWindow;
class View;
}

/**
 * One instance per Kate main window. Registers the toolbar/menu action, keeps it
 * enabled only for Markdown documents, and opens (or re-focuses) a preview tab.
 */
class PluginView : public QObject, public KXMLGUIClient, public KTextEditor::SessionConfigInterface
{
    Q_OBJECT
    Q_INTERFACES(KTextEditor::SessionConfigInterface)
public:
    PluginView(QObject *plugin, KTextEditor::MainWindow *mainWindow);
    ~PluginView() override;

    // Persist which documents have a preview open so the tabs come back with the session.
    void readSessionConfig(const KConfigGroup &config) override;
    void writeSessionConfig(KConfigGroup &config) override;

private Q_SLOTS:
    void showPreview();
    void updateActionState();
    void onWidgetRemoved(QWidget *widget);

private:
    bool currentIsMarkdown() const;
    PreviewWidget *openPreview(KTextEditor::Document *doc, KTextEditor::View *view);
    KTextEditor::View *viewForDocument(KTextEditor::Document *doc) const;

    KTextEditor::MainWindow *m_mainWindow = nullptr;
    QAction *m_action = nullptr;
    QHash<KTextEditor::Document *, QPointer<PreviewWidget>> m_previews;
};
