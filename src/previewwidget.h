#pragma once

#include <QElapsedTimer>
#include <QPointer>
#include <QUrl>
#include <QWidget>

#include <KTextEditor/Document>
#include <KTextEditor/View>

class QWebEngineView;
class QWebEngineProfile;
class QWebEngineUrlRequestInterceptor;
class QTimer;
class QAction;
class QKeyEvent;
class QKeySequence;
class QMouseEvent;

namespace KTextEditor
{
class MainWindow;
}

/**
 * A single Markdown preview tab: a QWebEngineView fed by a self-contained HTML
 * document. Source text is pushed to the page on every change; theming is driven
 * from the active editor theme (Application mode) or GitHub's palette.
 *
 * The widget outlives its document: Kate destroys the document when its editor tab
 * closes, so the source text and url are mirrored here and the preview freezes on
 * that copy until the same file is opened again.
 */
class PreviewWidget : public QWidget
{
    Q_OBJECT
public:
    PreviewWidget(KTextEditor::MainWindow *mainWindow, KTextEditor::View *view, KTextEditor::Document *doc, QWidget *parent = nullptr);
    ~PreviewWidget() override;

    KTextEditor::Document *document() const
    {
        return m_doc;
    }

    // Valid after the document is gone; the tab is filed under this for re-attaching.
    QUrl documentUrl() const
    {
        return m_url;
    }

    void attachDocument(KTextEditor::Document *doc, KTextEditor::View *view);
    void detachDocument();

    // Write the currently rendered preview (with all included CSS) to path as
    // a standalone HTML file. Asynchronous; deferred until the page has
    // finished loading if it has not yet.
    bool exportToFile(const QString &path);

    // The tool view (panel) was closed / opened. The kept loading modes
    // (LazyKeep, Eager) freeze the page while the panel is closed, so a closed
    // preview costs no CPU and toggles back instantly (a frozen page still
    // accepts script pushes). With releaseWhenIdle (LazyKeep) the page is
    // additionally discarded once the panel has stayed closed for a while, so
    // a preview left closed does not keep a whole renderer process resident.
    // Opening returns the page to Active — Qt automatically reloads a
    // discarded page, which re-runs the normal load pipeline (loadFinished
    // re-renders the mirrored document).
    void panelClosed(bool releaseWhenIdle);
    void panelOpened();

Q_SIGNALS:
    // Emitted once the standalone HTML file has been written to path.
    void exportFinished(const QString &path);

public Q_SLOTS:
    void applyTheme();

protected:
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private Q_SLOTS:
    void scheduleRender();
    void onDocumentUrlChanged();
    void snapshotSource();
    void applyOutlineSettings();
    // Push the configured image decode mode (Settings::ImageMode) into the
    // page, which lazy-loads / unloads images accordingly (see preview.js).
    void applyImageMode();

private:
    void updateTitle();
    void render();
    void performExport(const QString &path);
    void runJs(const QString &code);
    void loadPage();
    void openLink(const QUrl &url);
    void applyMediaPolicy();
    QUrl baseUrl() const;
    // Assemble the self-contained page. engines is the bitmask (kEngine* in
    // previewwidget.cpp) of optional engines to inline: a page only carries the
    // engines its document text can use, picked by enginesForText() in loadPage().
    QString buildHtml(int engines) const;

    // Forward input the preview doesn't use back to Kate: QWebEngineView's render
    // widget swallows keys/mouse buttons before Kate's shortcut machinery sees them.
    void installInputFilter();
    bool forwardKeyEvent(QKeyEvent *event);
    bool forwardMouseEvent(QMouseEvent *event);
    QAction *kateActionFor(const QKeySequence &seq) const;
    // Periodic policy while the panel is closed: freeze once the page is idle,
    // discard once the panel has stayed closed past the idle delay (LazyKeep).
    void idleTick();

    QPointer<KTextEditor::MainWindow> m_mainWindow;
    QWebEngineView *m_web = nullptr;
    QWebEngineProfile *m_profile = nullptr;
    QWebEngineUrlRequestInterceptor *m_guard = nullptr;
    QTimer *m_debounce = nullptr;
    QTimer *m_idleTimer = nullptr;
    QPointer<KTextEditor::Document> m_doc;
    QPointer<KTextEditor::View> m_view;
    QPointer<QWidget> m_inputTarget;
    QUrl m_url;
    QString m_text;
    QString m_pendingExportPath;
    // True while the current page is loaded and its renderer is alive; cleared
    // while a page is loading and when the page is Discarded (its renderer is
    // gone until Qt reloads it on the next panel open).
    bool m_loaded = false;
    bool m_remoteApplied = false;
    // Which optional engines (kEngine* in previewwidget.cpp) the page that is
    // currently loaded was built with. render() rebuilds the page when the
    // mirrored text starts needing an engine the page lacks.
    int m_pageEngines = 0;
    // Closed-panel lifecycle state (see panelClosed/panelOpened): whether the
    // panel is closed, how long it has been closed, whether the loading mode
    // releases the page once it stays closed (LazyKeep), and after how long
    // the page is discarded (override for tests via KATEXDOWN_IDLE_DISCARD_MS).
    bool m_panelClosed = false;
    bool m_releaseWhenIdle = false;
    int m_idleDiscardMs = 60000;
    QElapsedTimer m_closedSince;
    // Set once the document announced its close: its buffer gets emptied straight
    // after, so m_text must not be refreshed from it again.
    bool m_bufferStale = false;
};
