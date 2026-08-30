#pragma once
//
// talkback-cell-grid.h — the intercom grid's cell widget and its flow, made
// DELIBERATELY DUMB.
//
// WHY THIS FILE LOOKS LIKE IT DOES, and it is a history of four rounds rather
// than a preference:
//
//   Round 1 predicted a pixel budget ((available - gaps) / columns), elided
//   every name against it, and let the QGridLayout hand the cell a different
//   width. Names clipped mid-glyph.
//   Round 2 replaced the prediction with measurement -- a cell elided from its
//   own geometry, reported a height derived from the layout inside it and NO
//   width at all, rewrote every column's stretch, and pushed a minimum height
//   onto its container. An offscreen harness certified it.
//   The operator's real OBS clipped it anyway: the grid area got ~90 px, the
//   single "All talent / assigning..." cell was cut across its state line, and
//   500 px of empty panel sat underneath (2026-08-30).
//
// THREE ROUNDS OF CUSTOM SIZING, THREE HARNESS CERTIFICATIONS, THREE CLIPPED
// RENDERS ON THE OPERATOR'S SCREEN. The verdict is not "measure harder". It is
// that height negotiation written by hand in this codebase does not survive
// contact with a real OBS dock -- one of the previous rounds even reproduced a
// defect under Qt's "minimal" platform plugin and not under "windows", which is
// an invalidation race, i.e. exactly the class of bug hand-written negotiation
// creates and a harness cannot see.
//
// So there is no negotiation left to get wrong:
//
//   * A CELL HAS A FIXED HEIGHT. setFixedHeight() from the labels' OWN live
//     QFontMetrics, computed at construction and again on every font/style
//     change, floored at the design height. Fixed means minimum == maximum, so
//     qSmartMinSize() takes it verbatim and QPushButton's own hints (which are
//     derived from text() -- empty in a cell -- and were the reason round 2
//     overrode them) cannot get a say. DPI-safe because it is derived from live
//     metrics; simple because it never renegotiates.
//   * THE GRID IS A PLAIN QGridLayout OF FIXED-HEIGHT WIDGETS. Standard layouts
//     compute correct minimums for fixed-height children and propagate them up
//     through QWidget::minimumSizeHint() with no help. There is no sizeHint()
//     override, no minimumSizeHint() override, no setColumnStretch()
//     bookkeeping, no invalidate(), and nothing tells the container its height
//     moved -- all four are gone, and all four were height/width negotiation.
//   * A RE-FLOW THROWS THE LAYOUT AWAY AND BUILDS A NEW ONE. Not clear-and-
//     re-add: a fresh QGridLayout has exactly the columns we put in it, so a
//     wider flow's left-over column (which held a third of the operator's dock
//     in round 2, through a stretch nobody cleared) cannot exist as state.
//     At <= 25 cells this is cheap, and it is gated on the column count
//     actually changing, so it does not run on the panel's 10 Hz tick.
//
// What survived, because it works: the cell's REACTIVE end-elision -- it elides
// in its own event filter, from QFontMetrics of the font the label actually has
// against the width the label actually got, never from a budget computed
// elsewhere. And the column count still adapts to the dock's width from
// MEASURED inputs (a gauge string in the live label's metrics plus the cell's
// own measured chrome, talkback_dock_cell_min_px()); that is an input to a
// discrete choice between two and three, not a pixel anybody spends.
//
// Qt Widgets only -- no libobs, no engine. That is NOT so an offscreen harness
// can certify the vertical layout: this file's own history says such a
// certification is worth nothing. The panel that uses this
// (src/zoom-talkback-panel.cpp) carries a layout-verification instrument
// instead (COREVIDEO_TALKBACK_LAYOUT_TEST), which renders the REAL panel, with
// every cell state, in the operator's REAL OBS.

#include <QPushButton>
#include <QString>

#include <vector>

class QEvent;
class QLabel;
class QResizeEvent;
class QWidget;

// The breathing room inside a cell, applied as the cell layout's own margin
// rather than as stylesheet padding: a QPushButton with a layout inside it lays
// that layout out over its whole rect, not over the style's contents rect, so
// stylesheet padding would be counted by sizeFromContents() and then not
// actually applied.
constexpr int kTalkbackCellNamePad = 9;
constexpr int kTalkbackCellVerticalPad = 5;

// The floor under the measured height, and the design height at 1:1 metrics.
// It used to be `min-height: 46px` in cv-style.h; the sheet no longer sets one,
// because a height decided in two places is a height that disagrees with
// itself -- and the stylesheet's copy would have lost anyway, setFixedHeight()
// being an explicit minimum. Above 1:1 the measured value exceeds it and the
// floor is inert, which is the same treatment kTalkbackDockCellMinPx gets
// horizontally.
constexpr int kTalkbackCellMinHeightPx = 46;

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

    // NO sizeHint()/minimumSizeHint() OVERRIDES, ON PURPOSE. See the file
    // header: the height is fixed and the width is nobody's to defend.

protected:
    void resizeEvent(QResizeEvent *event) override;
    // Reacts to the labels' own resizes and font/style changes: the width a
    // label got and the font it got are both facts only the label has.
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void re_elide();
    // The whole of this cell's height policy, in one call, from live metrics.
    void apply_fixed_height();
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

// Flows the cells into `container` and returns the column count now in force.
//
// The container's QGridLayout is REPLACED on every re-flow rather than edited,
// so no left-over column, stretch factor or invalidation state can survive one
// (see the file header). The widgets are children of `container` and outlive
// their layout untouched -- including one the operator is holding, whose `down`
// state only an EnabledChange can clear.
//
// `available_px` must be the width the DOCK gives, never the container's own
// width: the container's width is downstream of this decision (Qt sizes a
// widget from the layout inside it), so feeding it back in is a loop that can
// only ever confirm whatever the grid already did.
//
// `current_columns` is the count from the last call (0 when nothing has been
// laid out yet, which forces a build). `freeze_columns` keeps that count
// whatever the width says -- the panel passes true while the operator is
// holding a key, because re-parenting the widget under their finger is not
// worth a column.
int talkback_layout_cell_grid(QWidget *container,
                              const std::vector<TalkbackCellButton *> &cells,
                              int available_px, int gap_px,
                              int current_columns, bool freeze_columns);
