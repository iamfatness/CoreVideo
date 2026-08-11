// src/zoom-tiles-audio.cpp
#include "zoom-tiles-audio.h"

#include <obs.h>
#include <util/base.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr const char *kParticipantAudioId = "zoom_participant_audio_source";
constexpr const char *kParticipantIdKey   = "participant_id";

struct ScanCtx {
    std::vector<TilesAudioSourceState> *out;
};

// True if a source with this uuid is still in the scene collection. A marker
// naming a deleted Tiles source is what makes its audio sources orphans.
bool owner_is_live(const char *uuid)
{
    if (!uuid || !*uuid) return false;
    obs_source_t *owner = obs_get_source_by_uuid(uuid);
    if (!owner) return false;
    obs_source_release(owner);
    return true;
}

// Finds a marked source by the participant it carries. Returns a new reference
// the caller must release, or nullptr.
obs_source_t *find_marked_source(uint32_t participant_id)
{
    struct FindCtx {
        uint32_t      want;
        obs_source_t *found;
    } ctx{participant_id, nullptr};

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
            obs_data_release(settings);

            if (!marked || pid != c->want) return true;
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

}  // namespace

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

void tiles_audio_apply(const TilesAudioPlan &plan, const std::string &group_name,
                       const std::string &self_uuid)
{
    if (plan.actions.empty()) return;
    if (group_name.empty() || self_uuid.empty()) return;

    obs_source_t *group_src = obs_get_source_by_name(group_name.c_str());
    if (!group_src) {
        blog(LOG_INFO,
             "[corevideo] tiles audio: group '%s' not found; nothing created",
             group_name.c_str());
        return;
    }
    obs_scene_t *group = obs_group_from_source(group_src);
    if (!group) {
        blog(LOG_WARNING,
             "[corevideo] tiles audio: '%s' is not a group; nothing created",
             group_name.c_str());
        obs_source_release(group_src);
        return;
    }

    for (const TilesAudioAction &action : plan.actions) {
        if (action.kind == TilesAudioActionKind::Create) {
            // A name already in use by something the plugin does not own is
            // deferred, never overwritten and never renamed around. The
            // operator's source keeps its name.
            obs_source_t *clash = obs_get_source_by_name(action.name.c_str());
            if (clash) {
                obs_source_release(clash);
                blog(LOG_WARNING,
                     "[corevideo] tiles audio: name '%s' is already taken; "
                     "skipping this participant rather than overwriting",
                     action.name.c_str());
                continue;
            }

            obs_data_t *settings = obs_data_create();
            obs_data_set_int(settings, kParticipantIdKey,
                             static_cast<long long>(action.participant_id));
            obs_data_set_string(settings, CV_TILES_AUDIO_OWNER_KEY,
                                self_uuid.c_str());
            obs_source_t *created = obs_source_create(
                kParticipantAudioId, action.name.c_str(), settings, nullptr);
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
                     action.name.c_str(), group_name.c_str());
            obs_source_release(created);
            continue;
        }

        obs_source_t *target = find_marked_source(action.participant_id);
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

    obs_source_release(group_src);
}
