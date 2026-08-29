#pragma once

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
// Every decision this panel renders -- which buttons are live, what the banner
// says, what the nomination cost, whether the chosen source is on a program
// track -- lives in src/talkback-dock-state.h so it can be exercised without
// OBS, Qt or a meeting. Keep it that way: the two Majors this feature shipped
// (F1, N1) both lived in wiring that no test could reach.
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

private:
    void refresh();
    void refresh_probe();
    TalkbackDockOpenKey dock_open_key() const;
    void rebuild_key_buttons(const std::vector<TalkbackDockKeyButton> &buttons);
    // Re-flows the existing key buttons into the widest grid their labels fit
    // in, and writes each label elided to its button. Safe to call at any time
    // -- it never enables, disables or deletes a button, so it cannot drop a
    // held key's `down` state (only an EnabledChange does that).
    void layout_key_buttons();
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

    // ── Key ─────────────────────────────────────────────────────────────────
    QWidget   *m_key_row  = nullptr;
    QCheckBox *m_latch_cb = nullptr;
    QLabel    *m_notice   = nullptr;
    std::vector<QPushButton *> m_key_buttons;
    // The UNELIDED label for each button in m_key_buttons, index for index.
    // The button's own text() is whatever fits, so it cannot be the source for
    // the next re-flow: eliding an already-elided string compounds.
    std::vector<QString> m_key_labels;
    // The grid the buttons are currently laid out in, so a resize that does
    // not change it does not re-parent every button.
    int m_key_columns = 0;

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

    // What the last rebuild of the key buttons was built from. The buttons are
    // rebuilt only when this changes: the tick runs ten times a second, and
    // deleting the widget the operator is holding is not a refresh, it is a
    // lost release (see talkback_dock_release_lost()).
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
