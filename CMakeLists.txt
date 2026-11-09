# =============================================================================
#  THE HYBRID CORE 2.0 — build. Zero third-party dependencies.
#  Windows (Visual Studio 2022):  cmake -B build -G "Visual Studio 17 2022" -A x64
#                                 cmake --build build --config Release
#  Linux/macOS (offline tools):   cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
# =============================================================================
cmake_minimum_required(VERSION 3.20)
project(HybridCore LANGUAGES CXX VERSION 2.0.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# MSVC: link the C++ runtime into the program so it never needs a separate "Visual C++ Redistributable" install.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(cmake/asio.cmake)

add_library(hybrid_core INTERFACE)
target_include_directories(hybrid_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(MSVC)
  # /arch:AVX2 enables AVX2 + FMA codegen; /fp:precise keeps loudness math deterministic.
  target_compile_options(hybrid_core INTERFACE /arch:AVX2 /O2 /fp:precise /permissive- /W4 /utf-8
    /wd4324)   # C4324 = "padded because of alignas": the padding is on purpose (64-byte cache-line alignment)
  target_compile_definitions(hybrid_core INTERFACE NOMINMAX _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(hybrid_core INTERFACE -mavx2 -mfma -O2 -Wall -Wextra)
  find_package(Threads REQUIRED)
  target_link_libraries(hybrid_core INTERFACE Threads::Threads)
endif()

add_executable(hc_tests tests/test_main.cpp)
target_link_libraries(hc_tests PRIVATE hybrid_core)

add_executable(hc_demo_stems apps/hc_demo_stems.cpp)
target_link_libraries(hc_demo_stems PRIVATE hybrid_core)

add_executable(hc_render apps/hc_render.cpp)
target_link_libraries(hc_render PRIVATE hybrid_core)
if(WIN32)
  target_link_libraries(hc_render PRIVATE avrt)
endif()

if(WIN32)
  add_executable(hc_play apps/hc_play.cpp src/platform/wasapi_output.cpp)
  target_link_libraries(hc_play PRIVATE hybrid_core ole32 avrt)

  # THE HYBRID CORE Studio — the faceplate app.
  add_executable(HybridCore WIN32 studio/main.cpp studio/gfx.cpp src/platform/wasapi_output.cpp src/platform/asio_output.cpp ${HC_ASIO_SOURCES} studio/studio.rc)
  target_include_directories(HybridCore PRIVATE studio ${HC_ASIO_INCLUDES})
  if(HC_WITH_ASIO)
    target_compile_definitions(HybridCore PRIVATE HC_HAS_ASIO_SDK)
  endif()
  # UNICODE only for our own sources: the Steinberg SDK's asiolist.cpp is written for the ANSI API.
  set_source_files_properties(studio/main.cpp studio/gfx.cpp src/platform/wasapi_output.cpp src/platform/asio_output.cpp
                              PROPERTIES COMPILE_DEFINITIONS "UNICODE;_UNICODE")
  target_link_libraries(HybridCore PRIVATE hybrid_core d3d11 dxgi d3dcompiler windowscodecs ole32 uuid avrt comdlg32 shell32 gdi32 user32)
  if(MSVC)
    target_link_options(HybridCore PRIVATE /ENTRY:wWinMainCRTStartup /MANIFEST:NO)
  else()
    target_link_options(HybridCore PRIVATE -municode)
  endif()

  # Installer: embeds the built HybridCore.exe (setup.rc points at ../build-win/HybridCore.exe;
  # build_windows.sh produces dist/HybridCore-Setup.exe the same way).
endif()

# =============================================================================
#  HEM STEM-TO-MIDI — 1 song or up to 13 stems -> MIDI (engine: include/hc/midi, app: apps/hem_stem_to_midi)
# =============================================================================
add_executable(hem_midi_tests tests/test_midi.cpp)
target_link_libraries(hem_midi_tests PRIVATE hybrid_core)
add_executable(hem_midi_realtest tests/test_midi_real.cpp)
target_include_directories(hem_midi_realtest PRIVATE tests)
target_link_libraries(hem_midi_realtest PRIVATE hybrid_core)

set(HEM_MIDI_LOADER apps/hem_stem_to_midi/audio_load.hpp)
if(WIN32)
  list(APPEND HEM_MIDI_LOADER apps/hem_stem_to_midi/decode_mf.cpp)
endif()
add_executable(hem_midi_cli apps/hem_stem_to_midi/cli.cpp ${HEM_MIDI_LOADER})
target_include_directories(hem_midi_cli PRIVATE apps/hem_stem_to_midi)
target_link_libraries(hem_midi_cli PRIVATE hybrid_core)
if(WIN32)
  target_link_libraries(hem_midi_cli PRIVATE mfplat mfreadwrite mfuuid ole32 uuid shell32)
  set_source_files_properties(apps/hem_stem_to_midi/decode_mf.cpp PROPERTIES COMPILE_DEFINITIONS "UNICODE;_UNICODE")

  add_executable(HEM-StemToMIDI WIN32 apps/hem_stem_to_midi/main.cpp apps/hem_stem_to_midi/decode_mf.cpp apps/hem_stem_to_midi/app.rc)
  target_include_directories(HEM-StemToMIDI PRIVATE apps/hem_stem_to_midi)
  target_compile_definitions(HEM-StemToMIDI PRIVATE UNICODE _UNICODE)
  target_link_libraries(HEM-StemToMIDI PRIVATE hybrid_core mfplat mfreadwrite mfuuid ole32 uuid comctl32 comdlg32 shell32 windowscodecs gdi32 user32)
  if(MSVC)
    target_link_options(HEM-StemToMIDI PRIVATE /ENTRY:wWinMainCRTStartup /MANIFEST:NO)
  else()
    target_link_options(HEM-StemToMIDI PRIVATE -municode)
  endif()
endif()
