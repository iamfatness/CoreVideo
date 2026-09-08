#pragma once
#include <cstdint>
#include <map>
#include <string>

// Current health only; the client's bounded diagnostics ring retains every
// error. Eight tiles failing three times used to create 24 modal dialogs.
// Membership is limited to live assignments, with one notice per episode.
// Caller serializes access. No callbacks or SDK calls occur under this state.
class MediaFailureState {
    struct Assignment { uint32_t participant; uint64_t epoch; };
    struct Failure { std::string cause; uint64_t since; unsigned attempts; uint32_t participant; };
    std::map<std::string, Assignment> assignments;
    std::map<std::string, Failure> failures;
    std::map<std::string, std::string> persistent;
    uint64_t epoch = 0;
public:
    void assign(const std::string &uuid, uint32_t participant) {
        auto it = assignments.find(uuid);
        if (it != assignments.end() && it->second.participant == participant) return;
        failures.erase(uuid);
        assignments[uuid] = {participant, ++epoch};
    }
    uint64_t ticket(const std::string &uuid, uint32_t participant) const {
        auto it = assignments.find(uuid);
        return it != assignments.end() && (it->second.participant == 0 || it->second.participant == participant) ? it->second.epoch : 0;
    }
    // True means the caller should emit the episode's single nonmodal notice.
    bool fail(const std::string &uuid, uint32_t participant, const std::string &cause,
              uint64_t now, bool present) {
        if (!ticket(uuid, participant)) return false;
        if (!present) { failures.erase(uuid); return false; }
        const bool first = failures.empty() && persistent.empty();
        auto it = failures.find(uuid);
        if (it == failures.end()) failures[uuid] = {cause, now, 1, participant};
        else { it->second.cause = cause; it->second.participant = participant; if (it->second.attempts < 3) ++it->second.attempts; }
        return first;
    }
    void delivered(const std::string &uuid, uint32_t participant, uint64_t token) {
        auto failed = failures.find(uuid);
        if (token && ticket(uuid, participant) == token && failed != failures.end() &&
            failed->second.participant == participant) failures.erase(failed);
    }
    // Shared-memory/audio errors have no reliable video recovery signal.
    // Keep them until explicit removal/stop/reset, never clear on video read.
    bool persistent_fail(const std::string &uuid, const std::string &message, bool registered) {
        if (uuid.empty() || !registered) return false;
        const bool first = failures.empty() && persistent.empty();
        persistent[uuid] = message;
        return first;
    }
    void stop() {
        failures.clear();
        persistent.clear();
        for (auto &entry : assignments) entry.second.epoch = ++epoch;
    }
    void remove(const std::string &uuid) { failures.erase(uuid); assignments.erase(uuid); persistent.erase(uuid); }
    template<class Present> void prune(Present present) {
        for (auto it = failures.begin(); it != failures.end();) {
            if (!present(it->second.participant)) it = failures.erase(it);
            else ++it;
        }
    }
    void reset() { stop(); assignments.clear(); ++epoch; }
    size_t size() const { return failures.size(); }
    bool failed(const std::string &uuid) const { return failures.count(uuid) || persistent.count(uuid); }
    bool terminal(uint64_t now) const {
        if (!persistent.empty()) return true;
        for (const auto &entry : failures)
            if (entry.second.attempts >= 3 || now >= entry.second.since + 10000) return true;
        return false;
    }
    std::string status(uint64_t now) const {
        std::string sticky;
        if (!persistent.empty())
            sticky = "Zoom media unavailable for " + std::to_string(persistent.size()) +
                " source(s). " + persistent.begin()->second.substr(0, 240) + " Click Retry Media; if unresolved, stop and restart media. ";
        if (failures.empty()) return sticky;
        // Cause is bounded in the summary; complete source/cause lives in logs.
        return sticky + std::string(terminal(now) ? "Zoom video unavailable for " : "Recovering Zoom video for ") +
            std::to_string(failures.size()) + " source(s). " +
            (terminal(now) ? "Click Retry Media. " : "Retrying automatically. ") +
            "Diagnostic: " + failures.begin()->second.cause.substr(0, 120);
    }
};
