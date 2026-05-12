cmake_minimum_required(VERSION 3.16)
project(obs-zoom-plugin VERSION 0.1.0)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 COMPONENTS Core Widgets REQUIRED)
set(CMAKE_AUTOMOC ON)
set(ZOOM_SDK_DIR "third_party/zoom-sdk" CACHE PATH "Zoom Meeting SDK directory")
add_library(obs-zoom-plugin MODULE
    src/plugin-main.cpp
    src/zoom-source.cpp
    src/zoom-auth.cpp
    src/zoom-meeting.cpp
    src/zoom-participants.cpp
    src/zoom-share-delegate.cpp
    src/zoom-settings.cpp
    src/zoom-settings-dialog.cpp
    src/zoom-video-delegate.cpp
    src/zoom-audio-delegate.cpp
    src/obs-utils.cpp
)
target_include_directories(obs-zoom-plugin PRIVATE
    src
    ${ZOOM_SDK_DIR}/h
)
target_link_libraries(obs-zoom-plugin PRIVATE
    OBS::libobs
    OBS::obs-frontend-api
    Qt6::Core
    Qt6::Widgets
    ${ZOOM_SDK_DIR}/lib/sdk.lib
)
set_target_properties(obs-zoom-plugin PROPERTIES PREFIX "")
if(WIN32)
    add_executable(ZoomObsEngine
        engine/src/main.cpp
        engine/src/engine-video.cpp
        engine/src/engine-audio.cpp
    )
    target_include_directories(ZoomObsEngine PRIVATE
        src
        engine/src
        ${ZOOM_SDK_DIR}/h
    )
    target_link_libraries(ZoomObsEngine PRIVATE
        ws2_32 crypt32 winhttp shlwapi
        ${ZOOM_SDK_DIR}/lib/sdk.lib
    )
    set_target_properties(ZoomObsEngine PROPERTIES
        WIN32_EXECUTABLE FALSE
    )
    install(TARGETS ZoomObsEngine
        RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}/obs-plugins/64bit"
    )
endif()
install(TARGETS obs-zoom-plugin
    LIBRARY DESTINATION "${CMAKE_INSTALL_PREFIX}/obs-plugins/64bit"
)
install(DIRECTORY data/
    DESTINATION "${CMAKE_INSTALL_PREFIX}/data/obs-plugins/obs-zoom-plugin"
)
