#pragma once
#include "layout-template.h"
#include <QString>
#include <QVector>

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
    LayoutTemplate        tmpl;
    QVector<SlotAssignment> slots;

    bool isValid() const { return tmpl.isValid(); }

    // Look up the participant id assigned to a given slot, or -1.
    int participantInSlot(int slotIndex) const
    {
        for (const auto &s : slots)
            if (s.slotIndex == slotIndex) return s.participantId;
        return -1;
    }
};
