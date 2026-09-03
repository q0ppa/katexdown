#include "configpage.h"
#include "katexdownpaths.h"
#include "settings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>

#include <KLocalizedString>

namespace
{
const QString repoUrl = QStringLiteral("https://github.com/uwuclxdy/katexdown");

QString currentVersion()
{
    return QStringLiteral(KATEXDOWN_VERSION);
}

// Compare dotted numeric versions ("0.10.0" > "0.9.1"). Returns -1/0/1. Any non-numeric
// suffix on a part is dropped by toInt(), which is fine: /releases/latest never returns a
// pre-release tag, so the inputs here are plain x.y.z.
int compareVersions(const QString &a, const QString &b)
{
    const QStringList pa = a.split(QLatin1Char('.'));
    const QStringList pb = b.split(QLatin1Char('.'));
    const int n = pa.size() > pb.size() ? pa.size() : pb.size();
    for (int i = 0; i < n; ++i) {
        const int va = i < pa.size() ? pa.at(i).toInt() : 0;
        const int vb = i < pb.size() ? pb.at(i).toInt() : 0;
        if (va != vb) {
            return va < vb ? -1 : 1;
        }
    }
    return 0;
}
} // namespace

ConfigPage::ConfigPage(QWidget *parent)
    : KTextEditor::ConfigPage(parent)
{
    auto *outer = new QVBoxLayout(this);
    auto *form = new QFormLayout();
    outer->addLayout(form);

    m_mode = new QComboBox(this);
    m_mode->addItem(i18n("GitHub"), Settings::GitHub);
    m_mode->addItem(i18n("Match editor / system theme"), Settings::Application);
    form->addRow(i18n("Style:"), m_mode);

    m_variant = new QComboBox(this);
    m_variant->addItem(i18n("Follow system (auto)"), Settings::Auto);
    m_variant->addItem(i18n("Light"), Settings::Light);
    m_variant->addItem(i18n("Dark"), Settings::Dark);
    form->addRow(i18n("GitHub variant:"), m_variant);

    m_loading = new QComboBox(this);
    m_loading->addItem(i18n("Lazy — load on first open; released when closed for a while"), Settings::LazyKeep);
    m_loading->addItem(i18n("Lazy — free everything when the preview is closed (minimal memory)"), Settings::LazyUnload);
    m_loading->addItem(i18n("Eager — load together with Kate (always ready)"), Settings::Eager);
    form->addRow(i18n("Preview loading:"), m_loading);
    auto *loadingHint = new QLabel(
        i18n("The preview is a web view with its own renderer process, which is what costs memory.\n"
             "\u2022 Lazy (default): created the first time you open the preview. Closing it freezes the page (no CPU, \n"
             "  instant re-open); once the panel has stayed closed for about a minute the page is released entirely, \n"
             "  and re-opening restores it automatically.\n"
             "\u2022 Lazy + free on close: destroyed whenever the panel is hidden; re-opening re-creates it (a short delay).\n"
             "\u2022 Eager: created at startup and never released; frozen while closed."),
        this);
    loadingHint->setWordWrap(true);
    loadingHint->setEnabled(false);
    form->addRow(QString(), loadingHint);

    m_imageMode = new QComboBox(this);
    m_imageMode->addItem(i18n("Eager — decode every image at once (classic)"), Settings::DecodeAll);
    m_imageMode->addItem(i18n("Auto — lazy decode; off-screen images are released in image-heavy documents (default)"), Settings::Adaptive);
    m_imageMode->addItem(i18n("Memory-saver — only images near the viewport stay decoded"), Settings::MemorySaver);
    form->addRow(i18n("Image memory mode:"), m_imageMode);
    auto *imageHint = new QLabel(
        i18n("How the preview deals with the images of large documents. The web engine decodes every image in a page at once when left to itself, so a document with hundreds of images can cost gigabytes of renderer memory.\n"
             "\u2022 Eager: everything decodes immediately — the classic behavior (heaviest).\n"
             "\u2022 Auto (default): images only decode as they approach the viewport; when a document grows many images, decoded memory is additionally released as images scroll out of it.\n"
             "\u2022 Memory-saver: only the images near the viewport ever decode, and each is released the moment it scrolls away. Renderer memory stays flat no matter how many images the document has."),
        this);
    imageHint->setWordWrap(true);
    imageHint->setEnabled(false);
    form->addRow(QString(), imageHint);

    // Renderer JS heap cap: the preview's renderer never collects the garbage
    // full re-renders leave behind on its own, so a cap makes V8 collect under
    // pressure and turns unbounded growth into a bounded plateau (see
    // design/lazyrender.md, "JS heap guard"). 0 disables the cap.
    m_v8Cap = new QSpinBox(this);
    m_v8Cap->setRange(0, 1024);
    m_v8Cap->setSingleStep(64);
    m_v8Cap->setSuffix(i18n(" MB"));
    m_v8Cap->setSpecialValueText(i18n("Off — Chromium's default (no cap)"));
    form->addRow(i18n("JS memory cap:"), m_v8Cap);
    auto *v8Hint = new QLabel(
        i18n("Bounds the preview's JavaScript heap (V8), in MB — the renderer otherwise never \n"
             "returns the memory that full re-renders of a document leave behind, and only a \n"
             "renderer restart reclaims it. The cap is free while a document's rendering fits \n"
             "under it and steps in once a session re-renders large documents again and again. \n"
             "Very large or math-heavy documents can render slower with a low cap; raise it or \n"
             "turn it off for those. Default 128 MB. Applies when the preview's web engine starts, \n"
             "so a change needs a Kate restart. An explicit --js-flags=… in the environment's \n"
             "QTWEBENGINE_CHROMIUM_FLAGS overrides this setting."),
        this);
    v8Hint->setWordWrap(true);
    v8Hint->setEnabled(false);
    form->addRow(QString(), v8Hint);

    m_githubCss = new QCheckBox(i18n("Use the built-in GitHub stylesheet"), this);
    form->addRow(QString(), m_githubCss);
    auto *cssHint = new QLabel(
        i18n("On by default. Disable it to let a custom stylesheet fully own the layout "
             "instead of layering on GitHub's look; add stylesheets below."),
        this);
    cssHint->setWordWrap(true);
    cssHint->setEnabled(false);
    form->addRow(QString(), cssHint);

    // Floating section outline: which heading levels it lists (H1-H5 default).
    auto *tocRowHost = new QWidget(this);
    auto *tocRow = new QHBoxLayout(tocRowHost);
    tocRow->setContentsMargins(0, 0, 0, 0);
    tocRow->setSpacing(6);
    for (int level = 1; level <= 6; ++level) {
        auto *box = new QCheckBox(QStringLiteral("H%1").arg(level), tocRowHost);
        box->setToolTip(i18n("List level %1 headings in the preview's section outline", level));
        m_tocLevel[level - 1] = box;
        tocRow->addWidget(box);
    }
    form->addRow(i18n("Section outline:"), tocRowHost);
    auto *tocHint = new QLabel(
        i18n("A floating button at the bottom right of the preview opens a list of the document's "
             "sections; click an entry to jump straight to that heading. Only the heading levels "
             "checked here are listed (H1-H5 by default). Unchecking all of them hides the button."),
        this);
    tocHint->setWordWrap(true);
    tocHint->setEnabled(false);
    form->addRow(QString(), tocHint);

    auto *hint = new QLabel(
        i18n("\"GitHub\" uses GitHub's own colors. \"Match editor / system theme\" recolors the "
             "same layout from the active editor theme, so the preview blends with the rest of Kate."),
        this);
    hint->setWordWrap(true);
    hint->setEnabled(false);
    outer->addWidget(hint);

    m_remoteMedia = new QCheckBox(i18n("Load media previews from remote URLs"), this);
    outer->addWidget(m_remoteMedia);

    auto *remoteHint = new QLabel(i18n("Off by default so the preview stays fully offline. Enable to let images load over http(s)."), this);
    remoteHint->setWordWrap(true);
    remoteHint->setEnabled(false);
    outer->addWidget(remoteHint);

    // Custom stylesheets: appended after the built-in github-markdown.css, in
    // listed order (later files win the cascade). Relative paths resolve
    // against the Katexdown data dir.
    auto *cssLabel = new QLabel(i18n("Custom stylesheets:"), this);
    cssLabel->setEnabled(false);
    outer->addWidget(cssLabel);

    m_cssList = new QListWidget(this);
    m_cssList->setMaximumHeight(96);
    m_cssList->setSelectionMode(QAbstractItemView::SingleSelection);
    outer->addWidget(m_cssList);

    auto *cssButtons = new QHBoxLayout();
    auto *cssAdd = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), i18n("Add…"), this);
    m_cssRemove = new QPushButton(QIcon::fromTheme(QStringLiteral("list-remove")), i18n("Remove"), this);
    m_cssUp = new QPushButton(QIcon::fromTheme(QStringLiteral("go-up")), QString(), this);
    m_cssDown = new QPushButton(QIcon::fromTheme(QStringLiteral("go-down")), QString(), this);
    m_cssRemove->setEnabled(false);
    m_cssUp->setEnabled(false);
    m_cssDown->setEnabled(false);
    cssButtons->addWidget(cssAdd);
    cssButtons->addWidget(m_cssRemove);
    cssButtons->addStretch(1);
    cssButtons->addWidget(m_cssUp);
    cssButtons->addWidget(m_cssDown);
    outer->addLayout(cssButtons);

    auto *listHint = new QLabel(
        i18n("Loaded in the order shown; a later file overrides an earlier one. Relative paths are "
             "resolved against the Katexdown data dir: <b>%1</b>.<br/>"
             "Math (LaTeX) works out of the box here too: run <tt>python3 tools/fetch-assets.py</tt> "
             "once (no arguments, no environment variables) and it downloads KaTeX into this folder.",
             katexdownpaths::dataDir()),
        this);
    listHint->setWordWrap(true);
    listHint->setEnabled(false);
    outer->addWidget(listHint);

    auto *folderRow = new QHBoxLayout();
    auto *openFolder = new QPushButton(QIcon::fromTheme(QStringLiteral("folder-open")), i18n("Open data folder…"), this);
    folderRow->addWidget(openFolder);
    folderRow->addStretch(1);
    outer->addLayout(folderRow);
    connect(openFolder, &QPushButton::clicked, this, []() {
        const QString dir = katexdownpaths::dataDir();
        QDir().mkpath(dir); // make sure it exists so the file manager opens it
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });

    m_net = new QNetworkAccessManager(this);

    auto *updateRow = new QHBoxLayout();
    m_checkButton = new QPushButton(i18n("Check for updates"), this);
    m_checkButton->setIcon(QIcon::fromTheme(QStringLiteral("system-software-update")));
    updateRow->addWidget(m_checkButton);
    m_updateStatus = new QLabel(i18n("Installed version: %1", currentVersion()), this);
    m_updateStatus->setTextFormat(Qt::RichText);
    m_updateStatus->setOpenExternalLinks(true);
    m_updateStatus->setTextInteractionFlags(Qt::TextBrowserInteraction);
    updateRow->addWidget(m_updateStatus, 1);
    outer->addLayout(updateRow);

    outer->addStretch();

    reset();

    connect(m_remoteMedia, &QCheckBox::toggled, this, [this]() { Q_EMIT changed(); });
    connect(m_githubCss, &QCheckBox::toggled, this, [this]() { Q_EMIT changed(); });
    connect(m_mode, &QComboBox::currentIndexChanged, this, [this]() {
        syncEnabled();
        Q_EMIT changed();
    });
    connect(m_variant, &QComboBox::currentIndexChanged, this, [this]() {
        Q_EMIT changed();
    });
    connect(m_loading, &QComboBox::currentIndexChanged, this, [this]() {
        Q_EMIT changed();
    });
    connect(m_imageMode, &QComboBox::currentIndexChanged, this, [this]() {
        Q_EMIT changed();
    });
    connect(m_v8Cap, &QSpinBox::valueChanged, this, [this]() {
        Q_EMIT changed();
    });
    for (int level = 1; level <= 6; ++level) {
        connect(m_tocLevel[level - 1], &QCheckBox::toggled, this, [this]() { Q_EMIT changed(); });
    }
    connect(m_checkButton, &QPushButton::clicked, this, &ConfigPage::checkForUpdates);

    connect(cssAdd, &QPushButton::clicked, this, &ConfigPage::addCssFile);
    connect(m_cssRemove, &QPushButton::clicked, this, &ConfigPage::removeCssFile);
    connect(m_cssUp, &QPushButton::clicked, this, [this]() { moveCssFile(-1); });
    connect(m_cssDown, &QPushButton::clicked, this, [this]() { moveCssFile(1); });
    connect(m_cssList, &QListWidget::itemSelectionChanged, this, [this]() {
        const bool has = m_cssList->currentRow() >= 0;
        m_cssRemove->setEnabled(has);
        m_cssUp->setEnabled(has && m_cssList->currentRow() > 0);
        m_cssDown->setEnabled(has && m_cssList->currentRow() < m_cssList->count() - 1);
    });
    connect(m_cssList, &QListWidget::itemChanged, this, [this](QListWidgetItem *) {
        Q_EMIT changed();
    });
}

QString ConfigPage::name() const
{
    return i18n("Katexdown");
}

QString ConfigPage::fullName() const
{
    return i18n("Katexdown");
}

QIcon ConfigPage::icon() const
{
    return QIcon::fromTheme(QStringLiteral("text-markdown"), QIcon::fromTheme(QStringLiteral("view-preview")));
}

void ConfigPage::syncEnabled()
{
    const bool github = m_mode->currentData().toInt() == Settings::GitHub;
    m_variant->setEnabled(github);
}

void ConfigPage::addCssFile()
{
    const QString file = QFileDialog::getOpenFileName(this, i18n("Add stylesheet"), katexdownpaths::dataDir(), QStringLiteral("*.css"));
    if (file.isEmpty()) {
        return;
    }
    m_cssList->addItem(file);
    Q_EMIT changed();
}

void ConfigPage::removeCssFile()
{
    const int row = m_cssList->currentRow();
    if (row < 0) {
        return;
    }
    delete m_cssList->takeItem(row);
    Q_EMIT changed();
}

void ConfigPage::moveCssFile(int delta)
{
    const int from = m_cssList->currentRow();
    const int to = from + delta;
    if (from < 0 || to < 0 || to >= m_cssList->count()) {
        return;
    }
    m_cssList->insertItem(to, m_cssList->takeItem(from));
    m_cssList->setCurrentRow(to);
    Q_EMIT changed();
}

void ConfigPage::checkForUpdates()
{
    m_checkButton->setEnabled(false);
    m_updateStatus->setText(i18n("Checking…"));

    QNetworkRequest req(QUrl(QStringLiteral("https://api.github.com/repos/uwuclxdy/katexdown/releases/latest")));
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("User-Agent", "katexdown");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply *reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        m_checkButton->setEnabled(true);

        if (reply->error() != QNetworkReply::NoError) {
            m_updateStatus->setText(i18n("Update check failed: %1", reply->errorString()));
            return;
        }

        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        const QString tag = obj.value(QStringLiteral("tag_name")).toString();
        if (tag.isEmpty()) {
            m_updateStatus->setText(i18n("Update check failed: no published release found."));
            return;
        }

        const QString latest = tag.startsWith(QLatin1Char('v')) ? tag.mid(1) : tag;
        if (compareVersions(latest, currentVersion()) > 0) {
            QString url = obj.value(QStringLiteral("html_url")).toString();
            if (url.isEmpty()) {
                url = repoUrl + QStringLiteral("/releases/latest");
            }
            m_updateStatus->setText(i18n("Update available: %1 (installed %2). <a href=\"%3\">Release notes</a>", tag, currentVersion(), url));
        } else {
            m_updateStatus->setText(i18n("Up to date (installed %1).", currentVersion()));
        }
    });
}

void ConfigPage::apply()
{
    Settings *s = Settings::self();
    s->setMode(static_cast<Settings::Mode>(m_mode->currentData().toInt()));
    s->setGhVariant(static_cast<Settings::GhVariant>(m_variant->currentData().toInt()));
    s->setLoadRemoteMedia(m_remoteMedia->isChecked());
    s->setUseGithubCss(m_githubCss->isChecked());
    s->setLoadingMode(static_cast<Settings::LoadingMode>(m_loading->currentData().toInt()));
    s->setImageMode(static_cast<Settings::ImageMode>(m_imageMode->currentData().toInt()));
    s->setV8HeapCapMb(m_v8Cap->value());
    QList<int> levels;
    for (int level = 1; level <= 6; ++level) {
        if (m_tocLevel[level - 1]->isChecked()) {
            levels.append(level);
        }
    }
    s->setTocLevels(levels);
    QStringList css;
    for (int i = 0; i < m_cssList->count(); ++i) {
        css << m_cssList->item(i)->text();
    }
    s->setCustomCssFiles(css);
}

void ConfigPage::reset()
{
    Settings *s = Settings::self();
    m_mode->setCurrentIndex(m_mode->findData(s->mode()));
    m_variant->setCurrentIndex(m_variant->findData(s->ghVariant()));
    m_remoteMedia->setChecked(s->loadRemoteMedia());
    m_githubCss->setChecked(s->useGithubCss());
    m_loading->setCurrentIndex(m_loading->findData(s->loadingMode()));
    m_imageMode->setCurrentIndex(m_imageMode->findData(s->imageMode()));
    m_v8Cap->setValue(s->v8HeapCapMb());
    m_cssList->clear();
    const QStringList css = s->customCssFiles();
    for (const QString &file : css) {
        m_cssList->addItem(file);
    }
    const QList<int> levels = s->tocLevels();
    for (int level = 1; level <= 6; ++level) {
        m_tocLevel[level - 1]->setChecked(levels.contains(level));
    }
    syncEnabled();
}

void ConfigPage::defaults()
{
    m_mode->setCurrentIndex(m_mode->findData(Settings::GitHub));
    m_variant->setCurrentIndex(m_variant->findData(Settings::Auto));
    m_remoteMedia->setChecked(false);
    m_githubCss->setChecked(true);
    m_loading->setCurrentIndex(m_loading->findData(Settings::LazyKeep));
    m_imageMode->setCurrentIndex(m_imageMode->findData(Settings::Adaptive));
    m_v8Cap->setValue(Settings::DefaultV8HeapCapMb);
    m_cssList->clear();
    for (int level = 1; level <= 6; ++level) {
        m_tocLevel[level - 1]->setChecked(level <= 5); // H1-H5 default, H6 off
    }
    syncEnabled();
}
