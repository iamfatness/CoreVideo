#pragma once
#include <QWidget>
#include <QVector>

class QPushButton;

class Sidebar : public QWidget {
    Q_OBJECT
public:
    enum class Page { Scenes, Templates, Themes, Participants, Macros, Settings };

    explicit Sidebar(QWidget *parent = nullptr);
    void setActivePage(Page p);

signals:
    void pageSelected(Page p);

private:
    struct NavItem { QString icon; QString label; Page page; QPushButton *btn = nullptr; };
    QVector<NavItem> m_items;
    void buildNav();
    void selectBtn(QPushButton *btn);
};
