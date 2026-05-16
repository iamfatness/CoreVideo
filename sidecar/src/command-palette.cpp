#include "command-palette.h"
#include <QVBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QKeyEvent>
#include <QGuiApplication>
#include <QScreen>

CommandPalette::CommandPalette(QWidget *parent)
    : QDialog(parent, Qt::Popup | Qt::FramelessWindowHint)
{
    setObjectName("commandPalette");
    setModal(true);
    setMinimumSize(560, 360);

    setStyleSheet(
        "#commandPalette { background: #14141c; border: 1px solid #2a2a3e; "
        "border-radius: 10px; }"
        "QLineEdit#cpSearch { background: #0e0e16; color: #e0e0f0; "
        "border: 1px solid #232336; border-radius: 6px; padding: 8px 10px; "
        "font-size: 14px; }"
        "QListWidget#cpList { background: transparent; color: #d0d0e0; "
        "border: none; outline: none; font-size: 13px; }"
        "QListWidget#cpList::item { padding: 8px 10px; border-radius: 4px; }"
        "QListWidget#cpList::item:selected { background: #2979ff; color: white; }");

    auto *vl = new QVBoxLayout(this);
    vl->setContentsMargins(10, 10, 10, 10);
    vl->setSpacing(8);

    m_search = new QLineEdit(this);
    m_search->setObjectName("cpSearch");
    m_search->setPlaceholderText("Type a command, scene, template, macro…");
    m_search->installEventFilter(this);
    vl->addWidget(m_search);

    m_list = new QListWidget(this);
    m_list->setObjectName("cpList");
    m_list->setUniformItemSizes(true);
    vl->addWidget(m_list, 1);

    connect(m_search, &QLineEdit::textChanged,
            this, &CommandPalette::onFilterChanged);
    connect(m_search, &QLineEdit::returnPressed, this, [this]() {
        if (auto *it = m_list->currentItem()) onItemActivated(it);
    });
    connect(m_list, &QListWidget::itemActivated,
            this, &CommandPalette::onItemActivated);
    connect(m_list, &QListWidget::itemClicked,
            this, &CommandPalette::onItemActivated);
}

void CommandPalette::clearCommands()
{
    m_entries.clear();
    m_list->clear();
}

void CommandPalette::addCommand(const QString &title,
                                const QString &category,
                                Action action)
{
    m_entries.append({title, category, std::move(action)});
}

void CommandPalette::run()
{
    m_search->clear();
    refilter();

    // Center over parent (or screen if orphaned)
    QWidget *anchor = parentWidget();
    QRect ref = anchor ? anchor->geometry()
                       : QGuiApplication::primaryScreen()->availableGeometry();
    if (anchor && anchor->window() != anchor)
        ref = anchor->window()->geometry();
    move(ref.center() - QPoint(width() / 2, height() / 2));

    m_search->setFocus();
    exec();
}

bool CommandPalette::eventFilter(QObject *o, QEvent *e)
{
    if (o == m_search && e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke->key() == Qt::Key_Down || ke->key() == Qt::Key_Up) {
            const int row = m_list->currentRow();
            const int count = m_list->count();
            if (count == 0) return true;
            int next = row + (ke->key() == Qt::Key_Down ? 1 : -1);
            if (next < 0) next = count - 1;
            if (next >= count) next = 0;
            m_list->setCurrentRow(next);
            return true;
        }
        if (ke->key() == Qt::Key_Escape) { reject(); return true; }
    }
    return QDialog::eventFilter(o, e);
}

void CommandPalette::onFilterChanged(const QString &)
{
    refilter();
}

void CommandPalette::refilter()
{
    const QString needle = m_search->text().trimmed().toLower();
    m_list->clear();
    for (int i = 0; i < m_entries.size(); ++i) {
        const auto &e = m_entries[i];
        if (!needle.isEmpty()
            && !e.title.toLower().contains(needle)
            && !e.category.toLower().contains(needle)) {
            continue;
        }
        auto *it = new QListWidgetItem(
            e.category.isEmpty() ? e.title
                                 : QString("%1   ·   %2").arg(e.title, e.category),
            m_list);
        it->setData(Qt::UserRole, i);
    }
    selectFirst();
}

void CommandPalette::selectFirst()
{
    if (m_list->count() > 0) m_list->setCurrentRow(0);
}

void CommandPalette::onItemActivated(QListWidgetItem *item)
{
    if (!item) return;
    const int idx = item->data(Qt::UserRole).toInt();
    if (idx < 0 || idx >= m_entries.size()) return;
    Action act = m_entries[idx].action;
    accept();
    if (act) act();
}
