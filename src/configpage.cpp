#include "configpage.h"
#include "settings.h"

#include <QComboBox>
#include <QFormLayout>
#include <QIcon>
#include <QLabel>
#include <QVBoxLayout>

#include <KLocalizedString>

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

    auto *hint = new QLabel(
        i18n("\"GitHub\" uses GitHub's own colors. \"Match editor / system theme\" recolors the "
             "same layout from the active editor theme, so the preview blends with the rest of Kate."),
        this);
    hint->setWordWrap(true);
    hint->setEnabled(false);
    outer->addWidget(hint);
    outer->addStretch();

    reset();

    connect(m_mode, &QComboBox::currentIndexChanged, this, [this]() {
        syncEnabled();
        Q_EMIT changed();
    });
    connect(m_variant, &QComboBox::currentIndexChanged, this, [this]() {
        Q_EMIT changed();
    });
}

QString ConfigPage::name() const
{
    return i18n("Markdown Preview");
}

QString ConfigPage::fullName() const
{
    return i18n("Markdown Preview");
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

void ConfigPage::apply()
{
    Settings *s = Settings::self();
    s->setMode(static_cast<Settings::Mode>(m_mode->currentData().toInt()));
    s->setGhVariant(static_cast<Settings::GhVariant>(m_variant->currentData().toInt()));
}

void ConfigPage::reset()
{
    Settings *s = Settings::self();
    m_mode->setCurrentIndex(m_mode->findData(s->mode()));
    m_variant->setCurrentIndex(m_variant->findData(s->ghVariant()));
    syncEnabled();
}

void ConfigPage::defaults()
{
    m_mode->setCurrentIndex(m_mode->findData(Settings::GitHub));
    m_variant->setCurrentIndex(m_variant->findData(Settings::Auto));
    syncEnabled();
}
