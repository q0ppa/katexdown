#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>

#include <KXMLGUIClient>

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
class PluginView : public QObject, public KXMLGUIClient
{
    Q_OBJECT
public:
    PluginView(QObject *plugin, KTextEditor::MainWindow *mainWindow);
    ~PluginView() override;

private Q_SLOTS:
    void showPreview();
    void updateActionState();
    void onWidgetRemoved(QWidget *widget);

private:
    bool currentIsMarkdown() const;

    KTextEditor::MainWindow *m_mainWindow = nullptr;
    QAction *m_action = nullptr;
    QHash<KTextEditor::Document *, QPointer<PreviewWidget>> m_previews;
};
