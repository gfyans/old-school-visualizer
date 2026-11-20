cmake_minimum_required(VERSION 3.21)
project(Visualizer VERSION 1.0.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT: no VC++ redistributable needed on the target machine.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(COMMON_DEFS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
set(COMMON_OPTS /W4 /permissive- /utf-8 $<$<CONFIG:Release>:/O2 /GL /Gy /fp:fast>)
set(COMMON_LINK $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>)

# ---------------------------------------------------------------------------
# Shaders: compiled at build time with fxc into C headers, embedded in the exe.
# ---------------------------------------------------------------------------
find_program(FXC fxc REQUIRED)

set(SHADER_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(SHADER_OUT ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})
file(GLOB SHADER_INCLUDES ${SHADER_DIR}/*.hlsli)
set(SHADER_HEADERS)

# compile_shader(<file.hlsl> <profile> <variable name>)
function(compile_shader src profile var)
    set(out ${SHADER_OUT}/${var}.h)
    add_custom_command(
        OUTPUT ${out}
        COMMAND ${FXC} /nologo /O3 /Qstrip_reflect /Qstrip_debug
                /T ${profile} /E main /I ${SHADER_DIR}
                /Vn ${var} /Fh ${out} ${SHADER_DIR}/${src}
        DEPENDS ${SHADER_DIR}/${src} ${SHADER_INCLUDES}
        COMMENT "fxc ${src}"
        VERBATIM)
    set(SHADER_HEADERS ${SHADER_HEADERS} ${out} PARENT_SCOPE)
endfunction()

compile_shader(fullscreen_vs.hlsl vs_5_0 g_fullscreen_vs)
compile_shader(composite_ps.hlsl  ps_5_0 g_composite_ps)
compile_shader(spectrum_ps.hlsl   ps_5_0 g_spectrum_ps)
compile_shader(nebula_ps.hlsl     ps_5_0 g_nebula_ps)
compile_shader(noise3d_cs.hlsl    cs_5_0 g_noise3d_cs)
compile_shader(eqbars_ps.hlsl     ps_5_0 g_eqbars_ps)
compile_shader(feedback_warp_ps.hlsl ps_5_0 g_feedback_warp_ps)
compile_shader(feedback_out_ps.hlsl  ps_5_0 g_feedback_out_ps)

# ---------------------------------------------------------------------------
# Audio analysis (pure C++, shared with tests)
# ---------------------------------------------------------------------------
add_library(audio_analysis STATIC src/audio/AudioAnalyzer.cpp src/audio/BeatTracker.cpp src/media/TrackPalette.cpp)
target_include_directories(audio_analysis PUBLIC src)
target_compile_definitions(audio_analysis PRIVATE ${COMMON_DEFS})
target_compile_options(audio_analysis PRIVATE ${COMMON_OPTS})

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(Visualizer WIN32
    src/main.cpp
    src/platform/Window.cpp
    src/platform/FrameLimiter.cpp
    src/platform/Settings.cpp
    src/render/Renderer.cpp
    src/render/GpuTimer.cpp
    src/audio/AudioCapture.cpp
    src/ui/Overlay.cpp
    src/ui/EqBars.cpp
    src/viz/VisualizerHost.cpp
    src/viz/spectrum/SpectrumVisualizer.cpp
    src/viz/nebula/NebulaVisualizer.cpp
    src/viz/feedback/FeedbackVisualizer.cpp
    src/media/NowPlaying.cpp
    res/app.rc
    ${SHADER_HEADERS})
set_source_files_properties(res/app.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_SOURCE_DIR}/res/app.ico)

target_include_directories(Visualizer PRIVATE src ${SHADER_OUT})
target_link_libraries(Visualizer PRIVATE audio_analysis d3d11 dxgi d2d1 dwrite ole32 shell32 windowsapp)
set_source_files_properties(src/media/NowPlaying.cpp PROPERTIES COMPILE_OPTIONS /bigobj)   # C++/WinRT
target_compile_definitions(Visualizer PRIVATE ${COMMON_DEFS})
target_compile_options(Visualizer PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${COMMON_OPTS}>)
target_link_options(Visualizer PRIVATE ${COMMON_LINK})

set_target_properties(Visualizer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin
    DEBUG_POSTFIX -debug)

# ---------------------------------------------------------------------------
# Tests (built into the build dir, not bin/)
# ---------------------------------------------------------------------------
add_executable(analyzer_test tests/analyzer_test.cpp)
target_link_libraries(analyzer_test PRIVATE audio_analysis)
target_compile_definitions(analyzer_test PRIVATE ${COMMON_DEFS})
target_compile_options(analyzer_test PRIVATE ${COMMON_OPTS})
target_link_options(analyzer_test PRIVATE ${COMMON_LINK})

add_executable(capture_probe tests/capture_probe.cpp src/audio/AudioCapture.cpp)
target_include_directories(capture_probe PRIVATE src)
target_link_libraries(capture_probe PRIVATE ole32)
target_compile_definitions(capture_probe PRIVATE ${COMMON_DEFS})
target_compile_options(capture_probe PRIVATE ${COMMON_OPTS})
target_link_options(capture_probe PRIVATE ${COMMON_LINK})
