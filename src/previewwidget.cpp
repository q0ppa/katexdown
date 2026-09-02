#include "previewwidget.h"
#include "katexdownpaths.h"
#include "settings.h"

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <QPalette>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineView>

#include <functional>

#include <KLocalizedString>
#include <KSyntaxHighlighting/Theme>
#include <KTextEditor/Document>
#include <KTextEditor/Editor>
#include <KTextEditor/MainWindow>
#include <KTextEditor/View>

using Theme = KSyntaxHighlighting::Theme;

namespace
{
QString readAsset(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return QString();
    }
    return QString::fromUtf8(f.readAll());
}

// Make an arbitrary string safe to inline inside a <script> block.
QString shieldScript(QString js)
{
    return js.replace(QLatin1String("</script"), QLatin1String("<\\/script"), Qt::CaseInsensitive);
}

// Encode a string as a JavaScript string literal (incl. surrounding quotes).
QString jsLiteral(const QString &s)
{
    const QJsonDocument doc(QJsonArray{s});
    const QByteArray json = doc.toJson(QJsonDocument::Compact); // ["..."]
    return QString::fromUtf8(json.mid(1, json.size() - 2));
}

QString compactJson(const QJsonObject &obj)
{
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

QColor colorOr(QRgb rgb, const QColor &fallback)
{
    return qAlpha(rgb) == 0 ? fallback : QColor::fromRgba(rgb);
}

QString hex(const QColor &c)
{
    return c.name(QColor::HexRgb);
}

QString rgba(const QColor &c, qreal a)
{
    return QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(a, 0, 'g', 3);
}

QColor blend(const QColor &a, const QColor &b, qreal t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t,
                            a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}

bool paletteIsDark()
{
    const QColor w = qApp->palette().color(QPalette::Active, QPalette::Window);
    return w.lightnessF() < 0.5;
}

struct VarPair {
    const char *key;
    const char *value;
};

QJsonObject githubVars(bool dark)
{
    static const VarPair darkVars[] = {
        {"--fgColor-default", "#f0f6fc"},     {"--fgColor-muted", "#9198a1"},
        {"--fgColor-accent", "#4493f8"},      {"--fgColor-danger", "#f85149"},
        {"--fgColor-success", "#3fb950"},     {"--fgColor-attention", "#d29922"},
        {"--fgColor-done", "#ab7df8"},        {"--bgColor-default", "#0d1117"},
        {"--bgColor-muted", "#151b23"},       {"--bgColor-neutral-muted", "#656c7633"},
        {"--bgColor-attention-muted", "#bb800926"},
        {"--borderColor-default", "#3d444d"}, {"--borderColor-muted", "#3d444db3"},
        {"--borderColor-neutral-muted", "#3d444db3"},
        {"--borderColor-accent-emphasis", "#1f6feb"},
        {"--borderColor-attention-emphasis", "#9e6a03"},
        {"--borderColor-danger-emphasis", "#da3633"},
        {"--borderColor-success-emphasis", "#238636"},
        {"--borderColor-done-emphasis", "#8957e5"},
    };
    static const VarPair lightVars[] = {
        {"--fgColor-default", "#1f2328"},     {"--fgColor-muted", "#59636e"},
        {"--fgColor-accent", "#0969da"},      {"--fgColor-danger", "#d1242f"},
        {"--fgColor-success", "#1a7f37"},     {"--fgColor-attention", "#9a6700"},
        {"--fgColor-done", "#8250df"},        {"--bgColor-default", "#ffffff"},
        {"--bgColor-muted", "#f6f8fa"},       {"--bgColor-neutral-muted", "#818b981f"},
        {"--bgColor-attention-muted", "#fff8c5"},
        {"--borderColor-default", "#d1d9e0"}, {"--borderColor-muted", "#d1d9e0b3"},
        {"--borderColor-neutral-muted", "#d1d9e0b3"},
        {"--borderColor-accent-emphasis", "#0969da"},
        {"--borderColor-attention-emphasis", "#9a6700"},
        {"--borderColor-danger-emphasis", "#cf222e"},
        {"--borderColor-success-emphasis", "#1a7f37"},
        {"--borderColor-done-emphasis", "#8250df"},
    };

    QJsonObject v;
    const VarPair *arr = dark ? darkVars : lightVars;
    const size_t count = sizeof(darkVars) / sizeof(darkVars[0]); // both tables are the same length
    for (size_t i = 0; i < count; ++i) {
        v.insert(QString::fromLatin1(arr[i].key), QString::fromLatin1(arr[i].value));
    }
    return v;
}

// Derive github-markdown CSS variables from the active editor theme + palette.
QJsonObject applicationVars(const Theme &theme, bool &outDark)
{
    const QColor bg = colorOr(theme.editorColor(Theme::BackgroundColor), qApp->palette().color(QPalette::Base));
    const QColor fg = colorOr(theme.textColor(Theme::Normal), qApp->palette().color(QPalette::Text));
    outDark = bg.lightnessF() < 0.5;

    QColor link = qApp->palette().color(QPalette::Active, QPalette::Link);
    if (!link.isValid()) {
        link = colorOr(theme.textColor(Theme::Function), fg);
    }
    const QColor border = blend(bg, fg, 0.18);
    const QColor danger = colorOr(theme.textColor(Theme::Error), QColor(0xCF, 0x22, 0x2E));
    const QColor attention = colorOr(theme.textColor(Theme::Warning), QColor(0x9A, 0x67, 0x00));
    const QColor success = colorOr(theme.textColor(Theme::String), QColor(0x1A, 0x7F, 0x37));
    const QColor done = colorOr(theme.textColor(Theme::Keyword), link);

    QJsonObject v;
    v.insert(QStringLiteral("--fgColor-default"), hex(fg));
    v.insert(QStringLiteral("--fgColor-muted"), hex(blend(fg, bg, 0.45)));
    v.insert(QStringLiteral("--fgColor-accent"), hex(link));
    v.insert(QStringLiteral("--fgColor-danger"), hex(danger));
    v.insert(QStringLiteral("--fgColor-success"), hex(success));
    v.insert(QStringLiteral("--fgColor-attention"), hex(attention));
    v.insert(QStringLiteral("--fgColor-done"), hex(done));
    v.insert(QStringLiteral("--bgColor-default"), hex(bg));
    v.insert(QStringLiteral("--bgColor-muted"), hex(blend(bg, fg, 0.04)));
    v.insert(QStringLiteral("--bgColor-neutral-muted"), rgba(fg, outDark ? 0.14 : 0.08));
    v.insert(QStringLiteral("--bgColor-attention-muted"), rgba(attention, 0.15));
    v.insert(QStringLiteral("--borderColor-default"), hex(border));
    v.insert(QStringLiteral("--borderColor-muted"), rgba(border, 0.7));
    v.insert(QStringLiteral("--borderColor-neutral-muted"), rgba(border, 0.7));
    v.insert(QStringLiteral("--borderColor-accent-emphasis"), hex(link));
    v.insert(QStringLiteral("--borderColor-attention-emphasis"), hex(attention));
    v.insert(QStringLiteral("--borderColor-danger-emphasis"), hex(danger));
    v.insert(QStringLiteral("--borderColor-success-emphasis"), hex(success));
    v.insert(QStringLiteral("--borderColor-done-emphasis"), hex(done));
    return v;
}

// Build a highlight.js token stylesheet from the active editor theme so code
// blocks render with the same colors as the editor.
QString applicationCodeCss(const Theme &theme)
{
    const QColor fg = colorOr(theme.textColor(Theme::Normal), qApp->palette().color(QPalette::Text));
    auto tc = [&](Theme::TextStyle s) {
        return hex(colorOr(theme.textColor(s), fg));
    };

    QString css;
    css += QStringLiteral(".hljs{color:%1;background:transparent}").arg(hex(fg));
    css += QStringLiteral(".hljs-comment,.hljs-quote{color:%1;font-style:italic}").arg(tc(Theme::Comment));
    css += QStringLiteral(".hljs-doctag,.hljs-meta .hljs-doctag{color:%1}").arg(tc(Theme::Documentation));
    css += QStringLiteral(".hljs-keyword,.hljs-selector-tag,.hljs-literal,.hljs-section,.hljs-name,.hljs-tag,.hljs-meta "
                          ".hljs-keyword{color:%1}")
               .arg(tc(Theme::Keyword));
    css += QStringLiteral(".hljs-built_in,.hljs-builtin-name,.hljs-title.class_,.hljs-class .hljs-title{color:%1}").arg(tc(Theme::BuiltIn));
    css += QStringLiteral(".hljs-type,.hljs-class{color:%1}").arg(tc(Theme::DataType));
    css += QStringLiteral(".hljs-string,.hljs-regexp,.hljs-addition,.hljs-meta .hljs-string{color:%1}").arg(tc(Theme::String));
    css += QStringLiteral(".hljs-number{color:%1}").arg(tc(Theme::DecVal));
    css += QStringLiteral(".hljs-symbol,.hljs-bullet,.hljs-link,.hljs-char,.hljs-char.escape_{color:%1}").arg(tc(Theme::Char));
    css += QStringLiteral(".hljs-title,.hljs-title.function_,.hljs-function .hljs-title{color:%1}").arg(tc(Theme::Function));
    css += QStringLiteral(".hljs-variable,.hljs-template-variable,.hljs-attr,.hljs-attribute,.hljs-selector-attr,.hljs-selector-class,"
                          ".hljs-selector-id{color:%1}")
               .arg(tc(Theme::Variable));
    css += QStringLiteral(".hljs-meta{color:%1}").arg(tc(Theme::Preprocessor));
    css += QStringLiteral(".hljs-deletion{color:%1}").arg(tc(Theme::Error));
    css += QStringLiteral(".hljs-emphasis{font-style:italic}.hljs-strong{font-weight:bold}");
    return css;
}

// Confine the page to its document folder: a malicious markdown (raw HTML + JS on a
// file:// origin) could otherwise read any local file via fetch/img/etc. Only assets
// under the canonical document root pass; remote requests are limited to images/media
// and only when the user opted in.
class LocalFileGuard : public QWebEngineUrlRequestInterceptor
{
public:
    using QWebEngineUrlRequestInterceptor::QWebEngineUrlRequestInterceptor;

    void setRoot(const QString &dir)
    {
        m_root = dir.isEmpty() ? QString() : QDir(dir).canonicalPath();
    }

    void setAllowRemote(bool allow)
    {
        m_allowRemote = allow;
    }

    void interceptRequest(QWebEngineUrlRequestInfo &info) override
    {
        const QUrl url = info.requestUrl();
        const QString scheme = url.scheme();
        if (scheme == QLatin1String("qrc") || scheme == QLatin1String("data") || scheme == QLatin1String("about") || scheme == QLatin1String("blob")) {
            return;
        }
        if (scheme == QLatin1String("file")) {
            const QString path = QFileInfo(url.toLocalFile()).canonicalFilePath();
            const bool ok = !m_root.isEmpty() && !path.isEmpty() && (path == m_root || path.startsWith(m_root + QLatin1Char('/')));
            if (!ok) {
                info.block(true);
            }
            return;
        }
        const bool isMedia = info.resourceType() == QWebEngineUrlRequestInfo::ResourceTypeImage
            || info.resourceType() == QWebEngineUrlRequestInfo::ResourceTypeMedia;
        if (!(m_allowRemote && isMedia)) {
            info.block(true);
        }
    }

private:
    QString m_root;
    bool m_allowRemote = false;
};

// Keep link clicks from turning the preview into a browser: same-document anchors
// scroll in place; everything else is handed back to the host via onLinkActivated.
class PreviewPage : public QWebEnginePage
{
public:
    using QWebEnginePage::QWebEnginePage;

    std::function<void(const QUrl &)> onLinkActivated;

    bool acceptNavigationRequest(const QUrl &url, NavigationType type, bool isMainFrame) override
    {
        Q_UNUSED(isMainFrame);
        if (type == NavigationTypeLinkClicked) {
            if (url.hasFragment() && url.matches(this->url(), QUrl::RemoveFragment)) {
                return true;
            }
            if (onLinkActivated) {
                QMetaObject::invokeMethod(
                    this, [cb = onLinkActivated, u = url]() { cb(u); }, Qt::QueuedConnection);
            }
            return false;
        }
        return true;
    }
};
} // namespace

PreviewWidget::PreviewWidget(KTextEditor::MainWindow *mainWindow, KTextEditor::View *view, KTextEditor::Document *doc, QWidget *parent)
    : QWidget(parent)
    , m_mainWindow(mainWindow)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    // Each preview owns an off-the-record profile carrying its own guard so multiple
    // open previews each confine to their own document folder.
    auto *guard = new LocalFileGuard(this);
    m_guard = guard;
    m_profile = new QWebEngineProfile(this);
    m_profile->setUrlRequestInterceptor(guard);

    m_web = new QWebEngineView(this);
    auto *page = new PreviewPage(m_profile, m_web);
    page->onLinkActivated = [this](const QUrl &url) {
        openLink(url);
    };
    m_web->setPage(page);
    m_web->settings()->setAttribute(QWebEngineSettings::FocusOnNavigationEnabled, false);
    m_web->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, true);
    // Watch the view for the lazily-created render widget so we can attach to it (below).
    m_web->installEventFilter(this);
    installInputFilter();
    layout->addWidget(m_web);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(150);
    connect(m_debounce, &QTimer::timeout, this, &PreviewWidget::render);

    connect(m_web, &QWebEngineView::loadFinished, this, [this](bool ok) {
        if (!ok) {
            return;
        }
        m_loaded = true;
        installInputFilter();
        applyTheme();
        render();
        applyOutlineSettings();
        if (!m_pendingExportPath.isEmpty()) {
            performExport(m_pendingExportPath);
        }
    });

    connect(Settings::self(), &Settings::changed, this, &PreviewWidget::applyTheme);
    connect(Settings::self(), &Settings::changed, this, &PreviewWidget::applyMediaPolicy);
    connect(Settings::self(), &Settings::changed, this, &PreviewWidget::applyOutlineSettings);

    setWindowIcon(QIcon::fromTheme(QStringLiteral("text-markdown")));
    applyMediaPolicy();
    attachDocument(doc, view);
}

void PreviewWidget::attachDocument(KTextEditor::Document *doc, KTextEditor::View *view)
{
    if (doc && m_doc == doc) {
        return;
    }
    if (m_doc) {
        m_doc->disconnect(this);
    }
    m_doc = doc;
    m_view = view;
    m_bufferStale = false;
    if (doc) {
        connect(doc, &KTextEditor::Document::textChanged, this, &PreviewWidget::scheduleRender);
        connect(doc, &KTextEditor::Document::documentUrlChanged, this, &PreviewWidget::onDocumentUrlChanged);
        connect(doc, &KTextEditor::Document::aboutToClose, this, &PreviewWidget::snapshotSource);
        // Follow mode: the preview stays open when the editor tab of its
        // document closes; freeze on the snapshot and stop touching it.
        connect(doc, &QObject::destroyed, this, [this, doc]() {
            if (m_doc.isNull() || m_doc == doc) {
                m_debounce->stop();
                m_view = nullptr;
                m_doc = nullptr;
                updateTitle();
            }
        });
    }
    loadPage(); // refreshes m_url, which updateTitle() reads
    updateTitle();
}

// Freeze on the mirrored source. Called when the document is about to be deleted,
// which on the multi-tab close path is the only warning that arrives at all.
void PreviewWidget::detachDocument()
{
    if (!m_doc) {
        return;
    }
    if (!m_bufferStale) {
        m_text = m_doc->text();
    }
    m_doc->disconnect(this);
    m_doc = nullptr;
    m_view = nullptr;
    m_debounce->stop();
    updateTitle();
}

// aboutToClose is the last moment the buffer still holds the document's text;
// closeUrl() empties it immediately afterwards.
void PreviewWidget::snapshotSource()
{
    if (m_doc) {
        m_text = m_doc->text();
        m_bufferStale = true;
    }
}

// Teardown order matters: the view (and its page) must die before the profile, and the
// profile before the guard it references. m_web is parented to this and the QObject child
// list destroys in reverse order of construction (web after profile after guard), which
// gives exactly that sequence; spelling it out keeps the invariant from drifting.
PreviewWidget::~PreviewWidget()
{
    delete m_web;
    m_web = nullptr;
    delete m_profile;
    m_profile = nullptr;
    delete m_guard;
    m_guard = nullptr;
}

// Read one optional asset from the data dir. Returns empty when the file is
// missing, so a cache that was never populated degrades gracefully.
static QString optionalAsset(const QString &name)
{
    return katexdownpaths::readIfPresent(katexdownpaths::resolveAssetPath(name));
}

// Concatenate the user's custom stylesheets (settings), resolving relative
// paths against the data dir. Missing files are skipped silently.
static QString userCss()
{
    QString css;
    const QStringList files = Settings::self()->customCssFiles();
    for (const QString &file : files) {
        css += katexdownpaths::readIfPresent(katexdownpaths::resolveAssetPath(file));
    }
    return css;
}

QString PreviewWidget::buildHtml() const
{
    const QString base = QStringLiteral(":/katexdown/");
    QString html = readAsset(base + QStringLiteral("preview.html"));
    // The bundled GitHub stylesheet is on by default but can be disabled so a
    // custom stylesheet owns the whole layout. Empty <style> is harmless.
    const QString githubCss = Settings::self()->useGithubCss() ? readAsset(base + QStringLiteral("css/github-markdown.css")) : QString();
    html.replace(QLatin1String("/*__GHMD_CSS__*/"), githubCss);
    html.replace(QLatin1String("/*__BASE_CSS__*/"), readAsset(base + QStringLiteral("css/base.css")));
    html.replace(QLatin1String("/*__HLJS_LIGHT__*/"), readAsset(base + QStringLiteral("css/hljs-github.min.css")));
    html.replace(QLatin1String("/*__HLJS_DARK__*/"), readAsset(base + QStringLiteral("css/hljs-github-dark.min.css")));
    // Optional extras from the data dir; empty when not downloaded yet.
    html.replace(QLatin1String("/*__KATEX_CSS__*/"), optionalAsset(QStringLiteral("katex-standalone.min.css")));
    html.replace(QLatin1String("/*__USER_CSS__*/"), userCss());
    html.replace(QLatin1String("/*__MARKDOWN_IT__*/"), shieldScript(readAsset(base + QStringLiteral("js/markdown-it.min.js"))));
    html.replace(QLatin1String("/*__HLJS_JS__*/"), shieldScript(readAsset(base + QStringLiteral("js/highlight.min.js"))));
    html.replace(QLatin1String("/*__JS_YAML__*/"), shieldScript(readAsset(base + QStringLiteral("js/js-yaml.min.js"))));
    html.replace(QLatin1String("/*__KATEX_JS__*/"), shieldScript(optionalAsset(QStringLiteral("katex.min.js"))));
    html.replace(QLatin1String("/*__TEXMATH_JS__*/"), shieldScript(optionalAsset(QStringLiteral("texmath.min.js"))));
    html.replace(QLatin1String("/*__PREVIEW_JS__*/"), shieldScript(readAsset(base + QStringLiteral("js/preview.js"))));
    return html;
}

QUrl PreviewWidget::baseUrl() const
{
    if (m_url.isLocalFile()) {
        return m_url.adjusted(QUrl::RemoveFilename);
    }
    return QUrl(QStringLiteral("qrc:/katexdown/"));
}

void PreviewWidget::loadPage()
{
    if (qEnvironmentVariableIsSet("KATEXDOWN_DEBUG")) {
        qDebug() << "[katexdown] loadPage (full setHtml rebuild), doc url:" << (m_doc ? m_doc->url().toString() : QStringLiteral("(none)"));
    }
    if (m_doc) {
        m_url = m_doc->url();
    }
    const QString root = m_url.isLocalFile() ? QFileInfo(m_url.toLocalFile()).absolutePath() : QString();
    static_cast<LocalFileGuard *>(m_guard)->setRoot(root);
    m_loaded = false;
    m_web->setHtml(buildHtml(), baseUrl());
}

// Closing a document clears its url before anything announces the close, so an
// unfiltered reload here would blank the page and drop the document folder the
// frozen content still resolves its images against. A rename or Save As always
// lands on a non-empty url.
void PreviewWidget::onDocumentUrlChanged()
{
    if (m_doc && m_doc->url().isEmpty() && !m_url.isEmpty()) {
        return;
    }
    loadPage();
    updateTitle();
}

// Serialize the live, fully rendered page (all styles inline, KaTeX already
// expanded) into a standalone .html file. When the page is not loaded yet
// (e.g. the export action triggered the very first lazy creation) the export
// is deferred until the page finishes loading.
bool PreviewWidget::exportToFile(const QString &path)
{
    if (path.isEmpty()) {
        return false;
    }
    if (!m_loaded) {
        m_pendingExportPath = path;
        return true;
    }
    performExport(path);
    return true;
}

void PreviewWidget::performExport(const QString &path)
{
    m_pendingExportPath.clear();
    // Make sure the very latest text is on screen before grabbing the DOM.
    if (m_doc && !m_bufferStale) {
        m_text = m_doc->text();
    }
    runJs(QStringLiteral("window.__setMarkdown(%1);").arg(jsLiteral(m_text)));
    // Re-render is synchronous in the page; give the compositor a moment and
    // then serialize the current DOM, styles and all.
    QTimer::singleShot(300, this, [this, path]() {
        m_web->page()->runJavaScript(QStringLiteral("document.documentElement.outerHTML"), [this, path](const QVariant &html) {
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly)) {
                qWarning("katexdown: cannot write export file %s", qPrintable(path));
                return;
            }
            f.write(html.toString().toUtf8());
            Q_EMIT exportFinished(path);
        });
    });
}

void PreviewWidget::openLink(const QUrl &url)
{
    if (url.isLocalFile() && m_mainWindow) {
        m_mainWindow->openUrl(url);
    } else if (!url.scheme().isEmpty()) {
        QDesktopServices::openUrl(url);
    }
}

void PreviewWidget::applyMediaPolicy()
{
    const bool remote = Settings::self()->loadRemoteMedia();
    m_web->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, remote);
    static_cast<LocalFileGuard *>(m_guard)->setAllowRemote(remote);
    if (m_loaded && remote != m_remoteApplied) {
        loadPage();
    }
    m_remoteApplied = remote;
}

void PreviewWidget::runJs(const QString &code)
{
    if (m_web && m_loaded) {
        m_web->page()->runJavaScript(code);
    }
}

void PreviewWidget::render()
{
    if (!m_loaded) {
        return;
    }
    if (m_doc) {
        m_text = m_doc->text();
        m_bufferStale = false; // live content supersedes any close-time snapshot
    }
    runJs(QStringLiteral("window.__setMarkdown(%1);").arg(jsLiteral(m_text)));
}

void PreviewWidget::scheduleRender()
{
    m_debounce->start();
}

void PreviewWidget::applyOutlineSettings()
{
    if (!m_loaded) {
        return;
    }
    // Push the configured heading levels into the page; the page rebuilds its
    // floating section list from the current DOM (no re-render needed).
    const QList<int> levels = Settings::self()->tocLevels();
    QStringList nums;
    nums.reserve(levels.size());
    for (int level : levels) {
        nums << QString::number(level);
    }
    runJs(QStringLiteral("window.__setOutlineLevels([%1]);").arg(nums.join(QLatin1Char(','))));
}

void PreviewWidget::applyTheme()
{
    if (!m_loaded) {
        return;
    }
    Settings *s = Settings::self();
    if (s->mode() == Settings::GitHub) {
        bool dark = s->ghVariant() == Settings::Dark || (s->ghVariant() == Settings::Auto && paletteIsDark());
        runJs(QStringLiteral("window.__setColorScheme(%1);").arg(dark ? QStringLiteral("true") : QStringLiteral("false")));
        runJs(QStringLiteral("window.__applyVars(%1);").arg(compactJson(githubVars(dark))));
        runJs(QStringLiteral("window.__useHljsTheme('%1');").arg(dark ? QStringLiteral("github-dark") : QStringLiteral("github")));
    } else {
        const Theme theme = m_view ? m_view->theme() : KTextEditor::Editor::instance()->theme();
        bool dark = false;
        const QJsonObject vars = applicationVars(theme, dark);
        runJs(QStringLiteral("window.__setColorScheme(%1);").arg(dark ? QStringLiteral("true") : QStringLiteral("false")));
        runJs(QStringLiteral("window.__applyVars(%1);").arg(compactJson(vars)));
        runJs(QStringLiteral("window.__setCodeCss(%1);").arg(jsLiteral(applicationCodeCss(theme))));
    }
}

void PreviewWidget::updateTitle()
{
    const QString name = m_url.isEmpty() ? i18n("Untitled") : m_url.fileName();
    setWindowTitle(m_doc ? i18n("Preview: %1", name) : i18n("Preview: %1 (closed)", name));
}

void PreviewWidget::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ThemeChange) {
        applyTheme();
    }
}

// QWebEngineView delivers input to a lazily-created internal child (its focus proxy),
// which can be swapped out across loads. Keep our filter attached to whatever child
// currently receives key/mouse events.
void PreviewWidget::installInputFilter()
{
    QWidget *proxy = m_web ? m_web->focusProxy() : nullptr;
    if (proxy == m_inputTarget) {
        return;
    }
    if (m_inputTarget) {
        m_inputTarget->removeEventFilter(this);
    }
    m_inputTarget = proxy;
    if (proxy) {
        proxy->installEventFilter(this);
    }
}

bool PreviewWidget::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_web) {
        if (event->type() == QEvent::ChildAdded || event->type() == QEvent::ChildPolished) {
            installInputFilter();
        }
        return QWidget::eventFilter(obj, event);
    }

    switch (event->type()) {
    case QEvent::KeyPress:
        if (forwardKeyEvent(static_cast<QKeyEvent *>(event))) {
            return true;
        }
        break;
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
        if (forwardMouseEvent(static_cast<QMouseEvent *>(event))) {
            return true;
        }
        break;
    default:
        break;
    }
    return QWidget::eventFilter(obj, event);
}

bool PreviewWidget::forwardKeyEvent(QKeyEvent *event)
{
    const Qt::KeyboardModifiers mods = event->modifiers();
    const int key = event->key();

    // Keys the preview handles itself: scrolling/navigation and selection clipboard.
    // These stay with the web view so reading the document keeps working.
    if (mods == Qt::NoModifier || mods == Qt::ShiftModifier) {
        switch (key) {
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_Left:
        case Qt::Key_Right:
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
        case Qt::Key_Home:
        case Qt::Key_End:
        case Qt::Key_Space:
            return false;
        default:
            break;
        }
    }
    if (mods == Qt::ControlModifier && (key == Qt::Key_C || key == Qt::Key_A || key == Qt::Key_Insert)) {
        return false;
    }

    QAction *action = kateActionFor(QKeySequence(event->keyCombination()));
    if (!action) {
        return false;
    }
    action->trigger();
    return true;
}

bool PreviewWidget::forwardMouseEvent(QMouseEvent *event)
{
    // The page uses left (click/select), right (context menu), and middle; hand the
    // side/extra buttons (Back/Forward/X buttons) to Kate's window so its mouse
    // bindings fire instead of being eaten by the web view.
    switch (event->button()) {
    case Qt::LeftButton:
    case Qt::RightButton:
    case Qt::MiddleButton:
    case Qt::NoButton:
        return false;
    default:
        break;
    }

    QWidget *win = m_mainWindow ? m_mainWindow->window() : nullptr;
    if (!win) {
        return false;
    }
    const QPoint global = event->globalPosition().toPoint();
    QMouseEvent copy(event->type(), win->mapFromGlobal(global), global, event->button(), event->buttons(), event->modifiers());
    QCoreApplication::sendEvent(win, &copy);
    return true;
}

QAction *PreviewWidget::kateActionFor(const QKeySequence &seq) const
{
    QWidget *win = (seq.isEmpty() || !m_mainWindow) ? nullptr : m_mainWindow->window();
    if (!win) {
        return nullptr;
    }
    const QList<QAction *> actions = win->findChildren<QAction *>();
    for (QAction *action : actions) {
        if (action->isEnabled() && action->shortcuts().contains(seq)) {
            return action;
        }
    }
    return nullptr;
}
