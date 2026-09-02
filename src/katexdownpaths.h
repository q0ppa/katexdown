#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

/**
 * Where Katexdown keeps its runtime data (downloaded math assets, custom css).
 *
 * Resolution order:
 *   1. $KATEXDOWN_DATA_DIR — explicit override (development / testing only;
 *      normal use does not need it).
 *   2. The platform's conventional user-data location:
 *        - Linux   ~/.config/katexdown
 *        - Windows %APPDATA%\katexdown   (C:\Users\<you>\AppData\Roaming\katexdown)
 *        - macOS   ~/Library/Application Support/katexdown
 *
 * This is exactly what tools/fetch-assets.py writes into when run without
 * arguments, so a plain `python3 tools/fetch-assets.py` once is all that is
 * needed — no environment variables, no command line flags, on any OS.
 *
 * The directory is read on every access and never cached, so switching
 * locations (e.g. for tests) only requires setting the environment variable
 * before Kate starts.
 */
namespace katexdownpaths
{
inline QString dataDir()
{
    const QByteArray env = qgetenv("KATEXDOWN_DATA_DIR");
    if (!env.isEmpty()) {
        return QFile::decodeName(env);
    }
#ifdef Q_OS_MACOS
    // macOS convention keeps app data outside ~/Library/Preferences.
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/katexdown");
#else
    // Linux: ~/.config · Windows: %APPDATA%
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/katexdown");
#endif
}

// Read a whole text file if it exists, otherwise return an empty string.
inline QString readIfPresent(const QString &path)
{
    QFile f(path);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) {
        return QString();
    }
    return QString::fromUtf8(f.readAll());
}

// Resolve a css path from the settings against the data dir: relative paths
// are taken relative to dataDir(), absolute paths are used as-is.
inline QString resolveAssetPath(const QString &path)
{
    if (path.isEmpty()) {
        return QString();
    }
    const QFileInfo info(path);
    if (info.isAbsolute()) {
        return path;
    }
    return QDir(dataDir()).filePath(path);
}
} // namespace katexdownpaths
