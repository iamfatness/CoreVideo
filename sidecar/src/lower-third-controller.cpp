#include "lower-third-controller.h"
#include <algorithm>

static QString participantInitialSubtitle(const ParticipantInfo &p)
{
    if (p.isSharingScreen)
        return QStringLiteral("Screen share");
    if (p.isTalking)
        return QStringLiteral("Speaking");
    return {};
}

void LowerThirdController::setOverride(int participantId,
                                       const LowerThirdOverride &value)
{
    if (participantId <= 0) return;
    m_overrides.insert(participantId, value);
}

void LowerThirdController::clearOverride(int participantId)
{
    m_overrides.remove(participantId);
}

bool LowerThirdController::isAutoLowerThird(const Overlay &overlay)
{
    return overlay.id.startsWith(QStringLiteral("cv-auto-lt-"));
}

QVector<Overlay> LowerThirdController::participantSyncedOverlays(
    const Look &look,
    const QVector<ParticipantInfo> &participants) const
{
    QVector<Overlay> overlays;
    if (!look.tmpl.isValid() || !look.tileStyle.showNameTag)
        return overlays;

    for (const auto &slot : look.tmpl.slotList) {
        const int participantId = look.participantInSlot(slot.index);
        if (participantId <= 0) continue;

        auto it = std::find_if(participants.begin(), participants.end(),
            [participantId](const ParticipantInfo &p) {
                return p.id == participantId;
            });
        if (it == participants.end()) continue;

        const LowerThirdOverride override =
            m_overrides.value(participantId, {});
        if (override.enabled && override.name.trimmed().isEmpty())
            continue;

        Overlay ov;
        ov.id = QStringLiteral("cv-auto-lt-%1-%2")
            .arg(slot.index).arg(participantId);
        ov.type = Overlay::LowerThird;
        ov.text1 = override.enabled ? override.name : it->name;
        ov.text2 = override.enabled
            ? override.subtitle
            : (m_template.subtitle.isEmpty()
                ? participantInitialSubtitle(*it)
                : m_template.subtitle);
        ov.x = std::clamp(slot.x + slot.width * 0.04, 0.0, 1.0);
        ov.w = std::clamp(slot.width * 0.70, 0.18, 0.92);
        ov.h = std::clamp(m_template.height, 0.06, 0.16);
        ov.y = std::clamp(slot.y + slot.height - ov.h - 0.035, 0.0, 0.94);
        ov.accent = it->color;
        overlays.append(ov);
    }
    return overlays;
}
