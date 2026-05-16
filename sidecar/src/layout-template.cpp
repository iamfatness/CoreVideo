#include "layout-template.h"
#include <QJsonArray>

LayoutTemplate LayoutTemplate::fromJson(const QJsonObject &obj)
{
    LayoutTemplate t;
    t.id          = obj["id"].toString();
    t.name        = obj["name"].toString();
    t.description = obj["description"].toString();
    t.columns     = obj["columns"].toInt(1);
    t.rows        = obj["rows"].toInt(1);

    for (const QJsonValue &v : obj["slots"].toArray()) {
        const QJsonObject s = v.toObject();
        TemplateSlot slot;
        slot.index   = s["index"].toInt();
        slot.x       = s["x"].toDouble();
        slot.y       = s["y"].toDouble();
        slot.width   = s["width"].toDouble(1.0);
        slot.height  = s["height"].toDouble(1.0);
        slot.label   = s["label"].toString();
        slot.isMain  = s["isMain"].toBool(false);
        t.slotList.append(slot);
    }
    return t;
}

QJsonObject LayoutTemplate::toJson() const
{
    QJsonArray slotArr;
    for (const auto &s : slotList) {
        slotArr.append(QJsonObject{
            {"index",  s.index},
            {"x",      s.x},
            {"y",      s.y},
            {"width",  s.width},
            {"height", s.height},
            {"label",  s.label},
            {"isMain", s.isMain},
        });
    }
    return {
        {"id",          id},
        {"name",        name},
        {"description", description},
        {"columns",     columns},
        {"rows",        rows},
        {"slots",       slotArr},
    };
}
