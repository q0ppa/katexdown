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
class QSplitter;
class QTimer;
class QWidget;
class QKeyEvent;

/**
 * Geometry helpers for the tool-view pane, kept free-function so the tests can
 * drive them against a plain QSplitter without a real Kate window. Kate's MDI
 * owns the pane width in its private splitters; these only nudge it.
 */
namespace PaneWidth
{
// Find the pane that holds `toolView`. This is the only place that knows
// Kate's MDI widget shape; if Kate changes it, adjust this function alone.
// False (and null outputs) when the host does not use that shape.
bool resolve(QWidget *toolView, QSplitter **own, QSplitter **outer);
// Width for `percent` of `total`, without the clamp that needs the sibling
// sizes (0 when there is nothing to divide).
int target(int total, int percent);
// Set `own`'s pane inside `outer` to `targetWidth`, taking the delta from the
// largest sibling so the other panes keep their sizes. Returns whether the
// splitter changed.
bool apply(QSplitter *outer, QSplitter *own, int targetWidth);
} // namespace PaneWidth

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
 * is created and released according to the selected loading mode:
 *   LazyKeep    on first panel show; while the panel is closed the page is
 *               frozen and, once the panel stays closed for a while, released
 *               entirely (renderer exits; re-opening reloads it)
 *   LazyUnload  on first panel show, destroyed on every hide
 *   Eager       at plugin load; frozen while closed, never released
 * Visibility is tracked through real show/hide events on the tool view, so
 * toggling via kate's own "Show Preview" menu entry or the sidebar button
 * obeys the same rules as the Katexdown action. The freeze/release behavior
 * lives in PreviewWidget (panelClosed/panelOpened); hiding only forwards the
 * policy decision here. When reading navigation is enabled, this object also
 * arbitrates Ctrl-pane focus using the actual editor/preview geometry.
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
    void moveFocusFromPreview(Qt::Key key);

private:
    bool currentIsMarkdown(KTextEditor::Document *doc) const;
    bool ensureToolView();
    bool ensurePreview();
    void destroyPreview();
    void onPanelShown();
    void onPanelHidden();
    // Opt-in width override (Settings::paneWidthOverride): resolve the splitter
    // that holds the tool-view stack and (re)apply the configured share.
    void schedulePaneWidthApply(int delayMs = 0);
    void applyPaneWidth();
    void updatePaneSplitterWatch(QSplitter *outer);
    Settings::LoadingMode loadingMode() const;
    bool handleEditorDirectionalEvent(QObject *watched, QEvent *event);
    QWidget *paneForWidget(QWidget *widget) const;
    QWidget *directionalPane(QWidget *source, Qt::Key key) const;

    KTextEditor::MainWindow *m_mainWindow = nullptr;
    KTextEditor::Plugin *m_plugin = nullptr;
    QAction *m_action = nullptr;
    QAction *m_exportAction = nullptr;
    // QPointer so a kate-side removal of the panel can never leave a dangling
    // pointer behind.
    QPointer<QWidget> m_toolView;
    QPointer<PreviewWidget> m_preview;
    bool m_panelVisible = false;
    // Width-override support: the outer splitter pane is watched for resizes so
    // a percentage stays proportional when the window is tiled/maximized.
    QTimer *m_paneWidthTimer = nullptr;
    QPointer<QSplitter> m_paneSplitter;
    bool m_paneWidthUnavailableWarned = false;
    // Whether the panel was open in the previous session (persisted via
    // SessionConfigInterface); decoupled from m_panelVisible so a kate-window
    // close (which hides the panel) does not forget that it was open.
    bool m_sessionVisible = false;
};
