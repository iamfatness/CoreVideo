set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(OBS_BUILD_DIR  "C:/obs-studio/build64" CACHE PATH "OBS build output directory")
set(ZOOM_SDK_DIR   "third_party/zoom-sdk"  CACHE PATH "Zoom Windows Meeting SDK directory")
list(APPEND CMAKE_PREFIX_PATH "${OBS_BUILD_DIR}")
