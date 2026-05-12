cmake_minimum_required(VERSION 3.16)
project(obs-zoom-plugin VERSION 0.1.0)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 COMPONENTS Core Network Widgets REQUIRED)
set(CMAKE_AUTOMOC ON)

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

# macOS distributes the Zoom SDK as a .framework bundle.
# Point ZOOM_SDK_FRAMEWORK at the ZoomSDK.framework directory if building on Apple.
set(ZOOM_SDK_FRAMEWORK "${ZOOM_SDK_DIR}/ZoomSDK.framework" CACHE PATH
    "Zoom Meeting SDK framework path (macOS only)")

# ── OBS plugin module ─────────────────────────────────────────────────────────
add_library(obs-zoom-plugin MODULE
    src/plugin-main.cpp
    src/zoom-source.cpp
    src/zoom-auth.cpp
    src/zoom-meeting.cpp
    src/zoom-participants.cpp
    src/zoom-share-delegate.cpp
    src/zoom-settings.cpp
    src/zoom-settings-dialog.cpp
    src/zoom-output-manager.cpp
    src/zoom-output-dialog.cpp
    src/zoom-output-profile.cpp
    src/zoom-control-server.cpp
    src/zoom-osc-server.cpp
    src/zoom-video-delegate.cpp
    src/zoom-audio-delegate.cpp
    src/zoom-audio-router.cpp
    src/zoom-participant-audio-source.cpp
    src/zoom-interpretation-audio-source.cpp
    src/obs-utils.cpp
)
target_include_directories(obs-zoom-plugin PRIVATE
    src
    ${ZOOM_SDK_INCLUDE_DIR}
)
target_link_libraries(obs-zoom-plugin PRIVATE
    OBS::libobs
    OBS::obs-frontend-api
    Qt6::Core
    Qt6::Network
    Qt6::Widgets
)
if(WIN32)
    target_link_libraries(obs-zoom-plugin PRIVATE ${ZOOM_SDK_DIR}/lib/sdk.lib)
elseif(APPLE)
    target_link_libraries(obs-zoom-plugin PRIVATE
        "-framework ${ZOOM_SDK_FRAMEWORK}"
        "-framework CoreFoundation"
        "-framework Foundation"
    )
endif()
set_target_properties(obs-zoom-plugin PROPERTIES PREFIX "")

# ── sdk.dll deployment (Windows only) ────────────────────────────────────────
if(WIN32)
    set(ZOOM_SDK_DLL "${ZOOM_SDK_BASE}/bin/sdk.dll")
    if(EXISTS "${ZOOM_SDK_DLL}")
        add_custom_command(TARGET obs-zoom-plugin POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ZOOM_SDK_DLL}"
                "$<TARGET_FILE_DIR:obs-zoom-plugin>/sdk.dll"
            COMMENT "Copying sdk.dll to output directory"
        )
        install(FILES "${ZOOM_SDK_DLL}"
            DESTINATION "${CMAKE_INSTALL_PREFIX}/obs-plugins/64bit"
        )
    else()
        message(WARNING "sdk.dll not found at ${ZOOM_SDK_DLL} — copy it manually before running")
    endif()
endif()

# ── Out-of-process capture engine ─────────────────────────────────────────────
set(ENGINE_SOURCES
    engine/src/main.cpp
    engine/src/engine-video.cpp
    engine/src/engine-audio.cpp
)
set(ENGINE_INCLUDES src engine/src ${ZOOM_SDK_INCLUDE_DIR})

if(WIN32)
    add_executable(ZoomObsEngine ${ENGINE_SOURCES})
    target_include_directories(ZoomObsEngine PRIVATE ${ENGINE_INCLUDES})
    target_link_libraries(ZoomObsEngine PRIVATE
        ws2_32 crypt32 winhttp shlwapi
        ${ZOOM_SDK_DIR}/lib/sdk.lib
    )
    set_target_properties(ZoomObsEngine PROPERTIES WIN32_EXECUTABLE FALSE)
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
        rt  # shm_open / shm_unlink on Linux
    )
    install(TARGETS ZoomObsEngine
        RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}/obs-plugins/64bit"
    )
endif()

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS obs-zoom-plugin
    LIBRARY DESTINATION "${CMAKE_INSTALL_PREFIX}/obs-plugins/64bit"
)
install(DIRECTORY data/
    DESTINATION "${CMAKE_INSTALL_PREFIX}/data/obs-plugins/obs-zoom-plugin"
)
