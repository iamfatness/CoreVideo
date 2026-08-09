#pragma once
#include <string>
#include <cstddef>
#include <cstdint>
#include <cerrno>

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
};
struct ShmAudioHeader {
    uint32_t sequence;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t reserved;
    uint32_t byte_len;
};

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
   };

   inline void shm_region_destroy(ShmRegion &r)
   {
       if (r.ptr)        { UnmapViewOfFile(r.ptr);   r.ptr        = nullptr; }
       if (r.map_handle) { CloseHandle(r.map_handle); r.map_handle = nullptr; }
       r.size = 0;
   }

   inline bool shm_region_create(ShmRegion &r, const std::string &name, size_t size)
   {
       shm_region_destroy(r);
       r.map_handle = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr,
                                         PAGE_READWRITE, 0,
                                         static_cast<DWORD>(size), name.c_str());
       if (!r.map_handle) { r.last_error = GetLastError(); return false; }
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
#else
#  include <sys/mman.h>
#  include <fcntl.h>
   struct ShmRegion {
       int         fd   = -1;
       void       *ptr  = nullptr;
       size_t      size = 0;
       std::string name; // stored so we can shm_unlink on destroy
       bool        owner = false;
       uint32_t    last_error = 0; // errno of the most recent failure
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
   }

   inline bool shm_region_create(ShmRegion &r, const std::string &name, size_t size)
   {
       shm_region_destroy(r);
       r.name = "/" + name; // shm_open requires a leading '/'
       r.owner = true;
       r.fd   = shm_open(r.name.c_str(), O_CREAT | O_RDWR, 0600);
       if (r.fd < 0) { r.last_error = static_cast<uint32_t>(errno); return false; }
       if (ftruncate(r.fd, static_cast<off_t>(size)) < 0) {
           r.last_error = static_cast<uint32_t>(errno);
           shm_region_destroy(r);
           return false;
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
#endif

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
