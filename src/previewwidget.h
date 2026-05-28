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

namespace KTextEditor
{
class MainWindow;
}

/**
 * A single Markdown preview tab: a QWebEngineView fed by a self-contained HTML
 * document. Source text is pushed to the page on every change; theming is driven
 * from the active editor theme (Application mode) or GitHub's palette.
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

public Q_SLOTS:
    void applyTheme();

protected:
    void changeEvent(QEvent *event) override;

private Q_SLOTS:
    void scheduleRender();
    void updateTitle();

private:
    void render();
    void runJs(const QString &code);
    void loadPage();
    void openLink(const QUrl &url);
    void applyMediaPolicy();
    QUrl baseUrl() const;
    static QString buildHtml();

    QPointer<KTextEditor::MainWindow> m_mainWindow;
    QWebEngineView *m_web = nullptr;
    QWebEngineProfile *m_profile = nullptr;
    QWebEngineUrlRequestInterceptor *m_guard = nullptr;
    QTimer *m_debounce = nullptr;
    QPointer<KTextEditor::Document> m_doc;
    QPointer<KTextEditor::View> m_view;
    bool m_loaded = false;
    bool m_remoteApplied = false;
};
