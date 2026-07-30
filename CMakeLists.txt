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
# are cut from (releases are tagged vX.Y.Z and published via
# scripts/release-local.ps1 / .github/workflows/release-windows.yml). Rather
# than keep them in sync by hand, packaging entry points pass the real
# release tag through COREVIDEO_RELEASE_VERSION, and that value - not
# PROJECT_VERSION - is what CoreVideo reports at runtime (logs, diagnostics
# bundle, update-availability check). Local/dev builds that don't set it
# fall back to PROJECT_VERSION.
set(COREVIDEO_RELEASE_VERSION "" CACHE STRING
    "Release tag (e.g. v0.1.27) to report as the runtime plugin version. \
Set by release-local.ps1/CI from the git tag being packaged; leave blank \
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
        target_link_libraries(corevideo_ffmpeg INTERFACE
            "${COREVIDEO_AVFILTER_LIB}" "${COREVIDEO_AVUTIL_LIB}")
        set(COREVIDEO_FFMPEG_TARGET corevideo_ffmpeg)
        set(COREVIDEO_FFMPEG_BIN_DIR "${FFMPEG_ROOT}/bin")
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
        src/zoom-participant-audio-source.cpp
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
        src/zoom-iso-panel.cpp
        src/zoom-dock.cpp
        src/zoom-control-server.cpp
        src/zoom-osc-server.cpp
        src/obs-utils.cpp
        src/hw-video-pipeline.cpp
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
        # Ship the FFmpeg runtime DLLs alongside the plugin on Windows so
        # OBS's plugin loader can resolve avfilter / avutil / avcodec /
        # swscale / swresample without polluting PATH.
        if(WIN32 AND COREVIDEO_FFMPEG_BIN_DIR AND EXISTS "${COREVIDEO_FFMPEG_BIN_DIR}")
            file(GLOB COREVIDEO_FFMPEG_DLLS "${COREVIDEO_FFMPEG_BIN_DIR}/*.dll")
            if(COREVIDEO_FFMPEG_DLLS)
                add_custom_command(TARGET obs-zoom-plugin POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        ${COREVIDEO_FFMPEG_DLLS}
                        "$<TARGET_FILE_DIR:obs-zoom-plugin>"
                    COMMENT "Copying FFmpeg runtime DLLs for obs-zoom-plugin")
            endif()
            install(DIRECTORY "${COREVIDEO_FFMPEG_BIN_DIR}/"
                DESTINATION "obs-plugins/64bit"
                FILES_MATCHING PATTERN "*.dll")
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

    add_executable(CoreVideoReconnectScheduleTest
        tests/zoom-reconnect-schedule-test.cpp
    )
    target_include_directories(CoreVideoReconnectScheduleTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoReconnectSchedule
             COMMAND CoreVideoReconnectScheduleTest)

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
