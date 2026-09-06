#include "previewwidget.h"
#include "katexdownpaths.h"
#include "kdxchromiumflags.h"
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
#include <QWebEngineHistory>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineView>

#include <functional>

#include <limits>

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

// ---------------------------------------------------------------------------
// Optional engine selection: the page inlines each engine's JS/CSS, so a page
// only carries the engines its document text can actually use:
//   kEngineKatex   KaTeX math ($...$ / $$...$$ / \(...\) / \[...\])
//   kEngineHljs    syntax highlighting for fenced code blocks
//   kEngineYaml    YAML front matter (a document starting with a "---" line)
// enginesForText() is a cheap, allocation-free, line-oriented scan. It only
// runs for pages that are missing an engine (a fully equipped page never
// re-scans), and a false negative is self-healing: render() rebuilds the page
// the moment the text starts needing an engine the page lacks. Lines inside
// fences are skipped, so dollar amounts and $-using code do not pull KaTeX
// in. Indented (non-fenced) code blocks are intentionally not detected:
// markdown-it still renders them without highlight.js — only the colors are
// missing until the next full load.
// ---------------------------------------------------------------------------
constexpr int kEngineNone = 0;
constexpr int kEngineKatex = 1 << 0;
constexpr int kEngineHljs = 1 << 1;
constexpr int kEngineYaml = 1 << 2;
constexpr int kEngineAll = kEngineKatex | kEngineHljs | kEngineYaml;

// Does the line [from, to) of text look like it contains LaTeX?
static bool lineLooksLikeMath(const QString &text, int from, int to)
{
    for (int p = from; p + 1 < to; ++p) {
        const QChar ch = text.at(p);
        if (ch == QLatin1Char('\\')) {
            // \( \) \[ \] are unambiguous math delimiters.
            const QChar n = text.at(p + 1);
            if (n == QLatin1Char('(') || n == QLatin1Char(')') || n == QLatin1Char('[') || n == QLatin1Char(']')) {
                return true;
            }
            ++p; // the escaped character cannot open another marker
            continue;
        }
        if (ch != QLatin1Char('$') || (p > from && text.at(p - 1) == QLatin1Char('\\'))) {
            continue; // escaped dollar signs are literal
        }
        if (p + 1 < to && text.at(p + 1) == QLatin1Char('$')) {
            return true; // $$ display math (may span lines)
        }
        // Inline $...$: mirror what markdown-it-texmath's dollars rule will
        // actually parse (its lazy regex plus the $_pre/$_post guards), so the
        // engine is loaded exactly when the page would turn the pair into
        // math — never less. The rule: an opener's previous char must not be a
        // digit ($_pre: "$5" after a number is currency, not math), the char
        // after the opener must exist and be non-space (the regex needs \S
        // content: "$ x$" is literal), and the closer is the first "$" whose
        // previous char is neither space nor a backslash. Content that starts
        // with a digit is math like any other — "$2^{192} - 2^{32}$" and
        // even a bare "$2$" render in KaTeX, so there is deliberately no
        // LaTeX-ish content requirement here. A closer followed by a digit
        // ($_post: "$5$10" is currency) vetoes the whole opener; texmath
        // retries only past that closer, and no later dollar on the line can
        // close the pair either, so scanning resumes there.
        if (p > from) {
            const ushort prev = text.at(p - 1).unicode();
            if (prev >= '0' && prev <= '9') {
                continue; // $_pre: never math right after a digit
            }
        }
        if (text.at(p + 1).isSpace()) {
            continue; // the regex needs non-space content after the opener
        }
        for (int q = p + 1; q < to; ++q) {
            const QChar c = text.at(q);
            if (c != QLatin1Char('$')) {
                continue;
            }
            const QChar before = text.at(q - 1);
            if (before.isSpace() || before == QLatin1Char('\\')) {
                continue; // cannot close here ("$x $" or an escaped dollar)
            }
            const ushort after = q + 1 < to ? text.at(q + 1).unicode() : 0;
            if (after >= '0' && after <= '9') {
                p = q; // $_post veto: "$5$10" is currency — resume past it
                break;
            }
            return true; // a real closing dollar: texmath parses this pair
        }
        // No closing dollar on this line: inline math does not span lines,
        // and nothing further on it can pair up.
        return false;
    }
    return false;
}

static int enginesForText(const QString &text)
{
    int engines = kEngineNone;
    const int len = text.size();
    int lineStart = 0;
    int lineNo = 0;
    QChar fence; // fence marker we are inside ('`' or '~'), null when outside

    while (lineStart <= len) {
        const int nl = text.indexOf(QLatin1Char('\n'), lineStart);
        const int lineEnd = nl < 0 ? len : nl;

        // Leading whitespace: fences may be indented up to three spaces.
        int c = lineStart;
        while (c < lineEnd && (text.at(c) == QLatin1Char(' ') || text.at(c) == QLatin1Char('\t'))) {
            ++c;
        }
        const bool indented = (c - lineStart) >= 4;
        const int first = c; // first non-blank column (lineEnd for blank lines)
        const bool fenceLine = !indented && first + 2 < lineEnd && text.at(first) == QLatin1Char('`')
            && text.at(first + 1) == QLatin1Char('`') && text.at(first + 2) == QLatin1Char('`');
        const bool tildeLine = !indented && first + 2 < lineEnd && text.at(first) == QLatin1Char('~')
            && text.at(first + 1) == QLatin1Char('~') && text.at(first + 2) == QLatin1Char('~');

        if (fence.isNull()) {
            if (lineNo == 0 && first == lineStart && lineEnd - lineStart >= 3
                && text.mid(lineStart, 3) == QLatin1String("---")) {
                // Front matter: the very first line is exactly "---" (possibly
                // with trailing spaces) — same rule as preview.js's FRONT_MATTER.
                bool onlySpaces = true;
                for (int k = lineStart + 3; k < lineEnd; ++k) {
                    if (!text.at(k).isSpace()) {
                        onlySpaces = false;
                        break;
                    }
                }
                if (onlySpaces) {
                    engines |= kEngineYaml;
                }
            }
            if (fenceLine) {
                engines |= kEngineHljs;
                fence = QLatin1Char('`');
            } else if (tildeLine) {
                engines |= kEngineHljs;
                fence = QLatin1Char('~');
            } else if (lineLooksLikeMath(text, lineStart, lineEnd)) {
                engines |= kEngineKatex;
            }
        } else if ((fence == QLatin1Char('`') && fenceLine) || (fence == QLatin1Char('~') && tildeLine)) {
            fence = QChar(); // closing fence: the marker must match
        }
        // Everything inside a fence is skipped by the math scan.

        if (engines == kEngineAll) {
            break;
        }
        lineStart = nl < 0 ? len + 1 : nl + 1;
        ++lineNo;
    }
    return engines;
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
    // Apply the configured V8 heap cap before this widget creates its own
    // profile below (which is what starts the web engine when the preview is
    // the first QWebEngine user in the process — see kdxchromiumflags.h).
    // The plugin constructor already tried this at plugin load; repeating it
    // here covers direct use (the tests drive this widget without the
    // plugin) and makes the call order-independent. Idempotent; no-op when
    // the environment already carries explicit --js-flags or the cap is off.
    kdxchromiumflags::applyV8HeapCap(kdxchromiumflags::effectiveV8HeapCapMb(Settings::self()->v8HeapCapMb()));

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
    // A Markdown reader does not use Chromium's interactive feature surface;
    // disabling it trims the renderer's GPU/compositor-side memory and raster
    // work. Images, scrolling, links and same-page anchors stay enabled.
    auto *webSettings = m_web->settings();
    webSettings->setAttribute(QWebEngineSettings::FocusOnNavigationEnabled, false);
    webSettings->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, true);
    webSettings->setAttribute(QWebEngineSettings::WebGLEnabled, false);
    webSettings->setAttribute(QWebEngineSettings::Accelerated2dCanvasEnabled, false);
    webSettings->setAttribute(QWebEngineSettings::PdfViewerEnabled, false);
    webSettings->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
    webSettings->setAttribute(QWebEngineSettings::FullScreenSupportEnabled, false);
    webSettings->setAttribute(QWebEngineSettings::LocalStorageEnabled, false);
    webSettings->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, true);
    // Watch the view for the lazily-created render widget so we can attach to it (below).
    m_web->installEventFilter(this);
    installInputFilter();
    layout->addWidget(m_web);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(150);
    connect(m_debounce, &QTimer::timeout, this, &PreviewWidget::render);

    // A discarded page has no renderer: until Qt reloads it (on the next
    // panel open, or — during a maintenance recycle — through loadPage()) it
    // is not "loaded", so renders/theme pushes skip it and the load pipeline
    // re-applies everything once the page is back. A discarded renderer has
    // given every byte back, so the maintenance counters reset here; the same
    // is true for a page the closed-panel policy froze (QtWebEngine's freeze
    // runs Chromium's memory purge, see lazyrender.md). Discard is also the
    // recycle primitive: while m_recycling is set, the discard was requested
    // by the maintenance cycle and the mirrored document is loaded straight
    // into the fresh renderer instead of waiting for the panel to reopen.
    connect(page, &QWebEnginePage::lifecycleStateChanged, this, [this](QWebEnginePage::LifecycleState state) {
        if (state == QWebEnginePage::LifecycleState::Discarded) {
            m_loaded = false;
            m_memEstimate = 0;
            m_lastRecycle.restart();
            // A discarded page's JS context (and its decode counter) is gone:
            // the fresh page starts from zero, and its first render replaces
            // nothing, so it must not be charged like a content-replacing
            // render (that would keep a large idle document "due" forever and
            // recycle it in a loop — see render()/noteRenderWork).
            m_renderedOnce = false;
            m_decodePollPending = false;
            m_lastDecodeCount = 0;
            m_lastDecodeBytes = 0;
            if (m_recycling && !m_panelClosed && m_pendingExportPath.isEmpty()) {
                // The recycle's fresh page: load the mirrored document now.
                loadPage();
            } else if (m_recycling && m_panelClosed) {
                // The panel closed mid-cycle: the closed-panel policy owns the
                // discarded page from here on.
                m_recycling = false;
                m_recycleScrollY = -1;
                m_recycleTimer->stop();
            }
        } else if (state == QWebEnginePage::LifecycleState::Frozen) {
            // Closed-panel freeze ran Chromium's purge: the estimate is spent.
            m_memEstimate = 0;
            m_lastRecycle.restart();
            m_decodePollPending = false;
        }
        noteActivity();
        if (qEnvironmentVariableIsSet("KATEXDOWN_DEBUG")) {
            qDebug() << "[katexdown] page lifecycle state" << int(state);
        }
    });

    // Closed-panel idle policy (panelClosed/panelOpened/idleTick): freezing on
    // close is instant; only the release of a long-closed preview waits for
    // m_idleDiscardMs. KATEXDOWN_IDLE_DISCARD_MS is a test hook (like
    // KATEXDOWN_DEBUG) to shorten that delay.
    const int overrideMs = qEnvironmentVariableIntValue("KATEXDOWN_IDLE_DISCARD_MS");
    if (overrideMs > 0) {
        m_idleDiscardMs = overrideMs;
    }
    // Renderer-memory maintenance knobs (see idleTick / performMemoryRecycle):
    // env overrides double as test hooks, exactly like the discard delay.
    readMemTuning();
    // Safety net for a recycle: if the discard never sticks or the fresh load
    // never finishes, the page must not stay hidden. (The normal path ends a
    // recycle at loadFinished, which beats this timer by orders of magnitude.)
    m_recycleTimer = new QTimer(this);
    m_recycleTimer->setSingleShot(true);
    connect(m_recycleTimer, &QTimer::timeout, this, &PreviewWidget::abortRecycle);
    m_lastActivity.start();
    m_lastRecycle.start();
    // One policy ticker serves both regimes: while the panel is closed it
    // drives the freeze/discard state machine, while it is open it schedules
    // renderer-memory maintenance recycles. A 1 Hz tick costs nothing.
    m_idleTimer = new QTimer(this);
    m_idleTimer->setInterval(1000);
    connect(m_idleTimer, &QTimer::timeout, this, &PreviewWidget::idleTick);
    m_idleTimer->start();

    connect(m_web, &QWebEngineView::loadFinished, this, [this](bool ok) {
        if (!ok) {
            // A failed load must not leave a mid-recycle page hidden; a failed
            // ordinary load has nothing to render anyway.
            if (m_recycling) {
                finalizeRecycle();
            }
            return;
        }
        m_loaded = true;
        // Each setHtml() is a fresh navigation; dropping the back/forward list
        // keeps previously rendered documents from lingering in the page cache
        // after a document switch (the preview never navigates back/forward).
        m_web->history()->clear();
        installInputFilter();
        // A fresh navigation brought a fresh JS context: the page's decode
        // counter restarted at zero, so the poll bookkeeping must follow.
        m_decodePollPending = false;
        m_lastDecodeCount = 0;
        m_lastDecodeBytes = 0;
        applyTheme();
        // Apply the image mode before the first render so the page never
        // renders once in the default mode and again in the configured one
        // (__setImageMode re-renders only when the mode actually changes).
        applyImageMode();
        render();
        applyOutlineSettings();
        if (!m_pendingExportPath.isEmpty()) {
            performExport(m_pendingExportPath);
        }
        // The panel may have closed while this page was still loading (Qt only
        // freezes loaded, hidden pages): apply the closed-panel policy now.
        if (m_panelClosed) {
            m_web->page()->setLifecycleState(QWebEnginePage::LifecycleState::Frozen);
        }
        noteActivity();
        // The fresh page of a maintenance recycle finished rendering: end the
        // cycle last, so the scroll restore lands on the rendered content.
        if (m_recycling) {
            finalizeRecycle();
        }
    });

    connect(Settings::self(), &Settings::changed, this, &PreviewWidget::applyTheme);
    connect(Settings::self(), &Settings::changed, this, &PreviewWidget::applyMediaPolicy);
    connect(Settings::self(), &Settings::changed, this, &PreviewWidget::applyOutlineSettings);
    connect(Settings::self(), &Settings::changed, this, &PreviewWidget::applyImageMode);

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
    // Renderer-memory maintenance at a document switch: when the current
    // renderer has accumulated enough dead memory, this switch is the moment
    // to recycle it — the page is discarded (its renderer process exits and
    // every byte is returned) and the fresh page loads the newly attached
    // document directly. The user perceives exactly the reload a document
    // switch already is, so the maintenance is invisible by construction
    // (see lazyrender.md / the renderer-memory maintenance below).
    if (m_loaded && doc && !m_panelClosed && m_pendingExportPath.isEmpty() && recycleDue()) {
        if (qEnvironmentVariableIsSet("KATEXDOWN_DEBUG")) {
            qDebug() << "[katexdown] document switch doubles as a renderer-memory recycle";
        }
        m_url = doc->url();
        updateTitle();
        noteActivity();
        // new document: start at the top. If the recycle cannot start right
        // now (e.g. a load is still settling), fall through to the normal
        // document load below — the next switch can recycle instead.
        if (performMemoryRecycle(false)) {
            return;
        }
    }
    // Reloadless switch: when the page that is currently loaded was built for
    // the same document folder (same relative-image base and the same local
    // file guard) and this text needs no engine the page lacks, re-rendering
    // in place is enough — a full setHtml navigation would re-execute every
    // inlined engine for no visible gain and is exactly the churn that makes
    // a long-lived renderer accumulate memory (see lazyrender.md / the
    // renderer-memory maintenance below). This is the common "a few markdown
    // tabs in one project" case; anything that changes the folder, the page
    // state or the engine set falls back to the full load below.
    bool canReloadless = false;
    if (m_loaded && doc && !m_panelClosed && m_pendingExportPath.isEmpty()) {
        const QUrl url = doc->url();
        const auto folderOf = [](const QUrl &u) {
            return u.isLocalFile() ? QFileInfo(u.toLocalFile()).absolutePath() : QString();
        };
        if (folderOf(url) == folderOf(m_pageUrl) && !url.isEmpty()) {
            m_text = doc->text(); // the freshest text decides the engine set
            canReloadless = (enginesForText(m_text) & (kEngineAll & ~m_pageEngines)) == 0;
        }
    }
    if (canReloadless) {
        m_url = doc->url();
        if (qEnvironmentVariableIsSet("KATEXDOWN_DEBUG")) {
            qDebug() << "[katexdown] reloadless document switch (same folder, same engines)";
        }
        m_pendingScrollReset = true;
        scheduleRender(); // pushes the new text; render() resets the scroll
        noteActivity();
        updateTitle();
        return;
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

QString PreviewWidget::buildHtml(int engines) const
{
    const QString base = QStringLiteral(":/katexdown/");
    QString html = readAsset(base + QStringLiteral("preview.html"));
    // The bundled GitHub stylesheet is on by default but can be disabled so a
    // custom stylesheet owns the whole layout. Empty <style> is harmless.
    const QString githubCss = Settings::self()->useGithubCss() ? readAsset(base + QStringLiteral("css/github-markdown.css")) : QString();
    html.replace(QLatin1String("/*__GHMD_CSS__*/"), githubCss);
    html.replace(QLatin1String("/*__BASE_CSS__*/"), readAsset(base + QStringLiteral("css/base.css")));
    // The syntax-highlight stylesheets are small and only match .hljs elements
    // (which exist only after highlight.js ran); keeping them on every page
    // means a theme switch can never leave a gap.
    html.replace(QLatin1String("/*__HLJS_LIGHT__*/"), readAsset(base + QStringLiteral("css/hljs-github.min.css")));
    html.replace(QLatin1String("/*__HLJS_DARK__*/"), readAsset(base + QStringLiteral("css/hljs-github-dark.min.css")));
    html.replace(QLatin1String("/*__USER_CSS__*/"), userCss());
    // markdown-it is the renderer core: always present. preview.js and
    // preview.html treat an engine slot left empty as "feature off", exactly
    // like a page whose data-dir assets are missing.
    html.replace(QLatin1String("/*__MARKDOWN_IT__*/"), shieldScript(readAsset(base + QStringLiteral("js/markdown-it.min.js"))));
    html.replace(QLatin1String("/*__PREVIEW_JS__*/"), shieldScript(readAsset(base + QStringLiteral("js/preview.js"))));
    // Optional engines, gated by the bits loadPage() chose for this text.
    // KaTeX is by far the heaviest (hundreds of KB of JS/CSS plus font data),
    // so its stylesheet is gated together with its scripts.
    const QString katexCss = (engines & kEngineKatex) ? optionalAsset(QStringLiteral("katex-standalone.min.css")) : QString();
    const QString katexJs = (engines & kEngineKatex) ? optionalAsset(QStringLiteral("katex.min.js")) : QString();
    const QString texmathJs = (engines & kEngineKatex) ? optionalAsset(QStringLiteral("texmath.min.js")) : QString();
    const QString hljsJs = (engines & kEngineHljs) ? readAsset(base + QStringLiteral("js/highlight.min.js")) : QString();
    const QString yamlJs = (engines & kEngineYaml) ? readAsset(base + QStringLiteral("js/js-yaml.min.js")) : QString();
    html.replace(QLatin1String("/*__KATEX_CSS__*/"), katexCss);
    html.replace(QLatin1String("/*__KATEX_JS__*/"), shieldScript(katexJs));
    html.replace(QLatin1String("/*__TEXMATH_JS__*/"), shieldScript(texmathJs));
    html.replace(QLatin1String("/*__HLJS_JS__*/"), shieldScript(hljsJs));
    html.replace(QLatin1String("/*__JS_YAML__*/"), shieldScript(yamlJs));
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
    // Engine selection below must reflect the live text: a document switch can
    // arrive before the page ever rendered (m_text would otherwise hold the
    // previous document's content).
    if (m_doc && !m_bufferStale) {
        m_text = m_doc->text();
    }
    if (qEnvironmentVariableIsSet("KATEXDOWN_DEBUG")) {
        qDebug() << "[katexdown] loadPage (full setHtml rebuild), doc url:" << (m_doc ? m_doc->url().toString() : QStringLiteral("(none)"));
    }
    if (m_doc) {
        m_url = m_doc->url();
    }
    const QString root = m_url.isLocalFile() ? QFileInfo(m_url.toLocalFile()).absolutePath() : QString();
    static_cast<LocalFileGuard *>(m_guard)->setRoot(root);
    m_loaded = false;
    // Inline only the engines this text can use (see enginesForText). If the
    // text later grows into an engine the page lacks, render() rebuilds it.
    m_pageEngines = enginesForText(m_text);
    m_pageUrl = m_url; // the folder this page will resolve relative paths against
    noteRenderWork(0, true); // every full load costs the renderer a fixed chunk
    noteActivity();
    if (qEnvironmentVariableIsSet("KATEXDOWN_DEBUG")) {
        qDebug() << "[katexdown]   built with engines:" << m_pageEngines;
    }
    m_web->setHtml(buildHtml(m_pageEngines), baseUrl());
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
        // A discarded page restores (reloads) on demand, which then finishes
        // the export through the normal load pipeline; anything else that is
        // not loaded yet finishes as soon as its load does.
        if (m_web->page()->lifecycleState() == QWebEnginePage::LifecycleState::Discarded) {
            m_web->page()->setLifecycleState(QWebEnginePage::LifecycleState::Active);
        }
        m_pendingExportPath = path;
        return true;
    }
    // The live text can need an engine the page was built without (math or a
    // code fence added after the last load). Rebuild the page once so the
    // export serializes the fully rendered document; the deferred export runs
    // when that load finishes.
    if (m_doc && !m_bufferStale) {
        m_text = m_doc->text();
    }
    if (m_pageEngines != kEngineAll && (enginesForText(m_text) & (kEngineAll & ~m_pageEngines)) != 0) {
        m_pendingExportPath = path;
        loadPage();
        return true;
    }
    performExport(path);
    return true;
}

void PreviewWidget::performExport(const QString &path)
{
    m_pendingExportPath.clear();
    // Callers (exportToFile, loadFinished after render()) have already pulled
    // the live text into m_text; push it once more so the serialized DOM is
    // guaranteed to match the current buffer.
    noteRenderWork(m_text.size(), false);
    noteActivity();
    runJs(QStringLiteral("window.__setMarkdown(%1);").arg(jsLiteral(m_text)));
    m_renderedOnce = true;
    // Re-render is synchronous in the page; give the compositor a moment and
    // then serialize the current DOM, styles and all.
    QTimer::singleShot(300, this, [this, path]() {
        // Serialize the current DOM through the page's export hook: __serializedHtml
    // returns documentElement.outerHTML with the memory-saving image machinery
    // normalized away (real srcs restored, lazy/placeholder state stripped), so
    // a standalone file always carries plain, eager images no matter which
    // image mode the live page runs in.
    m_web->page()->runJavaScript(QStringLiteral("window.__serializedHtml ? window.__serializedHtml() : document.documentElement.outerHTML"),
                                 [this, path](const QVariant &html) {
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
        // A fragment link ("../doc.md#section") names a section as well as a
        // document. Kate opens the document and drops the fragment there;
        // remember it so the preview can land on the section once this
        // document renders here (see render). When the preview is already
        // showing the target document and nothing is about to re-render it
        // (Kate only re-focused the same document), apply the anchor right
        // away; a live document is required, because for the frozen snapshot
        // of a closed document the click opens the file afresh and its render
        // applies the anchor.
        if (url.hasFragment()) {
            m_pendingFragmentDoc = url.adjusted(QUrl::RemoveFragment);
            m_pendingFragment = url.fragment();
            if (m_doc && m_loaded && !m_debounce->isActive() && m_pendingExportPath.isEmpty() && !m_recycling
                && sameDocumentAsCurrent(m_pendingFragmentDoc)) {
                attemptFragmentJump();
            }
        }
        m_mainWindow->openUrl(url);
    } else if (!url.scheme().isEmpty()) {
        QDesktopServices::openUrl(url);
    }
}

// Ask the page to scroll the pending fragment's section into view. The page
// answers whether it found the section; when it did not yet — typically
// because Kate opened the clicked document asynchronously, so the first render
// ran on empty text — the anchor stays pending and the next render of the
// target document retries it. render() clears it once any other document
// renders instead.
void PreviewWidget::attemptFragmentJump()
{
    if (m_pendingFragment.isEmpty() || !m_loaded) {
        return;
    }
    const QString code = QStringLiteral("window.__scrollToFragment(%1);").arg(jsLiteral(m_pendingFragment));
    m_web->page()->runJavaScript(code, [this](const QVariant &found) {
        if (found.toBool()) {
            m_pendingFragment.clear();
            m_pendingFragmentDoc = QUrl();
        }
    });
}

// Does doc name the document the preview is currently showing? m_url holds
// the document URL both while it is live (m_doc set) and after the editor tab
// closed (frozen snapshot of the last content).
bool PreviewWidget::sameDocumentAsCurrent(const QUrl &doc) const
{
    const QUrl current = (m_doc && !m_doc->url().isEmpty()) ? m_doc->url() : m_url;
    const QString a = doc.isLocalFile() ? QFileInfo(doc.toLocalFile()).absoluteFilePath() : QString();
    const QString b = current.isLocalFile() ? QFileInfo(current.toLocalFile()).absoluteFilePath() : QString();
    return !a.isEmpty() && a == b;
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

void PreviewWidget::applyImageMode()
{
    if (!m_loaded) {
        return;
    }
    const char *mode = nullptr;
    switch (Settings::self()->imageMode()) {
    case Settings::DecodeAll:
        mode = "eager";
        break;
    case Settings::MemorySaver:
        mode = "saver";
        break;
    case Settings::Adaptive:
    default:
        mode = "auto";
        break;
    }
    runJs(QStringLiteral("window.__setImageMode('%1');").arg(QLatin1String(mode)));
}

void PreviewWidget::render()
{
    if (!m_loaded) {
        return;
    }
    // During a recycle the page is hidden and then discarded (m_loaded false),
    // so pushes below simply skip until the fresh page loads and re-renders
    // the current text through the normal pipeline — nothing to cancel here.
    if (m_doc) {
        m_text = m_doc->text();
        m_bufferStale = false; // live content supersedes any close-time snapshot
    }
    // Cheap gate for the decode poll (see idleTick): only documents that can
    // contain images justify a per-second JS roundtrip. A false positive ("!["
    // inside a code fence) only costs a harmless poll that reports 0 decodes.
    m_textMayHaveImages = m_text.contains(QLatin1String("![")) || m_text.contains(QLatin1String("<img"));
    // A document can grow into engines the page was built without (math, a
    // code fence or front matter typed in after the last load). Rebuild the
    // page once so the engines join; loadFinished() then renders the current
    // text. Fully equipped pages skip the scan entirely.
    if (m_pageEngines != kEngineAll && (enginesForText(m_text) & (kEngineAll & ~m_pageEngines)) != 0) {
        if (qEnvironmentVariableIsSet("KATEXDOWN_DEBUG")) {
            qDebug() << "[katexdown] rebuilding page: text needs an engine the page lacks";
        }
        loadPage();
        return;
    }
    // Every full-document push that *replaces* already-rendered content costs
    // the renderer a chunk proportional to the text (measured in the
    // renderer-memory maintenance notes); count it now. A page's first render
    // (first open, or the fresh load after a maintenance recycle) replaces
    // nothing, so it is not charged — charging it would keep a large idle
    // document's estimate over the budget forever and recycle it in a loop.
    if (m_renderedOnce) {
        noteRenderWork(m_text.size(), false);
    }
    runJs(QStringLiteral("window.__setMarkdown(%1);").arg(jsLiteral(m_text)));
    m_renderedOnce = true;
    if (m_pendingScrollReset) {
        m_pendingScrollReset = false;
        runJs(QStringLiteral("window.scrollTo(0, 0);"));
    }
    // A fragment link whose target this document is: land on its section now
    // that the content is in the DOM (the anchor survives renders that ran
    // before an asynchronously opened document had text, until the page finds
    // the section). A render of any other document drops the anchor instead.
    if (!m_pendingFragment.isEmpty()) {
        if (sameDocumentAsCurrent(m_pendingFragmentDoc)) {
            attemptFragmentJump();
        } else {
            m_pendingFragment.clear();
            m_pendingFragmentDoc = QUrl();
        }
    }
    noteActivity();
}

void PreviewWidget::scheduleRender()
{
    noteActivity();
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

void PreviewWidget::panelClosed(bool releaseWhenIdle)
{
    m_panelClosed = true;
    m_releaseWhenIdle = releaseWhenIdle;
    m_closedSince.restart();
    // If a recycle is mid-flight, it must not un-hide or load on its own once
    // the panel is closed: the Discarded page below belongs to the closed-panel
    // policy now, and the cycle finalizes without touching it (see the discard
    // branch of the lifecycle handler and finalizeRecycle).
    // A closed panel does not need a live page. Qt only freezes loaded, hidden
    // pages, and it learns about the hide asynchronously — the request below
    // can be refused while the hide event is still being delivered, so the
    // idle timer retries every second until the freeze sticks (a frozen page
    // keeps its renderer and content and still accepts script pushes, so
    // toggling the panel back is instant). The timer also drives the
    // open-panel renderer-memory maintenance, so it never stops.
    if (m_loaded) {
        m_web->page()->setLifecycleState(QWebEnginePage::LifecycleState::Frozen);
    }
    noteActivity();
}

void PreviewWidget::panelOpened()
{
    m_panelClosed = false;
    // Unfreeze (instant for a frozen page); if the page was discarded while
    // closed, Qt reloads it now and the normal load pipeline refreshes the
    // mirrored content.
    if (m_web->page()->lifecycleState() != QWebEnginePage::LifecycleState::Active) {
        m_web->page()->setLifecycleState(QWebEnginePage::LifecycleState::Active);
    }
    noteActivity();
}

// One policy tick, every second. While the panel is closed it walks the
// freeze/discard state machine (a closed preview costs no CPU, and a
// long-closed LazyKeep page releases its renderer). While the panel is open
// it runs the renderer-memory maintenance: Chromium never reclaims the dead
// memory a long-lived renderer accumulates on its own — Qt refuses to freeze
// or discard a visible page, and the only reliable reclamation is letting the
// renderer process die — so once the estimated dead memory since the last
// recycle passes a budget (or a pure-time backstop expires) and the preview
// has been quiet for a while, the renderer is recycled (performMemoryRecycle).
// Document switches recycle as part of the switch instead (attachDocument),
// which the user perceives as the normal reload a switch already is.
void PreviewWidget::idleTick()
{
    if (m_panelClosed) {
        if (m_web->page()->lifecycleState() == QWebEnginePage::LifecycleState::Discarded) {
            return; // already released: nothing left to do while closed
        }
        if (!m_loaded || !m_pendingExportPath.isEmpty()) {
            return; // page still loading or an export is owed; try again next tick
        }
        const QWebEnginePage::LifecycleState state = m_web->page()->lifecycleState();
        if (state != QWebEnginePage::LifecycleState::Frozen) {
            // The freeze requested by panelClosed() may have been refused while Qt
            // was still delivering the hide event; now that the page is loaded and
            // hidden it can stick.
            m_web->page()->setLifecycleState(QWebEnginePage::LifecycleState::Frozen);
            return;
        }
        // Frozen and, in LazyKeep (releaseWhenIdle), closed long enough: release
        // the renderer entirely. The page comes back automatically (a reload) the
        // next time the panel opens. Eager stays frozen forever.
        if (m_releaseWhenIdle && m_closedSince.hasExpired(m_idleDiscardMs)) {
            if (qEnvironmentVariableIsSet("KATEXDOWN_DEBUG")) {
                qDebug() << "[katexdown] discarding the page of the long-closed preview";
            }
            m_web->page()->setLifecycleState(QWebEnginePage::LifecycleState::Discarded);
        }
        return;
    }

    // Open panel: renderer-memory maintenance.
    if (m_recycling) {
        // The discard may have been refused while Qt's visibility flag was
        // still catching up ("page is visible"); keep trying for a few ticks.
        // abortRecycle's timer is the backstop that guarantees the preview can
        // never stay hidden.
        if (!m_panelClosed && m_web->page()->lifecycleState() == QWebEnginePage::LifecycleState::Active
            && ++m_recycleDiscardTries <= 10) {
            m_web->page()->setVisible(false);
            m_web->page()->setLifecycleState(QWebEnginePage::LifecycleState::Discarded);
        }
        return;
    }
    if (!m_loaded) {
        return;
    }
    if (m_web->page()->lifecycleState() != QWebEnginePage::LifecycleState::Active) {
        return; // e.g. a load still settling
    }
    if (!m_web->isVisible()) {
        return; // only recycle a page the user can actually see
    }
    if (!recycleDue() || !recycleIdle() || webHasFocus()) {
        // Not recycling on this tick. If the document can contain images, poll
        // the page's decode counter anyway: image decodes are dirt that arrives
        // without a render or a navigation, and the poll callback re-checks the
        // recycle gates once the charge pushes the estimate over the budget.
        if (m_textMayHaveImages && !m_decodePollPending) {
            m_decodePollPending = true;
            m_web->page()->runJavaScript(
                QStringLiteral("window.__kdxDecodeStats ? window.__kdxDecodeStats() : '0:0'"),
                [this](const QVariant &v) { handleDecodeStats(v.toString()); });
        }
        return;
    }
    performMemoryRecycle(true); // same document: keep the scroll position
}

// Run one renderer-memory maintenance cycle (see the idleTick comment for why
// this is the reclaim primitive). The page is told it is hidden, discarded —
// its renderer process exits and every byte it ever held is returned — and a
// fresh page is then loaded from the mirrored document state, so content
// returns through the normal load pipeline. restoreScroll keeps the previous
// scroll position (same-document idle recycle); document-switch recycles pass
// false (a new document starts at the top). Public so tests can drive it
// directly; the policy ticker applies the quiet/focus/visibility gates first.
bool PreviewWidget::performMemoryRecycle(bool restoreScroll)
{
    if (!m_loaded || m_recycling || m_panelClosed || !m_pendingExportPath.isEmpty()
        || m_web->page()->lifecycleState() != QWebEnginePage::LifecycleState::Active) {
        return false;
    }
    if (qEnvironmentVariableIsSet("KATEXDOWN_DEBUG")) {
        qDebug() << "[katexdown] renderer-memory recycle: discarding the page to reclaim renderer memory";
    }
    m_recycling = true;
    m_recycleDiscardTries = 0;
    if (restoreScroll) {
        // window.scrollY answers asynchronously; start the cycle once we have it.
        m_web->page()->runJavaScript(QStringLiteral("window.scrollY"), [this](const QVariant &v) {
            startRecycleAfterScrollQuery(v.toDouble());
        });
    } else {
        startRecycleAfterScrollQuery(-1);
    }
    return true;
}

void PreviewWidget::startRecycleAfterScrollQuery(double scrollY)
{
    if (!m_recycling) {
        return; // aborted while the query was in flight
    }
    m_recycleScrollY = scrollY;
    beginRecycle();
}

void PreviewWidget::beginRecycle()
{
    if (m_panelClosed) {
        // The panel closed while the scroll query was in flight: the recycle
        // must not fight the closed-panel policy (Eager freezes closed pages
        // forever; LazyKeep decides itself when to release one).
        m_recycling = false;
        m_recycleScrollY = -1;
        m_recycleTimer->stop();
        noteActivity();
        return;
    }
    // Qt refuses to discard a page it considers visible ("page is visible");
    // telling the page it is hidden is what makes the lifecycle move legal.
    // The widget stays on screen; QtWebEngine keeps the last composited frame
    // while the page is hidden, so a same-document recycle shows a static page
    // for a few hundred ms and then the identical content again. If the
    // discard is refused anyway (Qt's visibility flag can lag), idleTick
    // retries while m_recycling is set, and the safety timer aborts the cycle
    // so the preview can never stay hidden.
    m_web->page()->setVisible(false);
    m_web->page()->setLifecycleState(QWebEnginePage::LifecycleState::Discarded);
    m_recycleTimer->start(5000);
}

// End of a recycle cycle: the fresh page finished loading (or the cycle was
// abandoned). Un-hide the page, restore the scroll for a same-document
// recycle, and reset the maintenance state. If the panel closed mid-cycle,
// the page belongs to the closed-panel policy and stays hidden/discarded.
void PreviewWidget::finalizeRecycle()
{
    if (!m_recycling) {
        return;
    }
    m_recycleTimer->stop();
    m_recycling = false;
    m_recycleDiscardTries = 0;
    if (!m_panelClosed) {
        m_web->page()->setVisible(true);
        if (m_recycleScrollY >= 0) {
            runJs(QStringLiteral("window.scrollTo(0, %1);").arg(m_recycleScrollY, 0, 'f', 1));
        }
    }
    m_recycleScrollY = -1;
    noteActivity();
}

// The safety net: a discard that never stuck, or a fresh load that never
// finished, must not leave the preview hidden. Un-hide and give up on the
// cycle; the next budget crossing will try again.
void PreviewWidget::abortRecycle()
{
    if (!m_recycling) {
        return;
    }
    m_recycleTimer->stop();
    m_recycling = false;
    m_recycleScrollY = -1;
    if (!m_panelClosed) {
        m_web->page()->setVisible(true);
    }
    noteActivity();
}

// The page answered the 1 Hz decode poll: charge any decodes that happened
// since the last poll against the dead-memory estimate, then apply the same
// gates idleTick would (panel open, page loaded/active/visible, quiet, not
// focused) and recycle when the budget is now crossed. Without this an
// image-heavy reading session accumulates sticky decoded frames the renderer
// never returns (measured at ~8-10 MB per photo scrolled past, in every image
// mode) while the estimate stays low and the maintenance never fires.
void PreviewWidget::handleDecodeStats(const QString &stats)
{
    m_decodePollPending = false;
    const int colon = stats.indexOf(QLatin1Char(':'));
    bool okCount = false;
    bool okBytes = false;
    const double count = colon > 0 ? stats.left(colon).toDouble(&okCount) : 0;
    const double bytes = colon > 0 ? stats.mid(colon + 1).toDouble(&okBytes) : 0;
    const double dCount = okCount ? count - m_lastDecodeCount : 0;
    const double dBytes = okBytes ? bytes - m_lastDecodeBytes : 0;
    if (dCount <= 0 && dBytes <= 0) {
        return; // nothing decoded since the last poll
    }
    m_lastDecodeCount = count;
    m_lastDecodeBytes = bytes;

    // KATEXDOWN_MEM_IMG_CHARGE_MB overrides the charge with a flat amount per
    // decode (deterministic for tests); otherwise the page's own conservative
    // byte figure is used (intrinsic pixels of the decode, capped).
    qint64 charge = 0;
    if (m_imgFlatChargeBytes > 0) {
        charge = m_imgFlatChargeBytes * static_cast<qint64>(dCount);
    } else if (dBytes > 0) {
        charge = static_cast<qint64>(dBytes);
    }
    if (charge > 0) {
        m_memEstimate += charge;
        if (qEnvironmentVariableIsSet("KATEXDOWN_DEBUG")) {
            qDebug() << "[katexdown] image decodes since last poll:" << dCount << "charging" << charge << "bytes";
        }
    }

    if (m_panelClosed || m_recycling || !m_loaded || !m_pendingExportPath.isEmpty()) {
        return;
    }
    if (m_web->page()->lifecycleState() != QWebEnginePage::LifecycleState::Active) {
        return;
    }
    if (!m_web->isVisible()) {
        return;
    }
    if (!recycleDue() || !recycleIdle() || webHasFocus()) {
        return;
    }
    performMemoryRecycle(true);
}

// Renderer-memory estimate (see the maintenance design in lazyrender.md): a
// full page load costs the renderer a fixed, content-independent chunk
// (re-executing every inlined engine), and every full-document markdown push
// costs a chunk proportional to the text — measured at roughly 0.5-0.9 kB of
// dead renderer memory per byte of markdown in the test environment. The
// estimate is deliberately conservative (over- rather than under-counts) so
// memory stays bounded even where the real ratio is higher than measured.
void PreviewWidget::noteRenderWork(qint64 textBytes, bool navigation)
{
    // 4 MB per full load, ~0.5 kB leaked per byte of pushed markdown.
    constexpr qint64 kFixedPerLoad = 4ll * 1024 * 1024;
    constexpr qint64 kLeakFactor = 512;
    if (navigation) {
        m_memEstimate += kFixedPerLoad;
    }
    if (textBytes > 0) {
        m_memEstimate += textBytes * kLeakFactor;
    }
}

void PreviewWidget::noteActivity()
{
    m_lastActivity.restart();
}

bool PreviewWidget::recycleDue() const
{
    return m_memEstimate >= m_memBudgetBytes || m_lastRecycle.elapsed() >= m_memMaxAgeMs;
}

bool PreviewWidget::recycleIdle() const
{
    return !m_debounce->isActive() && m_pendingExportPath.isEmpty() && m_lastActivity.elapsed() >= m_memIdleMs;
}

bool PreviewWidget::webHasFocus() const
{
    if (!m_web) {
        return false;
    }
    if (m_web->hasFocus()) {
        return true;
    }
    QWidget *proxy = m_web->focusProxy();
    return proxy && proxy->hasFocus();
}

void PreviewWidget::readMemTuning()
{
    const int budgetMb = qEnvironmentVariableIntValue("KATEXDOWN_MEM_BUDGET_MB");
    if (budgetMb > 0) {
        m_memBudgetBytes = qint64(budgetMb) * 1024 * 1024;
    }
    const int idleMs = qEnvironmentVariableIntValue("KATEXDOWN_MEM_IDLE_MS");
    if (idleMs > 0) {
        m_memIdleMs = idleMs;
    }
    const int maxAgeMs = qEnvironmentVariableIntValue("KATEXDOWN_MEM_MAX_AGE_MS");
    if (maxAgeMs > 0) {
        m_memMaxAgeMs = maxAgeMs;
    }
    // Flat per-decode image charge (MiB), overriding the page-reported byte
    // figure; doubles as a deterministic test hook (KATEXDOWN_MEM_*).
    const int imgChargeMb = qEnvironmentVariableIntValue("KATEXDOWN_MEM_IMG_CHARGE_MB");
    if (imgChargeMb > 0) {
        m_imgFlatChargeBytes = qint64(imgChargeMb) * 1024 * 1024;
    }
    if (qEnvironmentVariableIsSet("KATEXDOWN_MEM_OFF")) {
        m_memBudgetBytes = std::numeric_limits<qint64>::max();
        m_memMaxAgeMs = std::numeric_limits<int>::max();
    }
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
        if (event->type() == QEvent::Resize) {
            noteActivity();
        }
        return QWidget::eventFilter(obj, event);
    }

    // Anything the user does to the page — keys, clicks, scrolling, focus —
    // is activity: it defers a maintenance recycle (see recycleIdle). A
    // recycle already mid-flight is safe to let finish — edits during it
    // simply wait for the fresh page's load pipeline, which re-renders the
    // latest text.
    switch (event->type()) {
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::Wheel:
    case QEvent::TouchBegin:
    case QEvent::FocusIn:
        noteActivity();
        break;
    default:
        break;
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
