#include "settings.h"

#include <algorithm>

#include <KConfigGroup>
#include <KSharedConfig>

static QString groupName()
{
    return QStringLiteral("Katexdown");
}

// Heading levels recognized by the floating section outline (H1-H5 by default).
static QStringList defaultTocLevels()
{
    return QStringList{QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3"), QStringLiteral("4"), QStringLiteral("5")};
}

// Sanitize a stored level list: drop anything outside 1..6 and duplicates,
// keep the order. An empty (or all-invalid) stored list stays empty, which is
// the "outline off" state and must survive a save/load round-trip.
static QList<int> sanitizedLevels(const QStringList &raw)
{
    QList<int> levels;
    for (const QString &v : raw) {
        bool ok = false;
        const int level = v.toInt(&ok);
        if (ok && level >= 1 && level <= 6 && !levels.contains(level)) {
            levels.append(level);
        }
    }
    std::sort(levels.begin(), levels.end());
    return levels;
}

Settings::Settings(QObject *parent)
    : QObject(parent)
{
    load();
}

Settings *Settings::self()
{
    static Settings instance;
    return &instance;
}

void Settings::load()
{
    KConfigGroup cfg(KSharedConfig::openConfig(), groupName());
    const QString mode = cfg.readEntry("Theme", QStringLiteral("github"));
    m_mode = (mode == QLatin1String("application")) ? Application : GitHub;

    const QString variant = cfg.readEntry("GithubVariant", QStringLiteral("auto"));
    if (variant == QLatin1String("light")) {
        m_ghVariant = Light;
    } else if (variant == QLatin1String("dark")) {
        m_ghVariant = Dark;
    } else {
        m_ghVariant = Auto;
    }
    m_loadRemoteMedia = cfg.readEntry("LoadRemoteMedia", false);
    m_useGithubCss = cfg.readEntry("GithubCss", true);
    m_loadingMode = static_cast<Settings::LoadingMode>(cfg.readEntry("LoadingMode", int(LazyKeep)));
    const QString imgMode = cfg.readEntry("ImageMode", QStringLiteral("auto"));
    if (imgMode == QLatin1String("saver")) {
        m_imageMode = MemorySaver;
    } else if (imgMode == QLatin1String("eager")) {
        m_imageMode = DecodeAll;
    } else {
        m_imageMode = Adaptive;
    }
    m_customCssFiles = cfg.readEntry("CustomCssFiles", QStringList());
    // Missing key -> defaultTocLevels(); an explicitly empty list means "no
    // levels, outline off".
    m_tocLevels = sanitizedLevels(cfg.readEntry("TocLevels", defaultTocLevels()));
}

void Settings::save() const
{
    KConfigGroup cfg(KSharedConfig::openConfig(), groupName());
    cfg.writeEntry("Theme", m_mode == Application ? QStringLiteral("application") : QStringLiteral("github"));
    const char *variant = m_ghVariant == Light ? "light" : m_ghVariant == Dark ? "dark" : "auto";
    cfg.writeEntry("GithubVariant", QString::fromLatin1(variant));
    cfg.writeEntry("LoadRemoteMedia", m_loadRemoteMedia);
    cfg.writeEntry("GithubCss", m_useGithubCss);
    cfg.writeEntry("LoadingMode", int(m_loadingMode));
    const char *imgMode = m_imageMode == MemorySaver ? "saver" : m_imageMode == DecodeAll ? "eager" : "auto";
    cfg.writeEntry("ImageMode", QString::fromLatin1(imgMode));
    cfg.writeEntry("CustomCssFiles", m_customCssFiles);
    QStringList levels;
    for (int level : m_tocLevels) {
        levels << QString::number(level);
    }
    cfg.writeEntry("TocLevels", levels);
    cfg.sync();
}

void Settings::setMode(Mode mode)
{
    if (m_mode == mode) {
        return;
    }
    m_mode = mode;
    save();
    Q_EMIT changed();
}

void Settings::setGhVariant(GhVariant variant)
{
    if (m_ghVariant == variant) {
        return;
    }
    m_ghVariant = variant;
    save();
    Q_EMIT changed();
}

void Settings::setLoadRemoteMedia(bool enabled)
{
    if (m_loadRemoteMedia == enabled) {
        return;
    }
    m_loadRemoteMedia = enabled;
    save();
    Q_EMIT changed();
}

void Settings::setUseGithubCss(bool enabled)
{
    if (m_useGithubCss == enabled) {
        return;
    }
    m_useGithubCss = enabled;
    save();
    Q_EMIT changed();
}

void Settings::setLoadingMode(LoadingMode mode)
{
    if (m_loadingMode == mode) {
        return;
    }
    m_loadingMode = mode;
    save();
    Q_EMIT changed();
}

void Settings::setImageMode(ImageMode mode)
{
    if (m_imageMode == mode) {
        return;
    }
    m_imageMode = mode;
    save();
    Q_EMIT changed();
}

void Settings::setCustomCssFiles(const QStringList &files)
{
    if (m_customCssFiles == files) {
        return;
    }
    m_customCssFiles = files;
    save();
    Q_EMIT changed();
}

void Settings::setTocLevels(const QList<int> &levels)
{
    QList<int> clean;
    for (int level : levels) {
        if (level >= 1 && level <= 6 && !clean.contains(level)) {
            clean.append(level);
        }
    }
    std::sort(clean.begin(), clean.end());
    if (m_tocLevels == clean) {
        return;
    }
    m_tocLevels = clean;
    save();
    Q_EMIT changed();
}
