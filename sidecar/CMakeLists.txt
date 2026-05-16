cmake_minimum_required(VERSION 3.20)
project(CoreVideoSidecar VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 COMPONENTS Core Widgets Network REQUIRED)

set(SOURCES
    src/main.cpp
    src/mainwindow.cpp
    src/sidebar.cpp
    src/preview-canvas.cpp
    src/template-panel.cpp
    src/theme-panel.cpp
    src/participant-panel.cpp
    src/scenes-panel.cpp
    src/macros-panel.cpp
    src/settings-page.cpp
    src/layout-template.cpp
    src/template-manager.cpp
    src/look-library.cpp
    src/look-panel.cpp
    src/me-bus.cpp
    src/overlay-panel.cpp
    src/obs-client.cpp
    src/simple-websocket.cpp
    src/zoom-control-client.cpp
    src/obs-connect-dialog.cpp
    src/sidecar-control-server.cpp
    src/command-palette.cpp
)

set(RESOURCES sidecar.qrc)

qt6_add_resources(RESOURCE_SOURCES ${RESOURCES})

add_executable(CoreVideoSidecar ${SOURCES} ${RESOURCE_SOURCES})

target_include_directories(CoreVideoSidecar PRIVATE src)

target_link_libraries(CoreVideoSidecar PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
)

# Embed the data directory at build time
add_custom_command(TARGET CoreVideoSidecar POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/data"
        "$<TARGET_FILE_DIR:CoreVideoSidecar>/data"
    COMMENT "Copying Sidecar data directory"
)

if(BUILD_TESTING)
    get_target_property(_qt_core_dll Qt6::Core IMPORTED_LOCATION_RELEASE)
    if(NOT _qt_core_dll)
        get_target_property(_qt_core_dll Qt6::Core IMPORTED_LOCATION)
    endif()
    if(_qt_core_dll)
        get_filename_component(_qt_runtime_dir "${_qt_core_dll}" DIRECTORY)
    endif()

    add_test(NAME CoreVideoSidecarVersion
        COMMAND CoreVideoSidecar --version)
    if(WIN32 AND _qt_runtime_dir)
        set_tests_properties(CoreVideoSidecarVersion PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:CoreVideoSidecar>;PATH=path_list_prepend:${_qt_runtime_dir}")
    endif()
endif()

if(APPLE)
    set_target_properties(CoreVideoSidecar PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "CoreVideo Sidecar"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    )
elseif(WIN32)
    set_target_properties(CoreVideoSidecar PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

if(NOT DEFINED COREVIDEO_SIDECAR_INSTALL_DIR)
    set(COREVIDEO_SIDECAR_INSTALL_DIR "bin")
endif()

install(TARGETS CoreVideoSidecar
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION "${COREVIDEO_SIDECAR_INSTALL_DIR}"
)
install(DIRECTORY data/ DESTINATION "${COREVIDEO_SIDECAR_INSTALL_DIR}/data")
