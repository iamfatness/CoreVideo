#pragma once

#include "talkback-cell-grid.h"
#include "talkback-dock-state.h"

#include <QWidget>
#include <cstdint>
#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QFrame;
class QLabel;
class QListWidget;
class QPushButton;
class QScrollArea;
class QTimer;

// The Talkback dock.
//
// Milestone 7, and a DELIBERATE, OWNER-APPROVED DEVIATION FROM THE SPEC:
// docs/superpowers/specs/2026-08-24-zoom-talkback-design.md locks the keying
// surfaces to "Companion/Stream Deck, TCP/OSC control API, OBS hotkey. **Not**
// the dock", and its Dock section says "Configuration and tally only, by
// operator preference -- no talk button". The owner has since asked for a
// drivable dock, so this dock keys. Nothing else in that decision moved:
// identity is still by display name, keying still SELECTS a pre-provisioned
// channel, and every refusal still fails closed.
//
// WHY ITS OWN DOCK. This surface shipped first as a group box at the bottom of
// the Zoom Control dock, below Join, Engine and Routing. After the first live
// render the owner's verdict was that it had to be its own panel: keying is the
// mid-show action and it was sharing a column -- and a scroll position -- with
// setup controls nobody touches once a show is running. The Milestone 1 probe
// moved with it, as a quiet diagnostic at the bottom, so Zoom Control is back
// to join / engine / routing / speaker-director only.
//
// THE INTERCOM GRID (owner, after running the first version live with seven
// talent: "need to rethink how this works and how it will look"). The panel
// used to list the same people TWICE -- a key button each in a Key section, a
// tick box each in a Talent section -- so it grew twice as fast as the cast,
// and one 28-character Zoom display name flipped the adaptive key grid to a
// single full-width column: a 400 px tower of buttons for seven people, and
// nothing survivable at twenty-four.
//
// It is now ONE grid, on the model the operator already carries in their head
// from a Clear-Com/RTS panel: an "All talent" key across the top, then one
// COMPACT cell per person, two or three across by dock width, each cell both
// the status display and the talk key. The tick-box list is behind an
// [Edit talent] toggle and takes the grid's place while it is open, so there
// is exactly one list of people on screen at a time.
//
// A cell IS a key button -- a restyled QPushButton, not a new interaction
// path. Every rule the key buttons earned carries over untouched: press/
// release push-to-talk and latch, the single TalkbackDockOpenKey record,
// talkback_dock_release_lost(), the enablement delegation to
// talkback_dock_key_buttons(), the never-disable-a-held-button guard, and
// keying on the Qt main thread. What is new is what the cell SAYS: a state
// line per person (ready / ON AIR / no channel / no talkback / not in
// channel), from talkback_dock_cell_state().
//
// Every decision this panel renders -- which cells are live, what state each
// person is in, what the banner says, what the nomination cost, whether the
// chosen source is on a program track -- lives in src/talkback-dock-state.h
// so it can be exercised without OBS, Qt or a meeting. Keep it that way: the
// two Majors this feature shipped (F1, N1) both lived in wiring that no test
// could reach.
class ZoomTalkbackPanel : public QWidget {
    Q_OBJECT
public:
    explicit ZoomTalkbackPanel(QWidget *parent = nullptr);
    ~ZoomTalkbackPanel() override;
    void prepare_shutdown();
    void refresh_now();

protected:
    // Column count and label elision both depend on how wide the dock is right
    // now, so both are re-decided here rather than fixed at build time.
    void resizeEvent(QResizeEvent *event) override;
    // ...and on how big the fonts are, which a theme change or a move to a
    // display with different scaling can change without any resize at all.
    // Every size on this panel is derived from live metrics, so every one of
    // them has to be re-derived when the metrics move.
    void changeEvent(QEvent *event) override;

private:
    void refresh();
    void refresh_probe();
    TalkbackDockOpenKey dock_open_key() const;
    void rebuild_cells(const std::vector<TalkbackDockCell> &cells);
    // Re-flows the existing cells into two or three columns by the dock's
    // current width and elides each name to the column it landed in. Safe to
    // call at any time -- it never enables, disables or deletes a cell, so it
    // cannot drop a held key's `down` state (only an EnabledChange does that).
    void layout_cells();
    // Shows the grid or the talent editor, never both. Refuses to open the
    // editor while a key is open (talkback_dock_edit_mode()): hiding a held
    // button strands the key.
    void apply_edit_mode();
    // Sets the talent list's height to exactly the rows it should show, from
    // the widget's own measured row height.
    void size_nominee_list();
    void on_nominate_clicked();
    // `latch` is captured at PRESS time, not read at release: an operator who
    // toggles the Latch box while holding a button must not have the release
    // reinterpreted underneath them.
    void key_pressed(const std::string &target, bool latch);
    void key_released(const std::string &target);

    // ── ON AIR banner ───────────────────────────────────────────────────────
    // The one element an operator must be able to read from across the room.
    QFrame *m_banner        = nullptr;
    QLabel *m_banner_line   = nullptr;
    QLabel *m_banner_detail = nullptr;

    // ── The grid ────────────────────────────────────────────────────────────
    // One cell per keyable target. The cell is a QPushButton with two child
    // labels rather than a two-line button text, so the name and the state
    // line can carry different type -- the labels are
    // WA_TransparentForMouseEvents, so every press and release still lands on
    // the button and the keying machinery is untouched.
    struct KeyCell {
        TalkbackCellButton *button = nullptr;
        // Owned by the button, kept here because the panel's paint pass sets
        // style properties directly on them (a QLabel inside a restyled
        // QPushButton is not repolished by the button's own repolish).
        QLabel      *name   = nullptr;
        QLabel      *state  = nullptr;
        // The UNELIDED name, for the tooltip and the rebuild. The label's own
        // text() is whatever fits, so it cannot be the source for the next
        // re-flow: eliding an already-elided string compounds.
        QString      label;
        std::string  target;
        bool         all_talent = false;
    };
    // The scroll area, kept only for its VIEWPORT width: that is the width the
    // operator can actually see, and the one input the grid's own flow cannot
    // measure for itself. See layout_cells().
    QScrollArea *m_scroll = nullptr;
    QWidget   *m_grid_area = nullptr;
    QWidget   *m_cell_row  = nullptr;
    QCheckBox *m_latch_cb  = nullptr;
    QLabel    *m_notice    = nullptr;
    std::vector<KeyCell> m_cells;
    // The grid the cells are currently laid out in, so a resize that does not
    // change it does not re-parent every cell.
    int m_cell_columns = 0;

    // ── Edit talent ─────────────────────────────────────────────────────────
    // The checklist, which takes the grid's place rather than sitting under
    // it. m_edit_requested is what the operator asked for; whether it is
    // honoured is talkback_dock_edit_mode()'s answer, re-decided every tick.
    QWidget     *m_edit_area = nullptr;
    QPushButton *m_edit_btn  = nullptr;
    bool         m_edit_requested = false;

    // ── Talk source ─────────────────────────────────────────────────────────
    QComboBox *m_source_combo = nullptr;
    QLabel    *m_track_label  = nullptr;

    // ── Nomination ──────────────────────────────────────────────────────────
    QListWidget *m_nominee_list = nullptr;
    QPushButton *m_nominate_btn = nullptr;
    // The plan's answer, then its supporting names. Two labels so the headline
    // can carry normal weight and colour while the name runs stay secondary.
    QLabel      *m_plan_label   = nullptr;
    QLabel      *m_plan_detail  = nullptr;
    // The row height the talent list was last sized from, so a tick that
    // changes nothing does not re-set a fixed height ten times a second.
    int m_nominee_sized_rows = -1;
    int m_nominee_sized_row_h = -1;

    // ── Probe (Milestone 1 diagnostic) ──────────────────────────────────────
    // Collapsed by default. m_probe_participant_combo is a dedicated,
    // roster-driven selector: the Zoom Control dock's legacy m_participant_list
    // is only ever populated inside its refresh_outputs(), which returns
    // immediately whenever m_output_table is null (it always is now that
    // routing lives in the Output Manager dialog), so it never actually runs.
    QPushButton *m_probe_toggle            = nullptr;
    QWidget     *m_probe_body              = nullptr;
    QComboBox   *m_probe_participant_combo = nullptr;
    QPushButton *m_probe_btn               = nullptr;
    QLabel      *m_probe_status_label      = nullptr;

    // What the last rebuild of the grid was built from. The cells are rebuilt
    // only when this changes: the tick runs ten times a second, and deleting
    // the widget the operator is holding is not a refresh, it is a lost
    // release (see talkback_dock_release_lost()).
    std::string m_key_signature;
    // What the talent list currently shows, and what it was last built from.
    // A rebuild loses the tick-boxes the operator is in the middle of setting
    // -- and, when it happens on this dock's 100 ms tick, loses the CLICK
    // itself, because an ordinary click outlives one tick. The rule that keeps
    // that rare (and its live defect history) is in
    // talkback_nominee_list_refresh(), src/talkback-dock-state.h.
    TalkbackNomineeListState m_nominee_state;
    // The target THIS dock currently has keyed, empty when it has none. A key
    // opened over the control API is visible in the banner but is not ours to
    // close, so the dock's own release/backstop paths key off this rather than
    // off the controller's "open" flag.
    std::string m_dock_target;
    bool        m_dock_latched = false;
    // Whether a talkback key is open ANYWHERE -- this dock's or another
    // surface's -- as of the last tick. Read only by apply_edit_mode(), which
    // must put the grid back on screen when one is live; the keying paths all
    // use the richer TalkbackDockOpenKey record above.
    bool        m_any_key_open = false;
    // m2: the source scan (obs_enum_sources + obs_get_source_by_name) is the
    // only libobs-walking work on this dock's 100ms tick, and obs_enum_sources
    // holds obs->data.sources_mutex and addref/releases every source for the
    // walk. Monotonic ms of the last scan; 0 forces one on the next tick. The
    // rendered result is cached in the three fields below so the status
    // survives the ticks that skip the scan.
    uint64_t m_source_scan_ms = 0;
    // The same treatment for the probe's roster poll, which is the only other
    // repeated work here heavy enough to want it: monotonic ms of the last
    // poll, 0 forces one on the next tick. Unfolding the probe section, and
    // running a probe, both reset it so neither waits out the gate.
    uint64_t m_probe_poll_ms = 0;
    QString  m_track_short;
    QString  m_track_text;
    bool     m_track_risk = false;
    // Logged once, not ten times a second, if the controller's status ever
    // fails to parse.
    bool m_status_parse_logged = false;
    // The dock's own last refusal -- one the engine never sees (no source
    // chosen, the source is not in the current scene, OBS is at a sample rate
    // the Zoom talkback API will not take, a nominee named "all"). Cleared by
    // the next attempt that gets past it.
    QString m_notice_text;

    QTimer *m_refresh_timer = nullptr;
    bool    m_shutting_down = false;
};
