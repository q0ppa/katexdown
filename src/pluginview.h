#pragma once

#include <QObject>
#include <QPointer>

#include "settings.h"

#include <KXMLGUIClient>
#include <KTextEditor/Plugin>
#include <KTextEditor/SessionConfigInterface>

class QAction;
class QEvent;
class PreviewWidget;
class QWidget;

namespace KTextEditor
{
class Document;
class MainWindow;
class View;
}

/**
 * One instance per Kate main window. Owns a "Katexdown" tool view — a panel
 * beside the editor, like Kate's built-in preview — that mirrors the Markdown
 * document currently active in the editor.
 *
 * Memory model (see Settings::LoadingMode): the tool-view shell (a plain
 * QWidget) always exists so kate's sidebar/View-menu/session handling works,
 * but the expensive preview (a QWebEngineView with its own renderer process)
 * is created and destroyed according to the selected loading mode:
 *   LazyKeep    on first panel show, kept afterwards        (default)
 *   LazyUnload  on first panel show, destroyed on every hide
 *   Eager       at plugin load
 * Visibility is tracked through real show/hide events on the tool view, so
 * toggling via kate's own "Show Preview" menu entry or the sidebar button
 * obeys the same rules as the Katexdown action.
 */
class PluginView : public QObject, public KXMLGUIClient, public KTextEditor::SessionConfigInterface
{
    Q_OBJECT
    Q_INTERFACES(KTextEditor::SessionConfigInterface)
public:
    PluginView(KTextEditor::Plugin *plugin, KTextEditor::MainWindow *mainWindow);
    ~PluginView() override;

    // Persist whether the preview panel was open, so lazy modes can create the
    // preview at the safe, early moment on the next start instead of in the
    // middle of kate's session-restore cascade (which is what produced the
    // duplicated preview on window re-open).
    void readSessionConfig(const KConfigGroup &config) override;
    void writeSessionConfig(KConfigGroup &config) override;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private Q_SLOTS:
    void togglePreview();
    void followActiveView();
    void updateActionState();
    void exportPreviewHtml();
    void onSettingsChanged();
    void activatePanel();

private:
    bool currentIsMarkdown(KTextEditor::Document *doc) const;
    bool ensureToolView();
    bool ensurePreview();
    void destroyPreview();
    void onPanelShown();
    void onPanelHidden();
    Settings::LoadingMode loadingMode() const;

    KTextEditor::MainWindow *m_mainWindow = nullptr;
    KTextEditor::Plugin *m_plugin = nullptr;
    QAction *m_action = nullptr;
    QAction *m_exportAction = nullptr;
    // QPointer so a kate-side removal of the panel can never leave a dangling
    // pointer behind.
    QPointer<QWidget> m_toolView;
    QPointer<PreviewWidget> m_preview;
    bool m_panelVisible = false;
    // Whether the panel was open in the previous session (persisted via
    // SessionConfigInterface); decoupled from m_panelVisible so a kate-window
    // close (which hides the panel) does not forget that it was open.
    bool m_sessionVisible = false;
};
