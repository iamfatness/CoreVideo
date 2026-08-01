#include "cv-ffmpeg-loader.h"

#include <obs-module.h>

#if defined(_WIN32) && defined(COREVIDEO_HW_ACCEL)

#include <windows.h>
#include <delayimp.h>

#include <string>

// OBS bundles FFmpeg under the standard DLL names, and the Windows loader
// dedups modules by base name — a same-named private runtime can never be
// kept isolated from OBS's inside one process (that collision is exactly how
// OBS 32.2 broke pre-v0.1.30 installs). Our runtime is therefore renamed to
// cv-prefixed DLLs (cvfilter-11.dll, cvutil-60.dll, ... — see
// scripts/New-PrivateFfmpeg.ps1), shipped in the private corevideo-ffmpeg/
// subdirectory, and delay-loaded by absolute path through the hook below.
// Unique names mean the full-featured build (scale_cuda/vpp_qsv) is always
// ours, on every OBS version, and OBS's own FFmpeg never sees it.

#ifndef COREVIDEO_AVFILTER_DLL
#define COREVIDEO_AVFILTER_DLL "cvfilter-11.dll"
#endif
#ifndef COREVIDEO_AVUTIL_DLL
#define COREVIDEO_AVUTIL_DLL "cvutil-60.dll"
#endif

static CvFfmpegRuntime g_runtime = CvFfmpegRuntime::Unavailable;
static std::wstring g_private_dir;

static std::wstring widen(const char *narrow)
{
    std::wstring wide;
    for (const char *p = narrow; *p; ++p)
        wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
    return wide;
}

static std::wstring plugin_directory()
{
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&plugin_directory),
                            &self))
        return {};
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(self, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return {};
    std::wstring dir(path, len);
    size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return {};
    return dir.substr(0, slash + 1);
}

static bool is_delayed_ffmpeg_dll(const char *name)
{
    return _strnicmp(name, "cvfilter", 8) == 0 ||
           _strnicmp(name, "cvutil", 6) == 0;
}

static FARPROC WINAPI cv_delayload_hook(unsigned event, DelayLoadInfo *info)
{
    if (event != dliNotePreLoadLibrary || !info || !info->szDll)
        return nullptr;
    if (!is_delayed_ffmpeg_dll(info->szDll))
        return nullptr;

    // A same-named module already in the process always wins; the loader
    // would dedup to it on any name-based load anyway.
    if (HMODULE loaded = GetModuleHandleA(info->szDll))
        return reinterpret_cast<FARPROC>(loaded);

    if (g_runtime != CvFfmpegRuntime::PrivateCopies || g_private_dir.empty())
        return nullptr; // fall through to default search; init() already warned

    std::wstring path = g_private_dir + widen(info->szDll);
    // DLL_LOAD_DIR makes avfilter's own imports (avcodec/swscale/...) resolve
    // from corevideo-ffmpeg/ first instead of OBS's bin directory.
    HMODULE mod = LoadLibraryExW(path.c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                     LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!mod) {
        blog(LOG_ERROR,
             "[obs-zoom-plugin] Failed to load private FFmpeg runtime '%s' "
             "(error %lu) — hardware acceleration will fail over to CPU",
             info->szDll, GetLastError());
        return nullptr;
    }
    blog(LOG_INFO, "[obs-zoom-plugin] Loaded private FFmpeg runtime '%s'",
         info->szDll);
    return reinterpret_cast<FARPROC>(mod);
}

extern "C" const PfnDliHook __pfnDliNotifyHook2 = cv_delayload_hook;

static bool legacy_loose_dlls_present(const std::wstring &plugin_dir)
{
    static const wchar_t *families[] = {L"avcodec-*.dll",  L"avdevice-*.dll",
                                        L"avfilter-*.dll", L"avformat-*.dll",
                                        L"avutil-*.dll",   L"swresample-*.dll",
                                        L"swscale-*.dll"};
    for (const wchar_t *pattern : families) {
        WIN32_FIND_DATAW fd;
        HANDLE find = FindFirstFileW((plugin_dir + pattern).c_str(), &fd);
        if (find != INVALID_HANDLE_VALUE) {
            FindClose(find);
            return true;
        }
    }
    return false;
}

CvFfmpegRuntime cv_ffmpeg_loader_init()
{
    std::wstring plugin_dir = plugin_directory();
    if (plugin_dir.empty()) {
        blog(LOG_ERROR, "[obs-zoom-plugin] Could not locate plugin directory; "
                        "hardware acceleration disabled");
        g_runtime = CvFfmpegRuntime::Unavailable;
        return g_runtime;
    }

    // Pre-v0.1.30 installs left the FFmpeg runtime loose in obs-plugins/64bit,
    // which collides with OBS 32.2+'s own FFmpeg 8 and stops obs-ffmpeg.dll
    // from loading. We cannot delete them from here (no elevation, possibly
    // already mapped) — shout so support bundles show the cause.
    if (legacy_loose_dlls_present(plugin_dir))
        blog(LOG_ERROR,
             "[obs-zoom-plugin] Legacy FFmpeg DLLs found loose in the OBS "
             "plugin directory. On OBS 32.2+ these prevent OBS's own "
             "obs-ffmpeg module from loading. Run the CoreVideo v0.1.30+ "
             "installer (or delete av*-*.dll / sw*-*.dll from "
             "obs-plugins\\64bit) to fix this.");

    // cv* names are globally unique, so a resident copy can only be one we
    // (or a second CoreVideo init) loaded earlier — reuse it.
    if (GetModuleHandleA(COREVIDEO_AVFILTER_DLL) &&
        GetModuleHandleA(COREVIDEO_AVUTIL_DLL)) {
        g_runtime = CvFfmpegRuntime::ProcessCopies;
        return g_runtime;
    }

    g_private_dir = plugin_dir + L"corevideo-ffmpeg\\";
    DWORD attrs = GetFileAttributesW(
        (g_private_dir + widen(COREVIDEO_AVFILTER_DLL)).c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        g_runtime = CvFfmpegRuntime::PrivateCopies;
    } else {
        blog(LOG_ERROR,
             "[obs-zoom-plugin] Bundled FFmpeg runtime missing at "
             "corevideo-ffmpeg\\%s — hardware acceleration disabled",
             COREVIDEO_AVFILTER_DLL);
        g_runtime = CvFfmpegRuntime::Unavailable;
    }
    return g_runtime;
}

bool cv_ffmpeg_available()
{
    return g_runtime == CvFfmpegRuntime::ProcessCopies ||
           g_runtime == CvFfmpegRuntime::PrivateCopies;
}

const char *cv_ffmpeg_runtime_describe()
{
    switch (g_runtime) {
    case CvFfmpegRuntime::ProcessCopies:
        return "using bundled FFmpeg runtime (already resident)";
    case CvFfmpegRuntime::PrivateCopies:
        return "using bundled FFmpeg runtime from corevideo-ffmpeg/";
    case CvFfmpegRuntime::SystemLinked:
        return "using system-linked FFmpeg";
    default:
        return "no usable FFmpeg runtime — hardware acceleration disabled";
    }
}

#else // !_WIN32 || !COREVIDEO_HW_ACCEL

#ifdef COREVIDEO_HW_ACCEL
static CvFfmpegRuntime g_runtime = CvFfmpegRuntime::SystemLinked;
#else
static CvFfmpegRuntime g_runtime = CvFfmpegRuntime::Unavailable;
#endif

CvFfmpegRuntime cv_ffmpeg_loader_init()
{
    return g_runtime;
}

bool cv_ffmpeg_available()
{
    return g_runtime == CvFfmpegRuntime::SystemLinked;
}

const char *cv_ffmpeg_runtime_describe()
{
    return g_runtime == CvFfmpegRuntime::SystemLinked
               ? "using system-linked FFmpeg"
               : "built without FFmpeg hardware acceleration";
}

#endif
