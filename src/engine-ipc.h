#pragma once
#include <atomic>
#include <string>
#include <cstddef>
#include <cstdint>
#include <cerrno>
#include <cstring>
#include <vector>

// ── IPC command / event tokens ────────────────────────────────────────────────
#define IPC_CMD_INIT        "init"
#define IPC_CMD_JOIN        "join"
#define IPC_CMD_LEAVE       "leave"
#define IPC_CMD_SUBSCRIBE   "subscribe"
#define IPC_CMD_SUBSCRIBE_AUDIO "subscribe_audio"
#define IPC_CMD_UNSUBSCRIBE "unsubscribe"
#define IPC_CMD_START_MEDIA "start_media"
#define IPC_CMD_STOP_MEDIA  "stop_media"
#define IPC_CMD_QUIT        "quit"
#define IPC_CMD_TALKBACK_PROBE "talkback_probe"
#define IPC_CMD_TALKBACK_OPEN  "talkback_open"
#define IPC_CMD_TALKBACK_AUDIO "talkback_audio"
#define IPC_CMD_TALKBACK_CLOSE "talkback_close"
#define IPC_CMD_TALKBACK_START "talkback_start"
#define IPC_CMD_TALKBACK_STOP  "talkback_stop"
#define IPC_CMD_TALKBACK_NOMINATE "talkback_nominate"
#define IPC_EVT_READY       "ready"
#define IPC_EVT_AUTH_OK     "auth_ok"
#define IPC_EVT_AUTH_FAIL   "auth_fail"
#define IPC_EVT_JOINED      "joined"
#define IPC_EVT_LEFT        "left"
#define IPC_EVT_FRAME       "frame"
#define IPC_EVT_AUDIO       "audio"
#define IPC_EVT_ERROR       "error"

// Shared-memory name prefix (no leading slash — added per-platform below)
#define IPC_SHM_PREFIX "ZoomObsPlugin_"

// ── IPC / SHM hardening policy (pure logic, unit-tested) ─────────────────────

// Upper bound on distinct SHM-backed sources per media path. The video, share,
// and audio paths each keep one region per source UUID; without a cap a
// misbehaving or runaway peer could create unbounded SHM regions and exhaust
// memory. The product targets up to 8 feeds plus active-speaker/spotlight/
// screenshare slots; 32 leaves generous headroom while staying bounded.
static constexpr size_t kMaxShmSources = 32;

// True when registering a new source_uuid would push a media path past the SHM
// cap. Re-registering an already-present source never counts against the cap.
static inline bool shm_source_over_cap(size_t existing_count,
                                       bool already_present,
                                       size_t max_sources = kMaxShmSources)
{
    return !already_present && existing_count >= max_sources;
}

// True when a read-side SHM mapping must be (re)opened before consuming a
// frame event: never mapped, mapped too small, or the event carries a newer
// SHM generation than the mapping was opened against. The generation changes
// whenever the engine (re)creates the region — e.g. after an engine restart —
// at which point the old mapping points at an orphaned region that will never
// be written again; without this check the plugin would show a frozen frame
// forever. event_gen == 0 means the peer did not send a generation (older
// engine binary) — the generation check is skipped for backward compatibility.
static inline bool shm_mapping_stale(const void *mapped_ptr,
                                     size_t mapped_size,
                                     size_t needed_size,
                                     uint32_t event_gen,
                                     uint32_t mapped_gen)
{
    if (!mapped_ptr || mapped_size < needed_size) return true;
    if (event_gen != 0 && event_gen != mapped_gen) return true;
    return false;
}

// Heartbeat expiry: true when the peer has been silent for longer than
// timeout_ms. last_rx_ms == 0 means "nothing received yet" — never expired
// (callers seed the clock when the link comes up). A last_rx_ms in the future
// (clock skew / re-seed race) is treated as fresh, not expired.
static inline bool ipc_heartbeat_expired(uint64_t now_ms,
                                         uint64_t last_rx_ms,
                                         uint64_t timeout_ms)
{
    return last_rx_ms != 0 && now_ms > last_rx_ms &&
           (now_ms - last_rx_ms) > timeout_ms;
}

struct ShmFrameHeader {
    uint32_t sequence;
    uint32_t width;
    uint32_t height;
    uint32_t y_len;
    // The engine's tile_clock_now_ns() when the Zoom SDK handed this frame
    // over. Same clock, same cross-process equivalence caveat (Windows-only)
    // as ShmAudioSlot::capture_ns below -- see the comment there rather than
    // repeating it here. Paired with ShmAudioSlot::capture_ns this is what
    // makes the A/V offset a measured number instead of an assertion -- on
    // 2026-08-16 the product could not answer "what is our render latency"
    // because nothing carried a timestamp across the boundary.
    //
    // Deliberately UNLIKE ShmAudioSlot below, this header carries no version
    // guard. A new-engine/old-plugin skew (old plugin requests the smaller,
    // pre-capture_ns frame_bytes; that request succeeds as a partial view
    // into the larger region; width/height/y_len read fine from their
    // unchanged offsets; the pixel memcpy then reads from the wrong offset)
    // is SILENT -- no failed map, no rejected read, just a subtly wrong
    // frame. That is the one direction shm_mapping_stale()'s size check does
    // not catch (old-engine/new-plugin is the reverse skew and IS caught: the
    // larger request fails MapViewOfFile outright). Accepted anyway: unlike
    // the audio ring, no field here is available to repurpose as a guard the
    // way slot_count was, so a guard would mean adding a new field solely to
    // detect a skew that release-local.ps1 already prevents by packaging
    // plugin + engine + SDK as one matched build. Revisit if engine and
    // plugin are ever updated independently of each other.
    //
    // NOT WRITTEN BY EVERY PRODUCER. engine/src/engine-video.cpp stamps this;
    // engine/src/engine-share.cpp does NOT -- the screen-share writer sets
    // sequence/width/height/y_len and leaves capture_ns at whatever the fresh
    // region was zero-filled to. The plugin's reader treats 0 as "not
    // measured" and skips the latency store, so a screen-share output shows
    // "-" for A/V Offset permanently and always will until the share writer
    // stamps it too. That is a gap, not a design decision.
    uint64_t capture_ns;
};
// Audio is a RING, not a mailbox.
//
// It used to be one slot: the engine memcpy'd every Zoom buffer over the
// previous one, guarded by a seqlock. A seqlock prevents the reader seeing a
// TORN buffer and does nothing about LOSS -- so any reader stall destroyed
// 10 ms of audio permanently, silently, on a box where stalls are routine.
// Video keeps the mailbox on purpose (newest frame wins, a dropped frame is
// nearly invisible); the ear is not so forgiving, and Viz Engine ring-buffers
// audio for the same reason.
//
// 8 slots is 80 ms at Zoom's 10 ms buffer. That is CAPACITY, not latency: a
// reader keeping up sees about one slot of delay, and the depth is only spent
// while absorbing a stall.
static constexpr uint32_t kAudioRingSlots = 8;

// Talkback's slot size. OBS delivers AUDIO_OUTPUT_FRAMES (1024) frames per
// callback; 1024 frames of stereo int16 is 4096 bytes, so 8192 leaves headroom
// for a larger buffer without a resize. Talkback deliberately never resizes:
// a resize means a new _gN region name (a Windows section cannot grow while
// mapped), and re-handshaking a live talk key mid-sentence is worse than
// refusing one oversized buffer.
static constexpr uint32_t kTalkbackSlotBytes = 8192;

struct ShmAudioSlot {
    // Even and unchanged across a read = the payload was stable. Odd = a write
    // is in progress. Same seqlock discipline the single slot had, now per-slot.
    uint32_t sequence;
    // The engine's tile_clock_now_ns() (engine/src/tile-clock-log.h) when the
    // Zoom SDK handed this buffer over -- the engine is a standalone process
    // with no libobs headers, so it cannot call os_gettime_ns() directly. On
    // Windows both resolve to the same QPC counter (OBS's os_gettime_ns() and
    // libc++'s std::chrono::steady_clock both read QueryPerformanceCounter),
    // so the plugin can still subtract its own os_gettime_ns() reading
    // directly to measure pipeline latency. WARNING: this equality does NOT
    // hold on macOS -- libobs uses mach_absolute_time() there while libc++'s
    // steady_clock uses CLOCK_MONOTONIC, a different clock with a different
    // epoch. The mac-port branch must revisit this before trusting capture_ns
    // for cross-process latency there.
    uint64_t capture_ns;
    uint32_t byte_len;
    // Who this buffer belongs to. With coalesced notifications (see
    // ShmAudioHeader::notify) one pipe event can cover many slots, so the
    // event can no longer describe each buffer -- the slot describes itself.
    // 0 for the mixed/audience stream, which has no single participant.
    uint32_t participant_id;
};

struct ShmAudioHeader {
    // FREE-RUNNING count of slots written so far -- NOT a slot index, and it
    // never wraps at slot_count (only at 2^32, ~1.4 years at Zoom's ~100
    // buffers/sec). Apply `% slot_count` only where a physical offset is
    // derived (shm_audio_slot_offset()). This is what lets
    // audio_ring_slots_behind() tell "reader caught up" (0) apart from
    // "writer lapped the reader by exactly one full ring" (slot_count) --
    // collapsing those two under slot_count-modulo arithmetic was the original
    // bug (a stalled reader landed back on the same value as a caught-up one
    // and silently lost a full lap, unreported). The reader drains up to (not
    // including) this. Written last, after the slot is complete.
    uint32_t write_index;
    uint32_t slot_count;
    uint32_t slot_bytes;
    uint32_t sample_rate;
    uint16_t channels;
    // Edge-triggered notification flag. The engine used to send one
    // {"cmd":"audio"} pipe line PER 10ms BUFFER PER SOURCE (~1,700
    // messages/sec at full load), all parsed by the plugin's single reader
    // thread; when that thread fell behind, the pipe filled, the engine's
    // blocking write stalled the Zoom SDK callback thread, and audio queued
    // upstream inside the SDK. Measured live 2026-08-17: engine->plugin
    // latency 58-90ms under full gallery load vs 41-161us idle, with ring
    // overruns at zero throughout -- the ring itself never fell behind, the
    // wakeups did. The ring makes per-buffer events redundant: the reader
    // drains everything available on any wakeup, so the event only needs to
    // fire on the empty -> non-empty edge.
    //
    // Protocol (single writer, single reader; `notify` is plain uint16, the
    // fences do the work):
    //
    //   WRITER, after publishing write_index:
    //     atomic_thread_fence(seq_cst);
    //     if (notify == 0) { notify = 1; send one pipe event; }
    //
    //   READER, after draining to its write_index snapshot:
    //     notify = 0;
    //     atomic_thread_fence(seq_cst);
    //     if (read_index != write_index) { notify = 1; drain again; }
    //     else break;                      // truly empty
    //
    // WHY NO WAKEUP IS EVER LOST. A lost wakeup needs BOTH: the writer to
    // skip its event (it loaded notify == 1, missing the reader's clear) AND
    // the reader to stop (it loaded write_index and saw no new data). Each
    // side executes store -> seq_cst fence -> load. seq_cst fences are
    // totally ordered: if the reader's fence comes first, its notify=0 store
    // is visible to the writer's load, so the writer sends the event; if the
    // writer's fence comes first, its write_index store is visible to the
    // reader's re-check, so the reader drains again. Either way progress is
    // made. Both flags set concurrently is benign (one redundant event); the
    // seq_cst fences are REQUIRED -- plain x86 store->load may reorder, and
    // a release fence is not enough here.
    //
    // A new region starts with notify = 0, so the first buffer after any
    // generation change always sends its event and the reader learns to
    // remap.
    uint16_t notify;
};

inline size_t shm_audio_region_bytes(uint32_t slot_bytes)
{
    return sizeof(ShmAudioHeader) +
           static_cast<size_t>(kAudioRingSlots) *
               (sizeof(ShmAudioSlot) + slot_bytes);
}

inline size_t shm_audio_slot_offset(const ShmAudioHeader &h, uint32_t index)
{
    return sizeof(ShmAudioHeader) +
           static_cast<size_t>(index) * (sizeof(ShmAudioSlot) + h.slot_bytes);
}

// How many slots the reader has yet to drain. write_index and read_index are
// FREE-RUNNING counters (see ShmAudioHeader::write_index above) -- they never
// wrap at slot_count, only at 2^32 -- so this is a plain, un-modulo'd
// subtraction. Unsigned wraparound at 2^32 stays correct without special
// casing it. The return value is therefore NOT bounded by slot_count: a
// caller comparing it against slot_count is how a full-lap overrun is told
// apart from a caught-up reader (0 vs slot_count), which a modulo would
// collapse into the same value. slot_count is accepted for symmetry with the
// rest of this ring's API but is not used in the arithmetic; the
// caught-up-vs-overrun decision belongs to the caller.
inline uint32_t audio_ring_slots_behind(uint32_t write_index,
                                        uint32_t read_index,
                                        uint32_t slot_count)
{
    (void)slot_count;
    return write_index - read_index;
}

// ── The notify protocol, as code ─────────────────────────────────────────────
// Both sides go through these helpers so the flag has exactly one
// implementation to own. The first version inlined the flag handling at the
// drain-loop exit only, and every OTHER return path -- the first-event
// leveling, the director-handover gates, a failed remap -- consumed its wakeup
// and left notify set, after which the writer never notified again: every
// source played one 10ms buffer and went silent for its lifetime. Review
// caught it before install. The rule the helpers enforce: whoever accepts a
// wakeup OWNS the flag until audio_ring_reader_done() says the ring was seen
// empty after clearing it, or audio_ring_reader_abandon() hands ownership
// back to the writer.

// WRITER, immediately after publishing write_index. Returns true exactly when
// this publish crossed the empty->non-empty edge and one pipe event must be
// sent. The seq_cst fence pairs with the reader's (see ShmAudioHeader::notify
// for the total-order proof).
inline bool audio_ring_notify_after_publish(ShmAudioHeader *hdr)
{
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (hdr->notify != 0) return false;
    hdr->notify = 1;
    std::atomic_thread_fence(std::memory_order_release);
    return true;
}

// READER, after draining to its snapshot. Clears the flag, then re-checks the
// ring; returns true when the ring was seen empty AFTER the clear -- the only
// state in which sleeping is safe. Returns false when more data landed in the
// race window: the flag has been re-claimed (suppressing redundant events)
// and the caller must drain again.
inline bool audio_ring_reader_done(ShmAudioHeader *hdr, uint32_t read_index)
{
    hdr->notify = 0;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (read_index == hdr->write_index) return true;
    hdr->notify = 1;
    std::atomic_thread_fence(std::memory_order_release);
    return false;
}

// READER, when it must stop without having seen the ring empty (bounded-pass
// cap, or any exit after the mapping is valid but before a full drain).
// Leaves the flag CLEAR so the writer's next publish re-notifies. The failure
// mode this prevents is a consumed wakeup with the flag still set, which
// silences the source forever.
inline void audio_ring_reader_abandon(ShmAudioHeader *hdr)
{
    hdr->notify = 0;
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

// ── Platform-specific pipe / socket paths ─────────────────────────────────────
#if defined(WIN32)
#  include <windows.h>
   static constexpr const char *PIPE_P2E = "\\\\.\\pipe\\ZoomObsPlugin_P2E";
   static constexpr const char *PIPE_E2P = "\\\\.\\pipe\\ZoomObsPlugin_E2P";
#else
   static constexpr const char *SOCK_P2E = "/tmp/ZoomObsPlugin_P2E.sock";
   static constexpr const char *SOCK_E2P = "/tmp/ZoomObsPlugin_E2P.sock";
#endif

// ── Platform-agnostic file-descriptor type ───────────────────────────────────
#if defined(WIN32)
   using IpcFd = HANDLE;
   // INVALID_HANDLE_VALUE is a reinterpret_cast and not a constant expression
   // on MSVC — use inline const instead of constexpr.
   inline const IpcFd kIpcInvalidFd = INVALID_HANDLE_VALUE;
#else
#  include <unistd.h>
   using IpcFd = int;
   static constexpr IpcFd kIpcInvalidFd = -1;
#endif

// ── Shared-memory region ──────────────────────────────────────────────────────

// A region's name carries its generation from the second generation on.
// A Windows named section cannot be recreated at a larger size while any
// process still maps the old one (CreateFileMapping returns the existing
// smaller section and the larger MapViewOfFile fails forever), so a resize
// must move to a fresh name. Generations 0/1 keep the legacy unsuffixed
// name so engines that never resize interoperate with older plugins.
inline std::string shm_region_name(const std::string &base, uint32_t gen)
{
    if (gen <= 1) return base;
    return base + "_g" + std::to_string(gen);
}

#if defined(WIN32)
   struct ShmRegion {
       HANDLE  map_handle = nullptr;
       void   *ptr        = nullptr;
       size_t  size       = 0;
       uint32_t last_error = 0; // GetLastError() of the most recent failure
       // True when the most recent shm_region_create() OPENED a section that
       // already existed instead of creating a fresh one. A named section
       // outlives its creator for as long as ANY process maps it, so this
       // firing in the engine means some other process -- in practice a
       // wedged orphan ZoomObsEngine from a previous OBS session -- still
       // holds (and may still be WRITING) a region by this name. Proven live
       // 2026-08-17: a ghost writer sharing a ring sets ShmAudioHeader::notify
       // with no pipe to deliver its event, permanently suppressing the live
       // engine's edge notifications -- every source degrades to the 2.5s
       // keepalive, ~92% audio loss. Callers must surface this loudly.
       bool already_existed = false;
   };

   inline void shm_region_destroy(ShmRegion &r)
   {
       if (r.ptr)        { UnmapViewOfFile(r.ptr);   r.ptr        = nullptr; }
       if (r.map_handle) { CloseHandle(r.map_handle); r.map_handle = nullptr; }
       r.size = 0;
       r.already_existed = false;
   }

   inline bool shm_region_create(ShmRegion &r, const std::string &name, size_t size)
   {
       shm_region_destroy(r);
       r.map_handle = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr,
                                         PAGE_READWRITE, 0,
                                         static_cast<DWORD>(size), name.c_str());
       if (!r.map_handle) { r.last_error = GetLastError(); return false; }
       // CreateFileMapping SUCCEEDS on an existing name and hands back the
       // existing section; only GetLastError() distinguishes the two. See
       // ShmRegion::already_existed for why callers must not ignore this.
       r.already_existed = GetLastError() == ERROR_ALREADY_EXISTS;
       r.ptr  = MapViewOfFile(r.map_handle, FILE_MAP_WRITE, 0, 0, size);
       r.size = r.ptr ? size : 0;
       if (!r.ptr) {
           r.last_error = GetLastError();
           shm_region_destroy(r);
           return false;
       }
       r.last_error = 0;
       return true;
   }

   inline bool shm_region_open_read(ShmRegion &r, const std::string &name, size_t size)
   {
       shm_region_destroy(r);
       r.map_handle = OpenFileMappingA(FILE_MAP_READ, FALSE, name.c_str());
       if (!r.map_handle) return false;
       r.ptr = MapViewOfFile(r.map_handle, FILE_MAP_READ, 0, 0, size);
       r.size = r.ptr ? size : 0;
       if (!r.ptr) { shm_region_destroy(r); return false; }
       return true;
   }

   // Read-write open, for AUDIO ring regions only. The edge-triggered
   // notification protocol (ShmAudioHeader::notify) requires the READER to
   // clear a flag the writer checks, so a read-only view cannot implement it.
   // This deliberately relaxes the read-only mapping for audio: the engine
   // never reads an audio region back -- it only writes -- so the read-only
   // view never protected engine-side state; it only protected the plugin
   // from its own stray writes into data the plugin alone consumes. Video
   // regions keep read-only views: their protocol needs no reader writes.
   inline bool shm_region_open_readwrite(ShmRegion &r, const std::string &name,
                                         size_t size)
   {
       shm_region_destroy(r);
       r.map_handle = OpenFileMappingA(FILE_MAP_READ | FILE_MAP_WRITE, FALSE,
                                       name.c_str());
       if (!r.map_handle) { r.last_error = GetLastError(); return false; }
       r.ptr = MapViewOfFile(r.map_handle, FILE_MAP_READ | FILE_MAP_WRITE,
                             0, 0, size);
       r.size = r.ptr ? size : 0;
       if (!r.ptr) {
           r.last_error = GetLastError();
           const DWORD err = r.last_error;
           shm_region_destroy(r);
           r.last_error = err; // survive the destroy's reset
           return false;
       }
       return true;
   }
#else
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <fcntl.h>
#  include <cerrno>
   struct ShmRegion {
       int         fd   = -1;
       void       *ptr  = nullptr;
       size_t      size = 0;
       std::string name; // stored so we can shm_unlink on destroy
       bool        owner = false;
       uint32_t    last_error = 0; // errno of the most recent failure
       // See the Windows counterpart: the most recent shm_region_create()
       // found this name already present (another process created it and it
       // has not been unlinked). Same ghost-writer hazard, same obligation on
       // callers to surface it.
       bool already_existed = false;
   };

   inline void shm_region_destroy(ShmRegion &r)
   {
       if (r.ptr && r.ptr != MAP_FAILED) { munmap(r.ptr, r.size); r.ptr = nullptr; }
       if (r.fd >= 0) {
           close(r.fd);
           if (r.owner && !r.name.empty()) shm_unlink(r.name.c_str());
           r.fd = -1;
       }
       r.size = 0;
       r.name.clear();
       r.owner = false;
       r.already_existed = false;
   }

   inline bool shm_region_create(ShmRegion &r, const std::string &name, size_t size)
   {
       shm_region_destroy(r);
       r.name = "/" + name; // shm_open requires a leading '/'
       r.owner = true;
       // O_EXCL first purely as a probe: EEXIST is the only portable way to
       // learn the name was already present (see ShmRegion::already_existed).
       r.fd = shm_open(r.name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
       if (r.fd < 0 && errno == EEXIST) {
           r.already_existed = true;
           r.fd = shm_open(r.name.c_str(), O_RDWR, 0600);
           if (r.fd >= 0) {
               // An existing object must be size-checked with fstat, not
               // blindly ftruncated: on macOS a POSIX shm object is sized
               // exactly once and ftruncate on an existing one fails EINVAL,
               // which made every create-over-a-held-name fail outright
               // there. Big enough -> map as-is (open-and-flag). Undersized
               // -> grow it where the OS allows, recreate where it does not;
               // already_existed stays true either way so callers still
               // learn about the ghost holder.
               struct stat st {};
               const bool fits = fstat(r.fd, &st) == 0 &&
                                 static_cast<size_t>(st.st_size) >= size;
               if (!fits) {
#if defined(__APPLE__)
                   close(r.fd);
                   shm_unlink(r.name.c_str());
                   r.fd = shm_open(r.name.c_str(),
                                   O_CREAT | O_EXCL | O_RDWR, 0600);
                   if (r.fd >= 0 &&
                       ftruncate(r.fd, static_cast<off_t>(size)) < 0) {
                       r.last_error = static_cast<uint32_t>(errno);
                       shm_region_destroy(r);
                       return false;
                   }
#else
                   if (ftruncate(r.fd, static_cast<off_t>(size)) < 0) {
                       r.last_error = static_cast<uint32_t>(errno);
                       shm_region_destroy(r);
                       return false;
                   }
#endif
               }
           }
           if (r.fd < 0) { r.last_error = static_cast<uint32_t>(errno); return false; }
       } else {
           if (r.fd < 0) { r.last_error = static_cast<uint32_t>(errno); return false; }
           if (ftruncate(r.fd, static_cast<off_t>(size)) < 0) {
               r.last_error = static_cast<uint32_t>(errno);
               shm_region_destroy(r);
               return false;
           }
       }
       r.ptr  = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, r.fd, 0);
       r.size = (r.ptr != MAP_FAILED) ? size : 0;
       if (r.ptr == MAP_FAILED) {
           r.last_error = static_cast<uint32_t>(errno);
           r.ptr = nullptr;
           shm_region_destroy(r);
           return false;
       }
       r.last_error = 0;
       return true;
   }

   inline bool shm_region_open_read(ShmRegion &r, const std::string &name, size_t size)
   {
       shm_region_destroy(r);
       r.name = "/" + name;
       r.owner = false;
       r.fd = shm_open(r.name.c_str(), O_RDONLY, 0600);
       if (r.fd < 0) return false;
       r.ptr = mmap(nullptr, size, PROT_READ, MAP_SHARED, r.fd, 0);
       r.size = (r.ptr != MAP_FAILED) ? size : 0;
       if (r.ptr == MAP_FAILED) { r.ptr = nullptr; shm_region_destroy(r); return false; }
       return true;
   }

   // See the Windows counterpart above: audio rings only, because the
   // edge-triggered notify flag is reader-cleared by design.
   inline bool shm_region_open_readwrite(ShmRegion &r, const std::string &name,
                                         size_t size)
   {
       shm_region_destroy(r);
       r.name = "/" + name;
       r.owner = false;
       r.fd = shm_open(r.name.c_str(), O_RDWR, 0600);
       if (r.fd < 0) { r.last_error = static_cast<uint32_t>(errno); return false; }
       r.ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, r.fd, 0);
       r.size = (r.ptr != MAP_FAILED) ? size : 0;
       if (r.ptr == MAP_FAILED) {
           const uint32_t err = static_cast<uint32_t>(errno);
           r.ptr = nullptr;
           shm_region_destroy(r);
           r.last_error = err;
           return false;
       }
       return true;
   }
#endif

// ── I420 frame reader ─────────────────────────────────────────────────────────
enum class ShmFrameRead {
    Ok,          // dst holds one complete, consistent I420 frame
    OpenFailed,  // region absent or not yet published by the engine
    TooSmall,    // the announced frame does not fit the mapped region
    Invalid,     // unusable header dimensions, or every attempt read a torn frame
};

// Copies the newest complete I420 frame out of `region`, opening (or reopening)
// it whenever the mapping is absent, too small, or a generation behind.
//
// Two protocols are folded in here so that every consumer of engine frames gets
// both without restating either:
//
//  1. The seqlock. The engine bumps `sequence` to odd before writing pixels and
//     back to even after, so a reader that sees the same even value on both
//     sides of its copy knows the copy was not torn.
//  2. Region generations. A Windows named section cannot be recreated at a
//     larger size while any process still maps the old one, so the engine moves
//     to a generation-suffixed name on every resize (see shm_region_name). A
//     reader that ignores this keeps reading an orphaned region and shows a
//     frozen frame forever. Pass the generation from the frame event as
//     `event_shm_gen`; `mapped_shm_gen` is the caller's record of which
//     generation its mapping was opened against and is updated on reopen.
//     event_shm_gen == 0 means the engine did not report one (older binary).
//
// On Ok, dst holds the Y plane in [0, y_len) followed by U then V, each
// y_len/4 bytes. `dst` grows as needed and is never shrunk, so a caller that
// reuses one buffer stops allocating after the first frame. out_w/out_h/
// out_y_len are filled from the header whenever it was readable (including the
// TooSmall case, so callers can report the shortfall); dst is only meaningful
// on Ok. out_capture_ns is optional (default nullptr, so existing callers are
// unaffected) and, when Ok, receives the engine's capture_ns for latency
// measurement -- see ShmFrameHeader::capture_ns.
inline ShmFrameRead shm_read_i420_frame(ShmRegion &region,
                                        const std::string &base_name,
                                        uint32_t event_width,
                                        uint32_t event_height,
                                        uint32_t event_shm_gen,
                                        uint32_t &mapped_shm_gen,
                                        std::vector<uint8_t> &dst,
                                        uint32_t &out_w,
                                        uint32_t &out_h,
                                        uint32_t &out_y_len,
                                        uint64_t *out_capture_ns = nullptr)
{
    out_w = 0;
    out_h = 0;
    out_y_len = 0;
    if (event_width == 0 || event_height == 0) return ShmFrameRead::Invalid;

    const size_t frame_bytes = sizeof(ShmFrameHeader) +
        static_cast<size_t>(event_width) * event_height * 3 / 2;

    if (shm_mapping_stale(region.ptr, region.size, frame_bytes, event_shm_gen,
                          mapped_shm_gen)) {
        // A generation change means the engine recreated the region — our
        // mapping points at an orphaned one that will never be written again.
        // Drop it before opening: holding it is also what blocks an engine that
        // still uses the legacy name from recreating at a larger size.
        if (region.ptr && event_shm_gen != 0 && event_shm_gen != mapped_shm_gen)
            shm_region_destroy(region);
        if (!shm_region_open_read(region, shm_region_name(base_name, event_shm_gen),
                                  frame_bytes) &&
            // Engines predating suffixed names recreate the legacy name for
            // every generation — fall back to it.
            (event_shm_gen <= 1 ||
             !shm_region_open_read(region, base_name, frame_bytes)))
            return ShmFrameRead::OpenFailed;
        mapped_shm_gen = event_shm_gen;
    }

    const auto *hdr = static_cast<const ShmFrameHeader *>(region.ptr);
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint32_t seq1 = hdr->sequence;
        std::atomic_thread_fence(std::memory_order_acquire);
        if ((seq1 & 1u) != 0) continue;  // writer is mid-update

        const uint32_t w = hdr->width;
        const uint32_t h = hdr->height;
        const uint32_t y_len = hdr->y_len;
        const uint64_t capture_ns = hdr->capture_ns;
        out_w = w;
        out_h = h;
        out_y_len = y_len;
        // 64-bit compare: a 32-bit w*h could wrap and match a bogus y_len.
        if (w == 0 || h == 0 ||
            static_cast<uint64_t>(w) * h != static_cast<uint64_t>(y_len))
            return ShmFrameRead::Invalid;

        const size_t payload = static_cast<size_t>(y_len) +
            static_cast<size_t>(y_len) / 2;
        if (sizeof(ShmFrameHeader) + payload > region.size)
            return ShmFrameRead::TooSmall;

        if (dst.size() < payload) dst.resize(payload);
        std::memcpy(dst.data(),
                    static_cast<const uint8_t *>(region.ptr) + sizeof(ShmFrameHeader),
                    payload);
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint32_t seq2 = hdr->sequence;
        if (seq1 == seq2 && (seq2 & 1u) == 0) {
            if (out_capture_ns) *out_capture_ns = capture_ns;
            return ShmFrameRead::Ok;
        }
    }
    return ShmFrameRead::Invalid;
}

// ── Line-oriented I/O helpers ─────────────────────────────────────────────────
// Returns false on EOF, I/O error, or if the line exceeds max_len bytes.
static inline bool ipc_read_line(IpcFd fd, std::string &out,
                                 size_t max_len = 65536)
{
    out.clear();
#if defined(WIN32)
    char ch; DWORD n;
    while (ReadFile(fd, &ch, 1, &n, nullptr) && n == 1) {
        if (ch == '\n') return true;
        if (out.size() >= max_len) return false; // line too long
        out += ch;
    }
    return false; // EOF or error
#else
    char ch; ssize_t n;
    while ((n = read(fd, &ch, 1)) == 1) {
        if (ch == '\n') return true;
        if (out.size() >= max_len) return false; // line too long
        out += ch;
    }
    return false; // EOF (n==0) or error (n==-1)
#endif
}

// Writes msg followed by '\n', retrying short writes until the whole line is
// delivered. Returns true only if every byte was written; false on a closed,
// full, or broken pipe (or an invalid fd). Callers must treat false as a lost
// message and tear down / recover the link rather than assuming delivery.
static inline bool ipc_write_line(IpcFd fd, const std::string &msg)
{
    if (fd == kIpcInvalidFd) return false;
    const std::string out = msg + "\n";
    const char *p   = out.c_str();
    size_t      rem = out.size();
#if defined(WIN32)
    while (rem > 0) {
        DWORD written = 0;
        if (!WriteFile(fd, p, static_cast<DWORD>(rem), &written, nullptr))
            return false;            // pipe closed / broken
        if (written == 0) return false; // no progress — treat as failure
        p   += written;
        rem -= written;
    }
    return true;
#else
    while (rem > 0) {
        ssize_t n = write(fd, p, rem);
        if (n < 0) {
            if (errno == EINTR) continue; // interrupted — retry
            return false;                 // EPIPE / EBADF / etc.
        }
        if (n == 0) return false;         // no progress — treat as failure
        p   += static_cast<size_t>(n);
        rem -= static_cast<size_t>(n);
    }
    return true;
#endif
}
