#include "talkback-cell-grid.h"
#include "talkback-dock-state.h"

#include <QEvent>
#include <QFontMetrics>
#include <QGridLayout>
#include <QLabel>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QVBoxLayout>

#include <algorithm>

TalkbackCellButton::TalkbackCellButton(QWidget *parent)
    : QPushButton(parent)
{
    // role="cell" is what gives a cell its ground, its border and its state
    // colours (src/cv-style.h).
    setProperty("role", "cell");

    auto *cell_layout = new QVBoxLayout(this);
    cell_layout->setContentsMargins(kTalkbackCellNamePad, kTalkbackCellVerticalPad,
                                    kTalkbackCellNamePad, kTalkbackCellVerticalPad);
    cell_layout->setSpacing(0);

    const auto make_label = [this, cell_layout](const char *object_name) {
        auto *label = new QLabel(this);
        label->setObjectName(QString::fromLatin1(object_name));
        // Every press and release still lands on the button itself, so the
        // keying machinery is looking at exactly the widget it always was.
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        // IGNORED HORIZONTALLY, and this is the load-bearing line of the file.
        // A QLabel's minimum width is the width of its text, so a label that
        // defends its name would make the cell -- and through it the grid, the
        // panel and the dock -- as wide as the longest Zoom display name in
        // the room. That is the exact disease that turned seven people into a
        // 400 px tower of buttons, and it comes back through the layout's
        // MINIMUM even after the visible sizing was fixed: on the operator's
        // screen it made the panel body wider than the dock could show. A cell
        // elides, so it has no width to defend.
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        label->setMinimumWidth(0);
        // The width and the font a label ends up with are facts only the label
        // has; both change the answer to "what fits", so both re-elide.
        label->installEventFilter(this);
        cell_layout->addWidget(label);
        return label;
    };
    m_name  = make_label("talkbackCellName");
    m_state = make_label("talkbackCellState");
}

void TalkbackCellButton::set_name(const QString &full_name)
{
    if (m_full_name == full_name)
        return;
    m_full_name = full_name;
    elide_into(m_name, m_full_name);
}

void TalkbackCellButton::set_state_line(const QString &full_state)
{
    if (m_full_state == full_state)
        return;
    m_full_state = full_state;
    elide_into(m_state, m_full_state);
}

// ELIDED REACTIVELY, NEVER PREDICTIVELY. The metrics are the font the label
// ACTUALLY has (the stylesheet's, once polished, at whatever size the display
// resolved it to) and the room is the width the layout ACTUALLY gave it. No
// budget derived anywhere else is consulted, because every such budget on this
// panel was wrong on the operator's display.
void TalkbackCellButton::elide_into(QLabel *label, const QString &full)
{
    if (!label)
        return;
    const int room = label->contentsRect().width();
    // Nothing has been laid out yet: show the whole string rather than elide
    // against a default 100 px. The next resize corrects it, and the panel's
    // tick calls the flow again regardless.
    if (room <= 0) {
        if (label->text() != full)
            label->setText(full);
        return;
    }
    const QFontMetrics fm(label->font());
    const QString shown = fm.horizontalAdvance(full) <= room
        ? full
        : fm.elidedText(full, Qt::ElideRight, room);
    if (label->text() != shown)
        label->setText(shown);
}

void TalkbackCellButton::re_elide()
{
    if (m_eliding)
        return;
    m_eliding = true;
    elide_into(m_name, m_full_name);
    elide_into(m_state, m_full_state);
    m_eliding = false;
}

void TalkbackCellButton::resizeEvent(QResizeEvent *event)
{
    QPushButton::resizeEvent(event);
    // Run the inner layout NOW rather than waiting for the posted
    // LayoutRequest: the labels' new widths are what the elision is measured
    // against, and eliding against last frame's width is how a name gets
    // clipped for a whole frame on every resize.
    if (layout())
        layout()->activate();
    re_elide();
}

bool TalkbackCellButton::eventFilter(QObject *watched, QEvent *event)
{
    if ((watched == m_name || watched == m_state) &&
        (event->type() == QEvent::Resize ||
         event->type() == QEvent::FontChange ||
         event->type() == QEvent::StyleChange))
        re_elide();
    return QPushButton::eventFilter(watched, event);
}

void TalkbackCellButton::changeEvent(QEvent *event)
{
    QPushButton::changeEvent(event);
    // Not EnabledChange: QAbstractButton::changeEvent() clears `down` on that
    // one, which is the lost-release cause talkback_dock_release_lost() exists
    // for -- re-eliding there would be harmless, but the panel is careful
    // never to disable a held cell and this stays out of that path entirely.
    if (event->type() == QEvent::FontChange ||
        event->type() == QEvent::StyleChange)
        re_elide();
}

QSize TalkbackCellButton::sizeHint() const
{
    QSize hint = QPushButton::sizeHint();
    if (layout())
        hint.setHeight(std::max(hint.height(), layout()->totalMinimumSize().height()));
    return hint;
}

QSize TalkbackCellButton::minimumSizeHint() const
{
    QSize hint = QPushButton::minimumSizeHint();
    if (layout())
        hint.setHeight(std::max(hint.height(), layout()->totalMinimumSize().height()));
    hint.setWidth(0);
    return hint;
}

int talkback_layout_cell_grid(QGridLayout *grid,
                              const std::vector<TalkbackCellButton *> &cells,
                              int available_px, int gap_px,
                              int current_columns, bool freeze_columns)
{
    if (!grid || cells.empty())
        return current_columns;

    TalkbackCellButton *first = nullptr;
    for (auto *cell : cells)
        if (cell) { first = cell; break; }
    if (!first)
        return current_columns;

    // HOW NARROW A CELL MAY GET, MEASURED. The 118 px floor was derived at
    // 1:1 font metrics and is kept only as a floor: on a display that renders
    // the cell font larger, 118 px stops being "a minimum readable width" and
    // becomes "a width that fits four characters". So the real minimum is a
    // gauge string in the NAME LABEL'S OWN metrics plus the cell's OWN
    // chrome, both read off the live widget at decision time.
    const QFontMetrics name_fm(first->name_label()->font());
    const int measured_chrome = first->width() > 0 && first->name_label()->width() > 0
        ? std::max(0, first->width() - first->name_label()->contentsRect().width())
        : 2 * kTalkbackCellNamePad;
    const int min_cell_px = talkback_dock_cell_min_px(
        name_fm.horizontalAdvance(QString::fromLatin1(kTalkbackDockCellGauge)),
        measured_chrome, kTalkbackDockCellMinPx);

    int columns = talkback_dock_cell_columns(available_px, min_cell_px, gap_px);
    if (freeze_columns && current_columns != 0)
        columns = current_columns;

    if (columns != current_columns) {
        for (auto *cell : cells)
            if (cell) grid->removeWidget(cell);
        int column = 0, row = 0;
        for (auto *cell : cells) {
            if (!cell) continue;
            if (cell->all_talent()) {
                // Its own row, full width, above everyone.
                if (column != 0) { column = 0; ++row; }
                grid->addWidget(cell, row, 0, 1, columns);
                ++row;
                continue;
            }
            grid->addWidget(cell, row, column);
            if (++column == columns) { column = 0; ++row; }
        }
    }

    // EVERY COLUMN THE LAYOUT KNOWS ABOUT, on every pass -- not just the ones
    // this flow uses. THE LIVE DEFECT: a grid that had been three columns wide
    // and re-flowed to two left column 2 with the stretch the wider flow gave
    // it, and QGridLayout hands a stretched column its share whether or not
    // anything is in it. An empty third of the dock is what made the two
    // visible cells two thirds as wide as the pass that elided their names had
    // charged them for -- which is the whole of defect (1), mid-glyph clipping
    // with no ellipsis, on the operator's screen.
    for (int c = 0; c < grid->columnCount(); ++c)
        grid->setColumnStretch(c, c < columns ? 1 : 0);

    // THE CONTAINER'S HEIGHT COMES FROM THE LAYOUT, never from rows x an
    // assumed cell height. A re-flow that changes the row count changes the
    // height the grid needs, and the parent only re-reads that if something
    // says the geometry moved -- the operator's container kept the height it
    // had negotiated for the WIDER flow's fewer rows (four) while the narrower
    // flow needed five, and cut the last row in half. That is defect (2), and
    // the offscreen harness reproduces it exactly: 216 px of container for a
    // 272 px grid.
    //
    // HONESTLY: deleting these four lines does NOT bring the defect back in
    // the harness. What fixes it there is TalkbackCellButton reporting a
    // height from the layout inside it and no width at all, which lets Qt's
    // own propagation reach the right answer. They are kept anyway, as the
    // explicit statement of the rule, because the harness could only
    // reproduce defect (2) under one of the two Qt platform plugins it can
    // run -- an invalidation race is exactly the kind of thing that differs
    // between the harness and the OBS dock, and this is the cheap end of that
    // bet. Do not read them as pinned: they are not.
    grid->invalidate();
    if (QWidget *container = grid->parentWidget()) {
        const int need = grid->minimumSize().height();
        if (container->minimumHeight() != need)
            container->setMinimumHeight(need);
        container->updateGeometry();
    }
    return columns;
}
