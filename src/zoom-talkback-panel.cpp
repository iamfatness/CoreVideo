#include "zoom-talkback-panel.h"
#include "cv-combo-utils.h"
#include "cv-style.h"
#include "talkback-cell-grid.h"
#include "talkback-controller.h"
#include "talkback-dock-state.h"
#include "talkback-key.h"
#include "talkback-plan.h"
#include "zoom-engine-client.h"
#include "zoom-settings.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStyle>
#include <QTimer>
#include <QVariant>
#include <QVBoxLayout>

#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

// ── One spacing scale ───────────────────────────────────────────────────────
//
// The first render used 8 / 6 / 4 / 8 across four adjacent blocks, which is
// what "the UI doesn't feel polished" looks like before anyone can name it.
// These three numbers are the only ones this file is allowed to space with,
// and they match the sibling docks' own outer margin (zoom-dock.cpp and
// zoom-iso-panel.cpp both open with 8 px and space at 6-8). kGroupPad is gone
// with the group boxes it padded: the intercom grid IS the panel, and wrapping
// it in a captioned frame only spent height saying so.
static constexpr int kDockMargin  = 10;  // panel edge to content
static constexpr int kSectionGap  = 14;  // between the panel's own sections
static constexpr int kInnerGap    = 8;   // between controls in one section

// The gap between cells in the grid, kept equal to kInnerGap and named
// separately because talkback_dock_cell_columns() is given it as an argument.
static constexpr int kCellGap = kInnerGap;

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

// The cell's state as a stylesheet property value. One place maps the enum to
// a string, so a new state cannot reach the sheet under two spellings.
static const char *cell_state_name(TalkbackDockCellState state)
{
    switch (state) {
    case TalkbackDockCellState::OnAir:        return "onair";
    case TalkbackDockCellState::NoChannel:    return "nochannel";
    case TalkbackDockCellState::Unreachable:  return "unreachable";
    case TalkbackDockCellState::NotInChannel: return "notinchannel";
    case TalkbackDockCellState::Ready:        break;
    }
    return "ready";
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

    // EVERYTHING IS INSIDE A SCROLL AREA, and that is a fix, not a
    // convenience. An OBS dock is whatever height the operator dragged it to,
    // and a QWidget in one that wants more room than it has does not get
    // clipped politely -- Qt shrinks whatever is shrinkable. In the owner's
    // first look at this dock that landed on the two things that could give:
    // the talent list collapsed to its 3-row minimum (showing about two and a
    // half rows for five people, with a scrollbar) and the bottom row of key
    // buttons was squeezed flat against the group box that then held them.
    // Both were the same defect. With the content in a scroll area the
    // sections keep the size they ask for and the DOCK scrolls, which is also
    // how a 24-person grid stays usable in a short dock.
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    outer->addWidget(scroll);
    // Kept, because its VIEWPORT is the only honest answer to "how wide is the
    // dock" -- see layout_cells(), where reading the grid container's own
    // width instead was half of the 2026-08-29 clipping defect.
    m_scroll = scroll;

    // Transparent, so the dock keeps whatever ground the OBS theme gives it
    // rather than gaining a panel-coloured rectangle where the scroll area is.
    scroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; }"));
    scroll->viewport()->setAutoFillBackground(false);

    auto *body = new QWidget(scroll);
    body->setAutoFillBackground(false);
    scroll->setWidget(body);

    // ONE SPACING SCALE, applied everywhere below (kDockMargin outside,
    // kSectionGap between sections, kInnerGap inside one). The first render's
    // rhythm was ad hoc -- 8/6/4/8 in four adjacent blocks -- which is what
    // "not polished" looks like before you can name it.
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(kDockMargin, kDockMargin, kDockMargin,
                               kDockMargin);
    layout->setSpacing(kSectionGap);

    // ── 1. ON AIR banner ────────────────────────────────────────────────────
    //
    // First, full width, and louder than anything else on the panel. That is
    // the point of it: "am I audible to talent right now" is the only fact
    // here an operator has to be able to read without walking to the machine,
    // and in the first version it was a line of small red text under the
    // buttons. What it may say is decided by talkback_dock_banner(); this is
    // only where it is painted.
    //
    // The padding is deliberately modest: the strip earns its height from the
    // type size when it is LIVE (cv-style.h grows the line to 19px/800 for
    // that state alone), not from a permanent block of empty space that the
    // Off-air state -- which is what it shows for all but a few seconds of a
    // show -- has no use for.
    m_banner = new QFrame(body);
    m_banner->setObjectName(QStringLiteral("talkbackBanner"));
    m_banner->setProperty("state", QStringLiteral("off"));
    auto *banner_layout = new QVBoxLayout(m_banner);
    banner_layout->setContentsMargins(12, 7, 12, 7);
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

    // ── 2. The grid, or the talent editor ──────────────────────────────────
    //
    // ONE OF THE TWO, NEVER BOTH. The whole redesign is here: the panel used
    // to show the same people as key buttons AND as tick boxes, so seven
    // talent meant fourteen rows of person and a 24-person show was absurd.
    // The editor now takes the grid's place while it is open
    // (apply_edit_mode()), so there is exactly one list of people on screen.
    m_grid_area = new QWidget(body);
    auto *grid_area_layout = new QVBoxLayout(m_grid_area);
    grid_area_layout->setContentsMargins(0, 0, 0, 0);
    grid_area_layout->setSpacing(kInnerGap);

    m_cell_row = new QWidget(m_grid_area);
    auto *cell_grid = new QGridLayout(m_cell_row);
    cell_grid->setContentsMargins(0, 0, 0, 0);
    cell_grid->setSpacing(kCellGap);
    grid_area_layout->addWidget(m_cell_row);
    layout->addWidget(m_grid_area);

    // ── The talent editor ──────────────────────────────────────────────────
    //
    // Setup, not showtime. The hint, the button and the report say
    // "channels", never "nominate": the wire command is still
    // talkback_nominate and the code still calls it that, but the owner's
    // verdict on the operator-facing word was that it is "not a word that
    // really makes sense here". What the operator is actually doing is
    // standing up a channel per person, in advance, so the key press has
    // nothing left to do but open the microphone.
    m_edit_area = new QWidget(body);
    m_edit_area->setVisible(false);
    auto *edit_layout = new QVBoxLayout(m_edit_area);
    edit_layout->setContentsMargins(0, 0, 0, 0);
    edit_layout->setSpacing(kInnerGap);

    auto *nominate_hint = new QLabel(
        "Tick who you'll talk to. Each gets a standing channel.", m_edit_area);
    nominate_hint->setWordWrap(true);
    nominate_hint->setProperty("role", "muted");
    // The long version of the hint. Body copy under a caption is read once
    // and then skipped forever, so the panel gets ONE line and the paragraph
    // that explains WHY the channel exists before the press stays one hover
    // away -- the same trade the track warning makes with its short_text.
    nominate_hint->setToolTip(
        "Everyone ticked gets a standing Zoom channel, created now so a key "
        "press only has to open the microphone. Zoom allows 16 channels and 10 "
        "people per channel; anyone the budget cannot cover is named below.");
    edit_layout->addWidget(nominate_hint);

    m_nominee_list = new QListWidget(m_edit_area);
    m_nominee_list->setObjectName(QStringLiteral("talkbackNominees"));
    m_nominee_list->setSelectionMode(QAbstractItemView::NoSelection);
    m_nominee_list->setFocusPolicy(Qt::NoFocus);
    m_nominee_list->setToolTip(
        "Tick the people you may need to talk to. Zoom allows 16 channels and "
        "10 people per channel; anyone the budget cannot cover is named below.");
    // Height is set from the REAL row height once there are rows to measure --
    // see size_nominee_list(). Guessing it from fontMetrics() is what showed
    // two and a half rows for five people.
    size_nominee_list();
    edit_layout->addWidget(m_nominee_list);

    m_nominate_btn = new QPushButton(QStringLiteral("Assign channels"),
                                     m_edit_area);
    m_nominate_btn->setEnabled(false);
    // Full width, matching the Zoom Control dock's own section actions (Join,
    // Start Engine, Open Output Manager).
    edit_layout->addWidget(m_nominate_btn);
    connect(m_nominate_btn, &QPushButton::clicked,
            this, [this]() { on_nominate_clicked(); });

    // The budget outcome, in the operator's own words and with every shortfall
    // NAMED. This block is the whole reason the nomination reporting chain
    // exists (src/talkback-plan.h): a count tells the operator that somebody
    // is short, not who -- and who is the only part they can act on. Long name
    // lists are elided here and complete in the tooltip.
    //
    // TWO labels, not one: the headline is the answer ("6 channels in use of
    // 16 for 5 people") and the lines under it are the supporting detail. Set
    // as one muted 11px block they read as a footnote and the answer was lost
    // inside it.
    m_plan_label = new QLabel(m_edit_area);
    m_plan_label->setObjectName(QStringLiteral("talkbackPlan"));
    m_plan_label->setWordWrap(true);
    edit_layout->addWidget(m_plan_label);

    m_plan_detail = new QLabel(m_edit_area);
    m_plan_detail->setObjectName(QStringLiteral("talkbackPlanDetail"));
    m_plan_detail->setWordWrap(true);
    m_plan_detail->setVisible(false);
    edit_layout->addWidget(m_plan_detail);

    layout->addWidget(m_edit_area);

    // ── 3. The bottom strip ────────────────────────────────────────────────
    //
    // Everything that is not a person, in ONE compact row: the editor toggle,
    // the latch mode, and the microphone the director talks through. These
    // were three separate stacked sections before, each inside its own group
    // box, and between them they pushed the grid -- the only part of this
    // panel anybody looks at mid-show -- down a screen.
    auto *strip = new QHBoxLayout;
    strip->setSpacing(kInnerGap);

    m_edit_btn = new QPushButton(QStringLiteral("Edit talent"), body);
    m_edit_btn->setCheckable(true);
    m_edit_btn->setProperty("role", "strip");
    m_edit_btn->setToolTip(
        "Choose who gets a standing talkback channel. The grid is replaced "
        "while this is open, and it closes itself if a key goes live.");
    strip->addWidget(m_edit_btn);
    connect(m_edit_btn, &QPushButton::toggled, this, [this](bool on) {
        m_edit_requested = on;
        apply_edit_mode();
    });

    m_latch_cb = new QCheckBox(QStringLiteral("Latch"), body);
    m_latch_cb->setChecked(initial_settings.talkback_latch);
    m_latch_cb->setToolTip(
        "Off: hold a cell to talk. On: one press opens the key, the next "
        "closes it. A latch never survives a reconnect.");
    strip->addWidget(m_latch_cb);
    connect(m_latch_cb, &QCheckBox::toggled, this, [this](bool on) {
        if (m_shutting_down)
            return;
        auto s = ZoomPluginSettings::load();
        s.talkback_latch = on;
        s.save();
    });

    m_source_combo = new QComboBox(body);
    // Deliberately small: a QComboBox elides its own text, the full source
    // name is in its tooltip, and at the 320 px dock minimum this row has to
    // fit three controls. WHICH microphone is setup detail; the STATE of it
    // ("On air via track 1") is the line underneath, and that is the part
    // that matters mid-show.
    m_source_combo->setMinimumWidth(80);
    m_source_combo->setSizeAdjustPolicy(
        QComboBox::AdjustToMinimumContentsLengthWithIcon);
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
    strip->addWidget(m_source_combo, 1);
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
    layout->addLayout(strip);

    // ── 4. One line about the talk source ──────────────────────────────────
    //
    // The deferred half of the program/ISO leak guarantee. The structural half
    // holds unconditionally (a capture callback observes a source and cannot
    // add it to any mix -- tests/talkback-isolation-test.cpp) and the tap
    // already logs this at open() time (src/talkback-tap.cpp); this is the
    // same advisory where the operator is looking, and BEFORE the key rather
    // than on it. ONE line: the paragraph that explains it is its tooltip.
    m_track_label = new QLabel(body);
    m_track_label->setObjectName(QStringLiteral("talkbackTrackWarning"));
    m_track_label->setWordWrap(true);
    layout->addWidget(m_track_label);

    // The dock's OWN refusals -- the ones the engine never sees (no source
    // chosen, the source is not in the current scene, OBS is at a sample rate
    // the Zoom talkback API will not take). The banner shows the engine's.
    m_notice = new QLabel(body);
    m_notice->setObjectName(QStringLiteral("errorLabel"));
    m_notice->setWordWrap(true);
    m_notice->setVisible(false);
    layout->addWidget(m_notice);

    // ── 5. Probe (Milestone 1 diagnostic) ───────────────────────────────────
    //
    // Kept, kept reachable, and kept quiet. This is the "can this account even
    // open a channel" probe: it destroys its channel afterwards and plays an
    // audible tone at the participant. It is a diagnostic for when talkback
    // does not work at all, not a way to use it -- so it is collapsed by
    // default and lives at the very bottom.
    m_probe_toggle = new QPushButton(body);
    m_probe_toggle->setCheckable(true);
    m_probe_toggle->setProperty("role", "quiet");
    layout->addWidget(m_probe_toggle);

    m_probe_body = new QWidget(body);
    auto *probe_layout = new QVBoxLayout(m_probe_body);
    probe_layout->setContentsMargins(0, 0, 0, 0);
    probe_layout->setSpacing(kInnerGap);
    // One place decides what "folded" and "unfolded" look like, used both to
    // restore the saved state and to apply a fresh toggle, so the two cannot
    // disagree. It is applied directly rather than through the signal below
    // because seeding must not write the setting back during construction.
    const auto apply_probe_disclosure = [this](bool on) {
        if (m_probe_body)
            m_probe_body->setVisible(on);
        if (m_probe_toggle)
            m_probe_toggle->setText(
                on ? QStringLiteral("Diagnostic: talkback probe (hide)")
                   : QStringLiteral("Diagnostic: talkback probe (show)"));
        // Unfolding should populate it now, not up to a second from now.
        m_probe_poll_ms = 0;
    };
    m_probe_toggle->setChecked(initial_settings.talkback_probe_expanded);
    apply_probe_disclosure(initial_settings.talkback_probe_expanded);
    // Persisted, because folding this away does not merely hide it: while it is
    // folded refresh_probe() skips the roster poll entirely, so this setting is
    // what decides whether that work happens at all after a restart.
    connect(m_probe_toggle, &QPushButton::toggled, this,
            [this, apply_probe_disclosure](bool on) {
        apply_probe_disclosure(on);
        if (m_shutting_down)
            return;
        auto s = ZoomPluginSettings::load();
        s.talkback_probe_expanded = on;
        s.save();
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
        // The stage line under this button is about to start moving; do not
        // make the operator wait out the poll gate to see the first one.
        m_probe_poll_ms = 0;
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

// The Milestone 1 probe's own polled readouts.
//
// GATED THE SAME WAY THE SOURCE SCAN IS, and for the same reason (m2). This
// copies the whole roster out of ZoomEngineClient under its mutex and rebuilds
// a combo from it, which is the second-heaviest repeated work on this panel --
// and the panel is created at FINISHED_LOADING whether or not the operator ever
// opens the dock, with the probe section folded by default. Left ungated it was
// 10Hz of roster copying for a widget nobody can see. So: only while this dock
// is visible AND the probe is unfolded, and then only once a second. Nothing
// here needs tick resolution -- the key state that does stays on the 100ms tick
// in refresh().
void ZoomTalkbackPanel::refresh_probe()
{
    if (!isVisible() || !m_probe_body || !m_probe_body->isVisible())
        return;
    const uint64_t now_ms_tick = os_gettime_ns() / 1000000ULL;
    if (m_probe_poll_ms != 0 &&
        talkback_elapsed_ms(now_ms_tick, m_probe_poll_ms) < 1000)
        return;
    m_probe_poll_ms = now_ms_tick;

    const bool in_meeting =
        ZoomEngineClient::instance().state() == MeetingState::InMeeting;

    // Rebuilt from the live roster -- see the comment where this combo is
    // constructed for why it is not the Zoom Control dock's legacy participant
    // list.
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
        // this timer-driven read picks it up on the next poll, matching how
        // every other readout in this plugin's docks (last_error(), roster(),
        // active_speaker_id()) already reaches the UI. Only ever the LATEST
        // line, at whatever rate this polls -- so a stage superseded inside the
        // same second is not shown. That was already true of the 100ms version
        // for anything faster than a tick, and the stage that matters (the
        // terminal one) stays in the field until the next run.
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

    // -- Talent list -----------------------------------------------------------
    // Identity is by display name, never by Zoom user id (ids are
    // meeting-scoped: one captured now points at nobody after a rejoin and at
    // the wrong face once ids get recycled). Each row carries its name in
    // Qt::UserRole, because the row's TEXT can also say "(not in the meeting)".
    //
    // WHICH ROWS, IN WHICH ORDER, AND WHEN THIS WIDGET IS THROWN AWAY are all
    // decided by talkback_nominee_list_refresh() (src/talkback-dock-state.h),
    // where the live "moving from room to room the list doesn't update" defect
    // and its ordering rule are written up. This function only paints the
    // answer. The one thing that MUST stay here is that the operator's ticks
    // are read out of the widget on every tick: the widget is where they live,
    // and any state on this side is a mirror of it, never the source.
    const auto roster = engine.roster();
    std::vector<std::string> roster_names;
    roster_names.reserve(roster.size());
    for (const auto &p : roster) {
        // Someone with no display name cannot be addressed at all, and is left
        // out rather than listed as an id that would not resolve.
        if (p.display_name.empty()) continue;
        roster_names.push_back(p.display_name);
    }

    std::vector<std::string> checked_names;
    if (m_nominee_list) {
        for (int i = 0; i < m_nominee_list->count(); ++i) {
            auto *item = m_nominee_list->item(i);
            if (item && item->checkState() == Qt::Checked)
                checked_names.push_back(
                    item->data(Qt::UserRole).toString().toStdString());
        }
    }

    if (m_nominee_list &&
        talkback_nominee_list_refresh(m_nominee_state, roster_names,
                                      checked_names)) {
        m_nominee_list->clear();
        for (const auto &row : m_nominee_state.rows) {
            const QString name = QString::fromStdString(row.name);
            auto *item = new QListWidgetItem(
                row.present ? name : name + " (not in the meeting)");
            item->setData(Qt::UserRole, name);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(row.checked ? Qt::Checked : Qt::Unchecked);
            m_nominee_list->addItem(item);
        }
        // The row count just moved, so the height the list should stand at
        // moved with it. Cheap and self-no-opping; five people get five whole
        // rows, not the three-row minimum a squeezed layout used to force.
        size_nominee_list();
    }

    const int checked_count = static_cast<int>(checked_names.size());
    if (m_nominate_btn) {
        // Both conditions, not just InMeeting: talkback_nominate() is a silent
        // no-op when the engine pipe is not up, which is why the control API
        // acks "engine_not_running" separately from "not_in_meeting".
        m_nominate_btn->setEnabled(engine_running && in_meeting);
        // An empty nomination is a deliberate denominate (the engine's
        // nominate() documents it as such), not a mistake to block -- but it
        // must not be labelled as setting channels up.
        const QString label = checked_count == 0
            ? QStringLiteral("Clear all channels")
            : QString("Assign channels (%1)").arg(checked_count);
        if (m_nominate_btn->text() != label)
            m_nominate_btn->setText(label);
    }

    // -- The confirmed plan, and what it cost ----------------------------------
    const auto plan = engine.talkback_nomination_status();
    const auto report = talkback_dock_nomination_report(plan);
    // The ANSWER on its own line, at normal weight; the names that support it
    // underneath, secondary. One muted 11px block for both is what buried the
    // headline in the first render.
    const QString plan_headline = QString::fromStdString(report.headline);
    QString plan_detail;
    for (const auto &line : report.lines) {
        if (!plan_detail.isEmpty()) plan_detail += "\n";
        plan_detail += QString::fromStdString(line);
    }
    // The un-elided lists. talkback_dock_nomination_report() elides a name run
    // past five so the block stays scannable; this is where the rest of the
    // names go, so the elision never costs the operator a name. It hangs off
    // BOTH labels, because either one is what the pointer will be over.
    const QString full = QString::fromStdString(report.tooltip);
    if (m_plan_label) {
        if (m_plan_label->text() != plan_headline)
            m_plan_label->setText(plan_headline);
        if (m_plan_label->toolTip() != full)
            m_plan_label->setToolTip(full);
    }
    if (m_plan_detail) {
        if (m_plan_detail->text() != plan_detail)
            m_plan_detail->setText(plan_detail);
        if (m_plan_detail->toolTip() != full)
            m_plan_detail->setToolTip(full);
        m_plan_detail->setVisible(!plan_detail.isEmpty());
    }
    set_style_flag(m_plan_label, "warn", report.warn);
    set_style_flag(m_plan_detail, "warn", report.warn);

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
        for (const auto &cell : m_cells) {
            if (!cell.button) continue;
            if (cell.target == dock_key.target) {
                button_down = cell.button->isDown();
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
        // Recorded for apply_edit_mode() below, which is the only thing that
        // wants "is anything keyed" without caring whose key it is.
        m_any_key_open = key_open || !m_dock_target.empty();
    }

    // -- What the banner will say ----------------------------------------------
    // Decided here, above the grid, because the cells need its verdict too: the
    // one cell allowed to paint itself red is the one holding a key the banner
    // is calling ON AIR, and deriving that twice is how the strip and the
    // control under it would come to disagree.
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

    // -- The grid --------------------------------------------------------------
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

    // Editing is re-decided on every tick, not only when the toggle is
    // clicked: a key opened from Companion or the control API while the
    // editor is open has to put the grid back, and talkback_dock_edit_mode()
    // is the one place that rule lives. Keying wins over editing, always.
    apply_edit_mode();

    // RED MEANS THE DIRECTOR IS AUDIBLE, and only that. Decided once, here,
    // from both halves: the banner has to be calling this ON AIR (which
    // requires the ENGINE's confirmation, never the plugin's intent -- the C2
    // rule) and THIS DOCK has to be the surface holding it. Dropping the
    // ownership half would paint our own disabled cell live whenever
    // Companion or the control API keyed the same target, which is a red
    // control that cannot be pressed and does not belong to the person
    // looking at it. Where that key actually is, is already said in the
    // cell's refusal reason.
    const std::string live_target =
        (banner.state == TalkbackDockBannerState::Live && ctx.open.dock_owned)
            ? ctx.open.target
            : std::string();

    const auto cells = talkback_dock_cells(
        plan, ctx, engine.talkback_channel_presence(), live_target);

    std::string target_signature;
    for (const auto &c : cells) {
        target_signature += c.target;
        target_signature += '\n';
    }
    // Rebuild only when the SET OF TARGETS changes, and never while this dock
    // has a key open: deleting the widget the operator is holding loses its
    // release. Deferring costs little -- the engine refuses a re-nomination
    // outright while a session is live ("reason":"session_live", nominate() in
    // engine-talkback.cpp), so the confirmed plan seldom moves mid-press at
    // all, and when it does (a superseded ladder's terminal invalidating it)
    // the cells are rebuilt on the first tick after the key closes.
    if (target_signature != m_key_signature && m_dock_target.empty()) {
        rebuild_cells(cells);
        m_key_signature = target_signature;
    }

    for (const auto &c : cells) {
        for (auto &cell : m_cells) {
            if (!cell.button || cell.target != c.target)
                continue;
            // The cell's state drives its paint, on the button and on both of
            // its labels -- a QLabel inside a restyled QPushButton is not
            // repolished by the button's own repolish, and they carry the
            // cell's text colours. set_style_state() self-no-ops, so these
            // run unguarded ten times a second.
            //
            // Safe to repolish a cell the operator is HOLDING: the way a
            // QPushButton silently loses `down` is QAbstractButton::
            // changeEvent()'s EnabledChange arm, and a dynamic-property
            // repolish delivers StyleChange/PaletteChange/FontChange, none of
            // which that switch acts on. setEnabled() below is the call that
            // has to be guarded, and it is.
            const char *const cell_state = cell_state_name(c.state);
            set_style_state(cell.button, "cell", cell_state);
            set_style_state(cell.name, "cell", cell_state);
            set_style_state(cell.state, "cell", cell_state);
            // Through the cell, never straight at the label: the cell keeps
            // the full string and re-elides it against the room the label
            // actually has, and a setText() here would put an unelided line
            // back on screen until the next resize.
            cell.button->set_state_line(QString::fromStdString(c.state_line));

            // The tooltip is written BEFORE the never-disable guard below, so
            // a cell whose reason changed underneath a held key still says the
            // true thing. Writing a tooltip cannot clear a button's down
            // state; only an EnabledChange can, which is what the guard is
            // for. The full name is always in here, so the elision
            // layout_cells() applies never costs the operator a name.
            QString tip = QString::fromStdString(c.label);
            if (!c.enabled && !c.reason.empty())
                tip += "\n" + QString::fromStdString(c.reason);
            else if (c.enabled)
                tip = QString("Talk to %1.").arg(QString::fromStdString(c.label));
            if (!c.hint.empty())
                tip += "\n" + QString::fromStdString(c.hint);
            if (cell.button->toolTip() != tip)
                cell.button->setToolTip(tip);

            // NEVER disable a held cell. QAbstractButton clears its pressed
            // state on an EnabledChange without emitting released(), so
            // disabling one mid-press strands the key open until the backstop
            // above notices. talkback_dock_key_buttons() already keeps the
            // open key's own target enabled; this is the second line of
            // defence for any future caller that forgets.
            if (!c.enabled && cell.button->isDown())
                break;
            cell.button->setEnabled(c.enabled);
            // A disabled QPushButton greys itself, but its child labels carry
            // their own stylesheet colours and would stay bright inside it.
            // Flagged rather than left to a descendant selector, because Qt
            // resolves ":disabled QLabel#id" by specificity and source order
            // and the ID rule would win.
            set_style_flag(cell.name, "off", !c.enabled);
            set_style_flag(cell.state, "off", !c.enabled);
            break;
        }
    }

    // Column count and elision, re-decided on the tick as well as on resize.
    // Both depend on the dock's CURRENT width, and the width a cell is asked
    // about right after a rebuild is the one the layout has not run yet -- so
    // without this a first paint could keep a two-column grid at a width that
    // fits three. Self-no-opping: it re-grids only when the column count moves
    // and writes a label only when the string changes.
    layout_cells();

    // -- ON AIR banner ---------------------------------------------------------
    // Decided above the key buttons (see there); this is only the painting.
    //
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

// Takes the cell specs its caller already computed rather than recomputing
// them from a second, thinner context: the two would otherwise disagree for the
// one tick between the rebuild and the enable pass, which is exactly the window
// an operator's press lands in.
void ZoomTalkbackPanel::rebuild_cells(const std::vector<TalkbackDockCell> &cells)
{
    if (!m_cell_row)
        return;
    auto *layout = qobject_cast<QGridLayout *>(m_cell_row->layout());
    if (!layout)
        return;

    for (auto &cell : m_cells) {
        if (!cell.button) continue;
        // hide() as well as removeWidget(): taking a widget out of a layout
        // does not unmap it, so between here and the deferred delete an old
        // cell would keep painting at its last geometry -- on top of the fresh
        // grid, whose column count may have moved, so the stale and new
        // positions no longer coincide and the overlap would show.
        layout->removeWidget(cell.button);
        cell.button->hide();
        cell.button->deleteLater();
    }
    m_cells.clear();
    m_cell_columns = 0;

    for (const auto &spec : cells) {
        KeyCell cell;
        cell.target     = spec.target;
        cell.label      = QString::fromStdString(spec.label);
        cell.all_talent = spec.all_talent;

        // TWO LABELS INSIDE THE BUTTON, not a two-line button text: the name
        // and the state line carry different type, and QPushButton draws one
        // font. Both are transparent for mouse events, so every press and
        // release still lands on the button itself -- the keying machinery
        // (press/release, the backstop's isDown(), the never-disable guard)
        // is looking at exactly the widget it always was. All of that, plus
        // the reactive elision and the layout-derived height, is
        // TalkbackCellButton (src/talkback-cell-grid.h).
        cell.button = new TalkbackCellButton(m_cell_row);
        cell.button->setEnabled(spec.enabled);
        cell.button->set_all_talent(spec.all_talent);
        cell.button->set_name(cell.label);
        cell.button->set_state_line(QString::fromStdString(spec.state_line));
        cell.name  = cell.button->name_label();
        cell.state = cell.button->state_label();

        const char *const cell_state = cell_state_name(spec.state);
        cell.button->setProperty("cell", QString::fromLatin1(cell_state));
        cell.name->setProperty("cell", QString::fromLatin1(cell_state));
        cell.state->setProperty("cell", QString::fromLatin1(cell_state));
        cell.name->setProperty("off", !spec.enabled);
        cell.state->setProperty("off", !spec.enabled);
        // kind="all" goes on the labels as well as the button: all-talent
        // sits on a filled accent and the neutral cell's muted grey vanishes
        // on it. See the sheet for why this is a property and not a
        // descendant selector.
        if (spec.all_talent) {
            cell.button->setProperty("kind", "all");
            cell.name->setProperty("kind", "all");
            cell.state->setProperty("kind", "all");
        }

        QString tip = cell.label;
        if (spec.enabled)
            tip = QString("Talk to %1.").arg(cell.label);
        else if (!spec.reason.empty())
            tip += "\n" + QString::fromStdString(spec.reason);
        if (!spec.hint.empty())
            tip += "\n" + QString::fromStdString(spec.hint);
        cell.button->setToolTip(tip);

        // The target is captured by value: the cell outlives any particular
        // plan, and a captured pointer into one would not.
        const std::string target = spec.target;
        connect(cell.button, &QPushButton::pressed, this, [this, target]() {
            key_pressed(target, m_latch_cb && m_latch_cb->isChecked());
        });
        connect(cell.button, &QPushButton::released, this, [this, target]() {
            key_released(target);
        });
        m_cells.push_back(cell);
    }
    // Placement and the visible (possibly elided) name are one job, done in
    // one place, because a resize has to redo exactly the same work.
    layout_cells();
    // No setStyleSheet() here: this dock's own sheet already applies to
    // descendants created later, and the role/state properties are set above
    // before the cell is ever polished.
}

// THE LAYOUT THE REDESIGN IS FOR (owner, 2026-08-29: "need to rethink how this
// works and how it will look"). The old grid sized every button to the WIDEST
// label in the room, so "Ronny Hofsoy, Tromso, Norway" dropped seven people
// into one full-width column 400 px tall. A cell is sized to a MINIMUM
// READABLE WIDTH instead (talkback_dock_cell_columns(), two or three across,
// never one), and a name that does not fit that column is elided END-ONLY with
// the full name in the tooltip.
//
// End-only elision is not a style choice. A QPushButton does not elide at all;
// it CLIPS, and centred text in a too-narrow widget loses the same amount at
// each end -- which is how "Grant Whitehead" rendered as "rant Whitehead" on
// the first version. On a control that opens a live microphone to one named
// person, two names that differ only at the start reading identically is a
// wrong-person hazard, and it is the START of a name that has to survive.
//
// THE FLOW AND THE ELISION ARE NOT DECIDED HERE ANY MORE (operator's high-DPI
// screen, 2026-08-29, second look: names clipped mid-glyph with NO ellipsis,
// last row cut in half). This function used to derive a column width
// arithmetically and elide every name against that number, which was simply
// not the width the QGridLayout went on to give the cell -- the pixel budget
// and the geometry disagreed, and the geometry is what the operator sees.
// Both jobs now belong to code that measures instead of predicting:
// TalkbackCellButton elides in its own event filter from the font it actually
// has against the width it actually got, and talkback_layout_cell_grid()
// re-writes every column's stretch and hands the container the layout's own
// minimum height (src/talkback-cell-grid.cpp, where the mechanism is written
// up).
//
// What is still decided HERE is the one input neither of them can see: how
// wide the DOCK is. It is read off the scroll area's viewport, never off the
// grid container -- the container's width is downstream of the flow (Qt sizes
// a widget from the layout inside it), so feeding it back in is a loop that
// confirms whatever the grid already did, which is how a two-column grid
// stayed two columns inside a container the cells themselves had widened.
//
// This function may run while the operator is HOLDING a key. It only moves
// widgets and sets text, and neither can clear a QAbstractButton's `down`
// state -- only an EnabledChange does that, which is what refresh()'s
// never-disable guard is for. The re-parenting a column change would do is
// skipped while a key is held anyway, out of caution rather than necessity.
void ZoomTalkbackPanel::layout_cells()
{
    if (!m_cell_row || m_cells.empty())
        return;
    auto *layout = qobject_cast<QGridLayout *>(m_cell_row->layout());
    if (!layout)
        return;

    // The dock's own width, less the panel margins -- from the viewport,
    // because that is the width the operator can actually see. The panel's
    // own width is the fallback for the ticks before the scroll area has been
    // laid out.
    const int viewport_px = m_scroll && m_scroll->viewport()
        ? m_scroll->viewport()->width()
        : 0;
    const int available =
        (viewport_px > 1 ? viewport_px : width()) - 2 * kDockMargin;

    std::vector<TalkbackCellButton *> buttons;
    buttons.reserve(m_cells.size());
    for (const auto &cell : m_cells)
        if (cell.button) buttons.push_back(cell.button);

    m_cell_columns = talkback_layout_cell_grid(
        layout, buttons, available, kCellGap, m_cell_columns,
        /*freeze_columns=*/!m_dock_target.empty());
}

// The editor and the grid share one slot, and keying wins. The decision is
// talkback_dock_edit_mode()'s (src/talkback-dock-state.h); this only applies
// it, and pushes the toggle back up when the answer is no so the button never
// says "open" over a grid.
void ZoomTalkbackPanel::apply_edit_mode()
{
    // "A key is open" means ANY surface's, not only this dock's. Our own is
    // the dangerous one -- hiding a button we are holding strands it -- but a
    // key live from Companion or the control API is also the moment the
    // operator needs to be looking at the grid rather than at a checklist,
    // and nothing is lost by swapping: the ticks live in the list widget,
    // which is hidden, never destroyed.
    TalkbackDockOpenKey open = dock_open_key();
    if (!open.open && m_any_key_open) {
        open.open = true;
        open.dock_owned = false;
    }
    const bool editing = talkback_dock_edit_mode(m_edit_requested, open);
    if (m_grid_area) m_grid_area->setVisible(!editing);
    if (m_edit_area) m_edit_area->setVisible(editing);
    if (m_edit_btn) {
        if (m_edit_btn->isChecked() != editing) {
            // Reflect the answer without re-entering this function through
            // the toggled() handler.
            const QSignalBlocker block(m_edit_btn);
            m_edit_btn->setChecked(editing);
        }
        m_edit_requested = editing;
        const QString label = editing ? QStringLiteral("Done")
                                      : QStringLiteral("Edit talent");
        if (m_edit_btn->text() != label)
            m_edit_btn->setText(label);
    }
}

// The talent list is sized in WHOLE ROWS, measured from the widget rather than
// guessed from a font. The guess (fontMetrics().height() + 10) did not include
// the stylesheet's item padding or the frame, so five people rendered as about
// two and a half rows with a scrollbar. sizeHintForRow() asks the delegate,
// which does know, and a fixed height is what stops a short dock from
// squeezing the list to its minimum -- the scroll area now absorbs that.
void ZoomTalkbackPanel::size_nominee_list()
{
    if (!m_nominee_list)
        return;
    const int rows = talkback_dock_nominee_visible_rows(
        static_cast<std::size_t>(m_nominee_list->count()));
    int row_h = m_nominee_list->count() > 0
        ? m_nominee_list->sizeHintForRow(0)
        : -1;
    // Before the first row exists there is nothing to measure; the font-based
    // estimate is only ever the seed for an empty list, and the first real
    // rebuild replaces it.
    if (row_h <= 0)
        row_h = fontMetrics().height() + 12;
    if (rows == m_nominee_sized_rows && row_h == m_nominee_sized_row_h)
        return;
    m_nominee_sized_rows = rows;
    m_nominee_sized_row_h = row_h;
    m_nominee_list->setFixedHeight(rows * row_h +
                                   2 * m_nominee_list->frameWidth());
}

void ZoomTalkbackPanel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    layout_cells();
}

// A font or style change moves every measurement this panel is built out of --
// the talent list's row height, the gauge the column count is decided from,
// and what fits in a cell -- without moving the dock's geometry at all. OBS
// applies a theme after the dock is constructed, and a window dragged to a
// display with different scaling gets one of these too, so nothing here may
// be measured only once at build time.
void ZoomTalkbackPanel::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() != QEvent::FontChange &&
        event->type() != QEvent::StyleChange)
        return;
    // The row height it was sized from is now stale, and size_nominee_list()
    // no-ops against its cache unless the cache is cleared.
    m_nominee_sized_row_h = -1;
    size_nominee_list();
    layout_cells();
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
            "%1 cannot be given a channel: that name is how CoreVideo "
            "addresses the whole panel. Ask them to change their Zoom display "
            "name.")
            .arg(QString::fromStdString(collision));
        refresh();
        return;
    }

    auto &engine = ZoomEngineClient::instance();
    if (!engine.is_running()) {
        m_notice_text =
            QStringLiteral("The Zoom engine is not running. Start it before "
                           "assigning talkback channels.");
        refresh();
        return;
    }
    if (engine.state() != MeetingState::InMeeting) {
        m_notice_text =
            QStringLiteral("Join the meeting before assigning talkback "
                           "channels.");
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
