#pragma once

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

private:
    void updateTitle();
    void render();
    void performExport(const QString &path);
    void runJs(const QString &code);
    void loadPage();
    void openLink(const QUrl &url);
    void applyMediaPolicy();
    QUrl baseUrl() const;
    QString buildHtml() const;

    // Forward input the preview doesn't use back to Kate: QWebEngineView's render
    // widget swallows keys/mouse buttons before Kate's shortcut machinery sees them.
    void installInputFilter();
    bool forwardKeyEvent(QKeyEvent *event);
    bool forwardMouseEvent(QMouseEvent *event);
    QAction *kateActionFor(const QKeySequence &seq) const;

    QPointer<KTextEditor::MainWindow> m_mainWindow;
    QWebEngineView *m_web = nullptr;
    QWebEngineProfile *m_profile = nullptr;
    QWebEngineUrlRequestInterceptor *m_guard = nullptr;
    QTimer *m_debounce = nullptr;
    QPointer<KTextEditor::Document> m_doc;
    QPointer<KTextEditor::View> m_view;
    QPointer<QWidget> m_inputTarget;
    QUrl m_url;
    QString m_text;
    QString m_pendingExportPath;
    bool m_loaded = false;
    bool m_remoteApplied = false;
    // Set once the document announced its close: its buffer gets emptied straight
    // after, so m_text must not be refreshed from it again.
    bool m_bufferStale = false;
};
