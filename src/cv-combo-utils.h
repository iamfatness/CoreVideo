#pragma once
//
// cv-combo-utils.h — the small QComboBox and roster-label helpers shared by
// the Zoom Control dock and the Talkback dock.
//
// These were `static` functions in zoom-dock.cpp until the Talkback surface
// moved out into its own dock (src/zoom-talkback-panel.cpp) and needed the
// same three behaviours. They are here rather than copied because the
// non-obvious one is load-bearing on both sides: replace_combo_items() BLOCKS
// the combo's signals while it rebuilds and while it restores the selection,
// which is what lets both docks treat a currentIndexChanged as "the operator
// changed this" and persist it. A second, hand-copied version that forgot to
// block would silently rewrite the saved setting on every refresh tick.
//
// Qt only -- no libobs, no engine -- so either dock can include it freely.
#include <QAbstractItemView>
#include <QComboBox>
#include <QString>
#include <QVariant>

#include <cstdint>
#include <utility>
#include <vector>

#include "zoom-types.h"

// The user is mid-selection: rebuilding the model right now would close the
// popup under their cursor and lose the pick.
inline bool combo_popup_open(const QComboBox *combo)
{
    return combo && combo->view() && combo->view()->isVisible();
}

inline bool combo_items_match(
    const QComboBox *combo,
    const std::vector<std::pair<QString, QVariant>> &items)
{
    if (!combo || combo->count() != static_cast<int>(items.size()))
        return false;
    for (int i = 0; i < combo->count(); ++i) {
        if (combo->itemText(i) != items[static_cast<size_t>(i)].first ||
            combo->itemData(i) != items[static_cast<size_t>(i)].second)
            return false;
    }
    return true;
}

// Rebuild `combo` from `items`, preferring `preferred` (or the current data,
// when `preferred` is invalid) as the selection afterwards. A no-op on the
// model when the items already match, so a refresh tick that changes nothing
// costs nothing.
inline void replace_combo_items(
    QComboBox *combo,
    const std::vector<std::pair<QString, QVariant>> &items,
    const QVariant &preferred)
{
    if (!combo)
        return;

    const QVariant current = combo->currentData();
    const QVariant target = preferred.isValid() ? preferred : current;
    const bool same_items = combo_items_match(combo, items);

    if (!same_items) {
        combo->blockSignals(true);
        combo->clear();
        for (const auto &item : items)
            combo->addItem(item.first, item.second);
        combo->blockSignals(false);
    }

    const int idx = combo->findData(target);
    if (idx >= 0 && idx != combo->currentIndex()) {
        combo->blockSignals(true);
        combo->setCurrentIndex(idx);
        combo->blockSignals(false);
    } else if (idx < 0 && combo->currentIndex() < 0 && combo->count() > 0) {
        combo->blockSignals(true);
        combo->setCurrentIndex(0);
        combo->blockSignals(false);
    }
}

// Does this item list already offer `data` as a selectable value? Used to keep
// a saved talkback source visible when OBS does not currently have it.
inline bool combo_items_contain(
    const std::vector<std::pair<QString, QVariant>> &items, const QVariant &data)
{
    for (const auto &item : items)
        if (item.second == data)
            return true;
    return false;
}

inline QString participant_label(const ParticipantInfo &p)
{
    QString label = p.display_name.empty()
        ? QString("ID %1").arg(p.user_id)
        : QString::fromStdString(p.display_name);
    if (p.has_video) label += " [video]";
    return label;
}
