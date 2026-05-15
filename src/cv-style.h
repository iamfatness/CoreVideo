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

QFrame#recoveryPanel {
    background-color: rgba(240,160,0,0.10);
    border: 1px solid rgba(240,160,0,0.40);
    border-radius: 4px;
}
QFrame#recoveryPanel QLabel { color: #e0a020; background: transparent; border: none; }
)css");
}
