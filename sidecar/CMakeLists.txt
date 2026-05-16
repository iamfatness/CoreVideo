cmake_minimum_required(VERSION 3.20)
project(CoreVideoSidecar VERSION 0.1.4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 COMPONENTS Core Gui Widgets Network REQUIRED)

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
    src/look-render-plan.cpp
    src/look-panel.cpp
    src/lower-third-controller.cpp
    src/me-bus.cpp
    src/overlay-panel.cpp
    src/obs-client.cpp
    src/obs-look-renderer.cpp
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
    add_executable(CoreVideoSidecarCatalogTest
        tests/catalog-test.cpp
        src/layout-template.cpp
    )
    target_include_directories(CoreVideoSidecarCatalogTest PRIVATE src)
    target_link_libraries(CoreVideoSidecarCatalogTest PRIVATE Qt6::Core Qt6::Gui)

    add_executable(CoreVideoSidecarRendererConfigTest
        tests/renderer-config-test.cpp
        src/look-render-plan.cpp
    )
    target_include_directories(CoreVideoSidecarRendererConfigTest PRIVATE src)
    target_link_libraries(CoreVideoSidecarRendererConfigTest PRIVATE Qt6::Core Qt6::Gui)

    add_test(NAME CoreVideoSidecarCatalog
        COMMAND CoreVideoSidecarCatalogTest "${CMAKE_CURRENT_SOURCE_DIR}/data")
    add_test(NAME CoreVideoSidecarRendererConfig
        COMMAND CoreVideoSidecarRendererConfigTest)
    add_executable(CoreVideoSidecarLowerThirdTest
        tests/lower-third-test.cpp
        src/lower-third-controller.cpp
        src/layout-template.cpp
    )
    target_include_directories(CoreVideoSidecarLowerThirdTest PRIVATE src)
    target_link_libraries(CoreVideoSidecarLowerThirdTest PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
    add_test(NAME CoreVideoSidecarLowerThirds
        COMMAND CoreVideoSidecarLowerThirdTest)
    if(WIN32 AND _qt_runtime_dir)
        set_tests_properties(CoreVideoSidecarVersion PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:CoreVideoSidecar>;PATH=path_list_prepend:${_qt_runtime_dir}")
        set_tests_properties(CoreVideoSidecarCatalog PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:CoreVideoSidecarCatalogTest>;PATH=path_list_prepend:${_qt_runtime_dir}")
        set_tests_properties(CoreVideoSidecarRendererConfig PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:CoreVideoSidecarRendererConfigTest>;PATH=path_list_prepend:${_qt_runtime_dir}")
        set_tests_properties(CoreVideoSidecarLowerThirds PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:CoreVideoSidecarLowerThirdTest>;PATH=path_list_prepend:${_qt_runtime_dir}")
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
    foreach(_qt_dll_target Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)
        get_target_property(_qt_dll ${_qt_dll_target} IMPORTED_LOCATION_RELEASE)
        if(NOT _qt_dll)
            get_target_property(_qt_dll ${_qt_dll_target} IMPORTED_LOCATION)
        endif()
        if(_qt_dll)
            list(APPEND COREVIDEO_SIDECAR_QT_DLLS "${_qt_dll}")
        endif()
    endforeach()

    if(COREVIDEO_SIDECAR_QT_DLLS)
        add_custom_command(TARGET CoreVideoSidecar POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${COREVIDEO_SIDECAR_QT_DLLS}
                "$<TARGET_FILE_DIR:CoreVideoSidecar>"
            COMMENT "Copying Qt runtime DLLs for Sidecar"
        )
    endif()

    get_target_property(_qt_core_dll Qt6::Core IMPORTED_LOCATION_RELEASE)
    if(NOT _qt_core_dll)
        get_target_property(_qt_core_dll Qt6::Core IMPORTED_LOCATION)
    endif()
    if(_qt_core_dll)
        get_filename_component(_qt_bin_dir "${_qt_core_dll}" DIRECTORY)
        get_filename_component(_qt_root_dir "${_qt_bin_dir}" DIRECTORY)
        set(_qt_platform_plugin "${_qt_root_dir}/plugins/platforms/qwindows.dll")
        if(EXISTS "${_qt_platform_plugin}")
            add_custom_command(TARGET CoreVideoSidecar POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "$<TARGET_FILE_DIR:CoreVideoSidecar>/platforms"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_qt_platform_plugin}"
                    "$<TARGET_FILE_DIR:CoreVideoSidecar>/platforms/qwindows.dll"
                COMMENT "Copying Qt Windows platform plugin for Sidecar"
            )
        endif()
    endif()

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

if(WIN32)
    if(COREVIDEO_SIDECAR_QT_DLLS)
        install(FILES ${COREVIDEO_SIDECAR_QT_DLLS}
            DESTINATION "${COREVIDEO_SIDECAR_INSTALL_DIR}"
        )
    endif()
    if(_qt_platform_plugin AND EXISTS "${_qt_platform_plugin}")
        install(FILES "${_qt_platform_plugin}"
            DESTINATION "${COREVIDEO_SIDECAR_INSTALL_DIR}/platforms"
        )
    endif()
endif()
