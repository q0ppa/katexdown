#pragma once

#include <QObject>

/**
 * Plugin-wide settings, persisted in the application's KSharedConfig under the
 * group "Katexdown". A single instance broadcasts changed() so every open
 * preview re-themes at once.
 */
class Settings : public QObject
{
    Q_OBJECT
public:
    enum Mode {
        GitHub,      // GitHub's own light/dark palette
        Application, // match the active editor/system theme
    };
    enum GhVariant {
        Auto,  // follow the application palette (dark vs light)
        Light,
        Dark,
    };
    // When the heavy preview (web view + renderer process) exists:
    //   LazyKeep    create on first open, keep it after closing (default)
    //   LazyUnload  create on first open, destroy it on every close
    //   Eager       create at Kate startup (plugin enabled)
    enum LoadingMode {
        LazyKeep = 0,
        LazyUnload = 1,
        Eager = 2,
    };

    static Settings *self();

    Mode mode() const
    {
        return m_mode;
    }
    GhVariant ghVariant() const
    {
        return m_ghVariant;
    }
    bool loadRemoteMedia() const { return m_loadRemoteMedia; }
    bool useGithubCss() const { return m_useGithubCss; }
    LoadingMode loadingMode() const
    {
        return m_loadingMode;
    }
    QStringList customCssFiles() const
    {
        return m_customCssFiles;
    }

    void setMode(Mode mode);
    void setGhVariant(GhVariant variant);
    void setLoadRemoteMedia(bool enabled);
    void setUseGithubCss(bool enabled);
    void setLoadingMode(LoadingMode mode);
    void setCustomCssFiles(const QStringList &files);

    void load();
    void save() const;

Q_SIGNALS:
    void changed();

private:
    explicit Settings(QObject *parent = nullptr);

    Mode m_mode = GitHub;
    GhVariant m_ghVariant = Auto;
    bool m_loadRemoteMedia = false;
    // The bundled github-markdown.css is on by default; disabling it lets a
    // custom stylesheet fully own the layout instead of layering on GitHub's.
    bool m_useGithubCss = true;
    LoadingMode m_loadingMode = LazyKeep;
    // Custom stylesheets applied after the bundled github-markdown.css, in
    // listed order (later files win). Relative paths resolve against the
    // Katexdown data dir (see katexdownpaths.h).
    QStringList m_customCssFiles;
};
