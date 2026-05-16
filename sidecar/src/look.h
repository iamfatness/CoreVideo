#pragma once
#include "layout-template.h"
#include "overlay.h"
#include <QString>
#include <QVector>
#include <QJsonObject>
#include <QJsonArray>
#include <QColor>

// A SlotAssignment binds a participant to a slot in the active template.
// Participant id < 0 means "empty / unfilled."
struct SlotAssignment {
    int slotIndex     = -1;
    int participantId = -1;
};

struct TileStyle {
    QColor canvasColor = QColor("#6f2cff");
    QColor borderColor = QColor("#2979ff");
    double borderWidth = 1.0;
    double cornerRadius = 10.0;
    bool dropShadow = false;
    double opacity = 1.0;
    bool showNameTag = true;
    bool excludeNoVideo = true;

    static TileStyle fromJson(const QJsonObject &obj)
    {
        TileStyle s;
        if (obj.contains("canvasColor"))
            s.canvasColor = QColor::fromString(obj.value("canvasColor").toString());
        if (obj.contains("borderColor"))
            s.borderColor = QColor::fromString(obj.value("borderColor").toString());
        s.borderWidth = obj.value("borderWidth").toDouble(s.borderWidth);
        s.cornerRadius = obj.value("cornerRadius").toDouble(s.cornerRadius);
        s.dropShadow = obj.value("dropShadow").toBool(s.dropShadow);
        s.opacity = obj.value("opacity").toDouble(s.opacity);
        s.showNameTag = obj.value("showNameTag").toBool(s.showNameTag);
        s.excludeNoVideo = obj.value("excludeNoVideo").toBool(s.excludeNoVideo);
        return s;
    }

    QJsonObject toJson() const
    {
        return {
            {"canvasColor", canvasColor.name()},
            {"borderColor", borderColor.name()},
            {"borderWidth", borderWidth},
            {"cornerRadius", cornerRadius},
            {"dropShadow", dropShadow},
            {"opacity", opacity},
            {"showNameTag", showNameTag},
            {"excludeNoVideo", excludeNoVideo},
        };
    }
};

// A Look is the unit of "what's on air" or "what's staged next" — a fully
// described on-air composition. For slice 1 it carries layout + slot fills;
// theme + overlays will be added in later slices.
struct Look {
    QString               id;
    QString               name;
    QString               category;     // e.g. "News", "Talk Show", "Podcast"
    QString               description;
    QString               templateId;   // resolves via TemplateManager
    QString               themeId;      // resolves via ShowTheme::builtIns (optional)
    QString               backgroundImagePath;
    TileStyle             tileStyle;
    LayoutTemplate        tmpl;
    QVector<SlotAssignment> slotAssignments;
    QVector<Overlay>      overlays;

    bool isValid() const { return tmpl.isValid(); }

    // Look up the participant id assigned to a given slot, or -1.
    int participantInSlot(int slotIndex) const
    {
        for (const auto &s : slotAssignments)
            if (s.slotIndex == slotIndex) return s.participantId;
        return -1;
    }

    // Parse the disk format. Caller is responsible for resolving templateId
    // → LayoutTemplate (LookLibrary does this during load).
    static Look fromJson(const QJsonObject &obj)
    {
        Look l;
        l.id          = obj.value("id").toString();
        l.name        = obj.value("name").toString();
        l.category    = obj.value("category").toString();
        l.description = obj.value("description").toString();
        l.templateId  = obj.value("template").toString();
        l.themeId     = obj.value("theme").toString();
        l.backgroundImagePath = obj.value("backgroundImage").toString();
        l.tileStyle = TileStyle::fromJson(obj.value("tileStyle").toObject());
        const auto arr = obj.value("slots").toArray();
        for (const auto &v : arr) {
            const auto o = v.toObject();
            l.slotAssignments.append({ o.value("slotIndex").toInt(-1),
                                       o.value("participantId").toInt(-1) });
        }
        const auto ov = obj.value("overlays").toArray();
        for (const auto &v : ov)
            l.overlays.append(Overlay::fromJson(v.toObject()));
        return l;
    }

    QJsonObject toJson() const
    {
        QJsonArray slotArray;
        for (const auto &s : slotAssignments) {
            slotArray.append(QJsonObject{
                {"slotIndex", s.slotIndex},
                {"participantId", s.participantId},
            });
        }

        QJsonArray ov;
        for (const auto &o : overlays)
            ov.append(o.toJson());

        QJsonObject obj{
            {"id",          id},
            {"name",        name},
            {"category",    category},
            {"description", description},
            {"template",    templateId},
            {"theme",       themeId},
            {"tileStyle",   tileStyle.toJson()},
            {"slots",       slotArray},
            {"overlays",    ov},
        };
        if (!backgroundImagePath.isEmpty())
            obj["backgroundImage"] = backgroundImagePath;
        return obj;
    }
};

