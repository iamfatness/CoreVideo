#pragma once
#include <QString>

// CoreVideo plugin stylesheet — applied widget-level so it scopes to our
// widgets without interfering with OBS's global QApplication stylesheet.
//
// Button role variants (set via QPushButton::setProperty("role", "primary")):
//   "primary"  → Zoom-blue accent for affirmative actions (Join, Apply, Save)
//   "danger"   → Red tint for destructive or exit actions (Leave, Delete)
inline QString cv_stylesheet()
{
    return QString(R"css(
/* ─── Group Boxes ──────────────────────────────────────────────────────────── */
QGroupBox {
    font-weight: 600;
    color: #7a8faa;
    border: 1px solid #303030;
    border-radius: 6px;
    margin-top: 20px;
    padding: 12px 8px 8px 8px;
    background-color: rgba(0,0,0,0.10);
}
QGroupBox::title {
    subcontrol-origin: margin;
    subcontrol-position: top left;
    left: 10px;
    padding: 0 5px;
}

/* ─── Buttons ───────────────────────────────────────────────────────────────── */
QPushButton {
    padding: 5px 14px;
    border-radius: 4px;
    min-height: 26px;
    border: 1px solid #484848;
    background-color: #2c2c2c;
    color: #d0d0d0;
}
QPushButton:hover   { background-color: #383838; border-color: #5a5a5a; }
QPushButton:pressed { background-color: #202020; }
QPushButton:disabled { color: #505050; background-color: #1e1e1e; border-color: #303030; }

QPushButton[role="primary"] {
    background-color: #1D6DC2;
    border-color: #1D6DC2;
    color: #ffffff;
    font-weight: 600;
}
QPushButton[role="primary"]:hover   { background-color: #2479d6; border-color: #2479d6; }
QPushButton[role="primary"]:pressed { background-color: #185db0; border-color: #185db0; }
QPushButton[role="primary"]:disabled { background-color: #1a3a5c; border-color: #1a3a5c; color: #505050; }

QPushButton[role="danger"] {
    background-color: #3a1818;
    border-color: #6b2b2b;
    color: #ff7070;
}
QPushButton[role="danger"]:hover   { background-color: #471e1e; border-color: #883333; }
QPushButton[role="danger"]:pressed { background-color: #2e1212; }
QPushButton[role="danger"]:disabled { background-color: #1e1e1e; border-color: #303030; color: #505050; }

/* ─── Text Inputs ───────────────────────────────────────────────────────────── */
QLineEdit {
    padding: 5px 8px;
    border-radius: 4px;
    border: 1px solid #404040;
    background-color: #181818;
    color: #e2e2e2;
    min-height: 24px;
    selection-background-color: #1D6DC2;
}
QLineEdit:focus   { border-color: #1D6DC2; }
QLineEdit:disabled { color: #505050; }
QLineEdit[error="true"] { border: 1px solid #cc3333; background-color: #2a1515; }

/* ─── Combo Boxes ───────────────────────────────────────────────────────────── */
QComboBox {
    padding: 4px 8px;
    border-radius: 4px;
    border: 1px solid #404040;
    background-color: #181818;
    color: #e2e2e2;
    min-height: 24px;
}
QComboBox:hover { border-color: #5a5a5a; }
QComboBox:focus { border-color: #1D6DC2; }
QComboBox::drop-down { border: none; width: 20px; }
QComboBox QAbstractItemView {
    background-color: #242424;
    border: 1px solid #484848;
    selection-background-color: #1D6DC2;
    outline: none;
}

/* ─── Tables ────────────────────────────────────────────────────────────────── */
QTableWidget {
    background-color: #181818;
    border: 1px solid #2a2a2a;
    border-radius: 4px;
    gridline-color: #252525;
    color: #e2e2e2;
    alternate-background-color: #1d1d1d;
}
QTableWidget::item { padding: 3px 6px; }
QTableWidget::item:selected { background-color: #1D6DC2; color: #ffffff; }
QHeaderView::section {
    background-color: #202020;
    color: #7a8faa;
    font-weight: 600;
    padding: 5px 6px;
    border: none;
    border-bottom: 1px solid #2e2e2e;
    border-right:  1px solid #282828;
}

/* ─── Checkboxes ────────────────────────────────────────────────────────────── */
QCheckBox { spacing: 6px; color: #d0d0d0; }
QCheckBox::indicator {
    width: 14px; height: 14px;
    border-radius: 3px;
    border: 1px solid #555555;
    background-color: #181818;
}
QCheckBox::indicator:checked       { background-color: #1D6DC2; border-color: #1D6DC2; }
QCheckBox::indicator:checked:hover { background-color: #2479d6; }
QCheckBox::indicator:hover         { border-color: #888888; }

/* ─── Spin Boxes ────────────────────────────────────────────────────────────── */
QSpinBox {
    padding: 4px 6px;
    border-radius: 4px;
    border: 1px solid #404040;
    background-color: #181818;
    color: #e2e2e2;
    min-height: 24px;
}
QSpinBox:focus { border-color: #1D6DC2; }

/* ─── Scroll Bars ───────────────────────────────────────────────────────────── */
QScrollBar:vertical   { width: 6px;  background: transparent; margin: 0; }
QScrollBar:horizontal { height: 6px; background: transparent; margin: 0; }
QScrollBar::handle:vertical, QScrollBar::handle:horizontal {
    background: #404040; border-radius: 3px; min-height: 20px; min-width: 20px;
}
QScrollBar::handle:vertical:hover, QScrollBar::handle:horizontal:hover { background: #5a5a5a; }
QScrollBar::add-line:vertical,  QScrollBar::sub-line:vertical  { height: 0; }
QScrollBar::add-line:horizontal,QScrollBar::sub-line:horizontal { width: 0; }

/* ─── Named Widgets ─────────────────────────────────────────────────────────── */
QLabel#speakerValue { color: #999999; font-style: italic; }
QLabel#errorLabel   { color: #ee5555; font-size: 11px; }
QLabel[role="muted"] { color: #8a8a8a; font-size: 11px; }

)css"
    // SPLIT HERE, and it is not a formatting choice: MSVC caps a single
    // string literal at 16380 bytes (C2026), which the sheet crossed when the
    // intercom grid's rules landed. Adjacent literals concatenate at compile
    // time, so this is one string to Qt and two to the compiler. Split at a
    // section boundary so a future rule lands on the obvious side of it.
    R"css(
/* Talkback (Milestone 7), all of it on the Talkback dock. Each of the two
   pairs below is a calm default plus one flagged state, switched by a dynamic
   property from zoom-talkback-panel.cpp's set_style_flag(). Amber is "you
   should look at this" (the same #f0b429 the output table uses for a
   below-requested signal); red is reserved for a key that is actually live to
   talent, because that is the one state where the director is audible, and it
   appears only on the banner and on the button holding that key. */
QLabel#talkbackTrackWarning              { color: #8a8a8a; font-size: 11px; }
QLabel#talkbackTrackWarning[risk="true"] { color: #f0b429; font-weight: 600; }
/* The plan block reads top-down: the ANSWER ("6 channels in use of 16 for 5
   people") at body weight and body colour, the names that support it one step
   quieter. Setting both as one muted 11px run -- which is how it first
   shipped -- turned the answer into a footnote about itself. Amber still wins
   over both when something the operator asked for did not happen. */
QLabel#talkbackPlan                      { color: #d0d0d0; font-size: 12px; }
QLabel#talkbackPlan[warn="true"]         { color: #f0b429; font-weight: 600; }
QLabel#talkbackPlanDetail                { color: #8a8a8a; font-size: 11px; }
QLabel#talkbackPlanDetail[warn="true"]   { color: #e0a020; }

/* ─── Talkback: the ON AIR banner ───────────────────────────────────────────
   THE ONE ELEMENT ON THE PANEL THAT IS ALLOWED TO SHOUT, and the reason it
   exists: the first live render put "am I audible to talent right now" in
   small red text under the key buttons, which is not readable from the other
   side of a control room. Live is a filled red strip; keyed-but-unconfirmed
   and refused share the amber that means "look at this" everywhere else in
   this sheet; idle is deliberately quiet so the loud states carry meaning.
   The state comes from talkback_dock_banner() via a "state" property. */
QFrame#talkbackBanner {
    border-radius: 6px;
    border: 1px solid #303030;
    background-color: rgba(255,255,255,0.04);
}
QFrame#talkbackBanner[state="waiting"], QFrame#talkbackBanner[state="refused"] {
    border: 1px solid rgba(240,180,41,0.55);
    background-color: rgba(240,180,41,0.12);
}
QFrame#talkbackBanner[state="live"] {
    border: 2px solid #ff4d4d;
    background-color: #8c1c1c;
}
/* TALKBACK DELIVERY LAW 1 (2026-08-29): on air, and NOBODY CAN HEAR IT --
   Zoom accepts every buffer while this client's own meeting audio is muted and
   delivers silence. Amber on the LIVE red: the ground stays red because the
   key genuinely is open and the director is genuinely talking, and the border
   and type go amber because this sheet's amber means "look at this" in every
   other state. It must never be mistakable for the clean live rule above --
   that mistake IS the defect. */
QFrame#talkbackBanner[state="livemuted"] {
    border: 2px solid #f0b429;
    background-color: #8c1c1c;
}
/* Off air is what this strip shows for all but a few seconds of a show, so it
   is sized to be legible rather than to shout; the shouting is the LIVE rule
   below, which nearly doubles the type and fills the ground. A permanently
   16px/700 line just made the calm state tall. */
QLabel#talkbackBannerLine {
    background: transparent;
    border: none;
    color: #8a8a8a;
    font-size: 14px;
    font-weight: 700;
}
QLabel#talkbackBannerLine[state="waiting"],
QLabel#talkbackBannerLine[state="refused"] { color: #f0b429; }
QLabel#talkbackBannerLine[state="live"] {
    color: #ffffff;
    font-size: 19px;
    font-weight: 800;
}
/* Same size and weight as LIVE -- this line has to be readable from across the
   room exactly as urgently -- in amber, so the two states cannot be confused
   at a glance. See the frame rule above. */
QLabel#talkbackBannerLine[state="livemuted"] {
    color: #ffd166;
    font-size: 19px;
    font-weight: 800;
}
QLabel#talkbackBannerDetail {
    background: transparent;
    border: none;
    color: #8a8a8a;
    font-size: 11px;
}
QLabel#talkbackBannerDetail[state="waiting"],
QLabel#talkbackBannerDetail[state="refused"] { color: #e0a020; }
QLabel#talkbackBannerDetail[state="live"]    { color: #ffd8d8; }
/* The one detail line an operator has to ACT on ("ask the host to unmute
   CoreVideo"), so it is brighter than the other detail states, not dimmer. */
QLabel#talkbackBannerDetail[state="livemuted"] { color: #ffe3a3; }

/* --- Talkback: the intercom grid ------------------------------------------
   ONE CELL PER PERSON, and the cell is both the status display and the talk
   key. The version this replaces listed everybody twice (a key button each,
   a tick box each) and sized every button to the longest name in the room,
   so one 28-character Zoom display name turned seven people into a 400 px
   column of full-width buttons.

   A cell is COMPACT and FIXED-HEIGHT on purpose: this is a panel to be
   scanned, and 24 people have to stay usable. The two lines inside it are
   real child QLabels (QPushButton draws one font), so their colour is set
   here and not by the button's own `color`.

   The state comes from talkback_dock_cell_state() via a "cell" property. The
   colour vocabulary is the rest of this sheet's: red ONLY for a key that is
   actually live to talent, amber for "you should look at this", grey-out for
   "nothing you do here reaches them". */
QPushButton[role="cell"] {
    min-height: 46px;
    padding: 0px;
    text-align: left;
    border: 1px solid #4a5260;
    border-radius: 4px;
    background-color: #2f3540;
}
QPushButton[role="cell"]:hover   { background-color: #3a4250; border-color: #626d80; }
QPushButton[role="cell"]:pressed { background-color: #202430; }
QPushButton[role="cell"]:disabled { background-color: #1e1e1e; border-color: #303030; }

/* All talent: the target a director reaches for when something has gone
   wrong, and the one that keeps working when the channel budget has not
   covered everybody. It spans the full width of the grid, above everyone. */
QPushButton[role="cell"][kind="all"] {
    background-color: #1D6DC2; border-color: #1D6DC2;
}
QPushButton[role="cell"][kind="all"]:hover   { background-color: #2479d6; border-color: #2479d6; }
QPushButton[role="cell"][kind="all"]:pressed { background-color: #185db0; }
QPushButton[role="cell"][kind="all"]:disabled {
    background-color: #1a3a5c; border-color: #1a3a5c;
}

/* Amber edges for the two states that mean "this person cannot hear you and
   you can do something about it". Deliberately a border and not a fill: a
   filled cell competes with the ON AIR one, and only one thing on this panel
   is allowed to shout. */
QPushButton[role="cell"][cell="nochannel"],
QPushButton[role="cell"][cell="notinchannel"] {
    border: 1px solid rgba(240,180,41,0.65);
    background-color: rgba(240,180,41,0.10);
}
QPushButton[role="cell"][cell="nochannel"]:hover,
QPushButton[role="cell"][cell="notinchannel"]:hover {
    background-color: rgba(240,180,41,0.16);
}
/* Nothing reaches them at all -- their client cannot do talkback, or the
   budget could not cover them. Darker than a disabled cell rather than
   louder than one: there is no action here, and amber would send the
   operator to re-assign channels that cannot help. */
QPushButton[role="cell"][cell="unreachable"] {
    background-color: #191919; border-color: #2b2b2b;
}
QPushButton[role="cell"][cell="unreachable"]:hover {
    background-color: #1f1f1f; border-color: #333333;
}

/* The live key, painted the same red as the banner so the eye goes straight
   from the state to the control holding it. Qt resolves equal-specificity
   rules by source order but a pseudo-state adds specificity, so :hover and
   :pressed have to be spelled out or a held all-talent cell would repaint
   itself with the accent's own pressed colour for exactly as long as it is
   on air. */
QPushButton[role="cell"][cell="onair"],
QPushButton[role="cell"][kind="all"][cell="onair"] {
    background-color: #b02020; border-color: #ff4d4d;
}
QPushButton[role="cell"][cell="onair"]:hover,
QPushButton[role="cell"][kind="all"][cell="onair"]:hover {
    background-color: #c02626; border-color: #ff6b6b;
}
QPushButton[role="cell"][cell="onair"]:pressed,
QPushButton[role="cell"][kind="all"][cell="onair"]:pressed {
    background-color: #8c1c1c; border-color: #ff4d4d;
}
/* on-air + disabled should be unreachable: the state is only set for a key
   THIS dock holds, and talkback_dock_key_buttons() keeps that target's cell
   enabled. Spelt out anyway, because Qt resolves an unlisted overlap between
   [cell="onair"] and :disabled by specificity and source order rather than by
   intent, and a red control that cannot be pressed is the one thing this
   colour must never mean. Disabled wins. */
QPushButton[role="cell"][cell="onair"]:disabled,
QPushButton[role="cell"][kind="all"][cell="onair"]:disabled {
    background-color: #1e1e1e; border-color: #303030;
}

/* The two lines inside a cell. The NAME is what the operator aims at, so it
   carries the weight; the state line is a caption under it and is the only
   part that changes colour with the state. Both labels get [off="true"] when
   the cell is disabled -- flagged rather than left to a ":disabled QLabel#id"
   descendant rule, because Qt resolves that overlap by specificity and the ID
   rule would win, leaving bright text inside a greyed-out control. */
QLabel#talkbackCellName {
    background: transparent; border: none;
    color: #e4e4e4; font-size: 12px; font-weight: 600;
}
QLabel#talkbackCellName[cell="onair"] { color: #ffffff; }
/* All talent sits on a filled accent, so its two lines need their own
   contrast: the muted grey below is calibrated against the neutral cell
   ground and disappears on blue. Set from a property rather than a
   ":cell[kind=all] QLabel" descendant rule, because Qt resolves the overlap
   with the ID rules below by specificity and the ID would win. */
QLabel#talkbackCellName[kind="all"]   { color: #ffffff; }
QLabel#talkbackCellName[off="true"]   { color: #6a6a6a; }
QLabel#talkbackCellState {
    background: transparent; border: none;
    color: #909090; font-size: 10px;
}
QLabel#talkbackCellState[cell="nochannel"],
QLabel#talkbackCellState[cell="notinchannel"] { color: #f0b429; font-weight: 600; }
QLabel#talkbackCellState[cell="unreachable"]  { color: #6f6f6f; }
QLabel#talkbackCellState[cell="onair"] {
    color: #ffffff; font-weight: 800; letter-spacing: 1px;
}
QLabel#talkbackCellState[kind="all"] { color: rgba(255,255,255,0.78); }
QLabel#talkbackCellState[off="true"] { color: #565656; }

/* The bottom strip's own button ([Edit talent] / [Done]). Ordinary weight
   and ordinary size: it is setup, and the grid above it is the panel. */
QPushButton[role="strip"] {
    min-height: 24px;
    padding: 3px 10px;
    font-size: 11px;
    border: 1px solid #3a3a3a;
    background-color: #262626;
    color: #c8c8c8;
}
QPushButton[role="strip"]:hover   { background-color: #303030; border-color: #4a4a4a; }
QPushButton[role="strip"]:checked { background-color: #1D6DC2; border-color: #1D6DC2; color: #ffffff; }

/* A control that must stay reachable without competing for attention -- the
   Milestone 1 probe's disclosure toggle at the bottom of the Talkback dock. */
QPushButton[role="quiet"] {
    background-color: transparent;
    border: 1px solid #2e2e2e;
    color: #8a8a8a;
    font-size: 11px;
    min-height: 22px;
    text-align: left;
    padding: 3px 8px;
}
QPushButton[role="quiet"]:hover   { color: #b0b0b0; border-color: #3d3d3d; }
QPushButton[role="quiet"]:checked { color: #b0b0b0; }

/* ─── Talkback: the nominee list ────────────────────────────────────────────
   A CHECKABLE LIST OF PEOPLE, and it has to read as one. Left to OBS's own
   palette it rendered as a stretched blue selection bar that looked like a
   mis-styled button; selection is switched off in code (selecting a row here
   means nothing) and the rows get real padding so the tick boxes read as
   rows. */
QListWidget#talkbackNominees {
    background-color: #181818;
    border: 1px solid #2a2a2a;
    border-radius: 4px;
    color: #d8d8d8;
    outline: none;
}
QListWidget#talkbackNominees::item          { padding: 5px 6px; border: none; }
QListWidget#talkbackNominees::item:hover    { background-color: #202020; }
QListWidget#talkbackNominees::item:selected { background-color: transparent; color: #d8d8d8; }

QFrame#recoveryPanel {
    background-color: rgba(240,160,0,0.10);
    border: 1px solid rgba(240,160,0,0.40);
    border-radius: 4px;
}
QFrame#recoveryPanel QLabel { color: #e0a020; background: transparent; border: none; }
)css");
}
