#pragma once
#include "layout-template.h"
#include "overlay.h"
#include <QString>
#include <QVector>
#include <QJsonObject>
#include <QJsonArray>

// A SlotAssignment binds a participant to a slot in the active template.
// Participant id < 0 means "empty / unfilled."
struct SlotAssignment {
    int slotIndex     = -1;
    int participantId = -1;
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
    LayoutTemplate        tmpl;
    QVector<SlotAssignment> slots;
    QVector<Overlay>      overlays;

    bool isValid() const { return tmpl.isValid(); }

    // Look up the participant id assigned to a given slot, or -1.
    int participantInSlot(int slotIndex) const
    {
        for (const auto &s : slots)
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
        const auto arr = obj.value("slots").toArray();
        for (const auto &v : arr) {
            const auto o = v.toObject();
            l.slots.append({ o.value("slotIndex").toInt(-1),
                             o.value("participantId").toInt(-1) });
        }
        const auto ov = obj.value("overlays").toArray();
        for (const auto &v : ov)
            l.overlays.append(Overlay::fromJson(v.toObject()));
        return l;
    }
};

