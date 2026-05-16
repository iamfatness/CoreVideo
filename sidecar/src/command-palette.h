#pragma once
#include <QDialog>
#include <QString>
#include <QVector>
#include <functional>

class QLineEdit;
class QListWidget;
class QListWidgetItem;

// Spotlight-style command palette. Owner clears + repopulates commands each
// time before showing it via populate()/addCommand()/run().
class CommandPalette : public QDialog {
    Q_OBJECT
public:
    using Action = std::function<void()>;

    explicit CommandPalette(QWidget *parent = nullptr);

    void clearCommands();
    void addCommand(const QString &title,
                    const QString &category,
                    Action          action);

    // Show centered over the parent and focus the search field.
    void run();

protected:
    bool eventFilter(QObject *o, QEvent *e) override;

private slots:
    void onFilterChanged(const QString &text);
    void onItemActivated(QListWidgetItem *item);

private:
    struct Entry { QString title; QString category; Action action; };
    QVector<Entry> m_entries;

    QLineEdit   *m_search = nullptr;
    QListWidget *m_list   = nullptr;

    void refilter();
    void selectFirst();
};
