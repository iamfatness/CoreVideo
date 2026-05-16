#pragma once
#include <QString>
#include <QVector>
#include <QJsonObject>

struct TemplateSlot {
    int     index   = 0;
    double  x       = 0.0;   // left edge, 0..1 of canvas
    double  y       = 0.0;   // top edge, 0..1 of canvas
    double  width   = 1.0;
    double  height  = 1.0;
    QString label;
    bool    isMain  = false;
};

struct LayoutTemplate {
    QString              id;
    QString              name;
    QString              description;
    int                  columns = 1;
    int                  rows    = 1;
    QVector<TemplateSlot> slotList;

    bool isValid() const { return !id.isEmpty() && !slotList.isEmpty(); }

    static LayoutTemplate fromJson(const QJsonObject &obj);
    QJsonObject toJson() const;
};
