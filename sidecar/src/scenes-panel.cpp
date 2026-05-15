#include "scenes-panel.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QVBoxLayout>

ScenesPanel::ScenesPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── Header row ────────────────────────────────────────────────────────────
    auto *headerRow = new QHBoxLayout;
    headerRow->setContentsMargins(12, 10, 12, 6);
    headerRow->setSpacing(0);

    auto *headerLabel = new QLabel("Scenes", this);
    headerLabel->setObjectName("sectionHeader");
    headerLabel->setStyleSheet(
        "color: #c0c0e0;"
        "font-size: 11px;"
        "font-weight: 700;"
        "letter-spacing: 0.08em;"
        "text-transform: uppercase;"
        "padding: 0;");

    m_refreshBtn = new QPushButton("↻", this);
    m_refreshBtn->setObjectName("iconBtn");
    m_refreshBtn->setFixedSize(28, 28);
    m_refreshBtn->setStyleSheet("padding: 0;");

    headerRow->addWidget(headerLabel);
    headerRow->addStretch();
    headerRow->addWidget(m_refreshBtn);

    root->addLayout(headerRow);

    // ── Current scene row ─────────────────────────────────────────────────────
    auto *currentRow = new QHBoxLayout;
    currentRow->setContentsMargins(12, 4, 12, 4);
    currentRow->setSpacing(4);

    auto *programLabel = new QLabel("Program:", this);
    programLabel->setStyleSheet(
        "color: #5a5a7a;"
        "font-size: 11px;"
        "background: transparent;");

    m_currentLabel = new QLabel("—", this);
    m_currentLabel->setStyleSheet(
        "color: #20c460;"
        "font-size: 11px;"
        "font-weight: 600;"
        "background: transparent;");

    currentRow->addWidget(programLabel);
    currentRow->addWidget(m_currentLabel);
    currentRow->addStretch();

    root->addLayout(currentRow);

    // ── Scene list ────────────────────────────────────────────────────────────
    m_list = new QListWidget(this);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setStyleSheet(
        "QListWidget {"
        "  background: transparent;"
        "  border: none;"
        "  outline: none;"
        "}"
        "QListWidget::item {"
        "  color: #c0c0d8;"
        "  padding: 8px 12px;"
        "  border-radius: 6px;"
        "}"
        "QListWidget::item:hover {"
        "  background: #18182a;"
        "}"
        "QListWidget::item:selected {"
        "  background: #1d3de8;"
        "  color: #ffffff;"
        "}");

    connect(m_list, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem *item) {
                if (item)
                    emit sceneActivated(item->text());
            });

    root->addWidget(m_list, 1);

    // ── Activate button row ───────────────────────────────────────────────────
    auto *activateRow = new QHBoxLayout;
    activateRow->setContentsMargins(10, 10, 10, 10);
    activateRow->setSpacing(0);

    m_activateBtn = new QPushButton("▶  Activate Scene", this);
    m_activateBtn->setObjectName("toolBtn");
    m_activateBtn->setProperty("primary", true);
    m_activateBtn->setFixedHeight(34);

    connect(m_activateBtn, &QPushButton::clicked,
            this, [this]() {
                const QString scene = selectedScene();
                if (!scene.isEmpty())
                    emit sceneActivated(scene);
            });

    activateRow->addWidget(m_activateBtn);

    root->addLayout(activateRow);

    // ── Refresh connection ────────────────────────────────────────────────────
    connect(m_refreshBtn, &QPushButton::clicked,
            this, &ScenesPanel::refreshRequested);
}

void ScenesPanel::setScenes(const QStringList &scenes)
{
    const bool wasEmpty = m_list->count() == 0;
    m_list->clear();
    for (const QString &name : scenes)
        m_list->addItem(name);
    if (wasEmpty && m_list->count() > 0)
        m_list->setCurrentRow(0);
}

void ScenesPanel::setCurrentScene(const QString &name)
{
    m_currentLabel->setText(name);
}

QString ScenesPanel::selectedScene() const
{
    QListWidgetItem *item = m_list->currentItem();
    return item ? item->text() : QString();
}
