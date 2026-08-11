// src/zoom-tiles-audio.cpp
#include "zoom-tiles-audio.h"

#include <obs.h>
#include <util/base.h>

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

constexpr const char *kParticipantAudioId = "zoom_participant_audio_source";
constexpr const char *kParticipantIdKey   = "participant_id";

struct ScanCtx {
    std::vector<TilesAudioSourceState> *out;
};

// True if a source with this uuid is still in the scene collection. A marker
// naming a deleted Tiles source is what makes its audio sources orphans.
//
// Called from inside obs_enum_sources callbacks (tiles_audio_scan,
// find_marked_source below). obs_enum_sources holds obs->data.sources_mutex
// for the whole walk, and obs_get_source_by_uuid here re-locks that same
// mutex — safe only because it is a recursive mutex and this always runs on
// the enumerating thread. It holds because neither callback in this file
// creates or destroys a source while enumerating; a future callback that did
// would deadlock or corrupt the walk, so that has to stay true.
bool owner_is_live(const char *uuid)
{
    if (!uuid || !*uuid) return false;
    obs_source_t *owner = obs_get_source_by_uuid(uuid);
    if (!owner) return false;
    obs_source_release(owner);
    return true;
}

// Finds a marked source by the participant it carries, restricted to sources
// this Tiles source is allowed to touch: already owned by self_uuid, or an
// orphan (marked, but its creating Tiles source is no longer live). A source
// owned by a different LIVE Tiles source is never returned. The planner
// already refuses to plan anything for that case (zoom-tiles-audio-plan.h);
// if this lookup ignored ownership too, it could return that source anyway —
// enumeration order, not the plan, would decide which of two same-participant
// rows gets adopted or muted, and a different wall's audio could be stolen or
// silenced. Returns a new reference the caller must release, or nullptr.
obs_source_t *find_marked_source(uint32_t participant_id, const std::string &self_uuid)
{
    struct FindCtx {
        uint32_t      want;
        const char   *self_uuid;
        obs_source_t *found;
    } ctx{participant_id, self_uuid.c_str(), nullptr};

    obs_enum_sources(
        [](void *param, obs_source_t *src) -> bool {
            auto *c = static_cast<FindCtx *>(param);
            const char *id = obs_source_get_id(src);
            if (!id || std::strcmp(id, kParticipantAudioId) != 0) return true;

            obs_data_t *settings = obs_source_get_settings(src);
            if (!settings) return true;
            const char *owner =
                obs_data_get_string(settings, CV_TILES_AUDIO_OWNER_KEY);
            const bool marked = owner && *owner;
            const auto pid = static_cast<uint32_t>(
                obs_data_get_int(settings, kParticipantIdKey));

            bool accept = false;
            if (marked && pid == c->want) {
                const bool ours = std::strcmp(owner, c->self_uuid) == 0;
                // owner_is_live() must run before settings is released:
                // `owner` points into settings's own storage.
                accept = ours || !owner_is_live(owner);
            }
            obs_data_release(settings);

            if (!accept) return true;
            c->found = obs_source_get_ref(src);
            return false;  // stop enumerating
        },
        &ctx);

    return ctx.found;
}

void set_owner(obs_source_t *src, const std::string &self_uuid)
{
    obs_data_t *patch = obs_data_create();
    obs_data_set_string(patch, CV_TILES_AUDIO_OWNER_KEY, self_uuid.c_str());
    obs_source_update(src, patch);
    obs_data_release(patch);
}

// Tracks which participants already have a logged, still-unresolved name
// clash with a source this plugin does not own. Zoom ids do not get reused
// within a meeting and an unresolved clash needs the operator to act (rename
// or remove their conflicting source), so re-planning the same Create every
// apply is expected — logging it every time is not. Guarded by a mutex
// because tiles_audio_apply can be called for more than one Tiles source
// (different self_uuid, potentially different threads).
std::mutex                   g_clash_mutex;
std::unordered_set<uint32_t> g_warned_clash;

// Returns true the first time this participant is seen clashing since the
// last time it resolved.
bool should_log_name_clash(uint32_t participant_id)
{
    std::lock_guard<std::mutex> lock(g_clash_mutex);
    return g_warned_clash.insert(participant_id).second;
}

// Called whenever a participant's Create does not hit an unresolved clash
// this round, so a clash that resolves and later recurs (a different
// operator source reusing the name) is logged again rather than staying
// suppressed forever.
void clear_name_clash(uint32_t participant_id)
{
    std::lock_guard<std::mutex> lock(g_clash_mutex);
    g_warned_clash.erase(participant_id);
}

// Serialises the whole scan-and-apply pair across every Tiles source in the
// process — see tiles_audio_reconcile()'s declaration for why a per-source
// lock (engine_mutex) is not enough on its own.
//
// Non-recursive, and taken in exactly one place: tiles_audio_reconcile below.
// That is an invariant to keep, not an incidental fact. The whole
// scan-plan-apply runs inline under this lock on the UI thread, and
// tiles_audio_apply calls obs_source_create, obs_source_update and
// obs_scene_add — each of which fires libobs signals synchronously on that
// same thread. A future signal handler (source_create, item_add, a frontend
// callback) that reached apply_assignments and so tiles_audio_reconcile would
// therefore re-enter this lock on the thread already holding it, and
// std::mutex re-entered by its own owner is undefined behaviour — in practice
// a hung UI thread, mid-show, with the operator's scene collection half
// reconciled. Two call sites exist today, both in tiles_audio_reconcile's own
// body, so nothing re-enters. Anything that adds a libobs callback into this
// path has to preserve that, or defer its reconcile to a fresh queued task
// instead of running one inline.
std::mutex g_reconcile_mutex;

// Snapshots every marked source in the scene collection. Takes no arguments on
// purpose: ownership is read off each source's own marker, never inferred from
// who is asking. A marker naming a uuid with no live source is reported with an
// empty owner_uuid, which the planner reads as an adoptable orphan.
//
// Internal, along with tiles_audio_apply below: tiles_audio_reconcile is the
// only caller of either, and the pair is only correct when run together under
// g_reconcile_mutex.
std::vector<TilesAudioSourceState> tiles_audio_scan()
{
    std::vector<TilesAudioSourceState> out;
    ScanCtx ctx{&out};

    obs_enum_sources(
        [](void *param, obs_source_t *src) -> bool {
            auto *c = static_cast<ScanCtx *>(param);

            const char *id = obs_source_get_id(src);
            if (!id || std::strcmp(id, kParticipantAudioId) != 0) return true;

            obs_data_t *settings = obs_source_get_settings(src);
            if (!settings) return true;

            const char *owner =
                obs_data_get_string(settings, CV_TILES_AUDIO_OWNER_KEY);
            // No marker means the operator made this by hand. It is not ours,
            // it never becomes ours, and it is not reported to the planner.
            if (owner && *owner) {
                TilesAudioSourceState st;
                st.participant_id = static_cast<uint32_t>(
                    obs_data_get_int(settings, kParticipantIdKey));
                const char *name = obs_source_get_name(src);
                st.name   = name ? name : "";
                st.muted  = obs_source_muted(src);
                st.mixers = obs_source_get_audio_mixers(src);
                // An empty owner_uuid is the planner's signal for "adoptable".
                st.owner_uuid = owner_is_live(owner) ? owner : "";
                c->out->push_back(std::move(st));
            }

            obs_data_release(settings);
            return true;
        },
        &ctx);

    return out;
}

// Applies the plan. Create actions go into group_name, which must already
// exist — creating the group is the operator's act, and is also how they opt
// in. group_name is resolved lazily, only when a Create actually needs it:
// Adopt/Unmute/Mute/SetMixers never touch the group, so a missing or renamed
// group only skips new participants for this call — it never blocks muting
// someone who left the wall.
//
// An empty group_name is the feature being switched off (see
// tiles_audio_reconcile): every Create is skipped, and the Mute actions that
// make "off" mean off still run.
void tiles_audio_apply(const TilesAudioPlan &plan, const std::string &group_name,
                       const std::string &self_uuid)
{
    if (plan.actions.empty()) return;
    if (self_uuid.empty()) return;

    // Resolved lazily, on the first Create that actually needs it. Adopt,
    // Unmute, Mute and SetMixers never touch the group — gating them on it
    // resolving would leave a participant's audio live on air (or silent)
    // just because the operator renamed or deleted the group, which is
    // exactly the case this feature exists to avoid getting wrong.
    obs_source_t *group_src    = nullptr;
    obs_scene_t  *group        = nullptr;
    bool          group_tried  = false;
    bool          group_usable = false;

    for (const TilesAudioAction &action : plan.actions) {
        if (action.kind == TilesAudioActionKind::Create) {
            // No group named means the operator turned the feature off, and
            // off creates nothing — ever. Skipped rather than returned from,
            // because the same plan's Mute actions are the whole point of the
            // reconcile that gets here with no group.
            if (group_name.empty()) continue;
            if (!group_tried) {
                group_tried = true;
                group_src = obs_get_source_by_name(group_name.c_str());
                if (!group_src) {
                    blog(LOG_INFO,
                         "[corevideo] tiles audio: group '%s' not found; "
                         "skipping new participants this round (existing "
                         "ones are still muted/unmuted/retracked normally)",
                         group_name.c_str());
                } else {
                    group = obs_group_from_source(group_src);
                    if (!group) {
                        blog(LOG_WARNING,
                             "[corevideo] tiles audio: '%s' is not a group; "
                             "skipping new participants this round (existing "
                             "ones are still muted/unmuted/retracked "
                             "normally)",
                             group_name.c_str());
                    } else {
                        group_usable = true;
                    }
                }
            }
            if (!group_usable) continue;

            // A name already in use by something the plugin does not own is
            // deferred, never overwritten and never renamed around. The
            // operator's source keeps its name.
            //
            // A name already in use by something the plugin DOES own (its
            // marker is set) is a different case: most commonly a Zoom
            // rejoin, which mints a new participant id for the same display
            // name while our source for the old, now-stale id is still
            // sitting on it. That is our own naming collision, not the
            // operator's, so it is resolved by uniquifying rather than
            // deferred — deferring here would strand the rejoined
            // participant with no audio for the rest of the show.
            obs_source_t *clash = obs_get_source_by_name(action.name.c_str());
            std::string   create_name = action.name;

            if (clash) {
                obs_data_t *clash_settings = obs_source_get_settings(clash);
                bool clash_marked = false;
                if (clash_settings) {
                    const char *clash_owner = obs_data_get_string(
                        clash_settings, CV_TILES_AUDIO_OWNER_KEY);
                    clash_marked = clash_owner && *clash_owner;
                    obs_data_release(clash_settings);
                }
                obs_source_release(clash);

                if (!clash_marked) {
                    if (should_log_name_clash(action.participant_id)) {
                        blog(LOG_WARNING,
                             "[corevideo] tiles audio: name '%s' is already "
                             "taken by a source we do not own; skipping "
                             "participant %u rather than overwriting "
                             "(further clashes for this participant are "
                             "logged once until it resolves)",
                             action.name.c_str(), action.participant_id);
                    }
                    continue;
                }

                create_name =
                    action.name + " #" + std::to_string(action.participant_id);
                blog(LOG_INFO,
                     "[corevideo] tiles audio: name '%s' is already used by "
                     "our own source for a different participant; creating "
                     "'%s' instead",
                     action.name.c_str(), create_name.c_str());
            }

            clear_name_clash(action.participant_id);

            obs_data_t *settings = obs_data_create();
            obs_data_set_int(settings, kParticipantIdKey,
                             static_cast<long long>(action.participant_id));
            obs_data_set_string(settings, CV_TILES_AUDIO_OWNER_KEY,
                                self_uuid.c_str());
            obs_source_t *created = obs_source_create(
                kParticipantAudioId, create_name.c_str(), settings, nullptr);
            obs_data_release(settings);

            if (!created) {
                blog(LOG_WARNING,
                     "[corevideo] tiles audio: could not create a source for "
                     "participant %u",
                     action.participant_id);
                continue;
            }
            obs_source_set_audio_mixers(created, action.mixers);
            obs_source_set_muted(created, false);
            obs_sceneitem_t *item = obs_scene_add(group, created);
            if (!item)
                blog(LOG_WARNING,
                     "[corevideo] tiles audio: created '%s' but could not add "
                     "it to group '%s'",
                     create_name.c_str(), group_name.c_str());
            obs_source_release(created);
            continue;
        }

        obs_source_t *target = find_marked_source(action.participant_id, self_uuid);
        if (!target) continue;

        switch (action.kind) {
        case TilesAudioActionKind::Adopt:
            set_owner(target, self_uuid);
            break;
        case TilesAudioActionKind::Unmute:
            obs_source_set_muted(target, false);
            break;
        case TilesAudioActionKind::Mute:
            obs_source_set_muted(target, true);
            break;
        case TilesAudioActionKind::SetMixers:
            obs_source_set_audio_mixers(target, action.mixers);
            break;
        case TilesAudioActionKind::Create:
            break;  // handled above
        }
        obs_source_release(target);
    }

    if (plan.overflow > 0)
        blog(LOG_INFO,
             "[corevideo] tiles audio: %zu participant(s) past the five ISO "
             "stem tracks; they are on the program track only",
             plan.overflow);

    if (group_src) obs_source_release(group_src);
}

}  // namespace

void tiles_audio_reconcile(const std::vector<uint32_t>        &assignments,
                           const std::vector<ParticipantInfo> &roster,
                           const TilesAudioPlanParams          &params,
                           const std::string                   &group_name)
{
    if (!params.enabled) return;

    // An empty group name used to return here, which made turning the feature
    // off mean "stop reconciling" rather than "stop". Nothing about the
    // sources changed: they stayed in the group, unmuted, on tracks 1-6, in
    // every scene — so every participant kept going to air and to the stems —
    // and they stayed owned by a still-live Tiles source, so nothing else
    // could adopt or mute them either. An operator clearing the field mid-show
    // to stop the feature heard no change at all, and the remedies left to
    // them are the destructive ones: kill the group (everyone at once) or
    // delete sources by hand, on air.
    //
    // So off runs one last reconcile, with no assignments. The planner then
    // emits Mute for every source this Tiles source owns and nothing else:
    // Create is impossible from an empty assignment list (and refused by
    // tiles_audio_apply without a group anyway), and anything owned by another
    // Tiles source is untouched exactly as always. Consistent with the
    // feature's own mute-rather-than-delete rule, and reversible — naming a
    // group again unmutes whoever is back on the wall.
    //
    // The assignments are dropped here rather than at the call site so that
    // "off creates nothing" is a property of this function, not of a caller
    // remembering to pass an empty vector.
    const std::vector<uint32_t>  no_assignments;
    const std::vector<uint32_t> &wanted =
        group_name.empty() ? no_assignments : assignments;

    std::lock_guard<std::mutex> lock(g_reconcile_mutex);
    const TilesAudioPlan plan =
        plan_tiles_audio(wanted, tiles_audio_scan(), roster, params);

    // Logged because the operator has to be able to tell that clearing the
    // field took effect — the whole failure this fixes is a change with no
    // audible or visible confirmation. Silent when there was nothing to mute
    // (already off, or nothing owned), so a repeated off reconcile does not
    // repeat the line.
    if (group_name.empty() && !plan.actions.empty())
        blog(LOG_INFO,
             "[corevideo] tiles audio: participant audio group cleared; "
             "muting %zu source(s) this wall owns. They are kept, not deleted "
             "— name a group again to bring back whoever is on the wall.",
             plan.actions.size());

    tiles_audio_apply(plan, group_name, params.self_uuid);
}
