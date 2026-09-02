#pragma once

#include <QList>
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
    // When the heavy preview (web view + renderer process) exists, and what
    // happens to it while the panel is closed:
    //   LazyKeep    create on first open. On close the page is frozen (no CPU,
    //               instant toggle back); once the panel has stayed closed for
    //               a while the page is discarded, so a preview left closed
    //               does not keep a renderer process resident — re-opening
    //               reloads it automatically (default)
    //   LazyUnload  create on first open, destroy the whole preview on every
    //               close (minimal memory; re-opening re-creates it)
    //   Eager       create at Kate startup; frozen while closed, never released
    // The freeze/release policy itself lives in PreviewWidget
    // (panelClosed/panelOpened) and applies only to genuinely closed panels
    // (hiding the whole Kate window does not count).
    enum LoadingMode {
        LazyKeep = 0,
        LazyUnload = 1,
        Eager = 2,
    };
    // How the page treats images once a document is rendered (see the image
    // manager in data/js/preview.js, driven through __setImageMode):
    //   DecodeAll    every image decodes as soon as it is in the document
    //                (classic behavior, heaviest with many images)
    //   Auto         images load lazily (decode as they approach the
    //                viewport); once a document gets image-heavy the
    //                off-screen images are additionally unloaded again.
    //                The default — it leans towards MemorySaver.
    //   MemorySaver  only images near the viewport ever decode; decoded
    //                memory is released as soon as an image scrolls out of
    //                the keep zone (renderer memory no longer scales with
    //                the image count of the document).
    enum ImageMode {
        DecodeAll = 0,
        Adaptive = 1,
        MemorySaver = 2,
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
    ImageMode imageMode() const
    {
        return m_imageMode;
    }
    QStringList customCssFiles() const
    {
        return m_customCssFiles;
    }
    // Heading levels the preview's floating section outline lists (1..6,
    // ascending). Which levels count is configurable; the default is H1-H5.
    QList<int> tocLevels() const
    {
        return m_tocLevels;
    }

    void setMode(Mode mode);
    void setGhVariant(GhVariant variant);
    void setLoadRemoteMedia(bool enabled);
    void setUseGithubCss(bool enabled);
    void setLoadingMode(LoadingMode mode);
    void setImageMode(ImageMode mode);
    void setCustomCssFiles(const QStringList &files);
    void setTocLevels(const QList<int> &levels);

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
    // Image decode policy; Auto by default (see enum above).
    ImageMode m_imageMode = Adaptive;
    // Custom stylesheets applied after the bundled github-markdown.css, in
    // listed order (later files win). Relative paths resolve against the
    // Katexdown data dir (see katexdownpaths.h).
    QStringList m_customCssFiles;
    // Heading levels (1..6) recognized by the floating section outline.
    QList<int> m_tocLevels = {1, 2, 3, 4, 5};
};
