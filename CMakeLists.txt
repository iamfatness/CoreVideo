cmake_minimum_required(VERSION 3.16)
project(obs-zoom-plugin VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(COREVIDEO_BUILD_PLUGIN "Build the OBS plugin module" ON)
option(COREVIDEO_BUILD_ENGINE "Build the ZoomObsEngine helper process" ON)

# ── Embedded credentials (set via -D or CI secrets; empty = user must enter) ──
set(ZOOM_EMBED_SDK_KEY    "" CACHE STRING "Pre-embedded Zoom SDK key")
set(ZOOM_EMBED_SDK_SECRET "" CACHE STRING "Pre-embedded Zoom SDK secret")
set(ZOOM_EMBED_JWT_TOKEN  "" CACHE STRING "Pre-embedded Zoom JWT token")
configure_file(src/zoom-credentials.h.in
               "${CMAKE_CURRENT_BINARY_DIR}/zoom-credentials.h" @ONLY)
configure_file(src/obs-zoom-version.h.in
               "${CMAKE_CURRENT_BINARY_DIR}/obs-zoom-version.h" @ONLY)

if(COREVIDEO_BUILD_PLUGIN)
    set(CMAKE_AUTOMOC ON)
    find_package(LibObs REQUIRED)
    find_package(obs-frontend-api REQUIRED)
    find_package(Qt6 COMPONENTS Core Network Widgets REQUIRED)
endif()

# ── Optional FFmpeg hardware-accelerated video conversion ─────────────────────
option(ENABLE_FFMPEG_HW_ACCEL
    "Enable FFmpeg hardware-accelerated I420→NV12 video conversion (LGPL dynamic link required)"
    OFF)

if(ENABLE_FFMPEG_HW_ACCEL)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavfilter>=7
        libavutil>=57
    )
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

set(ZOOM_SDK_INCLUDE_DIR "${ZOOM_SDK_DIR}/h")
set(ZOOM_SDK_LIBRARY "${ZOOM_SDK_DIR}/lib/sdk.lib")

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
# Runtime check (bin/sdk.dll): only needed to package and run; if absent,
# the engine still builds but sdk.dll must be placed alongside it manually.
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
            "ZoomObsEngine.exe will be built but sdk.dll will not be packaged automatically — "
            "place the Zoom SDK bin/ DLLs alongside ZoomObsEngine.exe before running.")
    endif()
elseif(APPLE)
    if(EXISTS "${ZOOM_SDK_DIR}/ZoomSDK.framework")
        set(BUILD_ZOOM_ENGINE ON)
    else()
        message(WARNING
            "Zoom SDK framework not found at '${ZOOM_SDK_DIR}/ZoomSDK.framework'. "
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
        src/zoom-engine-client.cpp
        src/zoom-reconnect.cpp
        src/zoom-settings.cpp
        src/zoom-settings-dialog.cpp
        src/zoom-output-manager.cpp
        src/zoom-output-dialog.cpp
        src/zoom-output-profile.cpp
        src/zoom-dock.cpp
        src/zoom-control-server.cpp
        src/zoom-osc-server.cpp
        src/obs-utils.cpp
        src/hw-video-pipeline.cpp
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

    if(ENABLE_FFMPEG_HW_ACCEL)
        target_link_libraries(obs-zoom-plugin PRIVATE PkgConfig::FFMPEG)
        target_compile_definitions(obs-zoom-plugin PRIVATE COREVIDEO_HW_ACCEL)
    endif()

    if(APPLE)
        target_link_libraries(obs-zoom-plugin PRIVATE
            "-framework CoreFoundation"
            "-framework Foundation"
        )
        # Symbols provided by OBS at plugin load time; no link-time libobs needed.
        target_link_options(obs-zoom-plugin PRIVATE "-undefined dynamic_lookup")
    endif()

    set_target_properties(obs-zoom-plugin PROPERTIES PREFIX "")
endif()

set(ENGINE_SOURCES
    engine/src/main.cpp
    engine/src/engine-video.cpp
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
                DESTINATION "${CMAKE_INSTALL_PREFIX}/obs-plugins/64bit"
            )
        endif()
        install(TARGETS ZoomObsEngine
            RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}/obs-plugins/64bit"
        )
    elseif(APPLE)
        add_executable(ZoomObsEngine ${ENGINE_SOURCES})
        target_include_directories(ZoomObsEngine PRIVATE ${ENGINE_INCLUDES})
        target_link_libraries(ZoomObsEngine PRIVATE
            "-framework ${ZOOM_SDK_FRAMEWORK}"
            "-framework CoreFoundation"
            "-framework Foundation"
        )
        install(TARGETS ZoomObsEngine
            RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}/obs-plugins/64bit"
        )
    elseif(UNIX)
        add_executable(ZoomObsEngine ${ENGINE_SOURCES})
        target_include_directories(ZoomObsEngine PRIVATE ${ENGINE_INCLUDES})
        target_link_libraries(ZoomObsEngine PRIVATE
            ${ZOOM_SDK_DIR}/lib/libsdk.so
            rt
        )
        install(TARGETS ZoomObsEngine
            RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}/obs-plugins/64bit"
        )
    endif()
endif()

if(COREVIDEO_BUILD_PLUGIN)
    install(TARGETS obs-zoom-plugin
        LIBRARY DESTINATION "obs-plugins/64bit"
    )
    install(DIRECTORY data/
        DESTINATION "data/obs-plugins/obs-zoom-plugin"
    )
endif()

# ── CPack packaging ────────────────────────────────────────────────────────────
# Produces a ZIP that users extract directly into their OBS Studio folder.
#
# Windows: extract into  C:\Program Files\obs-studio\
# macOS:   extract into  /Applications/OBS.app/Contents/Resources/
#          or ~/Library/Application Support/obs-studio/
#
# The ZIP contains the standard OBS plugin layout:
#   obs-plugins/64bit/<plugin>.(dll|so)
#   data/obs-plugins/obs-zoom-plugin/locale/en-US.ini
set(CPACK_GENERATOR                 "ZIP")
set(CPACK_PACKAGE_NAME              "CoreVideo")
set(CPACK_PACKAGE_VENDOR            "CoreVideo")
set(CPACK_PACKAGE_DESCRIPTION_SHORT "OBS Studio plugin for live Zoom meeting capture")
set(CPACK_PACKAGE_VERSION           "${PROJECT_VERSION}")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)

include(CPack)
