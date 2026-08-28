#include "zoom-talkback-panel.h"
#include "cv-combo-utils.h"
#include "cv-style.h"
#include "talkback-controller.h"
#include "talkback-dock-state.h"
#include "talkback-key.h"
#include "talkback-plan.h"
#include "zoom-engine-client.h"
#include "zoom-settings.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QStyle>
#include <QTimer>
#include <QVariant>
#include <QVBoxLayout>

#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

// How many nominee rows are visible before the list scrolls. A roster is
// unbounded and a dock column is not; six rows is enough to tick a normal
// talent list without the panel pushing the key buttons off screen, which is
// the one thing on here that must never move.
static constexpr int kNomineeVisibleRows = 6;

// The panel's own tick. Deliberately the same 100ms the Zoom Control dock used
// when this surface lived inside it: the lost-release backstop
// (talkback_dock_release_lost()) reads the key button's own down state, so its
// resolution IS this interval, and a key whose release vanished stays open for
// however long the tick is. The one piece of work in here that walks libobs is
// self-gated to 1Hz and to this dock being visible -- see refresh().
static constexpr int kTalkbackTickMs = 100;

// Renders the latest talkback_probe stage line (raw compact JSON from
// EngineTalkback::report(), see engine/src/engine-talkback.cpp) for the
// status label, so the operator gets "create_channel: code=0" instead of a
// JSON blob. The wire format is not contractual across stages -- each stage
// reports whichever field is relevant to it -- so this is deliberately
// defensive: anything that doesn't parse as an object with a "stage" field
// falls back to the raw text verbatim rather than showing nothing or
// crashing. An empty label would be worse than an ugly one; this diagnostic
// exists specifically so the operator does not have to go read the log.
static QString format_talkback_probe_status(const std::string &raw)
{
    if (raw.empty())
        return QStringLiteral("No probe run yet.");

    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(raw));
    if (!doc.isObject())
        return QString::fromStdString(raw);
    const QJsonObject obj = doc.object();
    const QString stage = obj.value("stage").toString();
    if (stage.isEmpty())
        return QString::fromStdString(raw);

    // Checked in rough "most diagnostic" order; the first one present wins.
    static const char *kFields[] = {
        "code", "error", "supported", "buffers", "ok", "phase"
    };
    for (const char *field : kFields) {
        if (!obj.contains(field))
            continue;
        const QJsonValue v = obj.value(field);
        QString rendered;
        if (v.isBool())
            rendered = v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
        else if (v.isDouble())
            rendered = QString::number(v.toDouble(), 'f', 0);
        else if (v.isString())
            rendered = v.toString();
        else
            continue;
        return QString("%1: %2=%3").arg(stage, QString::fromLatin1(field), rendered);
    }
    return stage;
}

// A dynamic property plus a re-polish is how this plugin's docks switch a
// widget between styled states (see ZoomDock::on_join_clicked()'s "error"
// property on m_meeting_id): the stylesheet rules live in src/cv-style.h, not
// inline here, so there is one place that decides colour. No-op when the flag
// is unchanged -- this runs ten times a second.
static void set_style_flag(QWidget *widget, const char *name, bool value)
{
    if (!widget || widget->property(name).toBool() == value)
        return;
    widget->setProperty(name, value);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

// The same mechanism for a property that is one of several named states rather
// than a flag. The banner is the only thing on this panel that needs it.
static void set_style_state(QWidget *widget, const char *name, const char *value)
{
    if (!widget || widget->property(name).toString() == QLatin1String(value))
        return;
    widget->setProperty(name, QString::fromLatin1(value));
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

static const char *banner_state_name(TalkbackDockBannerState state)
{
    switch (state) {
    case TalkbackDockBannerState::Live:    return "live";
    case TalkbackDockBannerState::Waiting: return "waiting";
    case TalkbackDockBannerState::Refused: return "refused";
    case TalkbackDockBannerState::Off:     break;
    }
    return "off";
}

// Every OBS source that can produce audio, by NAME -- which is what
// TalkbackTap::open() takes (obs_get_source_by_name), and what survives the
// operator rebuilding a scene collection.
//
// CoreVideo's own participant/Zoom audio sources are deliberately NOT filtered
// out: relaying one participant privately to another is a legitimate thing to
// key, and the tap cannot tell the difference anyway.
static std::vector<std::pair<QString, QVariant>> talkback_audio_source_items()
{
    std::vector<std::pair<QString, QVariant>> items;
    items.emplace_back(QStringLiteral("Select audio source"), QVariant(QString()));
    // obs_enum_sources walks inputs and groups, not scenes (the same fact
    // zoom-supersource.cpp's background picker relies on); a group has no
    // audio output flag, so the flag test below is what actually selects.
    obs_enum_sources(
        [](void *param, obs_source_t *src) -> bool {
            auto *out =
                static_cast<std::vector<std::pair<QString, QVariant>> *>(param);
            if (!(obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO))
                return true;
            const char *name = obs_source_get_name(src);
            if (!name || !*name)
                return true;
            const QString qname = QString::fromUtf8(name);
            out->emplace_back(qname, QVariant(qname));
            return true;
        },
        &items);
    return items;
}

ZoomTalkbackPanel::ZoomTalkbackPanel(QWidget *parent)
    : QWidget(parent)
{
    setMinimumWidth(320);

    const ZoomPluginSettings initial_settings = ZoomPluginSettings::load();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    // ── 1. ON AIR banner ────────────────────────────────────────────────────
    //
    // First, full width, and louder than anything else on the panel. That is
    // the point of it: "am I audible to talent right now" is the only fact
    // here an operator has to be able to read without walking to the machine,
    // and in the first version it was a line of small red text under the
    // buttons. What it may say is decided by talkback_dock_banner(); this is
    // only where it is painted.
    m_banner = new QFrame(this);
    m_banner->setObjectName(QStringLiteral("talkbackBanner"));
    m_banner->setProperty("state", QStringLiteral("off"));
    auto *banner_layout = new QVBoxLayout(m_banner);
    banner_layout->setContentsMargins(12, 10, 12, 10);
    banner_layout->setSpacing(2);
    m_banner_line = new QLabel(QStringLiteral("Off air"), m_banner);
    m_banner_line->setObjectName(QStringLiteral("talkbackBannerLine"));
    m_banner_line->setWordWrap(true);
    banner_layout->addWidget(m_banner_line);
    m_banner_detail = new QLabel(m_banner);
    m_banner_detail->setObjectName(QStringLiteral("talkbackBannerDetail"));
    m_banner_detail->setWordWrap(true);
    m_banner_detail->setVisible(false);
    banner_layout->addWidget(m_banner_detail);
    layout->addWidget(m_banner);

    // ── 2. Key ──────────────────────────────────────────────────────────────
    //
    // The dominant block, because keying is the mid-show action and everything
    // else on this panel is setup done once. One button per keyable target,
    // rebuilt from the CONFIRMED plan by refresh(). Empty until something is
    // nominated -- there is nothing to key before that, and an always-present
    // button that always refuses teaches the operator to ignore refusals.
    auto *key_group = new QGroupBox(QStringLiteral("Key"), this);
    auto *key_layout = new QVBoxLayout(key_group);
    key_layout->setSpacing(6);

    m_key_row = new QWidget(key_group);
    auto *key_grid = new QGridLayout(m_key_row);
    key_grid->setContentsMargins(0, 0, 0, 0);
    key_grid->setSpacing(6);
    key_layout->addWidget(m_key_row);

    auto *latch_row = new QHBoxLayout;
    latch_row->setSpacing(8);
    m_latch_cb = new QCheckBox(QStringLiteral("Latch"), key_group);
    m_latch_cb->setChecked(initial_settings.talkback_latch);
    m_latch_cb->setToolTip(
        "Off: hold a key button to talk. On: one press opens the key, the next "
        "closes it. A latch never survives a reconnect.");
    latch_row->addWidget(m_latch_cb);
    latch_row->addStretch(1);
    key_layout->addLayout(latch_row);
    connect(m_latch_cb, &QCheckBox::toggled, this, [this](bool on) {
        if (m_shutting_down)
            return;
        auto s = ZoomPluginSettings::load();
        s.talkback_latch = on;
        s.save();
    });

    // The dock's OWN refusals -- the ones the engine never sees (no source
    // chosen, the source is not in the current scene, OBS is at a sample rate
    // the Zoom talkback API will not take). The banner shows the engine's.
    m_notice = new QLabel(key_group);
    m_notice->setObjectName(QStringLiteral("errorLabel"));
    m_notice->setWordWrap(true);
    m_notice->setVisible(false);
    key_layout->addWidget(m_notice);

    layout->addWidget(key_group);

    // ── 3. Talk source ──────────────────────────────────────────────────────
    //
    // One row: label, combo, and ONE short status line. The full explanation
    // of the program-track risk is the tooltip, not body copy -- see
    // TalkbackDockTrackWarning::short_text.
    auto *source_group = new QGroupBox(QStringLiteral("Talk source"), this);
    auto *source_layout = new QVBoxLayout(source_group);
    source_layout->setSpacing(4);

    auto *source_row = new QHBoxLayout;
    source_row->setSpacing(8);
    m_source_combo = new QComboBox(source_group);
    m_source_combo->setMinimumWidth(160);
    m_source_combo->setToolTip(
        "The OBS audio source you talk through. Use a dedicated source with "
        "every program track unchecked in Advanced Audio Properties.");
    // Seeded from the saved name, not from a scan: the tick below rebuilds
    // this from OBS, and seeding here is what makes the saved choice the
    // preferred selection when it does (replace_combo_items() keeps the
    // current data if it can still find it).
    m_source_combo->addItem(
        initial_settings.talkback_source.empty()
            ? QStringLiteral("Select audio source")
            : QString::fromStdString(initial_settings.talkback_source),
        QVariant(QString::fromStdString(initial_settings.talkback_source)));
    source_row->addWidget(new QLabel(QStringLiteral("Source"), source_group));
    source_row->addWidget(m_source_combo, 1);
    source_layout->addLayout(source_row);
    connect(m_source_combo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) {
                if (m_shutting_down || !m_source_combo)
                    return;
                // Only a real operator change reaches here:
                // replace_combo_items() blocks this combo's signals while it
                // rebuilds and while it restores the selection.
                auto s = ZoomPluginSettings::load();
                s.talkback_source =
                    m_source_combo->currentData().toString().toStdString();
                s.save();
                // Force the next tick to rescan rather than waiting out the
                // 1Hz gate: the operator has just changed the source and the
                // program-track status under it is now about the wrong one.
                m_source_scan_ms = 0;
            });

    // The deferred half of the program/ISO leak guarantee. The structural half
    // holds unconditionally (a capture callback observes a source and cannot
    // add it to any mix -- tests/talkback-isolation-test.cpp) and the tap
    // already logs this at open() time (src/talkback-tap.cpp); this is the
    // same advisory where the operator is looking, and BEFORE the key rather
    // than on it.
    m_track_label = new QLabel(source_group);
    m_track_label->setObjectName(QStringLiteral("talkbackTrackWarning"));
    m_track_label->setWordWrap(true);
    source_layout->addWidget(m_track_label);

    layout->addWidget(source_group);

    // ── 4. Nomination ───────────────────────────────────────────────────────
    //
    // Setup, not showtime -- so it sits below the key block and its button is
    // an ordinary one. The list is CHECKABLE, and its selection is turned off
    // entirely: a full-width selection highlight on a list of tick boxes reads
    // as a mis-styled button, and selecting a row here means nothing.
    auto *nominate_group = new QGroupBox(QStringLiteral("Nomination"), this);
    auto *nominate_layout = new QVBoxLayout(nominate_group);
    nominate_layout->setSpacing(6);

    auto *nominate_hint = new QLabel(
        "Everyone ticked gets a standing Zoom channel, created now so a key "
        "press only opens the microphone.", nominate_group);
    nominate_hint->setWordWrap(true);
    nominate_hint->setProperty("role", "muted");
    nominate_layout->addWidget(nominate_hint);

    m_nominee_list = new QListWidget(nominate_group);
    m_nominee_list->setObjectName(QStringLiteral("talkbackNominees"));
    m_nominee_list->setSelectionMode(QAbstractItemView::NoSelection);
    m_nominee_list->setFocusPolicy(Qt::NoFocus);
    m_nominee_list->setToolTip(
        "Tick the people you may need to talk to. Zoom allows 16 channels and "
        "10 people per channel; anyone the budget cannot cover is named below.");
    {
        // Sized in rows, from this widget's own metrics, so the list is the
        // same height whatever the OBS theme's font is.
        const int row_h = fontMetrics().height() + 10;
        m_nominee_list->setMinimumHeight(3 * row_h);
        m_nominee_list->setMaximumHeight(kNomineeVisibleRows * row_h);
    }
    nominate_layout->addWidget(m_nominee_list);

    auto *nominate_row = new QHBoxLayout;
    nominate_row->setSpacing(8);
    m_nominate_btn = new QPushButton(QStringLiteral("Nominate"), nominate_group);
    m_nominate_btn->setEnabled(false);
    nominate_row->addWidget(m_nominate_btn);
    nominate_row->addStretch(1);
    nominate_layout->addLayout(nominate_row);
    connect(m_nominate_btn, &QPushButton::clicked,
            this, [this]() { on_nominate_clicked(); });

    // The budget outcome, in the operator's own words and with every shortfall
    // NAMED. This block is the whole reason the nomination reporting chain
    // exists (src/talkback-plan.h): a count tells the operator that somebody
    // is short, not who -- and who is the only part they can act on. Long name
    // lists are elided here and complete in the tooltip.
    m_plan_label = new QLabel(nominate_group);
    m_plan_label->setObjectName(QStringLiteral("talkbackPlan"));
    m_plan_label->setWordWrap(true);
    nominate_layout->addWidget(m_plan_label);

    layout->addWidget(nominate_group);

    // ── 5. Probe (Milestone 1 diagnostic) ───────────────────────────────────
    //
    // Kept, kept reachable, and kept quiet. This is the "can this account even
    // open a channel" probe: it destroys its channel afterwards and plays an
    // audible tone at the participant. It is a diagnostic for when talkback
    // does not work at all, not a way to use it -- so it is collapsed by
    // default and lives at the very bottom.
    m_probe_toggle = new QPushButton(
        QStringLiteral("Diagnostic: talkback probe (show)"), this);
    m_probe_toggle->setCheckable(true);
    m_probe_toggle->setProperty("role", "quiet");
    layout->addWidget(m_probe_toggle);

    m_probe_body = new QWidget(this);
    m_probe_body->setVisible(false);
    auto *probe_layout = new QVBoxLayout(m_probe_body);
    probe_layout->setContentsMargins(0, 0, 0, 0);
    probe_layout->setSpacing(6);
    connect(m_probe_toggle, &QPushButton::toggled, this, [this](bool on) {
        if (m_probe_body)
            m_probe_body->setVisible(on);
        if (m_probe_toggle)
            m_probe_toggle->setText(
                on ? QStringLiteral("Diagnostic: talkback probe (hide)")
                   : QStringLiteral("Diagnostic: talkback probe (show)"));
    });

    auto *probe_info = new QLabel(
        "Opens a channel, invites the selected participant, and sends a "
        "3-second 440Hz test tone that they WILL hear, ducking their meeting "
        "audio to 30% for the duration; the channel is destroyed afterward. "
        "Requires host or co-host: a plain participant is refused with "
        "SDKERR_NO_PERMISSION.",
        m_probe_body);
    probe_info->setWordWrap(true);
    probe_info->setProperty("role", "muted");
    probe_layout->addWidget(probe_info);

    m_probe_participant_combo = new QComboBox(m_probe_body);
    m_probe_participant_combo->addItem(QStringLiteral("Select participant"),
                                       QVariant());
    m_probe_participant_combo->setToolTip(
        "Participant to invite into the probe talkback channel.");
    probe_layout->addWidget(m_probe_participant_combo);

    m_probe_btn = new QPushButton(QStringLiteral("Probe selected participant"),
                                  m_probe_body);
    m_probe_btn->setEnabled(false);
    probe_layout->addWidget(m_probe_btn);

    m_probe_status_label = new QLabel(
        format_talkback_probe_status(
            ZoomEngineClient::instance().talkback_probe_status()),
        m_probe_body);
    m_probe_status_label->setWordWrap(true);
    m_probe_status_label->setObjectName(QStringLiteral("talkbackProbeStatus"));
    m_probe_status_label->setProperty("role", "muted");
    probe_layout->addWidget(m_probe_status_label);

    connect(m_probe_btn, &QPushButton::clicked, this, [this]() {
        if (!m_probe_participant_combo)
            return;
        // Passing the NAME, never the id the combo could have carried
        // instead: Zoom user ids are meeting-scoped, so an id captured now
        // would point at nobody after a rejoin and at the wrong person once
        // ids get recycled -- the by-name resolution this whole feature (and
        // the Companion module, see CLAUDE.md) is built on.
        const QString name = m_probe_participant_combo->currentData().toString();
        if (name.isEmpty())
            return;
        ZoomEngineClient::instance().talkback_probe(name.toStdString());
    });
    layout->addWidget(m_probe_body);

    layout->addStretch(1);

    m_refresh_timer = new QTimer(this);
    m_refresh_timer->setInterval(kTalkbackTickMs);
    connect(m_refresh_timer, &QTimer::timeout, this, [this]() { refresh(); });
    m_refresh_timer->start();

    // -- Apply stylesheet last so all properties are set before evaluation ----
    setStyleSheet(cv_stylesheet());
    refresh();
}

ZoomTalkbackPanel::~ZoomTalkbackPanel()
{
    prepare_shutdown();
}

void ZoomTalkbackPanel::prepare_shutdown()
{
    m_shutting_down = true;
    // Close a key this dock opened before its buttons go away. A destroyed
    // QPushButton emits no released(), and after this function the refresh
    // timer that would notice (talkback_dock_release_lost()) is stopped -- so
    // without this the only thing left to close the key is
    // TalkbackController::stop() at plugin unload, which is later than it
    // should be for a key that is on air. key_off() is a no-op if nothing is
    // open, and a key opened over the control API is not this dock's to close.
    if (!m_dock_target.empty()) {
        TalkbackController::instance().key_off();
        m_dock_target.clear();
        m_dock_latched = false;
    }
    if (m_refresh_timer)
        m_refresh_timer->stop();
}

void ZoomTalkbackPanel::refresh_now()
{
    refresh();
}

// The key THIS DOCK is holding, if any -- the single record every dock-side
// keying decision reads (fix round 1, M1). It carries the mode the key was
// opened with, which is deliberately not re-read from the Latch checkbox: the
// checkbox says what the NEXT key would be, this says what the open one IS.
TalkbackDockOpenKey ZoomTalkbackPanel::dock_open_key() const
{
    TalkbackDockOpenKey open;
    open.open       = !m_dock_target.empty();
    open.dock_owned = open.open;
    open.target     = m_dock_target;
    open.latched    = m_dock_latched;
    return open;
}

// The Milestone 1 probe's own polled readouts. Split out from refresh() only
// because it is a diagnostic on a different clock of interest, not because it
// runs on a different one -- it is called from the same tick.
void ZoomTalkbackPanel::refresh_probe()
{
    const bool in_meeting =
        ZoomEngineClient::instance().state() == MeetingState::InMeeting;

    // Rebuilt every tick from the live roster -- see the comment where this
    // combo is constructed for why it is not the Zoom Control dock's legacy
    // participant list.
    if (m_probe_participant_combo && !combo_popup_open(m_probe_participant_combo)) {
        std::vector<std::pair<QString, QVariant>> items;
        items.emplace_back(QStringLiteral("Select participant"), QVariant());
        for (const auto &p : ZoomEngineClient::instance().roster()) {
            if (p.user_id == 0 || p.display_name.empty())
                continue;
            items.emplace_back(participant_label(p),
                               QVariant(QString::fromStdString(p.display_name)));
        }
        replace_combo_items(m_probe_participant_combo, items, QVariant());
    }
    if (m_probe_btn && m_probe_participant_combo) {
        // Disabled outside a live meeting (nobody to invite/nowhere to open a
        // channel) and until a participant is actually selected -- the combo's
        // placeholder item carries an invalid/empty data(), which
        // currentData().toString() reports as empty.
        m_probe_btn->setEnabled(
            in_meeting &&
            !m_probe_participant_combo->currentData().toString().isEmpty());
    }
    if (m_probe_status_label) {
        // Polled, not pushed: handle_event()'s talkback_probe branch just
        // stores the latest line under m_mtx (see zoom-engine-client.cpp) and
        // this timer-driven read picks it up on the next tick, matching how
        // every other readout in this plugin's docks (last_error(), roster(),
        // active_speaker_id()) already reaches the UI.
        m_probe_status_label->setText(format_talkback_probe_status(
            ZoomEngineClient::instance().talkback_probe_status()));
    }
}

void ZoomTalkbackPanel::refresh()
{
    if (!m_source_combo)
        return;

    auto &engine = ZoomEngineClient::instance();
    const bool engine_running = engine.is_running();
    const bool in_meeting = engine.state() == MeetingState::InMeeting;

    // -- Source picker and its program-track status ---------------------------
    //
    // m2: THE ONLY WORK IN THIS FUNCTION THAT WALKS LIBOBS, and the only part
    // that does not run at 10Hz. obs_enum_sources() holds
    // obs->data.sources_mutex for the whole walk and addrefs every source (the
    // constraint is spelled out at zoom-supersource.cpp's
    // warn_on_multiple_audio_walls()), and the Zoom Control dock already moved
    // its feed-health sweep to a 1Hz timer for exactly that reason. Source
    // lists and mixer assignments do not change ten times a second, and nobody
    // is reading a dock that is not on screen, so: once a second, and only
    // while THIS dock is visible. Everything else below -- the key state an
    // operator's press depends on -- stays on the 100ms tick.
    //
    // The rendered status is cached rather than recomputed, so the label does
    // not blank out on the ticks that skip the scan. A source change forces the
    // next tick to rescan (the combo's own handler resets the stamp), so the
    // operator never waits a second to see what they just picked.
    const uint64_t now_ms_tick = os_gettime_ns() / 1000000ULL;
    const bool scan_due =
        isVisible() &&
        (m_source_scan_ms == 0 ||
         talkback_elapsed_ms(now_ms_tick, m_source_scan_ms) >= 1000);
    if (scan_due && !combo_popup_open(m_source_combo)) {
        m_source_scan_ms = now_ms_tick;

        const QString chosen = m_source_combo->currentData().toString();
        auto items = talkback_audio_source_items();
        // Keep a chosen source that OBS does not currently have in the list, so
        // a saved choice survives the source being absent (a scene collection
        // still loading, a mic not plugged in) instead of being silently
        // replaced by whatever happens to be first.
        if (!chosen.isEmpty() && !combo_items_contain(items, chosen))
            items.emplace_back(chosen + " (not in OBS)", QVariant(chosen));
        replace_combo_items(m_source_combo, items, QVariant(chosen));

        const QString scanned = m_source_combo->currentData().toString();
        bool source_present = false;
        uint32_t mixers = 0;
        if (!scanned.isEmpty()) {
            obs_source_t *src =
                obs_get_source_by_name(scanned.toUtf8().constData());
            if (src) {
                source_present = true;
                mixers = obs_source_get_audio_mixers(src);
                obs_source_release(src);
            }
        }
        if (!scanned.isEmpty() && !source_present) {
            // Do NOT fall through to talkback_dock_track_warning() with
            // mixers = 0 here: that would report "off program, safe", which is
            // a safety claim about a source that is not there to be safe.
            m_track_short = QStringLiteral("Not in OBS right now");
            m_track_text =
                QString("%1 is not in OBS right now. Pick the source you are "
                        "actually going to talk through.").arg(scanned);
            m_track_risk = false;
        } else {
            const auto warning = talkback_dock_track_warning(
                scanned.toStdString(), mixers);
            m_track_short = QString::fromStdString(warning.short_text);
            m_track_text = QString::fromStdString(warning.text);
            m_track_risk = warning.on_air_risk;
        }
    }

    const QString source_name = m_source_combo->currentData().toString();

    if (m_track_label) {
        if (m_track_label->text() != m_track_short)
            m_track_label->setText(m_track_short);
        // The prose moved off the panel and into here (defect 3 from the first
        // live render: a wall of amber text beside a control is not a status).
        // Nothing is lost -- the explanation and the remedy are both in it.
        if (m_track_label->toolTip() != m_track_text)
            m_track_label->setToolTip(m_track_text);
    }
    set_style_flag(m_track_label, "risk", m_track_risk);

    // -- Nominee list ----------------------------------------------------------
    // Identity is by display name, never by Zoom user id (ids are
    // meeting-scoped: one captured now points at nobody after a rejoin and at
    // the wrong face once ids get recycled). Each row carries its name in
    // Qt::UserRole, because the row's TEXT can also say "(not in the meeting)".
    //
    // A TICKED NAME THAT HAS LEFT THE ROSTER STAYS ON THE LIST. Nominating
    // somebody who is not here right now is meaningful -- the engine
    // re-resolves nominations by name on every roster change and invites them
    // when they arrive (resolve_roster_change(), engine-talkback.cpp) -- so
    // dropping the row would silently drop them from the next Nominate press,
    // on the exact path where a talent has just disconnected and the director
    // is re-nominating to fix something else.
    const auto roster = engine.roster();
    std::vector<std::string> checked_names;
    if (m_nominee_list) {
        for (int i = 0; i < m_nominee_list->count(); ++i) {
            auto *item = m_nominee_list->item(i);
            if (item && item->checkState() == Qt::Checked)
                checked_names.push_back(
                    item->data(Qt::UserRole).toString().toStdString());
        }
    }

    // (name, present in the meeting right now)
    std::vector<std::pair<std::string, bool>> rows;
    const auto row_index = [&rows](const std::string &name) {
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (rows[i].first == name) return static_cast<int>(i);
        return -1;
    };
    for (const auto &p : roster) {
        // Someone with no display name cannot be nominated at all, and is left
        // out rather than listed as an id that would not resolve.
        if (p.display_name.empty()) continue;
        if (row_index(p.display_name) < 0) rows.emplace_back(p.display_name, true);
    }
    for (const auto &name : checked_names)
        if (row_index(name) < 0) rows.emplace_back(name, false);

    // Rebuilt only when those rows change: this runs ten times a second and a
    // rebuild throws away the tick-boxes the operator is in the middle of
    // setting. (The roster itself churns constantly -- two of Zoom's five
    // roster callbacks fire on every mute and camera toggle by anyone.)
    std::string roster_signature;
    for (const auto &row : rows) {
        roster_signature += row.second ? "+" : "-";
        roster_signature += row.first;
        roster_signature += '\n';
    }
    if (m_nominee_list && roster_signature != m_roster_signature) {
        m_nominee_list->clear();
        for (const auto &row : rows) {
            const QString name = QString::fromStdString(row.first);
            auto *item = new QListWidgetItem(
                row.second ? name : name + " (not in the meeting)");
            item->setData(Qt::UserRole, name);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            const bool was_checked =
                std::find(checked_names.begin(), checked_names.end(), row.first) !=
                checked_names.end();
            item->setCheckState(was_checked ? Qt::Checked : Qt::Unchecked);
            m_nominee_list->addItem(item);
        }
        m_roster_signature = roster_signature;
    }

    const int checked_count = static_cast<int>(checked_names.size());
    if (m_nominate_btn) {
        // Both conditions, not just InMeeting: talkback_nominate() is a silent
        // no-op when the engine pipe is not up, which is why the control API
        // acks "engine_not_running" separately from "not_in_meeting".
        m_nominate_btn->setEnabled(engine_running && in_meeting);
        // An empty nomination is a deliberate denominate (the engine's
        // nominate() documents it as such), not a mistake to block -- but it
        // must not be labelled "Nominate".
        const QString label = checked_count == 0
            ? QStringLiteral("Clear all nominations")
            : QString("Nominate (%1)").arg(checked_count);
        if (m_nominate_btn->text() != label)
            m_nominate_btn->setText(label);
    }

    // -- The confirmed plan, and what it cost ----------------------------------
    const auto plan = engine.talkback_nomination_status();
    const auto report = talkback_dock_nomination_report(plan);
    QString plan_text = QString::fromStdString(report.headline);
    for (const auto &line : report.lines)
        plan_text += "\n" + QString::fromStdString(line);
    if (m_plan_label) {
        if (m_plan_label->text() != plan_text)
            m_plan_label->setText(plan_text);
        // The un-elided lists. talkback_dock_nomination_report() elides a name
        // run past five so the block stays scannable; this is where the rest of
        // the names go, so the elision never costs the operator a name.
        const QString full = QString::fromStdString(report.tooltip);
        if (m_plan_label->toolTip() != full)
            m_plan_label->setToolTip(full);
    }
    set_style_flag(m_plan_label, "warn", report.warn);

    // -- Whose key is open -----------------------------------------------------
    // Polled and parsed, the same treatment format_talkback_probe_status()
    // gives the engine's probe line, rather than a new accessor on the
    // controller.
    //
    // A DOCUMENT THAT DOES NOT PARSE IS "UNKNOWN", NOT "NO KEY OPEN" (fix round
    // 1, n7). Treating it as no-key would clear m_dock_target below -- the dock
    // would forget it owns a live key, its release would find nothing to close,
    // and the backstop would go dormant, which is the same stranded director M1
    // produced. Practically unreachable (status_json() builds the string with
    // QJsonDocument in this same process), so the fallback is simply the dock's
    // own record of what it opened, and the ownership-clear below is skipped
    // for the tick.
    bool status_parsed = false;
    bool key_open = false;
    std::string open_target;
    {
        const QJsonDocument doc = QJsonDocument::fromJson(
            QByteArray::fromStdString(
                TalkbackController::instance().status_json()));
        if (doc.isObject()) {
            status_parsed = true;
            const QJsonObject obj = doc.object();
            key_open = obj.value("open").toBool();
            open_target = obj.value("target").toString().toStdString();
        }
    }
    if (!status_parsed) {
        if (!m_status_parse_logged) {
            m_status_parse_logged = true;   // once, not ten times a second
            blog(LOG_WARNING,
                 "[obs-zoom-plugin] talkback dock: could not parse the "
                 "controller's status; using this dock's own record of the key "
                 "it opened");
        }
        key_open = !m_dock_target.empty();
        open_target = m_dock_target;
    }

    // The controller closes keys the dock never asked it to close: the dead-man
    // switch, an engine refusal, a Leave, a plugin shutdown. Notice that here
    // instead of leaving the dock believing it still owns a key.
    if (status_parsed && !key_open && !m_dock_target.empty()) {
        m_dock_target.clear();
        m_dock_latched = false;
    }

    // The dock's own lost-release backstop -- see talkback_dock_release_lost()
    // in src/talkback-dock-state.h for why this surface reads the widget's own
    // state instead of the renewal machinery a socket surface needs, and why it
    // is cause-agnostic rather than aimed at one way a release can vanish.
    {
        const TalkbackDockOpenKey dock_key = dock_open_key();
        bool button_down = false;
        for (auto *button : m_key_buttons) {
            if (!button) continue;
            if (button->property("cvTalkbackTarget").toString().toStdString() ==
                dock_key.target) {
                button_down = button->isDown();
                break;
            }
        }
        if (talkback_dock_release_lost(dock_key, button_down)) {
            blog(LOG_WARNING,
                 "[obs-zoom-plugin] talkback dock: closing the key to \"%s\" -- "
                 "its button is no longer held and no release ever arrived",
                 m_dock_target.c_str());
            TalkbackController::instance().key_off();
            m_dock_target.clear();
            m_dock_latched = false;
            key_open = false;
            open_target.clear();
        }
    }

    // -- Key buttons -----------------------------------------------------------
    TalkbackDockKeyContext ctx;
    ctx.engine_running = engine_running;
    ctx.in_meeting = in_meeting;
    ctx.source_chosen = !source_name.isEmpty();
    // The dock's own record first -- it is the only thing that knows the MODE
    // an open key was opened in, which status_json() does not report -- and the
    // controller's view only for a key the dock does not own.
    ctx.open = dock_open_key();
    if (!ctx.open.open && key_open) {
        ctx.open.open = true;
        ctx.open.dock_owned = false;
        ctx.open.target = open_target;
    }
    const auto buttons = talkback_dock_key_buttons(plan, ctx);

    std::string target_signature;
    for (const auto &b : buttons) {
        target_signature += b.target;
        target_signature += '\n';
    }
    // Rebuild only when the SET OF TARGETS changes, and never while this dock
    // has a key open: deleting the widget the operator is holding loses its
    // release. Deferring costs little -- the engine refuses a re-nomination
    // outright while a session is live ("reason":"session_live", nominate() in
    // engine-talkback.cpp), so the confirmed plan seldom moves mid-press at
    // all, and when it does (a superseded ladder's terminal invalidating it)
    // the buttons are rebuilt on the first tick after the key closes.
    if (target_signature != m_key_signature && m_dock_target.empty()) {
        rebuild_key_buttons(buttons);
        m_key_signature = target_signature;
    }

    for (const auto &b : buttons) {
        for (auto *button : m_key_buttons) {
            if (!button ||
                button->property("cvTalkbackTarget").toString().toStdString() !=
                    b.target)
                continue;
            // The button of the key that is actually live gets the same red the
            // banner does, so the operator's eye can go straight from "ON AIR"
            // to the control holding it. Set before the enable pass below so a
            // held button is never left painted idle.
            //
            // Safe to repolish a button the operator is HOLDING: the way a
            // QPushButton silently loses `down` is QAbstractButton::
            // changeEvent()'s EnabledChange arm, and a dynamic-property
            // repolish delivers StyleChange/PaletteChange/FontChange, none of
            // which that switch acts on. setEnabled() below is the call that
            // has to be guarded, and it is.
            set_style_flag(button, "keyed",
                           key_open && open_target == b.target);
            // NEVER disable a held button. QAbstractButton clears its pressed
            // state on an EnabledChange without emitting released(), so
            // disabling one mid-press strands the key open until the backstop
            // above notices. talkback_dock_key_buttons() already keeps the open
            // key's own button enabled; this is the second line of defence for
            // any future caller that forgets.
            if (!b.enabled && button->isDown())
                break;
            button->setEnabled(b.enabled);
            const QString tip = b.enabled
                ? QString("Talk to %1.").arg(QString::fromStdString(b.label))
                : QString::fromStdString(b.reason);
            if (button->toolTip() != tip)
                button->setToolTip(tip);
            break;
        }
    }

    // -- ON AIR banner ---------------------------------------------------------
    const auto session = engine.talkback_session_status();
    TalkbackDockSessionView view;
    view.key_open = key_open;
    view.target = open_target;
    view.engine_live = session.live;
    view.engine_reason = session.reason;
    view.engine_recover = session.recover;
    view.members_known = session.members_known;
    view.members_present = session.members_present;
    view.members_total = session.members_total;
    const auto banner = talkback_dock_banner(view);

    // The tally dot is prepended here rather than in the pure header so that
    // header stays plain ASCII, and it is built from a code point rather than
    // typed as a glyph because these sources carry no BOM and this build passes
    // MSVC no /utf-8 -- a raw non-ASCII character in a narrow string literal
    // would be decoded as the system codepage. It is only ever shown for Live,
    // which is the state it means.
    QString banner_text = QString::fromStdString(banner.headline);
    if (banner.state == TalkbackDockBannerState::Live)
        banner_text = QString(QChar(0x25CF)) + "  " + banner_text;
    if (m_banner_line && m_banner_line->text() != banner_text)
        m_banner_line->setText(banner_text);
    const QString banner_detail = QString::fromStdString(banner.detail);
    if (m_banner_detail) {
        if (m_banner_detail->text() != banner_detail)
            m_banner_detail->setText(banner_detail);
        m_banner_detail->setVisible(!banner_detail.isEmpty());
    }
    // set_style_state() self-no-ops when the state has not moved, so these run
    // unguarded on every tick. The two labels are set as well as the frame: a
    // QLabel inside a restyled QFrame is not repolished by the frame's own
    // repolish, and they carry the banner's text colour.
    const char *const state_name = banner_state_name(banner.state);
    set_style_state(m_banner, "state", state_name);
    set_style_state(m_banner_line, "state", state_name);
    set_style_state(m_banner_detail, "state", state_name);

    if (m_notice) {
        if (m_notice->text() != m_notice_text)
            m_notice->setText(m_notice_text);
        m_notice->setVisible(!m_notice_text.isEmpty());
    }

    refresh_probe();
}

// Takes the button specs its caller already computed rather than recomputing
// them from a second, thinner context: the two would otherwise disagree for the
// one tick between the rebuild and the enable pass, which is exactly the window
// an operator's press lands in.
void ZoomTalkbackPanel::rebuild_key_buttons(
    const std::vector<TalkbackDockKeyButton> &buttons)
{
    if (!m_key_row)
        return;
    auto *layout = qobject_cast<QGridLayout *>(m_key_row->layout());
    if (!layout)
        return;

    for (auto *button : m_key_buttons) {
        if (!button) continue;
        layout->removeWidget(button);
        button->deleteLater();
    }
    m_key_buttons.clear();

    int column = 0, row = 0;
    for (const auto &spec : buttons) {
        auto *button = new QPushButton(QString::fromStdString(spec.label),
                                       m_key_row);
        button->setProperty("cvTalkbackTarget",
                            QString::fromStdString(spec.target));
        // role="key" is what makes these the biggest controls on the panel;
        // all-talent additionally carries the accent, because it is the target
        // a director reaches for when something has gone wrong and is the one
        // that keeps working when the channel budget has not covered everybody.
        button->setProperty("role", "key");
        if (spec.all_talent)
            button->setProperty("kind", "all");
        button->setEnabled(spec.enabled);
        button->setToolTip(spec.enabled
            ? QString("Talk to %1.").arg(QString::fromStdString(spec.label))
            : QString::fromStdString(spec.reason));
        // The target is captured by value: the button outlives any particular
        // plan, and a captured pointer into one would not.
        const std::string target = spec.target;
        connect(button, &QPushButton::pressed, this, [this, target]() {
            key_pressed(target, m_latch_cb && m_latch_cb->isChecked());
        });
        connect(button, &QPushButton::released, this, [this, target]() {
            key_released(target);
        });
        layout->addWidget(button, row, column);
        m_key_buttons.push_back(button);
        if (++column == 2) { column = 0; ++row; }
    }
    // No setStyleSheet() here: this dock's own sheet already applies to
    // descendants created later, and the role properties are set above before
    // the button is ever polished.
}

void ZoomTalkbackPanel::on_nominate_clicked()
{
    if (!m_nominee_list)
        return;

    std::vector<std::string> nominees;
    for (int i = 0; i < m_nominee_list->count(); ++i) {
        auto *item = m_nominee_list->item(i);
        // Qt::UserRole, not text(): a row for someone who has left says "(not
        // in the meeting)" in its label, and the engine must be sent the name.
        if (item && item->checkState() == Qt::Checked)
            nominees.push_back(item->data(Qt::UserRole).toString().toStdString());
    }

    // Refused here rather than left to the engine, because the engine's own
    // refusal reason ("target_name_collision") does not say WHO. A participant
    // whose display name is "all" (any casing) collides with the all-talent
    // target, and keying that name would go to the whole panel -- the exact
    // privacy promise talkback is built on. See talkback-plan.h.
    const std::string collision = talkback_nominate_sentinel_collision(nominees);
    if (!collision.empty()) {
        m_notice_text = QString(
            "%1 cannot be nominated: that name is how CoreVideo addresses the "
            "whole panel. Ask them to change their Zoom display name.")
            .arg(QString::fromStdString(collision));
        refresh();
        return;
    }

    auto &engine = ZoomEngineClient::instance();
    if (!engine.is_running()) {
        m_notice_text =
            QStringLiteral("The Zoom engine is not running. Start it before "
                           "nominating talkback talent.");
        refresh();
        return;
    }
    if (engine.state() != MeetingState::InMeeting) {
        m_notice_text =
            QStringLiteral("Join the meeting before nominating talkback talent.");
        refresh();
        return;
    }

    m_notice_text.clear();
    blog(LOG_INFO,
         "[obs-zoom-plugin] talkback dock: nominating %d name(s)",
         static_cast<int>(nominees.size()));
    // Fire-and-acknowledge: the plan outcome arrives asynchronously and is
    // polled out of talkback_nomination_status() by refresh() above.
    engine.talkback_nominate(nominees);
    refresh();
}

void ZoomTalkbackPanel::key_pressed(const std::string &target, bool latch)
{
    // THREAD NOTE, and the reason it is written here rather than assumed: this
    // handler runs on the Qt main thread -- Qt delivers QPushButton::pressed
    // from the widget's own thread -- which is the SAME thread
    // TalkbackController's QTimer drives evaluate() and key_off() on. So
    // calling key_on()/key_off() directly, with no dispatch, is correct here,
    // exactly as it is for ZoomControlServer's socket handlers.
    //
    // A NON-Qt-thread keying surface must NOT copy this call shape without
    // first confirming it lands on that same thread: an OBS hotkey callback is
    // delivered by libobs, not by Qt, and key_off()'s two-phase close (flip the
    // flag under m_mtx, then tear the tap down outside it) is only safe against
    // a concurrent evaluate() because of that thread identity.
    // Fix round 1 (M1, Major): whether this press CLOSES the open key is
    // decided from the key itself -- the mode captured when it was opened --
    // not from the Latch checkbox as it stands now. Unchecking Latch while a
    // latched key is live used to skip this branch, fall through to key_on()'s
    // "A talkback key is already open" refusal, and leave the director live to
    // talent with the dock's only close affordance answering with an error.
    // `latch` below therefore decides only what a NEW key opens as.
    if (talkback_dock_press_action(dock_open_key(), target, latch) ==
        TalkbackDockPressAction::CloseHeldKey) {
        TalkbackController::instance().key_off();
        m_dock_target.clear();
        m_dock_latched = false;
        refresh();
        return;
    }

    const QString source = m_source_combo
        ? m_source_combo->currentData().toString()
        : QString();
    if (source.isEmpty()) {
        m_notice_text = QStringLiteral(
            "Choose the OBS audio source you talk through before keying.");
        refresh();
        return;
    }

    std::string error;
    // needs_renewal = false. src/talkback-key.h's rule is that a surface whose
    // release is in-process and reliable does not need the lost-release
    // backstop, and names the OBS hotkey as the example; a dock button's
    // release is the same shape -- a Qt signal on the thread the controller
    // itself runs on, with no transport in between that could drop it. Passing
    // true instead would demand a heartbeat from this dock's UI-thread timer to
    // keep a key alive, which would close a genuinely held key whenever the OBS
    // UI stalls for a second (a scene collection load, a modal dialog).
    //
    // A release can still go missing in-process -- QAbstractButton drops its
    // pressed state without emitting released() on an EnabledChange, on a
    // non-popup focus loss, and anywhere setDown(false) is reached -- so
    // talkback_dock_release_lost() runs from the refresh tick and asks the
    // widget whether it is still down, never why it stopped being down. It
    // covers all of those causes and any Qt adds later, and reading the widget
    // (rather than a deadline) is what makes it unable to false-close during a
    // UI stall. Do not delete it on the belief that some particular cause has
    // been ruled out.
    const bool ok = TalkbackController::instance().key_on(
        target, source.toStdString(),
        latch ? TalkbackKeyMode::Latch : TalkbackKeyMode::PushToTalk,
        /*needs_renewal=*/false, error);
    if (!ok) {
        m_notice_text = QString::fromStdString(error);
        refresh();
        return;
    }
    m_notice_text.clear();
    m_dock_target = target;
    m_dock_latched = latch;
    refresh();
}

void ZoomTalkbackPanel::key_released(const std::string &target)
{
    // The same record the press path reads (M1): a latch releases nothing (the
    // next press on its own target closes it), a stray release for a target
    // this dock is not holding closes nothing, and a key another surface owns
    // is not this dock's to close.
    if (!talkback_dock_release_closes(dock_open_key(), target))
        return;
    TalkbackController::instance().key_off();
    m_dock_target.clear();
    m_dock_latched = false;
    refresh();
}
