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

/* Talkback (Milestone 7), all of it on the Talkback dock. Each of the two
   pairs below is a calm default plus one flagged state, switched by a dynamic
   property from zoom-talkback-panel.cpp's set_style_flag(). Amber is "you
   should look at this" (the same #f0b429 the output table uses for a
   below-requested signal); red is reserved for a key that is actually live to
   talent, because that is the one state where the director is audible, and it
   appears only on the banner and on the button holding that key. */
QLabel#talkbackTrackWarning              { color: #8a8a8a; font-size: 11px; }
QLabel#talkbackTrackWarning[risk="true"] { color: #f0b429; font-weight: 600; }
QLabel#talkbackPlan                      { color: #b0b0b0; font-size: 11px; }
QLabel#talkbackPlan[warn="true"]         { color: #f0b429; }

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
QLabel#talkbackBannerLine {
    background: transparent;
    border: none;
    color: #8a8a8a;
    font-size: 16px;
    font-weight: 700;
}
QLabel#talkbackBannerLine[state="waiting"],
QLabel#talkbackBannerLine[state="refused"] { color: #f0b429; }
QLabel#talkbackBannerLine[state="live"] {
    color: #ffffff;
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

/* ─── Talkback: key buttons ─────────────────────────────────────────────────
   Sized to be the biggest controls on the panel, because keying is the
   mid-show action and everything else there is setup done once. kind="all" is
   the all-talent target, which is what a director reaches for when something
   has gone wrong; keyed="true" is the one that is actually live, painted the
   same red as the banner so the eye goes straight from the state to the
   control holding it. */
QPushButton[role="key"] {
    min-height: 44px;
    padding: 6px 10px;
    font-size: 14px;
    font-weight: 600;
    border: 1px solid #4a5260;
    background-color: #2f3540;
    color: #e4e4e4;
}
QPushButton[role="key"]:hover   { background-color: #3a4250; border-color: #626d80; }
QPushButton[role="key"]:pressed { background-color: #202430; }
QPushButton[role="key"]:disabled {
    background-color: #1e1e1e; border-color: #303030; color: #565656;
}
QPushButton[role="key"][kind="all"] {
    background-color: #1D6DC2; border-color: #1D6DC2; color: #ffffff;
}
QPushButton[role="key"][kind="all"]:hover   { background-color: #2479d6; border-color: #2479d6; }
QPushButton[role="key"][kind="all"]:pressed { background-color: #185db0; }
QPushButton[role="key"][kind="all"]:disabled {
    background-color: #1a3a5c; border-color: #1a3a5c; color: #565656;
}
/* The live key stays red under the cursor and under a held press. Qt resolves
   equal-specificity rules by source order but a pseudo-state adds specificity,
   so the :hover/:pressed variants have to be spelled out or a held all-talent
   button would repaint itself with the accent's own pressed colour for exactly
   as long as it is on air. */
QPushButton[role="key"][keyed="true"],
QPushButton[role="key"][kind="all"][keyed="true"] {
    background-color: #b02020; border-color: #ff4d4d; color: #ffffff;
}
QPushButton[role="key"][keyed="true"]:hover,
QPushButton[role="key"][kind="all"][keyed="true"]:hover {
    background-color: #c02626; border-color: #ff6b6b; color: #ffffff;
}
QPushButton[role="key"][keyed="true"]:pressed,
QPushButton[role="key"][kind="all"][keyed="true"]:pressed {
    background-color: #8c1c1c; border-color: #ff4d4d; color: #ffffff;
}
/* keyed + disabled should be unreachable: the flag is only set for a key THIS
   dock holds, and talkback_dock_key_buttons() keeps that button enabled. Spelt
   out anyway, because Qt resolves an unlisted overlap between [keyed="true"]
   and :disabled by specificity and source order rather than by intent, and a
   red control that cannot be pressed is the one thing this colour must never
   mean. Disabled wins. */
QPushButton[role="key"][keyed="true"]:disabled,
QPushButton[role="key"][kind="all"][keyed="true"]:disabled {
    background-color: #1e1e1e; border-color: #303030; color: #565656;
}

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
