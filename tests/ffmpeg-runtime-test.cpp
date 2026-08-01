// Verifies the renamed private FFmpeg runtime (see New-PrivateFfmpeg.ps1):
//  - cvfilter loads from corevideo-ffmpeg/ with its whole dependency chain,
//  - the full-featured build is intact (scale_cuda present),
//  - no original-named (av*/sw*) module enters the process, which is the
//    collision that broke OBS 32.2 (issue #165 / PR #162 history).
#include <windows.h>
#include <cstdio>
#include <string>

typedef const void *(*avfilter_get_by_name_fn)(const char *);

static int fail(const char *msg)
{
    std::fprintf(stderr, "FAIL: %s (last error %lu)\n", msg, GetLastError());
    return 1;
}

int main(int argc, char **argv)
{
    std::wstring dir;
    if (argc > 1) {
        std::string narrow(argv[1]);
        dir.assign(narrow.begin(), narrow.end());
    } else {
        wchar_t path[MAX_PATH];
        DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (len == 0 || len >= MAX_PATH)
            return fail("GetModuleFileNameW");
        std::wstring exe(path, len);
        dir = exe.substr(0, exe.find_last_of(L"\\/") + 1) +
              L"corevideo-ffmpeg";
    }

    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((dir + L"\\cvfilter-*.dll").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE)
        return fail("no cvfilter-*.dll in corevideo-ffmpeg directory");
    FindClose(find);

    std::wstring cvfilter = dir + L"\\" + fd.cFileName;
    HMODULE mod = LoadLibraryExW(cvfilter.c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                     LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!mod)
        return fail("LoadLibraryExW(cvfilter) failed");

    auto get_by_name = reinterpret_cast<avfilter_get_by_name_fn>(
        GetProcAddress(mod, "avfilter_get_by_name"));
    if (!get_by_name)
        return fail("avfilter_get_by_name not exported");

    if (!get_by_name("scale_cuda"))
        return fail("scale_cuda filter missing from bundled avfilter");
    std::printf("scale_cuda: present\n");
    std::printf("vpp_qsv: %s\n",
                get_by_name("vpp_qsv") ? "present" : "absent (informational)");

    static const wchar_t *originals[] = {
        L"avcodec-62.dll",  L"avdevice-62.dll", L"avfilter-11.dll",
        L"avformat-62.dll", L"avutil-60.dll",   L"swresample-6.dll",
        L"swscale-9.dll"};
    for (const wchar_t *name : originals) {
        if (GetModuleHandleW(name)) {
            std::fwprintf(stderr,
                          L"FAIL: original-named module %s leaked into the "
                          L"process\n",
                          name);
            return 1;
        }
    }

    std::printf("PASS: private FFmpeg runtime loads fully isolated\n");
    return 0;
}
