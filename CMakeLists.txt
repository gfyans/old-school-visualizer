cmake_minimum_required(VERSION 3.21)
project(Visualizer VERSION 1.8.0 LANGUAGES CXX RC)

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

# compile_shader(<file.hlsl> <profile> <variable name> [NAME=VALUE defines...])
function(compile_shader src profile var)
    set(out ${SHADER_OUT}/${var}.h)
    set(defs)
    foreach(d ${ARGN})
        list(APPEND defs /D ${d})
    endforeach()
    add_custom_command(
        OUTPUT ${out}
        COMMAND ${FXC} /nologo /O3 /Qstrip_reflect /Qstrip_debug
                /T ${profile} /E main /I ${SHADER_DIR} ${defs}
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
compile_shader(nebula_taa_ps.hlsl ps_5_0 g_nebula_taa_ps)
compile_shader(nebula_post_ps.hlsl ps_5_0 g_nebula_post_ps)
compile_shader(noise3d_cs.hlsl    cs_5_0 g_noise3d_cs)
compile_shader(eqbars_ps.hlsl     ps_5_0 g_eqbars_ps)
foreach(pass curl vorticity splat_vel splat_dye divergence pressure gradient advect_vel advect_dye)
    string(TOUPPER ${pass} PASS_DEFINE)
    compile_shader(liquid_sim_ps.hlsl ps_5_0 g_liquid_${pass}_ps PASS_${PASS_DEFINE})
endforeach()
compile_shader(liquid_display_ps.hlsl ps_5_0 g_liquid_display_ps)
compile_shader(neongrid_ps.hlsl      ps_5_0 g_neongrid_ps)
compile_shader(scope_beam_vs.hlsl    vs_5_0 g_scope_beam_vs)
compile_shader(scope_beam_ps.hlsl    ps_5_0 g_scope_beam_ps)
compile_shader(scope_decay_ps.hlsl   ps_5_0 g_scope_decay_ps)
compile_shader(scope_crt_ps.hlsl     ps_5_0 g_scope_crt_ps)
compile_shader(aurora_ps.hlsl        ps_5_0 g_aurora_ps)
compile_shader(galaxy_vs.hlsl        vs_5_0 g_galaxy_vs)
compile_shader(galaxy_ps.hlsl        ps_5_0 g_galaxy_ps)
compile_shader(galaxy_out_ps.hlsl    ps_5_0 g_galaxy_out_ps)
compile_shader(fractal_ps.hlsl       ps_5_0 g_fractal_ps)
compile_shader(capture_blit_ps.hlsl  ps_5_0 g_capture_blit_ps)
compile_shader(signal_ps.hlsl        ps_5_0 g_signal_ps)
compile_shader(signal_mesh_vs.hlsl   vs_5_0 g_signal_mesh_vs)
compile_shader(signal_mesh_ps.hlsl   ps_5_0 g_signal_mesh_ps)
compile_shader(detonation_ps.hlsl    ps_5_0 g_detonation_ps)
compile_shader(capture_yuv_ps.hlsl   ps_5_0 g_capture_y_ps PLANE=0)
compile_shader(capture_yuv_ps.hlsl   ps_5_0 g_capture_uv_ps PLANE=1)

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
    src/viz/liquid/LiquidVisualizer.cpp
    src/capture/Screenshot.cpp
    src/viz/neongrid/NeonGridVisualizer.cpp
    src/viz/scope/ScopeVisualizer.cpp
    src/viz/aurora/AuroraVisualizer.cpp
    src/viz/galaxy/GalaxyVisualizer.cpp
    src/viz/fractal/FractalVisualizer.cpp
    src/viz/signal/SignalVisualizer.cpp
    src/viz/detonation/DetonationVisualizer.cpp
    src/capture/Recorder.cpp
    src/update/Updater.cpp
    src/media/NowPlaying.cpp
    res/app.rc
    ${SHADER_HEADERS})
set_source_files_properties(res/app.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_SOURCE_DIR}/res/app.ico)
# Signal's models: third-party assets kept out of git (see tools/mesh/stl_to_mesh.py). Each is embedded
# when present (RCDATA 101 skull, 102 gas-mask head, 103 brain); without them Signal draws procedural
# subjects instead.
set(SIGNAL_MODELS cyberskull gashead brain)
set(SIGNAL_RC "${CMAKE_BINARY_DIR}/signal_models.rc")
set(SIGNAL_RC_TEXT "// Generated by CMakeLists.txt: the Signal models found in assets/.\n")
set(SIGNAL_RC_DEPENDS "")
set(SIGNAL_ID 101)
foreach(model IN LISTS SIGNAL_MODELS)
    set(mesh "${CMAKE_SOURCE_DIR}/assets/${model}.mesh")
    if(EXISTS ${mesh})
        string(APPEND SIGNAL_RC_TEXT "${SIGNAL_ID} RCDATA \"${mesh}\"\n")
        list(APPEND SIGNAL_RC_DEPENDS ${mesh})
        message(STATUS "Signal: embedding assets/${model}.mesh")
    endif()
    math(EXPR SIGNAL_ID "${SIGNAL_ID} + 1")
endforeach()
if(SIGNAL_RC_DEPENDS)
    file(CONFIGURE OUTPUT ${SIGNAL_RC} CONTENT "${SIGNAL_RC_TEXT}" @ONLY)
    target_sources(Visualizer PRIVATE ${SIGNAL_RC})
    set_source_files_properties(${SIGNAL_RC} PROPERTIES OBJECT_DEPENDS "${SIGNAL_RC_DEPENDS}")
endif()

target_include_directories(Visualizer PRIVATE src ${SHADER_OUT})
target_link_libraries(Visualizer PRIVATE audio_analysis d3d11 dxgi d2d1 dwrite ole32 shell32 windowsapp mfplat mfreadwrite mfuuid winhttp bcrypt windowscodecs)
target_compile_definitions(Visualizer PRIVATE VISUALIZER_VERSION="${PROJECT_VERSION}")
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

# Dev tools for tuning on real music (see tools/analysis/README.md)
add_executable(vis_record tools/recorder/recorder.cpp src/audio/AudioCapture.cpp
    src/media/NowPlaying.cpp src/platform/Settings.cpp)
target_include_directories(vis_record PRIVATE src)
target_link_libraries(vis_record PRIVATE audio_analysis ole32 shell32 windowsapp)
target_compile_definitions(vis_record PRIVATE ${COMMON_DEFS})
target_compile_options(vis_record PRIVATE ${COMMON_OPTS})

add_executable(vis_replay tools/replay/replay.cpp)
target_link_libraries(vis_replay PRIVATE audio_analysis)
target_compile_definitions(vis_replay PRIVATE ${COMMON_DEFS})
target_compile_options(vis_replay PRIVATE ${COMMON_OPTS})

# Checks a recorded video's audio for dropouts (record a steady tone)
add_executable(vis_mp4check tools/mp4check/mp4check.cpp)
target_link_libraries(vis_mp4check PRIVATE mfplat mfreadwrite mfuuid ole32)
target_compile_definitions(vis_mp4check PRIVATE ${COMMON_DEFS})
target_compile_options(vis_mp4check PRIVATE ${COMMON_OPTS})

add_executable(capture_probe tests/capture_probe.cpp src/audio/AudioCapture.cpp)
target_include_directories(capture_probe PRIVATE src)
target_link_libraries(capture_probe PRIVATE ole32)
target_compile_definitions(capture_probe PRIVATE ${COMMON_DEFS})
target_compile_options(capture_probe PRIVATE ${COMMON_OPTS})
target_link_options(capture_probe PRIVATE ${COMMON_LINK})
