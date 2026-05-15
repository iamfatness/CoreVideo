#pragma once
#include <QString>
#include <QColor>
#include <QVector>
#include <QJsonObject>

struct ShowTheme {
    QString id;
    QString name;
    QColor  accent;          // primary accent (buttons, selected borders)
    QColor  accentAlt;       // secondary (e.g. talking ring)
    QColor  background;      // canvas/slot bg
    QColor  panelBg;         // sidebar / right-panel bg
    QColor  textPrimary;
    QColor  textSecondary;

    bool isValid() const { return !id.isEmpty(); }

    static ShowTheme fromJson(const QJsonObject &obj)
    {
        ShowTheme t;
        t.id            = obj.value("id").toString();
        t.name          = obj.value("name").toString();
        t.accent        = QColor::fromString(obj.value("accent").toString());
        t.accentAlt     = QColor::fromString(obj.value("accentAlt").toString());
        t.background    = QColor::fromString(obj.value("background").toString());
        t.panelBg       = QColor::fromString(obj.value("panelBg").toString());
        t.textPrimary   = QColor::fromString(obj.value("textPrimary").toString());
        t.textSecondary = QColor::fromString(obj.value("textSecondary").toString());
        return t;
    }

    QJsonObject toJson() const
    {
        QJsonObject obj;
        obj["id"]            = id;
        obj["name"]          = name;
        obj["accent"]        = accent.name();
        obj["accentAlt"]     = accentAlt.name();
        obj["background"]    = background.name();
        obj["panelBg"]       = panelBg.name();
        obj["textPrimary"]   = textPrimary.name();
        obj["textSecondary"] = textSecondary.name();
        return obj;
    }

    // Built-in presets
    static QVector<ShowTheme> builtIns()
    {
        QVector<ShowTheme> list;

        ShowTheme midnight;
        midnight.id            = "midnight";
        midnight.name          = "Midnight Blue";
        midnight.accent        = QColor("#2979ff");
        midnight.accentAlt     = QColor("#20c4ff");
        midnight.background    = QColor("#0d0d12");
        midnight.panelBg       = QColor("#101016");
        midnight.textPrimary   = QColor("#e0e0f0");
        midnight.textSecondary = QColor("#6060a0");
        list.append(midnight);

        ShowTheme forest;
        forest.id            = "forest";
        forest.name          = "Forest";
        forest.accent        = QColor("#20c460");
        forest.accentAlt     = QColor("#00e090");
        forest.background    = QColor("#080f0a");
        forest.panelBg       = QColor("#0c1410");
        forest.textPrimary   = QColor("#d0f0d8");
        forest.textSecondary = QColor("#406050");
        list.append(forest);

        ShowTheme crimson;
        crimson.id            = "crimson";
        crimson.name          = "Crimson";
        crimson.accent        = QColor("#e03040");
        crimson.accentAlt     = QColor("#ff6080");
        crimson.background    = QColor("#12080a");
        crimson.panelBg       = QColor("#18080c");
        crimson.textPrimary   = QColor("#f0d0d8");
        crimson.textSecondary = QColor("#804050");
        list.append(crimson);

        ShowTheme gold;
        gold.id            = "gold";
        gold.name          = "Gold";
        gold.accent        = QColor("#e0a020");
        gold.accentAlt     = QColor("#ffcc40");
        gold.background    = QColor("#110e04");
        gold.panelBg       = QColor("#181404");
        gold.textPrimary   = QColor("#f0e8c0");
        gold.textSecondary = QColor("#806030");
        list.append(gold);

        ShowTheme violet;
        violet.id            = "violet";
        violet.name          = "Violet";
        violet.accent        = QColor("#9040e0");
        violet.accentAlt     = QColor("#c060ff");
        violet.background    = QColor("#0e080f");
        violet.panelBg       = QColor("#14081a");
        violet.textPrimary   = QColor("#e8d0f8");
        violet.textSecondary = QColor("#704090");
        list.append(violet);

        return list;
    }
};
