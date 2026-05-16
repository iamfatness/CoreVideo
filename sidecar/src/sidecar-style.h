#pragma once
#include "show-theme.h"
#include <QString>

// Returns a stylesheet with theme colours substituted.
// Defaults (no theme / null) produce the original Midnight Blue palette.
inline QString sidecar_stylesheet(const ShowTheme *theme = nullptr)
{
    const QString accent     = theme ? theme->accent.name()        : "#2979ff";
    const QString accentNav  = theme ? theme->accent.darker(110).name() : "#1d3de8";
    const QString accentPri  = theme ? theme->accent.darker(115).name() : "#1e3ae8";
    const QString accentHov  = theme ? theme->accent.lighter(115).name() : "#2548ff";
    const QString bg         = theme ? theme->background.name()    : "#0d0d12";
    const QString panelBg    = theme ? theme->panelBg.name()       : "#101016";
    const QString textPri    = theme ? theme->textPrimary.name()   : "#dde0f0";
    const QString textSec    = theme ? theme->textSecondary.name() : "#6868a0";

    return QString(R"css(
/* ─── Global ──────────────────────────────────────────────────────────────── */
* {
    font-family: "SF Pro Display", "Segoe UI", "Helvetica Neue", Arial, sans-serif;
    font-size: 13px;
    outline: none;
}
QMainWindow, QWidget {
    background-color: #0d0d12;
    color: #dde0f0;
}
QDialog {
    background-color: #131319;
}

/* ─── Sidebar ─────────────────────────────────────────────────────────────── */
#sidebar {
    background-color: #101016;
    border-right: 1px solid #1c1c26;
    min-width: 210px;
    max-width: 210px;
}
#logoArea {
    background-color: #101016;
    border-bottom: 1px solid #1c1c26;
    padding: 0;
}
#appName {
    font-size: 15px;
    font-weight: 700;
    color: #ffffff;
    background: transparent;
}
#appSub {
    font-size: 10px;
    color: #4a4a70;
    background: transparent;
}
#searchBox {
    background-color: #1a1a24;
    border: 1px solid #262636;
    border-radius: 8px;
    padding: 7px 12px 7px 30px;
    color: #c0c0d8;
    selection-background-color: #2979ff;
}
#searchBox:focus { border-color: #2979ff; }

QPushButton#navBtn {
    text-align: left;
    padding: 9px 14px 9px 38px;
    border: none;
    border-radius: 8px;
    background: transparent;
    color: #6868a0;
    font-size: 13px;
    font-weight: 500;
}
QPushButton#navBtn:hover {
    background-color: #18182a;
    color: #d0d0f0;
}
QPushButton#navBtn[active="true"] {
    background-color: #1d3de8;
    color: #ffffff;
}

/* ─── Top bar ─────────────────────────────────────────────────────────────── */
#topBar {
    background-color: #0d0d12;
    border-bottom: 1px solid #181828;
    min-height: 56px;
    max-height: 56px;
}
#showLabel {
    font-size: 10px;
    color: #44446a;
    background: transparent;
}
#showName {
    font-size: 17px;
    font-weight: 700;
    color: #ffffff;
    background: transparent;
}
QPushButton#engineOnBtn {
    background-color: #00c44f;
    color: #001a0d;
    border: none;
    border-radius: 16px;
    padding: 6px 18px;
    font-weight: 800;
    font-size: 12px;
    min-height: 32px;
    min-width: 110px;
}
QPushButton#engineOnBtn:hover { background-color: #00db58; }
QPushButton#engineOffBtn {
    background-color: #222230;
    color: #606080;
    border: 1px solid #2a2a40;
    border-radius: 16px;
    padding: 6px 18px;
    font-weight: 700;
    font-size: 12px;
    min-height: 32px;
    min-width: 110px;
}
QPushButton#obsBtn {
    background-color: #1e1e2e;
    color: #c0c0d8;
    border: 1px solid #2a2a3e;
    border-radius: 8px;
    padding: 5px 14px;
    font-weight: 600;
    font-size: 12px;
    min-height: 30px;
}
QPushButton#obsBtn:hover { background-color: #252538; border-color: #3a3a58; }
QPushButton#obsBtn[connected="true"] { border-color: #00c44f; color: #00c44f; }

QPushButton#iconBtn {
    background: transparent;
    border: none;
    border-radius: 8px;
    padding: 6px;
    color: #5050808;
    min-width: 32px;
    min-height: 32px;
    font-size: 16px;
}
QPushButton#iconBtn:hover { background-color: #1a1a28; }

/* ─── Preview area ────────────────────────────────────────────────────────── */
#previewArea {
    background-color: #0d0d12;
}
#previewHeader {
    font-size: 13px;
    font-weight: 600;
    color: #dde0f0;
    background: transparent;
    padding: 0;
}
#previewDivider {
    background-color: #1c1c28;
    max-width: 1px;
    min-width: 1px;
}
QComboBox#aspectCombo {
    background-color: #1a1a26;
    border: 1px solid #252535;
    border-radius: 7px;
    color: #dde0f0;
    padding: 4px 8px;
    font-weight: 600;
    min-height: 28px;
}
QComboBox#aspectCombo::drop-down { border: none; width: 16px; }
QComboBox::QAbstractItemView {
    background-color: #1a1a26;
    border: 1px solid #2e2e44;
    selection-background-color: #2979ff;
}

/* ─── Right panel ─────────────────────────────────────────────────────────── */
#rightPanel {
    background-color: #101016;
    border-left: 1px solid #1c1c26;
    min-width: 280px;
    max-width: 280px;
}
#sectionHeader {
    font-size: 12px;
    font-weight: 700;
    color: #dde0f0;
    background: transparent;
    padding: 12px 12px 6px 12px;
}
QPushButton#chevronBtn {
    background: transparent;
    border: none;
    color: #5050808;
    font-size: 16px;
    padding: 0 8px;
}

/* Template thumbnail buttons */
QPushButton#tmplBtn {
    background-color: #1a1a24;
    border: 1.5px solid #252535;
    border-radius: 10px;
    color: #9090b8;
    font-size: 11px;
    font-weight: 600;
    padding: 6px 4px;
    min-width: 118px;
    min-height: 80px;
}
QPushButton#tmplBtn:hover {
    border-color: #4060e0;
    color: #dde0ff;
    background-color: #151528;
}
QPushButton#tmplBtn[selected="true"] {
    border-color: #2979ff;
    background-color: #10102a;
    color: #ffffff;
}

/* Participant rows */
#participantRow {
    background: transparent;
    border-bottom: 1px solid #1c1c26;
}
QPushButton#assignBtn {
    background-color: #1e3adf;
    color: #ffffff;
    border: none;
    border-radius: 6px;
    padding: 4px 10px;
    font-size: 11px;
    font-weight: 700;
    min-width: 68px;
    min-height: 26px;
}
QPushButton#assignBtn:hover { background-color: #2548f0; }

/* ─── Show phase bar ──────────────────────────────────────────────────────── */
#phaseSegment {
    background-color: #171724;
    border: 1px solid #232338;
    border-radius: 8px;
}
QPushButton#phasePreBtn, QPushButton#phaseLiveBtn, QPushButton#phasePostBtn {
    background-color: transparent;
    border: none;
    border-radius: 0;
    font-size: 10px;
    font-weight: 800;
    letter-spacing: 0.05em;
    padding: 4px 10px;
    min-height: 24px;
    color: #40405e;
}
QPushButton#phasePreBtn  { border-radius: 6px 0 0 6px; }
QPushButton#phasePostBtn { border-radius: 0 6px 6px 0; }
QPushButton#phasePreBtn:hover, QPushButton#phaseLiveBtn:hover, QPushButton#phasePostBtn:hover {
    background-color: #1c1c2e;
    color: #7070a0;
}
QPushButton#phasePreBtn:checked  { background-color: #252545; color: #9090d0; }
QPushButton#phaseLiveBtn:checked {
    background-color: #6a1010;
    color: #ff5050;
    border: none;
}
QPushButton#phasePostBtn:checked { background-color: #252545; color: #9090d0; }

/* ─── Bottom toolbar ──────────────────────────────────────────────────────── */
#bottomBar {
    background-color: #0d0d12;
    border-top: 1px solid #181828;
    min-height: 76px;
    max-height: 76px;
}
QPushButton#toolBtn {
    background-color: #17172296;
    border: 1px solid #25253a;
    border-radius: 18px;
    color: #dde0f0;
    font-size: 13px;
    font-weight: 500;
    padding: 6px 16px;
    min-height: 36px;
}
QPushButton#toolBtn:hover {
    background-color: #22223a;
    border-color: #36365a;
}
QPushButton#toolBtn:pressed { background-color: #131326; }
QPushButton#toolBtn[primary="true"] {
    background-color: #1e3ae8;
    border-color: #1e3ae8;
    color: #ffffff;
    font-weight: 700;
}
QPushButton#toolBtn[primary="true"]:hover { background-color: #2548ff; }

/* ─── Scrollbars ──────────────────────────────────────────────────────────── */
QScrollBar:vertical   { width: 5px; background: transparent; margin: 0; }
QScrollBar:horizontal { height: 5px; background: transparent; margin: 0; }
QScrollBar::handle:vertical, QScrollBar::handle:horizontal {
    background: #252540; border-radius: 2px; min-height: 20px; min-width: 20px;
}
QScrollBar::handle:vertical:hover, QScrollBar::handle:horizontal:hover {
    background: #363660;
}
QScrollBar::add-line:vertical,  QScrollBar::sub-line:vertical  { height: 0; }
QScrollBar::add-line:horizontal,QScrollBar::sub-line:horizontal { width: 0;  }
QScrollBar::corner { background: transparent; }
)css")
    .replace("#2979ff", accent)
    .replace("#1d3de8", accentNav)
    .replace("#1e3ae8", accentPri)
    .replace("#2548ff", accentHov)
    .replace("#0d0d12", bg)
    .replace("#101016", panelBg)
    .replace("#dde0f0", textPri)
    .replace("#6868a0", textSec);
}
