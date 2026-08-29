#pragma once
//
// talkback-cell-grid.h — the intercom grid's cell widget and its flow, made
// SELF-TRUING.
//
// WHY THIS FILE EXISTS, and it is a live defect, not a refactor (operator's
// high-DPI screen, 2026-08-29, second look):
//
//   (1) names clipped MID-GLYPH with no ellipsis -- "Grant Whiteh", "Jeffrey
//       Wiltsh", "Ronny Hofso" -- on a control that opens a live microphone to
//       one named person, which is the wrong-person hazard the elision was
//       added to close in the first place;
//   (2) the last row of cells cut in half across its state line.
//
// Both came from the same habit: the panel MEASURED once, in one place, and
// then spent the answer as a pixel budget somewhere else. It derived a column
// width arithmetically ((available - gaps) / columns), elided each name
// against that number, and let the QGridLayout hand the cell a different
// width -- so the elision was computed for 147 px while the label was painted
// in 103. The grid's own geometry is the only thing that knows what a cell
// actually got, and it was not being asked.
//
// The specific mechanism on the operator's screen, measured off the
// screenshot: the grid re-flowed from three columns to two, and
// setColumnStretch() was written only for the columns the NEW flow uses. The
// third column kept the stretch the WIDER flow gave it, so an empty column
// held a third of the dock: two cells were laid out across two thirds of the
// width the elision pass had charged them for, and the row height the
// container had negotiated for four rows was a row short for five.
//
// So nothing here predicts a pixel:
//   * a cell elides in its OWN event filter, from QFontMetrics of the font the
//     label actually has against the width the layout actually gave it;
//   * a cell reports a height from the layout inside it, not from a constant,
//     so it cannot be handed less room than its two lines need;
//   * a cell reports NO minimum width at all, because it elides -- which is
//     what stops one long Zoom display name from deciding the dock's width
//     for everybody (the disease that produced the 400 px tower this grid
//     replaced, coming back through the layout's minimum);
//   * the flow re-writes the stretch of EVERY column the layout knows about,
//     live ones and left-over ones, and hands the container the layout's own
//     minimum height.
//
// Qt Widgets only -- no libobs, no engine -- so an offscreen harness can build
// the real cells, at a real font scale, and measure what they render. The
// panel that uses this (src/zoom-talkback-panel.cpp) cannot be constructed
// outside OBS, which is exactly why the previous round's "verified offscreen"
// verdict did not survive contact with the operator's display.

#include <QPushButton>
#include <QSize>
#include <QString>

#include <vector>

class QEvent;
class QGridLayout;
class QLabel;
class QResizeEvent;

// The horizontal breathing room inside a cell, applied as the cell layout's
// own margin rather than as stylesheet padding: a QPushButton with a layout
// inside it lays that layout out over its whole rect, not over the style's
// contents rect, so stylesheet padding would be counted by sizeFromContents()
// and then not actually applied.
constexpr int kTalkbackCellNamePad = 9;
constexpr int kTalkbackCellVerticalPad = 5;

// One person (or "All talent") in the intercom grid: a restyled QPushButton --
// so every rule the key buttons earned still applies to it verbatim (press /
// release, the backstop's isDown(), the never-disable-a-held-cell guard) --
// carrying two child labels, because QPushButton draws ONE font and the name
// and the state line are different type.
class TalkbackCellButton : public QPushButton {
    Q_OBJECT
public:
    explicit TalkbackCellButton(QWidget *parent = nullptr);

    // The FULL name. What gets painted is whatever fits, recomputed on every
    // resize; the caller keeps the full string in the tooltip.
    void set_name(const QString &full_name);
    void set_state_line(const QString &full_state);
    const QString &full_name() const { return m_full_name; }

    // All talent spans every column and is not a person. Set before the first
    // flow.
    void set_all_talent(bool on) { m_all_talent = on; }
    bool all_talent() const { return m_all_talent; }

    // For the panel's paint pass, which sets style properties on the labels
    // (a QLabel inside a restyled QPushButton is not repolished by the
    // button's own repolish).
    QLabel *name_label() const { return m_name; }
    QLabel *state_label() const { return m_state; }

    // HEIGHT FROM THE LAYOUT, NOT FROM THE STYLE'S min-height. QPushButton
    // derives both hints from its own text() -- which is empty here -- so a
    // cell whose two labels need more room than the stylesheet's min-height
    // would report a height that fits neither, and the row would be cut. The
    // WIDTH is deliberately left at nothing: a cell elides, so it never has a
    // minimum width to defend, and claiming one is how the longest name in
    // the room would decide the layout for everybody again.
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent *event) override;
    // Reacts to the labels' own resizes and font/style changes: the width a
    // label got and the font it got are both facts only the label has.
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void re_elide();
    static void elide_into(QLabel *label, const QString &full);

    QLabel *m_name  = nullptr;
    QLabel *m_state = nullptr;
    QString m_full_name;
    QString m_full_state;
    bool    m_all_talent = false;
    // setText() inside an event filter watching the same label; one level of
    // re-entry is enough to loop.
    bool    m_eliding = false;
};

// Flows the cells into `grid` and returns the column count now in force.
//
// `available_px` must be the width the DOCK gives, never the grid container's
// own width: the container's width is downstream of this decision (Qt sizes a
// widget from the layout inside it), so feeding it back in is a loop that can
// only ever confirm whatever the grid already did.
//
// `current_columns` is the count from the last call (0 when nothing has been
// laid out yet). `freeze_columns` keeps that count whatever the width says --
// the panel passes true while the operator is holding a key, because
// re-parenting the widget under their finger is not worth a column.
int talkback_layout_cell_grid(QGridLayout *grid,
                              const std::vector<TalkbackCellButton *> &cells,
                              int available_px, int gap_px,
                              int current_columns, bool freeze_columns);
