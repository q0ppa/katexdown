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

    // Run one renderer-memory maintenance cycle right now: the page is told it
    // is hidden and then Discarded, which makes QtWebEngine exit the renderer
    // process — the only thing that reliably reclaims the dead memory a
    // long-lived renderer accumulates (Qt refuses to freeze or discard a
    // visible page, and a frozen page only purges when it was never shown).
    // A fresh page is then loaded from the mirrored document state, so content
    // comes back through the normal load pipeline. With restoreScroll the
    // previous scroll position is kept (same-document idle recycle); document
    // switches pass false (a new document starts at the top). The policy
    // ticker (idleTick) decides when a recycle is safe; tests call this
    // directly to pin the reclamation behavior. Returns false (and does
    // nothing) unless the page is loaded and active and the panel is open.
    bool performMemoryRecycle(bool restoreScroll);

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
    // Periodic policy while the panel is closed (freeze on close, discard a
    // long-closed LazyKeep page) and while it is open (renderer-memory
    // maintenance: recycle the renderer once the estimated dead memory since
    // the last recycle passes a budget and the preview has been quiet for a
    // while; see performMemoryRecycle).
    void idleTick();

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
    // ---- Renderer-memory maintenance (see the idleTick comment) ----
    // Estimate the dead memory a long-lived renderer holds since its last
    // recycle: every full-document markdown push costs a chunk proportional to
    // the text size (measured ~0.5-0.9 kB per byte in the test environment)
    // and every full page load adds a fixed cost. Chromium never reclaims any
    // of it by itself (no GC runs and Qt refuses to freeze a visible page), so
    // once the estimate passes a budget the renderer is recycled.
    void noteRenderWork(qint64 textBytes, bool navigation);
    void noteActivity();
    // Recycle bookkeeping: hide + discard the page, load a fresh one, restore
    // the panel. The cycle is driven by lifecycle events + idleTick retries.
    void beginRecycle();
    void finalizeRecycle();
    void abortRecycle();
    bool recycleDue() const;
    bool recycleIdle() const;
    bool webHasFocus() const;
    void readMemTuning();
    // Scroll restoration for a same-document recycle: the page answers
    // window.scrollY asynchronously, then the cycle starts.
    void startRecycleAfterScrollQuery(double scrollY);

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
    // The base URL (document folder) the currently loaded page was built
    // against. A document switch that stays in the same folder (and needs no
    // engine the page lacks) re-renders in place instead of paying for a full
    // setHtml navigation — see attachDocument.
    QUrl m_pageUrl;
    // Renderer-memory maintenance: estimated dead renderer memory since the
    // last recycle, the budget that triggers a recycle, the quiet window a
    // same-document recycle waits for (no text change, input or pending work;
    // document switches recycle as part of the switch instead), and a
    // pure-time backstop so slow leaks (e.g. infrequent tab switches) still
    // get recycled. Tunable via env (KATEXDOWN_MEM_*) for tests; see
    // readMemTuning().
    qint64 m_memEstimate = 0;
    qint64 m_memBudgetBytes = 128ll * 1024 * 1024;
    int m_memIdleMs = 6000;
    int m_memMaxAgeMs = 180000;
    // True between "the recycle was decided" and "the fresh page finished
    // loading". While set, the lifecycle machinery treats a Discarded event as
    // part of the recycle (load the mirrored document again) instead of as a
    // closed-panel release.
    bool m_recycling = false;
    QElapsedTimer m_lastRecycle;
    QElapsedTimer m_lastActivity;
    // Scroll position to restore once the fresh page of a same-document
    // recycle has rendered (negative = none).
    double m_recycleScrollY = -1;
    // Fallback: if the fresh load never finishes (or a discard never sticks),
    // this timer un-hides the page / aborts the cycle so the preview can never
    // get stuck hidden.
    QTimer *m_recycleTimer = nullptr;
    int m_recycleDiscardTries = 0;
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
    // A reloadless document switch (attachDocument fast path) re-renders in
    // place like an edit would; the next render() must reset the scroll to the
    // top, because a new document must start at its beginning.
    bool m_pendingScrollReset = false;
};
