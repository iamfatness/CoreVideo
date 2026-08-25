cmake_minimum_required(VERSION 3.16)
project(obs-zoom-plugin VERSION 0.1.13)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(COREVIDEO_BUILD_PLUGIN "Build the OBS plugin module" ON)
option(COREVIDEO_BUILD_ENGINE "Build the ZoomObsEngine helper process" ON)
option(COREVIDEO_BUILD_SIDECAR "Build the CoreVideo Sidecar control app" ON)

include(CTest)

# Embedded credentials (set via -D or CI secrets; empty = user must enter)
set(ZOOM_EMBED_SDK_KEY          "" CACHE STRING "Pre-embedded Zoom SDK key")
set(ZOOM_EMBED_SDK_SECRET       "" CACHE STRING "Pre-embedded Zoom SDK secret")
set(ZOOM_EMBED_JWT_TOKEN        "" CACHE STRING "Pre-embedded Zoom JWT token")
set(ZOOM_EMBED_OAUTH_CLIENT_ID  "y6sIWSwiTZe1JygMx4C9EQ" CACHE STRING "Pre-embedded Zoom OAuth public client ID")
# Default to the production broker start URL so every build — including local
# release builds — carries the full production OAuth identity. A blank broker
# URL makes the plugin honor developer credential overrides from local OBS
# config, which is how dev credentials leaked into tester sessions.
set(ZOOM_EMBED_OAUTH_AUTHORIZATION_URL "https://corevideo.iamfatness.us/oauth/start" CACHE STRING "Pre-embedded Zoom OAuth authorization URL")
set(ZOOM_EMBED_MEETING_SDK_PUBLIC_APP_KEY "y6sIWSwiTZe1JygMx4C9EQ" CACHE STRING "Pre-embedded Zoom Meeting SDK public app key")
configure_file(src/zoom-credentials.h.in
               "${CMAKE_CURRENT_BINARY_DIR}/zoom-credentials.h" @ONLY)

# The project() version above is a development placeholder that only gets
# hand-bumped occasionally; it drifts behind the git tags actual releases
# are cut from (releases are tagged vX.Y.Z and built and published locally
# via scripts/release-local.ps1; CI publishes nothing). Rather
# than keep them in sync by hand, packaging entry points pass the real
# release tag through COREVIDEO_RELEASE_VERSION, and that value - not
# PROJECT_VERSION - is what CoreVideo reports at runtime (logs, diagnostics
# bundle, update-availability check). Local/dev builds that don't set it
# fall back to PROJECT_VERSION.
set(COREVIDEO_RELEASE_VERSION "" CACHE STRING
    "Release tag (e.g. v0.1.27) to report as the runtime plugin version. \
Set by release-local.ps1 from the git tag being packaged; leave blank \
for development builds.")
if(COREVIDEO_RELEASE_VERSION)
    string(REGEX REPLACE "^[vV]" "" COREVIDEO_REPORTED_VERSION "${COREVIDEO_RELEASE_VERSION}")
else()
    set(COREVIDEO_REPORTED_VERSION "${PROJECT_VERSION}")
endif()

configure_file(src/obs-zoom-version.h.in
               "${CMAKE_CURRENT_BINARY_DIR}/obs-zoom-version.h" @ONLY)

if(COREVIDEO_BUILD_PLUGIN)
    set(CMAKE_AUTOMOC ON)
    find_package(LibObs REQUIRED)
    find_package(obs-frontend-api REQUIRED)
    find_package(Qt6 COMPONENTS Core Network Widgets REQUIRED)

    get_target_property(_qt_core_location Qt6::Core IMPORTED_LOCATION_RELEASE)
    if(NOT _qt_core_location)
        get_target_property(_qt_core_location Qt6::Core IMPORTED_LOCATION)
    endif()
    if(_qt_core_location)
        get_filename_component(_qt_bin_dir "${_qt_core_location}" DIRECTORY)
        get_filename_component(_qt_root_dir "${_qt_bin_dir}" DIRECTORY)
        set(COREVIDEO_QT_PLUGIN_DIR "${_qt_root_dir}/plugins")
    endif()
endif()

# Optional FFmpeg hardware-accelerated video conversion
option(ENABLE_FFMPEG_HW_ACCEL
    "Enable FFmpeg hardware-accelerated I420 to NV12 video conversion (LGPL dynamic link required)"
    OFF)

option(COREVIDEO_HW_ACCEL
    "Alias for ENABLE_FFMPEG_HW_ACCEL; kept for documented local build commands"
    OFF)

if(COREVIDEO_HW_ACCEL AND NOT ENABLE_FFMPEG_HW_ACCEL)
    set(ENABLE_FFMPEG_HW_ACCEL ON CACHE BOOL
        "Enable FFmpeg hardware-accelerated I420 to NV12 video conversion (LGPL dynamic link required)"
        FORCE)
endif()

if(ENABLE_FFMPEG_HW_ACCEL)
    # Two ways to find FFmpeg:
    #  (a) FFMPEG_ROOT pointing at a prebuilt dev tree with include/, lib/,
    #      bin/ — used on Windows where pkg-config isn't standard.
    #  (b) pkg-config (libavfilter / libavutil) — Linux + Homebrew on macOS.
    if(DEFINED FFMPEG_ROOT)
        file(TO_CMAKE_PATH "${FFMPEG_ROOT}" FFMPEG_ROOT)
    endif()
    if(DEFINED FFMPEG_ROOT AND EXISTS "${FFMPEG_ROOT}")
        find_path(COREVIDEO_AVFILTER_INC libavfilter/avfilter.h
            HINTS "${FFMPEG_ROOT}/include" REQUIRED NO_DEFAULT_PATH)
        find_library(COREVIDEO_AVFILTER_LIB
            NAMES avfilter avfilter-10 avfilter-9 avfilter-8
            HINTS "${FFMPEG_ROOT}/lib" REQUIRED NO_DEFAULT_PATH)
        find_library(COREVIDEO_AVUTIL_LIB
            NAMES avutil avutil-59 avutil-58 avutil-57
            HINTS "${FFMPEG_ROOT}/lib" REQUIRED NO_DEFAULT_PATH)
        add_library(corevideo_ffmpeg INTERFACE)
        target_include_directories(corevideo_ffmpeg INTERFACE
            "${COREVIDEO_AVFILTER_INC}")
        set(COREVIDEO_FFMPEG_BIN_DIR "${FFMPEG_ROOT}/bin")
        if(WIN32 AND MSVC)
            # OBS bundles FFmpeg under the standard DLL names and the Windows
            # loader dedups modules by base name, so a same-named private
            # runtime can never be isolated from OBS's. Rename our runtime to
            # cv-prefixed names (avfilter-11.dll -> cvfilter-11.dll, patched
            # in the PE name strings — see scripts/New-PrivateFfmpeg.ps1) so
            # the plugin always loads its own full-featured build.
            set(COREVIDEO_FFMPEG_RUNTIME_DIR
                "${CMAKE_CURRENT_BINARY_DIR}/cv-ffmpeg-runtime")
            file(GLOB _cv_patched_check "${COREVIDEO_FFMPEG_RUNTIME_DIR}/cvfilter-*.dll")
            if(NOT _cv_patched_check)
                execute_process(
                    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                        -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/New-PrivateFfmpeg.ps1"
                        -FfmpegBinDir "${COREVIDEO_FFMPEG_BIN_DIR}"
                        -OutDir "${COREVIDEO_FFMPEG_RUNTIME_DIR}"
                    RESULT_VARIABLE _cv_patch_result
                    OUTPUT_VARIABLE _cv_patch_output
                    ERROR_VARIABLE _cv_patch_output)
                if(NOT _cv_patch_result EQUAL 0)
                    message(FATAL_ERROR "Private FFmpeg runtime generation "
                        "failed:\n${_cv_patch_output}")
                endif()
            endif()

            file(GLOB _cv_avfilter_bin "${COREVIDEO_FFMPEG_RUNTIME_DIR}/cvfilter-*.dll")
            file(GLOB _cv_avutil_bin "${COREVIDEO_FFMPEG_RUNTIME_DIR}/cvutil-*.dll")
            if(NOT _cv_avfilter_bin OR NOT _cv_avutil_bin)
                message(FATAL_ERROR "ENABLE_FFMPEG_HW_ACCEL: cvfilter/cvutil "
                    "DLL not found in ${COREVIDEO_FFMPEG_RUNTIME_DIR}")
            endif()
            list(GET _cv_avfilter_bin 0 _cv_avfilter_bin)
            list(GET _cv_avutil_bin 0 _cv_avutil_bin)
            # The prebuilt import libs are dlltool-style long-format archives,
            # which /DELAYLOAD cannot match (LNK4199 leaves the imports
            # static) — and they reference the original DLL names anyway.
            # Generate short-format import libs from the renamed DLLs'
            # export tables so the delayed imports resolve to cv* names.
            get_filename_component(_cv_msvc_bin "${CMAKE_LINKER}" DIRECTORY)
            set(_cv_delaylib_dir "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg-delay-libs")
            file(MAKE_DIRECTORY "${_cv_delaylib_dir}")
            foreach(_cv_dll IN ITEMS "${_cv_avfilter_bin}" "${_cv_avutil_bin}")
                get_filename_component(_cv_dll_we "${_cv_dll}" NAME_WE)
                set(_cv_out_lib "${_cv_delaylib_dir}/${_cv_dll_we}.lib")
                execute_process(
                    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                        -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/New-DelayImportLib.ps1"
                        -DllPath "${_cv_dll}"
                        -DumpbinExe "${_cv_msvc_bin}/dumpbin.exe"
                        -LibExe "${_cv_msvc_bin}/lib.exe"
                        -OutLib "${_cv_out_lib}"
                    RESULT_VARIABLE _cv_gen_result
                    OUTPUT_VARIABLE _cv_gen_output
                    ERROR_VARIABLE _cv_gen_output)
                if(NOT _cv_gen_result EQUAL 0)
                    message(FATAL_ERROR "Import library regeneration failed "
                        "for ${_cv_dll}:\n${_cv_gen_output}")
                endif()
                target_link_libraries(corevideo_ffmpeg INTERFACE "${_cv_out_lib}")
            endforeach()
        else()
            target_link_libraries(corevideo_ffmpeg INTERFACE
                "${COREVIDEO_AVFILTER_LIB}" "${COREVIDEO_AVUTIL_LIB}")
        endif()
        set(COREVIDEO_FFMPEG_TARGET corevideo_ffmpeg)
        message(STATUS "FFmpeg HW accel: using FFMPEG_ROOT=${FFMPEG_ROOT}")
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
            libavfilter>=7
            libavutil>=57
        )
        set(COREVIDEO_FFMPEG_TARGET PkgConfig::FFMPEG)
        message(STATUS "FFmpeg HW accel: using pkg-config")
    endif()
endif()

set(ZOOM_SDK_DIR "third_party/zoom-sdk" CACHE PATH "Zoom Meeting SDK directory")

if(IS_ABSOLUTE "${ZOOM_SDK_DIR}")
    set(ZOOM_SDK_BASE "${ZOOM_SDK_DIR}")
else()
    set(ZOOM_SDK_BASE "${CMAKE_CURRENT_SOURCE_DIR}/${ZOOM_SDK_DIR}")
endif()

if(WIN32 AND NOT EXISTS "${ZOOM_SDK_BASE}/h/zoom_sdk.h")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|AARCH64" AND EXISTS "${ZOOM_SDK_BASE}/zoom-sdk/arm64/h/zoom_sdk.h")
        set(ZOOM_SDK_DIR "${ZOOM_SDK_BASE}/zoom-sdk/arm64" CACHE PATH "Zoom Meeting SDK directory" FORCE)
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8 AND EXISTS "${ZOOM_SDK_BASE}/zoom-sdk/x64/h/zoom_sdk.h")
        set(ZOOM_SDK_DIR "${ZOOM_SDK_BASE}/zoom-sdk/x64" CACHE PATH "Zoom Meeting SDK directory" FORCE)
    elseif(EXISTS "${ZOOM_SDK_BASE}/zoom-sdk/x86/h/zoom_sdk.h")
        set(ZOOM_SDK_DIR "${ZOOM_SDK_BASE}/zoom-sdk/x86" CACHE PATH "Zoom Meeting SDK directory" FORCE)
    endif()
endif()

if(IS_ABSOLUTE "${ZOOM_SDK_DIR}")
    set(ZOOM_SDK_ACTIVE_DIR "${ZOOM_SDK_DIR}")
else()
    set(ZOOM_SDK_ACTIVE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${ZOOM_SDK_DIR}")
endif()

set(ZOOM_SDK_INCLUDE_DIR "${ZOOM_SDK_ACTIVE_DIR}/h")
set(ZOOM_SDK_LIBRARY "${ZOOM_SDK_ACTIVE_DIR}/lib/sdk.lib")

if(WIN32)
    if(EXISTS "${ZOOM_SDK_DIR}/bin/sdk.dll")
        set(ZOOM_SDK_RUNTIME_DIR "${ZOOM_SDK_DIR}/bin")
    elseif(EXISTS "${ZOOM_SDK_BASE}/bin/sdk.dll")
        set(ZOOM_SDK_RUNTIME_DIR "${ZOOM_SDK_BASE}/bin")
    else()
        set(ZOOM_SDK_RUNTIME_DIR "")
    endif()
endif()

# Decide whether we have everything we need to build the engine binary.
# Build-time check: only headers + import lib are required to compile/link.
# Runtime check (bin/sdk.dll): required for any install/package that includes
# ZoomObsEngine. A package without it loads the plugin but cannot join Zoom.
set(BUILD_ZOOM_ENGINE OFF)
if(NOT COREVIDEO_BUILD_ENGINE)
    message(STATUS "ZoomObsEngine build disabled by COREVIDEO_BUILD_ENGINE=OFF")
elseif(WIN32)
    if(EXISTS "${ZOOM_SDK_INCLUDE_DIR}/zoom_sdk.h" AND EXISTS "${ZOOM_SDK_LIBRARY}")
        set(BUILD_ZOOM_ENGINE ON)
    else()
        message(WARNING
            "Zoom SDK not found under '${ZOOM_SDK_DIR}'. "
            "Building plugin DLL only; ZoomObsEngine.exe will not be produced. "
            "Set ZOOM_SDK_DIR to a directory containing h/zoom_sdk.h + lib/sdk.lib.")
    endif()
    if(BUILD_ZOOM_ENGINE AND "${ZOOM_SDK_RUNTIME_DIR}" STREQUAL "")
        message(WARNING
            "Zoom SDK runtime DLLs not found (expected bin/sdk.dll under '${ZOOM_SDK_DIR}'). "
            "ZoomObsEngine.exe can be built for developer checks, but install/package will fail "
            "until the Zoom SDK bin/ runtime DLLs are available.")
    endif()
elseif(APPLE)
    # Resolve against the absolute active dir: if(EXISTS) requires an absolute
    # path, and ZOOM_SDK_DIR is usually the relative "third_party/zoom-sdk".
    if(EXISTS "${ZOOM_SDK_ACTIVE_DIR}/ZoomSDK.framework")
        set(BUILD_ZOOM_ENGINE ON)
    else()
        message(WARNING
            "Zoom SDK framework not found at '${ZOOM_SDK_ACTIVE_DIR}/ZoomSDK.framework'. "
            "Building plugin bundle only; ZoomObsEngine will not be produced.")
    endif()
elseif(UNIX)
    if(EXISTS "${ZOOM_SDK_DIR}/lib/libsdk.so")
        set(BUILD_ZOOM_ENGINE ON)
    else()
        message(WARNING
            "Zoom SDK shared library not found at '${ZOOM_SDK_DIR}/lib/libsdk.so'. "
            "Building plugin only; ZoomObsEngine will not be produced.")
    endif()
endif()

# macOS distributes the Zoom SDK as a .framework bundle.
# Point ZOOM_SDK_FRAMEWORK at the ZoomSDK.framework directory if building on Apple.
set(ZOOM_SDK_FRAMEWORK "${ZOOM_SDK_DIR}/ZoomSDK.framework" CACHE PATH
    "Zoom Meeting SDK framework path (macOS only)")

if(COREVIDEO_BUILD_PLUGIN)
    add_library(obs-zoom-plugin MODULE
        src/plugin-main.cpp
        src/zoom-source.cpp
        src/talkback-tap.cpp
        src/talkback-controller.cpp
        src/zoom-participant-audio-source.cpp
        src/zoom-supersource.cpp
        src/zoom-tiles-background.cpp
        src/zoom-tiles-audio.cpp
        src/zoom-tiles-effect.cpp
        src/zoom-tile-grid.cpp
        src/zoom-engine-client.cpp
        src/speaker-director.cpp
        src/zoom-reconnect.cpp
        src/zoom-settings.cpp
        src/zoom-settings-dialog.cpp
        src/zoom-oauth.cpp
        src/zoom-output-manager.cpp
        src/zoom-output-dialog.cpp
        src/zoom-diagnostics-dialog.cpp
        src/zoom-output-profile.cpp
        src/zoom-iso-recorder.cpp
        src/iso-ffmpeg-pipe.cpp
        src/zoom-iso-panel.cpp
        src/zoom-dock.cpp
        src/zoom-control-server.cpp
        src/zoom-osc-server.cpp
        src/obs-utils.cpp
        src/hw-video-pipeline.cpp
        src/cv-ffmpeg-loader.cpp
        src/cv-widgets.cpp
        src/cv-onboarding.cpp
        src/cv-update-check.cpp
    )

    target_include_directories(obs-zoom-plugin PRIVATE
        src
        "${CMAKE_CURRENT_BINARY_DIR}"
    )
    if(WIN32)
        target_compile_definitions(obs-zoom-plugin PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    target_link_libraries(obs-zoom-plugin PRIVATE
        OBS::libobs
        OBS::obs-frontend-api
        Qt6::Core
        Qt6::Network
        Qt6::Widgets
    )
    if(WIN32)
        target_link_libraries(obs-zoom-plugin PRIVATE crypt32)
        add_executable(CoreVideoOAuthCallback src/oauth-callback-helper.cpp)
        target_link_libraries(CoreVideoOAuthCallback PRIVATE ws2_32 shell32 ole32)
        install(TARGETS CoreVideoOAuthCallback
            RUNTIME DESTINATION "obs-plugins/64bit"
        )
    elseif(APPLE)
        target_link_libraries(obs-zoom-plugin PRIVATE
            "-framework CoreServices"
        )
        add_executable(CoreVideoOAuthCallback MACOSX_BUNDLE
            src/oauth-callback-helper-macos.mm
        )
        target_link_libraries(CoreVideoOAuthCallback PRIVATE
            "-framework Cocoa"
        )
        set_target_properties(CoreVideoOAuthCallback PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST
                "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CoreVideoOAuthCallback-Info.plist.in"
        )
        install(TARGETS CoreVideoOAuthCallback
            BUNDLE DESTINATION "obs-plugins/64bit"
        )
    endif()

    if(ENABLE_FFMPEG_HW_ACCEL)
        target_link_libraries(obs-zoom-plugin PRIVATE ${COREVIDEO_FFMPEG_TARGET})
        target_compile_definitions(obs-zoom-plugin PRIVATE COREVIDEO_HW_ACCEL)
        # Ship the FFmpeg runtime in the private corevideo-ffmpeg/ subdirectory
        # and delay-load it. OBS 32.2+ bundles FFmpeg 8 under the same DLL
        # names; loose copies in obs-plugins/64bit made OBS's own obs-ffmpeg
        # module bind our build and fail to load. cv-ffmpeg-loader.cpp resolves
        # the delayed imports (in-process copies first, else the private dir).
        if(WIN32 AND MSVC AND COREVIDEO_FFMPEG_RUNTIME_DIR)
            file(GLOB COREVIDEO_FFMPEG_DLLS "${COREVIDEO_FFMPEG_RUNTIME_DIR}/*")
            add_custom_command(TARGET obs-zoom-plugin POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "$<TARGET_FILE_DIR:obs-zoom-plugin>/corevideo-ffmpeg"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${COREVIDEO_FFMPEG_DLLS}
                    "$<TARGET_FILE_DIR:obs-zoom-plugin>/corevideo-ffmpeg"
                COMMENT "Copying private FFmpeg runtime for obs-zoom-plugin")
            install(DIRECTORY "${COREVIDEO_FFMPEG_RUNTIME_DIR}/"
                DESTINATION "obs-plugins/64bit/corevideo-ffmpeg")

            get_filename_component(_cv_avfilter_dll "${_cv_avfilter_bin}" NAME)
            get_filename_component(_cv_avutil_dll "${_cv_avutil_bin}" NAME)
            target_compile_definitions(obs-zoom-plugin PRIVATE
                "COREVIDEO_AVFILTER_DLL=\"${_cv_avfilter_dll}\""
                "COREVIDEO_AVUTIL_DLL=\"${_cv_avutil_dll}\"")
            target_link_options(obs-zoom-plugin PRIVATE
                "/DELAYLOAD:${_cv_avfilter_dll}"
                "/DELAYLOAD:${_cv_avutil_dll}")
            target_link_libraries(obs-zoom-plugin PRIVATE delayimp)
        endif()
    endif()

    if(APPLE)
        target_link_libraries(obs-zoom-plugin PRIVATE
            "-framework CoreFoundation"
            "-framework Foundation"
            "-framework Security"   # Keychain token storage (zoom-settings.cpp)
        )
        # Symbols provided by OBS at plugin load time; no link-time libobs
        # needed. The SHELL: prefix is required so CMake passes these as TWO
        # separate linker arguments ("-undefined" "dynamic_lookup"); without it
        # the space-containing string is forwarded as a single argument and the
        # flag never takes effect (obs_*/blog/config_* stay undefined at link).
        target_link_options(obs-zoom-plugin PRIVATE "SHELL:-undefined dynamic_lookup")
    endif()

    set_target_properties(obs-zoom-plugin PROPERTIES PREFIX "")
endif()

set(ENGINE_SOURCES
    engine/src/main.cpp
    engine/src/engine-video.cpp
    engine/src/engine-share.cpp
    engine/src/engine-audio.cpp
    engine/src/engine-talkback.cpp
)
set(ENGINE_INCLUDES src engine/src ${ZOOM_SDK_INCLUDE_DIR})

if(BUILD_ZOOM_ENGINE)
    if(WIN32)
        add_executable(ZoomObsEngine ${ENGINE_SOURCES})
        target_include_directories(ZoomObsEngine PRIVATE ${ENGINE_INCLUDES})
        target_link_libraries(ZoomObsEngine PRIVATE
            ws2_32 crypt32 winhttp shlwapi
            ${ZOOM_SDK_LIBRARY}
        )
        set_target_properties(ZoomObsEngine PROPERTIES WIN32_EXECUTABLE FALSE)
        if(NOT "${ZOOM_SDK_RUNTIME_DIR}" STREQUAL "")
            add_custom_command(TARGET ZoomObsEngine POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${ZOOM_SDK_RUNTIME_DIR}"
                    "$<TARGET_FILE_DIR:ZoomObsEngine>"
                COMMENT "Copying Zoom SDK runtime files to output directory"
            )
            install(DIRECTORY "${ZOOM_SDK_RUNTIME_DIR}/"
                DESTINATION "obs-plugins/64bit/zoom-runtime"
            )
        else()
            install(CODE [[
                message(FATAL_ERROR
                    "Zoom SDK runtime DLLs are missing. A CoreVideo package that includes "
                    "ZoomObsEngine must also include sdk.dll and the Zoom SDK bin/ runtime files. "
                    "Set ZOOM_SDK_DIR to the Zoom Meeting SDK architecture directory that contains bin/sdk.dll.")
            ]])
        endif()
        install(TARGETS ZoomObsEngine
            RUNTIME DESTINATION "obs-plugins/64bit/zoom-runtime"
        )

        add_executable(ZoomSdkAuthProbe tools/zoom-sdk-auth-probe.cpp)
        target_include_directories(ZoomSdkAuthProbe PRIVATE ${ZOOM_SDK_INCLUDE_DIR})
        target_link_libraries(ZoomSdkAuthProbe PRIVATE ${ZOOM_SDK_LIBRARY})
        set_target_properties(ZoomSdkAuthProbe PROPERTIES WIN32_EXECUTABLE FALSE)
    elseif(APPLE)
        # SCAFFOLD ONLY — the macOS engine is NOT the C++ engine. The macOS Zoom
        # Meeting SDK (ZoomSDK.framework) is a pure Objective-C framework and
        # exposes none of the ZOOMSDK:: C++ interfaces the shared ENGINE_SOURCES
        # are written against (no zoom_sdk.h, no IMeetingService, ...). Those
        # sources cannot compile here. Build a small Objective-C++ scaffold that
        # links the real framework (validating the SDK fetch + link path) and
        # fails loudly at runtime on any meeting request. A functional macOS
        # engine requires an Objective-C++ rewrite — tracked as a PR gap.
        #
        # -F<dir> adds the framework search path for both `#import <ZoomSDK/...>`
        # header resolution and `-framework ZoomSDK` linking.
        add_executable(ZoomObsEngine engine/src/main-macos.mm)
        target_include_directories(ZoomObsEngine PRIVATE src engine/src)
        # -F must be on BOTH the compile line (so `#import <ZoomSDK/ZoomSDK.h>`
        # resolves via the framework's Headers dir) and the link line (so
        # `-framework ZoomSDK` finds the framework binary).
        target_compile_options(ZoomObsEngine PRIVATE "-F${ZOOM_SDK_ACTIVE_DIR}")
        target_link_libraries(ZoomObsEngine PRIVATE
            "-F${ZOOM_SDK_ACTIVE_DIR}"
            "-framework ZoomSDK"
            "-framework Foundation"
        )
        # rpath so the linker/loader can resolve ZoomSDK.framework from where the
        # SDK was fetched (CI compile/link validation only; not a shippable rpath).
        set_target_properties(ZoomObsEngine PROPERTIES
            BUILD_WITH_INSTALL_RPATH OFF
            INSTALL_RPATH "@loader_path/zoom-runtime"
        )
        # Relative DESTINATION (resolved against the install prefix). An
        # absolute "${CMAKE_INSTALL_PREFIX}/..." expands to the configure-time
        # default (/usr/local) and ignores `cmake --install --prefix`, which
        # fails with "cannot create directory: /usr/local/...".
        install(TARGETS ZoomObsEngine
            RUNTIME DESTINATION "obs-plugins/64bit"
        )
    elseif(UNIX)
        add_executable(ZoomObsEngine ${ENGINE_SOURCES})
        target_include_directories(ZoomObsEngine PRIVATE ${ENGINE_INCLUDES})
        target_link_libraries(ZoomObsEngine PRIVATE
            ${ZOOM_SDK_DIR}/lib/libsdk.so
            rt
        )
        install(TARGETS ZoomObsEngine
            RUNTIME DESTINATION "obs-plugins/64bit"
        )
    endif()
endif()

if(COREVIDEO_BUILD_PLUGIN)
    install(TARGETS obs-zoom-plugin
        LIBRARY DESTINATION "obs-plugins/64bit"
    )
    if(WIN32 AND _qt_bin_dir)
        foreach(_qt_runtime
            Qt6Core.dll
            Qt6Gui.dll
            Qt6Network.dll
            Qt6Widgets.dll
        )
            if(EXISTS "${_qt_bin_dir}/${_qt_runtime}")
                install(FILES "${_qt_bin_dir}/${_qt_runtime}"
                    DESTINATION "obs-plugins/64bit"
                )
            else()
                message(WARNING "Qt runtime DLL not found: ${_qt_bin_dir}/${_qt_runtime}")
            endif()
        endforeach()
    endif()
    if(WIN32 AND COREVIDEO_QT_PLUGIN_DIR AND EXISTS "${COREVIDEO_QT_PLUGIN_DIR}/tls")
        install(FILES
            "${COREVIDEO_QT_PLUGIN_DIR}/tls/qcertonlybackend.dll"
            "${COREVIDEO_QT_PLUGIN_DIR}/tls/qschannelbackend.dll"
            DESTINATION "obs-plugins/64bit/plugins/tls"
        )
    endif()
    if(WIN32 AND COREVIDEO_QT_PLUGIN_DIR AND EXISTS "${COREVIDEO_QT_PLUGIN_DIR}/platforms/qwindows.dll")
        install(FILES
            "${COREVIDEO_QT_PLUGIN_DIR}/platforms/qwindows.dll"
            DESTINATION "obs-plugins/64bit/plugins/platforms"
        )
    endif()
    install(DIRECTORY data/
        DESTINATION "data/obs-plugins/obs-zoom-plugin"
    )
endif()

# CPack packaging
# Produces a ZIP that users extract directly into their OBS Studio folder.
#
# Windows: extract into  C:\Program Files\obs-studio\
# macOS:   extract into  /Applications/OBS.app/Contents/Resources/
#          or ~/Library/Application Support/obs-studio/
#
# The ZIP contains the standard OBS plugin layout:
#   obs-plugins/64bit/<plugin>.(dll|so)
#   data/obs-plugins/obs-zoom-plugin/locale/en-US.ini
if(COREVIDEO_BUILD_SIDECAR)
    set(COREVIDEO_SIDECAR_INSTALL_DIR "obs-plugins/64bit" CACHE STRING
        "Install directory for the CoreVideo Sidecar executable")
    add_subdirectory(sidecar)
endif()

if(BUILD_TESTING)
    add_executable(CoreVideoOutputHealthTest
        tests/output-health-test.cpp
    )
    target_include_directories(CoreVideoOutputHealthTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoOutputHealth
             COMMAND CoreVideoOutputHealthTest)

    if(WIN32 AND MSVC AND COREVIDEO_FFMPEG_RUNTIME_DIR AND COREVIDEO_BUILD_PLUGIN)
        add_executable(CoreVideoFfmpegRuntimeTest
            tests/ffmpeg-runtime-test.cpp
        )
        add_dependencies(CoreVideoFfmpegRuntimeTest obs-zoom-plugin)
        add_test(NAME CoreVideoFfmpegRuntime
                 COMMAND CoreVideoFfmpegRuntimeTest)

        # Compiles the real loader (blog stubbed in the test) against the
        # staged runtime; libobs supplies headers only, never a link dep.
        add_executable(CoreVideoFfmpegPreloadTest
            tests/ffmpeg-preload-test.cpp
            src/cv-ffmpeg-loader.cpp
        )
        target_include_directories(CoreVideoFfmpegPreloadTest PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            $<TARGET_PROPERTY:OBS::libobs,INTERFACE_INCLUDE_DIRECTORIES>
        )
        target_compile_definitions(CoreVideoFfmpegPreloadTest PRIVATE
            COREVIDEO_HW_ACCEL
            "COREVIDEO_AVFILTER_DLL=\"${_cv_avfilter_dll}\""
            "COREVIDEO_AVUTIL_DLL=\"${_cv_avutil_dll}\"")
        add_dependencies(CoreVideoFfmpegPreloadTest obs-zoom-plugin)
        add_test(NAME CoreVideoFfmpegPreload
                 COMMAND CoreVideoFfmpegPreloadTest)
    endif()

    # Self-spawning: the test binary is its own slow/fast/wedged child, so
    # it needs no external programs. Qt-free by design (see the header).
    add_executable(CoreVideoIsoFfmpegPipeTest
        tests/iso-ffmpeg-pipe-test.cpp
        src/iso-ffmpeg-pipe.cpp
    )
    target_include_directories(CoreVideoIsoFfmpegPipeTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoIsoFfmpegPipe
             COMMAND CoreVideoIsoFfmpegPipeTest)

    add_executable(CoreVideoIsoEncoderPlanTest
        tests/iso-encoder-plan-test.cpp
    )
    target_include_directories(CoreVideoIsoEncoderPlanTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoIsoEncoderPlan
             COMMAND CoreVideoIsoEncoderPlanTest)

    add_executable(CoreVideoSpeakerDirectorTest
        tests/speaker-director-test.cpp
        src/speaker-director.cpp
    )
    target_include_directories(CoreVideoSpeakerDirectorTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoSpeakerDirector
             COMMAND CoreVideoSpeakerDirectorTest)

    add_executable(CoreVideoJoinInputTest
        tests/join-input-test.cpp
        src/obs-utils.cpp
    )
    target_include_directories(CoreVideoJoinInputTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoJoinInput
             COMMAND CoreVideoJoinInputTest)

    add_executable(CoreVideoVersionCompareTest
        tests/version-compare-test.cpp
    )
    target_include_directories(CoreVideoVersionCompareTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoVersionCompare
             COMMAND CoreVideoVersionCompareTest)

    # IPC/SHM hardening policy: heartbeat timeout, SHM generation guard, and
    # source cap (plus Windows pipe line-I/O coverage on Windows).
    add_executable(CoreVideoIpcHardeningTest
        tests/ipc-hardening-test.cpp
    )
    target_include_directories(CoreVideoIpcHardeningTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoIpcHardening
             COMMAND CoreVideoIpcHardeningTest)

    # Plugin->engine command routing. Pins that "unsubscribe" does not route to
    # the subscribe branch (it did, because "unsubscribe" contains "subscribe"
    # and the dispatch tested substrings in source order).
    add_executable(CoreVideoEngineCommandTest
        tests/engine-command-test.cpp
    )
    target_include_directories(CoreVideoEngineCommandTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoEngineCommand
             COMMAND CoreVideoEngineCommandTest)

    # The shared seqlock frame reader: tear-free copies plus the region
    # generation handling that keeps a resized feed from freezing. Uses real
    # shared memory and a concurrent writer thread, so it needs Threads (and
    # librt for shm_open on Linux).
    find_package(Threads REQUIRED)
    add_executable(CoreVideoShmFrameReaderTest
        tests/shm-frame-reader-test.cpp
    )
    target_include_directories(CoreVideoShmFrameReaderTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(CoreVideoShmFrameReaderTest PRIVATE Threads::Threads)
    if(NOT WIN32 AND NOT APPLE)
        target_link_libraries(CoreVideoShmFrameReaderTest PRIVATE rt)
    endif()
    add_test(NAME CoreVideoShmFrameReader
             COMMAND CoreVideoShmFrameReaderTest)

    # Release-before-resubscribe: the rule that stops a re-pointed source_uuid
    # from being read through a mapping of the region the engine just rebuilt
    # (the 2026-08-10 white-flash-on-speaker-change incident). Uses real shared
    # memory, so it needs librt for shm_open on Linux.
    add_executable(CoreVideoShmResubscribeTest
        tests/shm-resubscribe-test.cpp
    )
    target_include_directories(CoreVideoShmResubscribeTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    if(NOT WIN32 AND NOT APPLE)
        target_link_libraries(CoreVideoShmResubscribeTest PRIVATE rt)
    endif()
    add_test(NAME CoreVideoShmResubscribe
             COMMAND CoreVideoShmResubscribeTest)

    # Monotonic SHM region generations (src/shm-generation.h): the counter that
    # keeps a resize off a name another process may still map must survive the
    # per-source target struct being destroyed on a re-subscribe. Third
    # production incident from that one defect. Threads for the concurrency
    # case, real shared memory on Windows for the ACCESS_DENIED reproduction.
    add_executable(CoreVideoShmGenerationTest
        tests/shm-generation-test.cpp
    )
    target_include_directories(CoreVideoShmGenerationTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(CoreVideoShmGenerationTest PRIVATE Threads::Threads)
    if(NOT WIN32 AND NOT APPLE)
        target_link_libraries(CoreVideoShmGenerationTest PRIVATE rt)
    endif()
    add_test(NAME CoreVideoShmGeneration
             COMMAND CoreVideoShmGenerationTest)

    # The last residual of the same defect class: the generation table is
    # per engine PROCESS, so an engine restart puts the next create back on the
    # legacy unsuffixed name whatever the dead engine had climbed to. Pins why
    # the plugin must drop every mapping when a new engine comes up, and why
    # releasing only the region a re-subscribe touches leaves audio blocked.
    # Real shared memory on Windows for the ACCESS_DENIED reproduction.
    add_executable(CoreVideoShmEngineRestartTest
        tests/shm-engine-restart-test.cpp
    )
    target_include_directories(CoreVideoShmEngineRestartTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    if(NOT WIN32 AND NOT APPLE)
        target_link_libraries(CoreVideoShmEngineRestartTest PRIVATE rt)
    endif()
    add_test(NAME CoreVideoShmEngineRestart
             COMMAND CoreVideoShmEngineRestartTest)

    # The other half of surviving an engine restart (src/audio-subscription-
    # state.h). Releasing the mapping unblocks the new engine's first create;
    # something still has to ask it for the feed. Video is re-subscribed by
    # ZoomOutputManager::resubscribe_all(), which iterates ZoomSource only — the
    # dedicated CoreVideo audio sources are not in it, and a `subscribed` flag
    # left over from the dead engine made their one re-subscribe path a no-op,
    # so they went silent for the session. Header-only logic; no Qt/OBS.
    add_executable(CoreVideoAudioSubscriptionStateTest
        tests/audio-subscription-state-test.cpp
    )
    target_include_directories(CoreVideoAudioSubscriptionStateTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoAudioSubscriptionState
             COMMAND CoreVideoAudioSubscriptionStateTest)

    # Centralized Zoom auth/join decision path + error catalog (issue #89).
    # Header-only logic; no Qt/OBS/SDK dependencies, no secrets.
    add_executable(CoreVideoJoinDecisionTest
        tests/join-decision-test.cpp
    )
    target_include_directories(CoreVideoJoinDecisionTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoJoinDecision
             COMMAND CoreVideoJoinDecisionTest)

    # IPC line-I/O framing and write-failure semantics (POSIX socket-based test).
    if(NOT WIN32)
        add_executable(CoreVideoIpcLineIoTest
            tests/ipc-line-io-test.cpp
        )
        target_include_directories(CoreVideoIpcLineIoTest PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
        )
        add_test(NAME CoreVideoIpcLineIo
                 COMMAND CoreVideoIpcLineIoTest)
    endif()

    # Reconnect back-off schedule and generation-guard state machine. Pure
    # C++, no Qt/OBS/Zoom SDK dependency, so this builds and runs on every
    # platform/job (including the Linux GCC portability job).
    add_executable(CoreVideoReconnectBackoffTest
        tests/zoom-reconnect-backoff-test.cpp
    )
    target_include_directories(CoreVideoReconnectBackoffTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoReconnectBackoff
             COMMAND CoreVideoReconnectBackoffTest)

    # Retry rules for the Zoom SDK init handshake when an orphaned engine still
    # holds the SDK (SDKERR_OTHER_SDK_INSTANCE_RUNNING). Pure logic, no Qt/OBS.
    add_executable(CoreVideoSdkInitRetryTest
        tests/zoom-sdk-init-retry-test.cpp
    )
    target_include_directories(CoreVideoSdkInitRetryTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoSdkInitRetry
             COMMAND CoreVideoSdkInitRetryTest)

    add_executable(CoreVideoReconnectScheduleTest
        tests/zoom-reconnect-schedule-test.cpp
    )
    target_include_directories(CoreVideoReconnectScheduleTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoReconnectSchedule
             COMMAND CoreVideoReconnectScheduleTest)

    # CoreVideo Tiles: the pure units behind the tile wall. None of these link
    # Qt, OBS, or the Zoom SDK, so they run in every job.
    add_executable(CoreVideoTileClockProbeTest
        tests/tile-clock-probe-test.cpp
        src/tile-clock-probe.cpp
    )
    target_include_directories(CoreVideoTileClockProbeTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoTileClockProbe
             COMMAND CoreVideoTileClockProbeTest)

    add_executable(CoreVideoTileGridTest
        tests/tile-grid-test.cpp
        src/zoom-tile-grid.cpp
    )
    target_include_directories(CoreVideoTileGridTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoTileGrid
             COMMAND CoreVideoTileGridTest)

    # Tile-slot reassignment ordering — exercised with concurrent threads.
    add_executable(CoreVideoTileSlotTest
        tests/tile-slot-test.cpp
    )
    target_include_directories(CoreVideoTileSlotTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(CoreVideoTileSlotTest PRIVATE Threads::Threads)
    add_test(NAME CoreVideoTileSlot
             COMMAND CoreVideoTileSlotTest)

    add_executable(CoreVideoTileFillTest
        tests/tile-fill-test.cpp
    )
    target_include_directories(CoreVideoTileFillTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoTileFill
             COMMAND CoreVideoTileFillTest)

    # When a tile's plane textures must be recreated, and when their pixels are
    # actually stale. Pure rules, so they are testable without a GPU device.
    add_executable(CoreVideoTileTextureTest
        tests/tile-texture-test.cpp
    )
    target_include_directories(CoreVideoTileTextureTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoTileTexture
             COMMAND CoreVideoTileTextureTest)

    # Border width / corner radius clamping against the tile they are drawn on.
    # Both are operator-reachable and both degenerate the tile if trusted.
    add_executable(CoreVideoTileBorderTest
        tests/tile-border-test.cpp
    )
    target_include_directories(CoreVideoTileBorderTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoTileBorder
             COMMAND CoreVideoTileBorderTest)

    # One tile's critically damped spring. Pure maths; no OBS, no GPU.
    add_executable(CoreVideoTileMotionTest
        tests/tile-motion-test.cpp
    )
    target_include_directories(CoreVideoTileMotionTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoTileMotion
             COMMAND CoreVideoTileMotionTest)

    # Identity-keyed animation state: reflow, blips, phases, exit invariants.
    add_executable(CoreVideoTileAnimatorTest
        tests/zoom-tile-animator-test.cpp
    )
    target_include_directories(CoreVideoTileAnimatorTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoTileAnimator
             COMMAND CoreVideoTileAnimatorTest)

    # Precedence between a source's own speaker properties and the dock-owned
    # global settings. A source that carries nothing must not overwrite them —
    # see the 2026-08-11 live-show incident described in the test.
    add_executable(CoreVideoSpeakerSettingsMergeTest
        tests/speaker-settings-merge-test.cpp
    )
    target_include_directories(CoreVideoSpeakerSettingsMergeTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoSpeakerSettingsMerge
             COMMAND CoreVideoSpeakerSettingsMergeTest)

    # Which frames the hidden director-preview subscription may put on air. It
    # warms the next speaker only; a frame from anyone else is a stale in-flight
    # frame and publishing it is the wrong face on a live broadcast — see the
    # 2026-08-11 incident described in the test.
    add_executable(CoreVideoDirectorPreviewFrameGuardTest
        tests/director-preview-frame-guard-test.cpp
    )
    target_include_directories(CoreVideoDirectorPreviewFrameGuardTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoDirectorPreviewFrameGuard
             COMMAND CoreVideoDirectorPreviewFrameGuardTest)

    # How long the preview slot is kept alive AFTER a cut. Releasing it in the
    # same breath as the re-subscribe left nothing publishing for the 735-1277ms
    # the engine needs to rebuild the main region — the 2026-08-16 flash.
    add_executable(CoreVideoDirectorHandoverTest
        tests/director-handover-test.cpp
    )
    target_include_directories(CoreVideoDirectorHandoverTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoDirectorHandover
             COMMAND CoreVideoDirectorHandoverTest)

    # Ramps in the first buffer after true digital silence (a bot's mic
    # streaming zero-valued PCM between spoken turns, not a dropped
    # callback) instead of jumping straight to full amplitude — the
    # 2026-08-18 "clicks/pops on the active speaker feed" report.
    add_executable(CoreVideoAudioSilenceFadeTest
        tests/audio-silence-fade-test.cpp
    )
    target_include_directories(CoreVideoAudioSilenceFadeTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoAudioSilenceFade
             COMMAND CoreVideoAudioSilenceFadeTest)

    # Backfills silence into an ISO WAV across gaps so its own timeline
    # tracks real elapsed time instead of shrinking by every silent
    # stretch — 2026-08-21 design review ("does ZoomISO handle variable
    # fps / stay in sync with audio").
    add_executable(CoreVideoIsoAudioGapFillTest
        tests/iso-audio-gap-fill-test.cpp
    )
    target_include_directories(CoreVideoIsoAudioGapFillTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoIsoAudioGapFill
             COMMAND CoreVideoIsoAudioGapFillTest)

    # Paces ISO video to a fixed cadence before ffmpeg's raw pipe, since
    # -use_wallclock_as_timestamps was confirmed (ffprobe -show_frames,
    # 2026-08-21) to have no effect on this project's ffmpeg build's
    # rawvideo demuxer. See src/iso-video-pacer.h.
    add_executable(CoreVideoIsoVideoPacerTest
        tests/iso-video-pacer-test.cpp
    )
    target_include_directories(CoreVideoIsoVideoPacerTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoIsoVideoPacer
             COMMAND CoreVideoIsoVideoPacerTest)

    # The SDK does not always honour the full-range colorspace the engine asks
    # for: 16 of 15,203 frames from a live meeting arrived limited-range, on 5
    # of 6 participants, and rendered unexpanded as a one-frame brightness pop
    # (the "gamma flash"). See src/i420-range-expand.h.
    add_executable(CoreVideoI420RangeExpandTest
        tests/i420-range-expand-test.cpp
    )
    target_include_directories(CoreVideoI420RangeExpandTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoI420RangeExpand
             COMMAND CoreVideoI420RangeExpandTest)

    # A Zoom waiting room is an open-ended wait on a human, not a stalled join.
    # Charged against the join deadline it auto-left a live meeting after 114s
    # (2026-08-22). See src/join-watchdog.h.
    add_executable(CoreVideoJoinWatchdogTest
        tests/join-watchdog-test.cpp
    )
    target_include_directories(CoreVideoJoinWatchdogTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoJoinWatchdog
             COMMAND CoreVideoJoinWatchdogTest)

    # The talkback probe's test tone. Phase must carry across buffers: a restart
    # every buffer is an audible click that would be misread as a transport
    # fault. See src/talkback-tone.h.
    add_executable(CoreVideoTalkbackToneTest
        tests/talkback-tone-test.cpp
    )
    target_include_directories(CoreVideoTalkbackToneTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoTalkbackTone
             COMMAND CoreVideoTalkbackToneTest)

    # libobs planar float -> Zoom interleaved int16. Every failure mode here is
    # silent (noise or silence, never an error), so both the clamp and the
    # channel order are pinned. See src/talkback-pcm.h.
    add_executable(CoreVideoTalkbackPcmTest
        tests/talkback-pcm-test.cpp
    )
    target_include_directories(CoreVideoTalkbackPcmTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoTalkbackPcm
             COMMAND CoreVideoTalkbackPcmTest)

    # The talkback ring, driven in reverse (plugin writes, engine reads).
    # Uses real concurrency to prove the seqlock and the free-running indices
    # hold with the roles swapped. See src/talkback-ring.h.
    add_executable(CoreVideoTalkbackRingTest
        tests/talkback-ring-test.cpp
    )
    target_include_directories(CoreVideoTalkbackRingTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(CoreVideoTalkbackRingTest PRIVATE Threads::Threads)
    add_test(NAME CoreVideoTalkbackRing
             COMMAND CoreVideoTalkbackRingTest)

    # When a talkback key may stay open, and every way it must close. Talkback
    # is the one feature where a bug is heard outside the control room, so the
    # closing direction is pinned exhaustively. See src/talkback-key.h.
    add_executable(CoreVideoTalkbackKeyTest
        tests/talkback-key-test.cpp
    )
    target_include_directories(CoreVideoTalkbackKeyTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoTalkbackKey
             COMMAND CoreVideoTalkbackKeyTest)

    # Talkback audio must never reach program or ISO. Source-level invariant:
    # the tap may observe, never route. See tests/talkback-isolation-test.cpp
    # for why this is a source scan rather than a runtime test.
    add_executable(CoreVideoTalkbackIsolationTest
        tests/talkback-isolation-test.cpp
    )
    add_test(NAME CoreVideoTalkbackIsolation
             COMMAND CoreVideoTalkbackIsolationTest
                     "${CMAKE_CURRENT_SOURCE_DIR}/src/talkback-tap.cpp")

    # Which subsystem a CreateChannel response belongs to. Two now ask (the
    # probe and the talkback session) and the response carries no clue; routing
    # it wrong cuts the director off mid-sentence or hands the session a
    # channel tick() is about to destroy. See src/talkback-channel-owner.h.
    add_executable(CoreVideoTalkbackChannelOwnerTest
        tests/talkback-channel-owner-test.cpp
    )
    target_include_directories(CoreVideoTalkbackChannelOwnerTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoTalkbackChannelOwner
             COMMAND CoreVideoTalkbackChannelOwnerTest)

    # The master clock every audio buffer is stamped from. Wall-clock arrival
    # stamps let IPC jitter reach OBS, which resampled continuously — the
    # 2026-08-16 "audio is very bad" report.
    add_executable(CoreVideoAudioTimelineTest
        tests/audio-timeline-test.cpp
    )
    target_include_directories(CoreVideoAudioTimelineTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoAudioTimeline
             COMMAND CoreVideoAudioTimelineTest)

    # Index arithmetic of the audio SHM ring. The single-slot mailbox it
    # replaces dropped 10ms of audio on every reader stall, silently.
    add_executable(CoreVideoAudioRingTest
        tests/audio-ring-test.cpp
    )
    target_include_directories(CoreVideoAudioRingTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoAudioRing
             COMMAND CoreVideoAudioRingTest)

    # Coalescing rules of the reader-thread -> media-lane queue. Inline media
    # dispatch let a 1080p video backlog delay audio events ~1s and starve the
    # ring's edge wakeups; the queue makes that backlog impossible.
    add_executable(CoreVideoMediaEventQueueTest
        tests/media-event-queue-test.cpp
    )
    target_include_directories(CoreVideoMediaEventQueueTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoMediaEventQueue
             COMMAND CoreVideoMediaEventQueueTest)

    # Which participants a video-assignment picker offers. The rule that the
    # picker's own current selection is never hidden is the load-bearing one:
    # dropping it would unbind the source when somebody's camera goes off.
    add_executable(CoreVideoParticipantFilterTest
        tests/participant-filter-test.cpp
    )
    target_include_directories(CoreVideoParticipantFilterTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoParticipantFilter
             COMMAND CoreVideoParticipantFilterTest)

    # Measures the luma range actually delivered by the SDK, so the full-range
    # vs limited-range question is settled by measurement, not assumption.
    add_executable(CoreVideoLumaRangeProbeTest
        tests/luma-range-probe-test.cpp
    )
    target_include_directories(CoreVideoLumaRangeProbeTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoLumaRangeProbe
             COMMAND CoreVideoLumaRangeProbeTest)

    # Per-slot left/right crop composed with the cover-crop. The composition
    # order is the point: the slot crop narrows the usable source first, and the
    # cover-crop then fills the tile from what is left. Reversing the two cuts a
    # different part of the frame and only shows on an off-centre subject, so it
    # is pinned by a test rather than by eye. zoom-tile-grid.cpp is needed
    # because solve_cover_crop lives there.
    add_executable(CoreVideoTileCropTest
        tests/tile-crop-test.cpp
        src/zoom-tile-grid.cpp
    )
    target_include_directories(CoreVideoTileCropTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoTileCrop
             COMMAND CoreVideoTileCropTest)

    # Tile shape presets and the gutter/margin percentages. The exactness of
    # the default spacing is the point: it has to reproduce the wall's shipped
    # canvas_height / 135.0 bit-for-bit, or every existing scene reflows.
    # zoom-tile-grid.cpp is needed because the crop registration check at the
    # end calls solve_slot_crop, which calls solve_cover_crop.
    add_executable(CoreVideoTileShapeTest
        tests/tile-shape-test.cpp
        src/zoom-tile-grid.cpp
    )
    target_include_directories(CoreVideoTileShapeTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoTileShape
             COMMAND CoreVideoTileShapeTest)

    # The outer glow's quad geometry: a tile rect expanded by the glow size and
    # clamped to the canvas. The clamping is the point — a tile against the
    # canvas edge produces an expanded rect that runs off it, and the tile's
    # position inside the clamped quad is what keeps the halo registered against
    # the tile rather than sliding by however much was clipped.
    # zoom-tile-grid.cpp is needed because the invariant sweep at the end runs
    # over real solved walls via solve_tile_grid / snap_tile_grid_even.
    add_executable(CoreVideoTileGlowTest
        tests/tile-glow-test.cpp
        src/zoom-tile-grid.cpp
    )
    target_include_directories(CoreVideoTileGlowTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoTileGlow
             COMMAND CoreVideoTileGlowTest)

    # Per-participant audio reconciliation. Pure — no libobs — because every
    # action it emits mutates the operator's scene collection, and the two
    # rules that matter (never duplicate a participant another Tiles source
    # owns, never delete anyone) have to be provable off-rig.
    add_executable(CoreVideoTilesAudioPlanTest
        tests/tiles-audio-plan-test.cpp
    )
    target_include_directories(CoreVideoTilesAudioPlanTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoTilesAudioPlan
             COMMAND CoreVideoTilesAudioPlanTest)

    # Which missing effect handles are fatal and which only cost a feature.
    # Pure policy — no libobs — because the consequence is operational, not
    # graphical: the DLL and data/effects/corevideo-tiles.effect install
    # independently, and treating a stale effect file as fatal would cost a live
    # operator the whole wall rather than the one feature the old file predates.
    add_executable(CoreVideoTilesEffectPolicyTest
        tests/tiles-effect-policy-test.cpp
    )
    target_include_directories(CoreVideoTilesEffectPolicyTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoTilesEffectPolicy
             COMMAND CoreVideoTilesEffectPolicyTest)

    # Retry pacing for the tiles silent-slot sweep: the sweep interval and the
    # per-slot exponential backoff / attempt cap that stop an unbounded
    # renderer create-destroy loop in the engine.
    add_executable(CoreVideoTileRetryTest
        tests/tile-retry-test.cpp
    )
    target_include_directories(CoreVideoTileRetryTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoTileRetry
             COMMAND CoreVideoTileRetryTest)

    # OSC wire-format and TCP control-request parsing. These use QByteArray /
    # QJsonObject / QString, so they only build where Qt6::Core is available
    # (i.e. when COREVIDEO_BUILD_PLUGIN found it above). Guarded rather than
    # required so the Linux portability job and any Qt-less configuration
    # keep working unchanged.
    if(TARGET Qt6::Core)
        add_executable(CoreVideoOscWireTest
            tests/zoom-osc-wire-test.cpp
        )
        target_include_directories(CoreVideoOscWireTest PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
        )
        target_link_libraries(CoreVideoOscWireTest PRIVATE Qt6::Core)
        add_test(NAME CoreVideoOscWire
                 COMMAND CoreVideoOscWireTest)

        add_executable(CoreVideoControlParseTest
            tests/zoom-control-parse-test.cpp
        )
        target_include_directories(CoreVideoControlParseTest PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
        )
        target_link_libraries(CoreVideoControlParseTest PRIVATE Qt6::Core)
        add_test(NAME CoreVideoControlParse
                 COMMAND CoreVideoControlParseTest)

        if(WIN32 AND _qt_bin_dir)
            set_tests_properties(CoreVideoOscWire PROPERTIES
                ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:${_qt_bin_dir}")
            set_tests_properties(CoreVideoControlParse PROPERTIES
                ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:${_qt_bin_dir}")
        endif()
    endif()
endif()

set(CPACK_GENERATOR                 "ZIP")
set(CPACK_PACKAGE_NAME              "CoreVideo")
set(CPACK_PACKAGE_VENDOR            "CoreVideo")
set(CPACK_PACKAGE_DESCRIPTION_SHORT "OBS Studio plugin for live Zoom meeting capture")
set(CPACK_PACKAGE_VERSION           "${PROJECT_VERSION}")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)

include(CPack)
