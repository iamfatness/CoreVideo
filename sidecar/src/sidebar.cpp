#include "sidebar.h"
#include <QStyle>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpacerItem>

Sidebar::Sidebar(QWidget *parent) : QWidget(parent)
{
    setObjectName("sidebar");
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);

    auto *vLayout = new QVBoxLayout(this);
    vLayout->setContentsMargins(0, 0, 0, 0);
    vLayout->setSpacing(0);

    // ── Logo area ─────────────────────────────────────────────────────────────
    auto *logoArea = new QWidget(this);
    logoArea->setObjectName("logoArea");
    logoArea->setFixedHeight(60);
    auto *logoRow = new QHBoxLayout(logoArea);
    logoRow->setContentsMargins(14, 0, 14, 0);

    auto *icon = new QLabel("⬛", logoArea);  // placeholder brand icon
    icon->setFixedSize(36, 36);
    icon->setAlignment(Qt::AlignCenter);
    icon->setStyleSheet(
        "background: qlineargradient(x1:0,y1:0,x2:1,y2:1,"
        "stop:0 #2050ff, stop:1 #8020ff);"
        "border-radius: 8px; font-size: 18px;");

    auto *textCol = new QVBoxLayout;
    textCol->setSpacing(0);
    auto *appName = new QLabel("CoreVideo", logoArea);
    appName->setObjectName("appName");
    auto *appSub  = new QLabel("Sidecar", logoArea);
    appSub->setObjectName("appSub");
    textCol->addWidget(appName);
    textCol->addWidget(appSub);

    logoRow->addWidget(icon);
    logoRow->addSpacing(8);
    logoRow->addLayout(textCol, 1);
    vLayout->addWidget(logoArea);

    // ── Search box ────────────────────────────────────────────────────────────
    auto *searchBox = new QLineEdit(this);
    searchBox->setObjectName("searchBox");
    searchBox->setPlaceholderText("Search…");
    {
        auto *wrap = new QWidget(this);
        auto *wl   = new QHBoxLayout(wrap);
        wl->setContentsMargins(10, 8, 10, 4);
        wl->addWidget(searchBox);
        vLayout->addWidget(wrap);
    }

    // ── Nav items ─────────────────────────────────────────────────────────────
    m_items = {
        {"  ⊞", "Scenes",            Page::Scenes},
        {"  ⊟", "Looks",             Page::Templates},
        {"  ◉", "Show Themes",       Page::Themes},
        {"  ◎", "Participants",      Page::Participants},
        {"  ⚡", "Macros",           Page::Macros},
        {"  ⚙", "Settings",         Page::Settings},
    };

    auto *navArea = new QWidget(this);
    auto *navLayout = new QVBoxLayout(navArea);
    navLayout->setContentsMargins(8, 4, 8, 4);
    navLayout->setSpacing(2);

    for (auto &item : m_items) {
        auto *btn = new QPushButton(item.icon + "  " + item.label, navArea);
        btn->setObjectName("navBtn");
        btn->setCheckable(false);
        item.btn = btn;
        navLayout->addWidget(btn);
        connect(btn, &QPushButton::clicked, this, [this, &item]() {
            selectBtn(item.btn);
            emit pageSelected(item.page);
        });
    }
    navLayout->addStretch(1);
    vLayout->addWidget(navArea, 1);

    buildNav();
}

void Sidebar::buildNav()
{
    if (!m_items.isEmpty())
        setActivePage(Page::Templates);
}

void Sidebar::setActivePage(Page p)
{
    for (auto &item : m_items) {
        item.btn->setProperty("active", item.page == p ? "true" : "false");
        item.btn->style()->unpolish(item.btn);
        item.btn->style()->polish(item.btn);
    }
}

void Sidebar::selectBtn(QPushButton *active)
{
    for (auto &item : m_items) {
        const bool sel = (item.btn == active);
        item.btn->setProperty("active", sel ? "true" : "false");
        item.btn->style()->unpolish(item.btn);
        item.btn->style()->polish(item.btn);
    }
}
