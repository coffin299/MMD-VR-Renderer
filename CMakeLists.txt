cmake_minimum_required(VERSION 3.20)
project(MMD-VR-Renderer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(MMDVR_BUILD_TESTS "Build unit tests" ON)

add_library(mmdvr_core STATIC
  src/core/Diagnostics.cpp
  src/core/MathUtil.cpp
  src/core/ProjectionEngine.cpp
  src/core/StereoEngine.cpp
  src/core/ImageSequenceWriter.cpp
  src/core/VideoEncoder.cpp
  src/core/SphericalMp4Inject.cpp
  src/core/JobState.cpp
  src/core/RenderController.cpp
  src/camera/CameraController.cpp
  src/capture/FrameBuffer.cpp
)
target_include_directories(mmdvr_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json
)
target_compile_definitions(mmdvr_core PUBLIC _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
if(MSVC)
  target_compile_options(mmdvr_core PRIVATE /W4 /permissive- /utf-8)
endif()

# MinHook (fetched by tools/fetch_deps.ps1)
set(MINHOOK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook)
if(EXISTS ${MINHOOK_DIR}/include/MinHook.h)
  add_library(minhook STATIC
    ${MINHOOK_DIR}/src/buffer.c
    ${MINHOOK_DIR}/src/hook.c
    ${MINHOOK_DIR}/src/trampoline.c
    ${MINHOOK_DIR}/src/hde/hde64.c
  )
  target_include_directories(minhook PUBLIC ${MINHOOK_DIR}/include)
  target_compile_definitions(minhook PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  message(WARNING "MinHook not found. Run tools/fetch_deps.ps1 before building the plugin.")
endif()

# Plugin binary. MMD must load it as d3d9.dll; we also keep a same-bytes copy as
# d3d9_mmdvr.dll next to it (same naming style as d3d9_mme.dll) for uninstall visibility.
# Chain: MMD → d3d9.dll (this) → d3d9_mme.dll (MME) → system d3d9.
add_library(d3d9_mmdvr SHARED
  src/plugin/dllmain.cpp
  src/plugin/D3D9Proxy.cpp
  src/plugin/DeviceHooks.cpp
  src/plugin/MenuUI.cpp
  src/plugin/RenderDialog.cpp
  src/plugin/ProgressDialog.cpp
  src/plugin/PluginHost.cpp
  src/plugin/MmdTimeline.cpp
  src/plugin/MmdWaveDetect.cpp
  src/plugin/UiStrings.cpp
  src/capture/D3D9CaptureBackend.cpp
  src/plugin/d3d9.def
)
set_target_properties(d3d9_mmdvr PROPERTIES OUTPUT_NAME "d3d9_mmdvr" PREFIX "")
target_include_directories(d3d9_mmdvr PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src/plugin
)
target_link_libraries(d3d9_mmdvr PRIVATE mmdvr_core)
if(TARGET minhook)
  target_link_libraries(d3d9_mmdvr PRIVATE minhook)
  target_compile_definitions(d3d9_mmdvr PRIVATE MMDVR_HAS_MINHOOK=1)
endif()
# Do not link system d3d9.lib (would conflict with our proxy DLL name). Use headers + runtime LoadLibrary.
target_link_libraries(d3d9_mmdvr PRIVATE comdlg32.lib shell32.lib user32.lib gdi32.lib ole32.lib comctl32.lib dxgi.lib)
target_compile_definitions(d3d9_mmdvr PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
if(MSVC)
  target_compile_options(d3d9_mmdvr PRIVATE /W4 /permissive- /utf-8)
endif()
# Convenience alias + installable d3d9.dll copy beside the named build output.
add_custom_command(TARGET d3d9_mmdvr POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:d3d9_mmdvr>
    $<TARGET_FILE_DIR:d3d9_mmdvr>/d3d9.dll
  COMMENT "Copy d3d9_mmdvr.dll -> d3d9.dll for MMD install")
add_custom_target(d3d9 DEPENDS d3d9_mmdvr)

if(MMDVR_BUILD_TESTS)
  add_executable(mmdvr_tests
    tests/test_main.cpp
    tests/test_projection.cpp
    tests/test_stereo.cpp
    tests/test_job_state.cpp
    tests/test_math.cpp
  )
  target_link_libraries(mmdvr_tests PRIVATE mmdvr_core)
  enable_testing()
  add_test(NAME mmdvr_tests COMMAND mmdvr_tests)
endif()

add_executable(mmdvr_offline src/tools/offline_compose_main.cpp)
target_link_libraries(mmdvr_offline PRIVATE mmdvr_core)
