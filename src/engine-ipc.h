#pragma once
#include <string>
#include <cstddef>

// ── IPC command / event tokens ────────────────────────────────────────────────
#define IPC_CMD_INIT        "init"
#define IPC_CMD_JOIN        "join"
#define IPC_CMD_LEAVE       "leave"
#define IPC_CMD_SUBSCRIBE   "subscribe"
#define IPC_CMD_UNSUBSCRIBE "unsubscribe"
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
   static constexpr IpcFd kIpcInvalidFd = INVALID_HANDLE_VALUE;
#else
#  include <unistd.h>
   using IpcFd = int;
   static constexpr IpcFd kIpcInvalidFd = -1;
#endif

// ── Shared-memory region ──────────────────────────────────────────────────────
#if defined(WIN32)
   struct ShmRegion {
       HANDLE  map_handle = nullptr;
       void   *ptr        = nullptr;
       size_t  size       = 0;
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
       if (!r.map_handle) return false;
       r.ptr  = MapViewOfFile(r.map_handle, FILE_MAP_WRITE, 0, 0, size);
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
   };

   inline void shm_region_destroy(ShmRegion &r)
   {
       if (r.ptr && r.ptr != MAP_FAILED) { munmap(r.ptr, r.size); r.ptr = nullptr; }
       if (r.fd >= 0) {
           close(r.fd);
           if (!r.name.empty()) shm_unlink(r.name.c_str());
           r.fd = -1;
       }
       r.size = 0;
       r.name.clear();
   }

   inline bool shm_region_create(ShmRegion &r, const std::string &name, size_t size)
   {
       shm_region_destroy(r);
       r.name = "/" + name; // shm_open requires a leading '/'
       r.fd   = shm_open(r.name.c_str(), O_CREAT | O_RDWR, 0600);
       if (r.fd < 0) return false;
       if (ftruncate(r.fd, static_cast<off_t>(size)) < 0) { shm_region_destroy(r); return false; }
       r.ptr  = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, r.fd, 0);
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

static inline void ipc_write_line(IpcFd fd, const std::string &msg)
{
    std::string out = msg + "\n";
#if defined(WIN32)
    DWORD written;
    WriteFile(fd, out.c_str(), static_cast<DWORD>(out.size()), &written, nullptr);
#else
    const char *p   = out.c_str();
    size_t      rem = out.size();
    while (rem > 0) {
        ssize_t n = write(fd, p, rem);
        if (n <= 0) break;
        p   += static_cast<size_t>(n);
        rem -= static_cast<size_t>(n);
    }
#endif
}
