#include "talkback-cell-grid.h"
#include "talkback-dock-state.h"

#include <QEvent>
#include <QFontMetrics>
#include <QGridLayout>
#include <QLabel>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <QWidget>

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
        // IGNORED HORIZONTALLY. This is Qt's own idiom for a label that elides
        // -- not a hand-written negotiation trick -- and it is what keeps the
        // longest Zoom display name in the room from deciding the dock's width
        // for everybody: a QLabel's minimum width is the width of its text, and
        // defending that is the disease that turned seven people into a 400 px
        // tower of buttons. Vertically Fixed, so the cell's own fixed height is
        // divided between two lines that each ask for exactly one.
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

    apply_fixed_height();
}

// THE WHOLE OF THIS CELL'S HEIGHT POLICY. Two lines of the labels' own live
// metrics plus the layout's own vertical margin, floored at the design height,
// and then FIXED -- minimum == maximum -- so the grid has nothing to negotiate
// and QPushButton's text()-derived hints (empty here) have no say.
//
// Recomputed on font and style changes rather than once at construction: OBS
// applies a theme after the dock exists, and a window dragged to a display with
// different scaling changes the metrics with no resize at all. That is the
// whole DPI story; there is no scale factor read anywhere.
void TalkbackCellButton::apply_fixed_height()
{
    if (!m_name || !m_state)
        return;
    // The taller of the two lines, twice: the state line is smaller type than
    // the name, and sizing both rows to the larger costs a few pixels and
    // removes the question of which label got which.
    const int line = std::max(m_name->fontMetrics().height(),
                              m_state->fontMetrics().height());
    const int wanted = std::max(kTalkbackCellMinHeightPx,
                                line * 2 + 2 * kTalkbackCellVerticalPad);
    if (minimumHeight() == wanted && maximumHeight() == wanted)
        return;
    setFixedHeight(wanted);
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
// panel was wrong on the operator's display. This is the part of the previous
// round that worked and is kept verbatim.
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
    // clipped for a whole frame on every resize. This runs the cell's OWN
    // layout over its own rect; it negotiates nothing with anybody.
    if (layout())
        layout()->activate();
    re_elide();
}

bool TalkbackCellButton::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_name || watched == m_state) {
        if (event->type() == QEvent::FontChange ||
            event->type() == QEvent::StyleChange) {
            // The metrics the height was derived from have just moved.
            apply_fixed_height();
            re_elide();
        } else if (event->type() == QEvent::Resize) {
            re_elide();
        }
    }
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
        event->type() == QEvent::StyleChange) {
        apply_fixed_height();
        re_elide();
    }
}

int talkback_layout_cell_grid(QWidget *container,
                              const std::vector<TalkbackCellButton *> &cells,
                              int available_px, int gap_px,
                              int current_columns, bool freeze_columns)
{
    if (!container || cells.empty())
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
    //
    // This is the one measurement left in this file, and it is an INPUT to a
    // discrete choice between two columns and three -- not a pixel budget
    // anybody spends afterwards. Nothing downstream is elided, positioned or
    // sized against it; the layout does all of that from the widgets.
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

    // GATED ON AN ACTUAL CHANGE. The panel calls this ten times a second and on
    // every resize; a column count that has not moved is a no-op, and the
    // caller resets its count to 0 when the cells themselves are rebuilt, which
    // is what forces the build below.
    if (columns == current_columns && container->layout())
        return columns;

    // NAIVE, AND THAT IS THE POINT. The old layout is deleted outright rather
    // than edited, so a wider flow's left-over column -- which is what held a
    // third of the operator's dock in the round this replaces, through a
    // stretch factor nobody cleared -- cannot exist as state at all. Deleting a
    // QLayout deletes its QLayoutItems and NOT the widgets: every cell is a
    // child of `container` and comes through untouched, including one under the
    // operator's finger (only an EnabledChange clears a QAbstractButton's
    // `down`, and nothing here enables or disables anything).
    delete container->layout();

    auto *grid = new QGridLayout(container);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(gap_px);

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

    // NOTHING FOLLOWS. No column stretch to write, no invalidate(), and nothing
    // told about a height that moved: every cell has a fixed height, so
    // QGridLayout's own minimum is exact, and QWidget::minimumSizeHint() hands
    // it up the chain to the scroll area with no help from us. Three rounds of
    // helping is what this file's header is about.
    return columns;
}
