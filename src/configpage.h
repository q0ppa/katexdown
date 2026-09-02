#pragma once

#include <KTextEditor/ConfigPage>

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QNetworkAccessManager;

/**
 * Settings page shown under Kate's plugin configuration: choose between GitHub
 * colors and the active editor/system theme, plus the GitHub light/dark
 * variant, custom stylesheets, remote media, and which heading levels the
 * floating section outline in the preview recognizes.
 */
class ConfigPage : public KTextEditor::ConfigPage
{
    Q_OBJECT
public:
    explicit ConfigPage(QWidget *parent = nullptr);

    QString name() const override;
    QString fullName() const override;
    QIcon icon() const override;

    void apply() override;
    void reset() override;
    void defaults() override;

private:
    void syncEnabled();
    void checkForUpdates();
    void addCssFile();
    void removeCssFile();
    void moveCssFile(int delta);

    QComboBox *m_mode = nullptr;
    QComboBox *m_variant = nullptr;
    QComboBox *m_loading = nullptr;
    QComboBox *m_imageMode = nullptr;
    QCheckBox *m_githubCss = nullptr;
    QCheckBox *m_remoteMedia = nullptr;
    // Heading level 1..6 -> index 0..5.
    QCheckBox *m_tocLevel[6] = {};
    QListWidget *m_cssList = nullptr;
    QPushButton *m_cssRemove = nullptr;
    QPushButton *m_cssUp = nullptr;
    QPushButton *m_cssDown = nullptr;
    QPushButton *m_checkButton = nullptr;
    QLabel *m_updateStatus = nullptr;
    QNetworkAccessManager *m_net = nullptr;
};
