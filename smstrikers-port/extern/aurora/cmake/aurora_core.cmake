add_library(aurora_core STATIC
        lib/aurora.cpp
        lib/device.cpp
        lib/device.hpp
        lib/input.cpp
        lib/window.cpp
        lib/logging.cpp
        lib/system_info.cpp
        lib/system_info.hpp
        lib/io.hpp
        lib/io.cpp
        lib/thread.hpp
        lib/thread.cpp
        lib/time_internal.hpp
        lib/time.cpp
)
add_library(aurora::core ALIAS aurora_core)
set_target_properties(aurora_core PROPERTIES FOLDER "aurora")

target_compile_definitions(aurora_core PUBLIC AURORA TARGET_PC)
target_include_directories(aurora_core PUBLIC include)
target_link_libraries(aurora_core PUBLIC fmt::fmt ${AURORA_SDL3_TARGET} xxhash)
target_link_libraries(aurora_core PRIVATE absl::btree absl::flat_hash_map sqlite3 TracyClient)
if (AURORA_ENABLE_GX AND AURORA_CACHE_USE_ZSTD)
    target_compile_definitions(aurora_core PRIVATE AURORA_CACHE_USE_ZSTD)
    target_link_libraries(aurora_core PRIVATE zstd::libzstd)
endif ()

if (CMAKE_SYSTEM_NAME STREQUAL Windows)
    # stuff for fetching system info.
    target_link_libraries(aurora_core PRIVATE wbemuuid.lib comsuppw.lib ntdll.lib DXGI.lib)
elseif (APPLE)
    target_sources(aurora_core PRIVATE lib/system_info_mac.mm)
endif ()

if (IOS)
    find_library(COREHAPTICS_FRAMEWORK CoreHaptics REQUIRED)
    target_sources(aurora_core PRIVATE lib/device_ios.mm)
    set_source_files_properties(lib/device_ios.mm PROPERTIES COMPILE_FLAGS -fobjc-arc)
    target_link_libraries(aurora_core PUBLIC ${COREHAPTICS_FRAMEWORK})
endif ()

if (AURORA_ENABLE_GX)
    target_sources(aurora_core PRIVATE lib/imgui.cpp)
    target_link_libraries(aurora_core PUBLIC imgui)
endif ()

if(AURORA_ENABLE_RMLUI)
    target_compile_definitions(aurora_core PUBLIC AURORA_ENABLE_RMLUI)

    target_sources(aurora_core PRIVATE
            lib/rmlui.cpp
            lib/rmlui/RuntimeTextureProvider.cpp
            lib/rmlui/RmlUi_Backend_Aurora.cpp
            lib/rmlui/WebGPURenderInterface.cpp
            lib/rmlui/SystemInterface_Aurora.cpp
            lib/rmlui/FileInterface_SDL.cpp
            lib/rmlui/GlassFilter.cpp
    )
    target_link_libraries(aurora_core PUBLIC rmlui)

    target_link_libraries(aurora_core PUBLIC rmlui_backends)
endif ()

if (AURORA_ENABLE_GX)
    target_compile_definitions(aurora_core PUBLIC AURORA_ENABLE_GX)
    # Hand-rolled GLES backend. Dawn (lib/dawn/, lib/webgpu/gpu*.cpp) is gone; the
    # GL device layer (lib/gl/device.cpp) implements the historical aurora::webgpu
    # interface directly against OpenGLES. gpu_prof/sdl2shim_present keep their
    # files (Tracy stub / device EFB present) with the Dawn touchpoints removed.
    target_sources(aurora_core PRIVATE
            lib/webgpu/gpu_prof.cpp
            lib/webgpu/sdl2shim_present.cpp
            lib/gl/device.cpp
            lib/gl/gl_loader.cpp
            lib/gl/context.cpp
            lib/gl/backend.cpp
            lib/gl/pass.cpp
            lib/gl/program.cpp
            lib/gl/binary_cache.cpp
            lib/gl/census.cpp
            lib/gl/textures.cpp
            lib/gl/buffers.cpp
            lib/gl/fbo_cache.cpp
            lib/gl/state.cpp
    )
endif ()
