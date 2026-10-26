# bbport GPU library: shadPS4 video core + shader recompiler + Gnm/VideoOut
# drivers, adapted to the native_probe runtime through shims in shim/.
cmake_minimum_required(VERSION 3.24)
project(bbgpu LANGUAGES C CXX)
option(BB_DIAGNOSTICS "Build capture, profiling and trace support" OFF)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# Frame pointers keep the loader's thread dumps (watchdog, SIGUSR2 sampling) walkable.
add_compile_options(-fno-omit-frame-pointer)

# bbport: link-time optimization (BB_LTO, default on) and profile-guided optimization
# (BB_PGO: off, generate = instrumented build writing into BB_PGO_DIR, use = build with that
# profile). Covers bbgpu and the static libraries linked into it. No -march: the same build
# runs on the Steam Deck.
option(BB_LTO "Link-time optimization of bbgpu" ON)
set(BB_PGO "off" CACHE STRING "Profile-guided optimization: off, generate, use")
set(BB_PGO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pgo" CACHE PATH "Profile directory")
if (BB_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT bb_ipo_ok OUTPUT bb_ipo_error)
    if (bb_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(WARNING "LTO unavailable: ${bb_ipo_error}")
    endif()
endif()
if (BB_PGO STREQUAL "generate")
    # Atomic counters: the GPU, recording, preparation and copy threads run the same code.
    add_compile_options(-fprofile-generate=${BB_PGO_DIR} -fprofile-update=atomic)
    add_link_options(-fprofile-generate=${BB_PGO_DIR})
    add_compile_definitions(BB_PGO_GENERATE)
elseif (BB_PGO STREQUAL "use" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Clang: code without recorded counts is optimized normally, and functions changed since the profile
    # (or built differently for it, bbgpu_init) are compiled without it.
    add_compile_options(-fprofile-use=${BB_PGO_DIR} -Wno-profile-instr-missing
                        -Wno-profile-instr-out-of-date -Wno-profile-instr-unprofiled)
elseif (BB_PGO STREQUAL "use")
    # GCC (Linux, .gcda files): partial training keeps unprofiled code optimized normally; counters from
    # concurrent threads can be slightly inconsistent (correction).
    add_compile_options(-fprofile-use=${BB_PGO_DIR} -fprofile-partial-training
                        -fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch)
endif()

find_package(Vulkan REQUIRED)
find_package(fmt REQUIRED)
find_package(Boost REQUIRED)
# bbport (Windows): MSYS2 has no magic_enum/miniz packages; the submodules are used instead.
find_package(magic_enum QUIET)
if (NOT magic_enum_FOUND)
    add_library(magic_enum INTERFACE)
    add_library(magic_enum::magic_enum ALIAS magic_enum)
    target_include_directories(magic_enum INTERFACE third_party/magic_enum/include
                                                    third_party/magic_enum/include/magic_enum)
endif()
find_package(tsl-robin-map REQUIRED)
find_package(VulkanMemoryAllocator CONFIG QUIET)
if (NOT VulkanMemoryAllocator_FOUND)
    # MSYS2 ships the header (include/vma/vk_mem_alloc.h) without a CMake package.
    find_path(VMA_INCLUDE_DIR vk_mem_alloc.h PATH_SUFFIXES vma REQUIRED)
    add_library(VulkanMemoryAllocator INTERFACE)
    add_library(GPUOpen::VulkanMemoryAllocator ALIAS VulkanMemoryAllocator)
    target_include_directories(VulkanMemoryAllocator INTERFACE ${VMA_INCLUDE_DIR})
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(XXHASH REQUIRED IMPORTED_TARGET libxxhash)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
if (NOT WIN32)
    pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
endif()
find_package(miniz QUIET)
if (NOT miniz_FOUND)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    add_subdirectory(third_party/miniz EXCLUDE_FROM_ALL)
    unset(CMAKE_POLICY_VERSION_MINIMUM)
    add_library(miniz::miniz ALIAS miniz)
endif()
find_package(Zydis REQUIRED)
find_program(GLSLANG_COMPILER NAMES glslang glslangValidator REQUIRED)

set(SIRIT_USE_SYSTEM_SPIRV_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/sirit EXCLUDE_FROM_ALL)

# bbport: FSR 3.1 temporal upscaling / frame generation, native Vulkan (MIT, FireBurn/FSR-Vulkan).
set(FFX_VK_PORTABLE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FFX_VK_PORTABLE_BUILD_PROBE OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/fsr-vulkan EXCLUDE_FROM_ALL)

# Host shaders -> headers (same scheme as shadPS4's host_shaders/CMakeLists.txt).
set(SHADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/host_shaders)
set(SHADER_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/include)
set(SHADER_DIR ${SHADER_INCLUDE}/video_core/host_shaders)
set(SHADER_HEADERS "")
function(add_host_shader FILENAME SHADER_NAME)
    set(OUT ${SHADER_DIR}/${SHADER_NAME}.h)
    string(TOUPPER ${SHADER_NAME} SYMBOL)
    set(DEFINES ${ARGN})
    list(TRANSFORM DEFINES PREPEND "-D")
    add_custom_command(OUTPUT ${OUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
        COMMAND ${GLSLANG_COMPILER} --target-env vulkan1.3 -o ${OUT} --vn ${SYMBOL} ${DEFINES} ${SHADER_SRC}/${FILENAME}
        COMMAND ${CMAKE_COMMAND} "-DHEADER_FILE=${OUT}" -P ${SHADER_SRC}/AddShaderHeaderInclude.cmake
        DEPENDS ${SHADER_SRC}/${FILENAME} VERBATIM)
    set(SHADER_HEADERS ${SHADER_HEADERS} ${OUT} PARENT_SCOPE)
endfunction()
add_host_shader(color_to_ms_depth.frag color_to_ms_depth_frag)
add_host_shader(ms_depth_to_color.frag ms_depth_to_color_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_msaa_frag SRC_MSAA=1)
add_host_shader(fault_buffer_process.comp fault_buffer_process_comp)
add_host_shader(fs_tri.vert fs_tri_vert)
add_host_shader(fsr.comp fsr_easu_comp SAMPLE_EASU=1)
add_host_shader(fsr.comp fsr_rcas_comp SAMPLE_RCAS=1)
# Frame generation (vk_frame_generation.cpp): its depth input and the UI over generated frames.
add_host_shader(frame_gen_inputs.comp frame_gen_inputs_comp)
add_host_shader(frame_gen_ui.comp frame_gen_ui_comp)
add_host_shader(frame_gen_flow_down.comp frame_gen_flow_down_comp)
add_host_shader(frame_gen_obj_cost.comp frame_gen_obj_cost_comp)
add_host_shader(frame_gen_flow_final.comp frame_gen_flow_final_comp)
add_host_shader(frame_gen_obj_pick.comp frame_gen_obj_pick_comp)
add_host_shader(post_process.frag post_process_frag)
add_host_shader(hdr_output.frag hdr_output_frag)
add_host_shader(hdr_exposure.comp hdr_exposure_comp)
add_host_shader(camera_motion.comp camera_motion_comp)
add_host_shader(drive_motion.comp drive_motion_comp)
add_host_shader(drive_native_motion.comp drive_native_motion_comp)
add_host_shader(drive_native_motion.comp drive_native_motion_ids_comp HAS_PREVIOUS_IDS=1)
add_host_shader(drive_aa_copy.comp drive_aa_copy_comp)
add_host_shader(taa_sharpen.comp drive_aa_sharpen_comp HALF_INPUT=1)
add_host_shader(upscale_merge.comp upscale_merge_comp)
add_host_shader(upscale_reactive.comp upscale_reactive_comp)
add_host_shader(neural_prepare.comp neural_prepare_comp)
add_host_shader(neural_resolve.comp neural_resolve_comp)
add_host_shader(neural_ui.comp neural_ui_comp)
add_host_shader(taa.comp taa_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_ldr_comp LDR_OUTPUT=1)
add_host_shader(buffer_multi_copy.comp buffer_multi_copy_comp)
add_host_shader(depth_resample.frag depth_resample_frag)
add_host_shader(depth_resample.frag depth_stencil_resample_frag HAS_STENCIL=1)
add_host_shader(depth_resample.frag depth_stencil_bits_frag STENCIL_BITS=1)
add_host_shader(camera_motion.comp camera_motion_debug_comp DEBUG_OVERLAY=1)
add_host_shader(cube_lod_check.comp cube_lod_check_comp)
add_host_shader(cube_lod_check.comp cube_lod_check_cube_comp CUBE=1)
foreach(BPP IN ITEMS 8 16 32 64 96 128)
    add_host_shader(tiling.comp tiling_micro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=0)
    add_host_shader(tiling.comp tiling_macro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=1)
endforeach()
add_custom_target(host_shaders DEPENDS ${SHADER_HEADERS})

file(GLOB_RECURSE VIDEO_CORE CONFIGURE_DEPENDS shadps4/video_core/*.cpp)
file(GLOB_RECURSE RECOMPILER CONFIGURE_DEPENDS shadps4/shader_recompiler/*.cpp)
list(FILTER VIDEO_CORE EXCLUDE REGEX "renderdoc\\.cpp$")
set(COMMON
    shadps4/common/decoder.cpp shadps4/common/error.cpp
    shadps4/common/io_file.cpp shadps4/common/native_clock.cpp shadps4/common/number_utils.cpp
    shadps4/common/path_util.cpp shadps4/common/rdtsc.cpp shadps4/common/spin_lock.cpp shadps4/common/recursive_lock.cpp
    shadps4/common/string_util.cpp shadps4/common/thread.cpp shadps4/common/signal_context.cpp
    shim/common/logging/log.cpp)
file(GLOB AVPLAYER CONFIGURE_DEPENDS shadps4/core/libraries/avplayer/*.cpp)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
file(GLOB SHIM_SOURCES CONFIGURE_DEPENDS shim/*.cpp shim/*/*.cpp)
# Windows: a DLL cannot leave the runtime_* symbols for DriveClubGame.exe to resolve, so the
# library is static there and linked into the executable.
if (WIN32)
    set(BBGPU_KIND STATIC)
else()
    set(BBGPU_KIND SHARED)
endif()
add_library(bbgpu ${BBGPU_KIND} ${VIDEO_CORE} ${RECOMPILER} ${COMMON} ${SHIM_SOURCES}
    shadps4/core/libraries/gnmdriver/gnmdriver.cpp
    shadps4/core/libraries/videoout/driver.cpp shadps4/core/libraries/videoout/video_out.cpp
    shadps4/core/libraries/kernel/equeue.cpp ${AVPLAYER}
    shadps4/core/libraries/videodec/video_utils.cpp
    shadps4/core/libraries/videodec/videodec.cpp shadps4/core/libraries/videodec/videodec_impl.cpp)
add_dependencies(bbgpu host_shaders)
# bbport co-op: PS4 network and PSN libraries with the shadNet client, from shadp2p
# (github.com/Wozzardman/shadp2p, GPL-2.0), for private Bloodborne co-op servers.
# Protobuf: prefer upstream's CMake package; distributions that only ship module-mode files
# (Ubuntu 24.04's libprotobuf-dev 3.21 has no protobuf-config.cmake) use FindProtobuf instead.
find_package(Protobuf CONFIG QUIET)
if (NOT Protobuf_FOUND)
    find_package(Protobuf REQUIRED)
    if (NOT TARGET protobuf::libprotobuf)
        list(GET Protobuf_LIBRARIES 0 BBPROTOBUF_LIBRARY)
        add_library(protobuf::libprotobuf UNKNOWN IMPORTED)
        set_target_properties(protobuf::libprotobuf PROPERTIES
            IMPORTED_LOCATION "${BBPROTOBUF_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${Protobuf_INCLUDE_DIRS}")
    endif()
    if (NOT TARGET protobuf::protoc)
        add_executable(protobuf::protoc IMPORTED)
        set_target_properties(protobuf::protoc PROPERTIES
            IMPORTED_LOCATION "${Protobuf_PROTOC_EXECUTABLE}")
    endif()
endif()
set(SHADNET_PROTO_OUT "${CMAKE_CURRENT_BINARY_DIR}/shadnet_proto_gen")
file(MAKE_DIRECTORY "${SHADNET_PROTO_OUT}")
add_custom_command(
    OUTPUT "${SHADNET_PROTO_OUT}/shadnet.pb.cc" "${SHADNET_PROTO_OUT}/shadnet.pb.h"
    COMMAND protobuf::protoc "--proto_path=${CMAKE_CURRENT_SOURCE_DIR}/shadps4/shadnet"
            "--cpp_out=${SHADNET_PROTO_OUT}" "${CMAKE_CURRENT_SOURCE_DIR}/shadps4/shadnet/shadnet.proto"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shadps4/shadnet/shadnet.proto")
file(GLOB_RECURSE BBNET_SOURCES CONFIGURE_DEPENDS shadps4/core/libraries/network/*.cpp
    shadps4/core/libraries/np/*.cpp shadps4/shadnet/*.cpp)
# Trophies and the system dialogs stay in the C runtime; their shadPS4 versions need the
# emulator's UI layers.
list(FILTER BBNET_SOURCES EXCLUDE REGEX
    "np_trophy|trophy_ui|np_sns_facebook_dialog|np_profile_dialog/|np_commerce/")
find_package(nlohmann_json CONFIG REQUIRED)
add_library(bbnet STATIC ${BBNET_SOURCES} "${SHADNET_PROTO_OUT}/shadnet.pb.cc" bbnet/bbnet_glue.cpp)
target_include_directories(bbnet PRIVATE shim shadps4 shadps4/shadnet "${SHADNET_PROTO_OUT}"
    third_party/imgui third_party/httplib)
target_link_libraries(bbnet PRIVATE nlohmann_json::nlohmann_json)
# HTTPS (the community game server only speaks TLS) and gzip replies in cpp-httplib.
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
target_compile_definitions(bbnet PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT CPPHTTPLIB_ZLIB_SUPPORT)
target_link_libraries(bbnet PRIVATE OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB)
# libScePngDec (bbnet/png_dec.cpp): the game decodes downloaded pictures (avatars) with it.
find_package(PNG REQUIRED)
target_sources(bbnet PRIVATE bbnet/png_dec.cpp)
target_link_libraries(bbnet PRIVATE PNG::PNG)
if (WIN32)
    target_sources(bbnet PRIVATE third_party/wepoll/wepoll.c)
    target_include_directories(bbnet PRIVATE third_party/wepoll)
endif()
# UPnP port mapping (net_upnp.h). Windows links the static MSYS2 package; Linux the shared one.
find_path(MINIUPNPC_INCLUDE miniupnpc.h PATH_SUFFIXES miniupnpc REQUIRED)
find_library(MINIUPNPC_LIBRARY NAMES libminiupnpc.a miniupnpc REQUIRED)
target_include_directories(bbnet PRIVATE "${MINIUPNPC_INCLUDE}")
if (WIN32)
    target_compile_definitions(bbnet PRIVATE MINIUPNP_STATICLIB)
endif()
target_link_libraries(bbnet PRIVATE "${MINIUPNPC_LIBRARY}")
target_compile_definitions(bbnet PRIVATE BOOST_ASIO_STANDALONE NDEBUG BB_DIAGNOSTICS=$<BOOL:${BB_DIAGNOSTICS}>)
target_compile_options(bbnet PRIVATE -Wno-deprecated-declarations -Wno-missing-field-initializers)
target_link_libraries(bbnet PUBLIC Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum
    protobuf::libprotobuf)
if (WIN32)
    target_compile_definitions(bbnet PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(bbnet PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-include ${CMAKE_CURRENT_SOURCE_DIR}/shim/bb_win_compat.h>)
    target_link_libraries(bbnet PUBLIC ws2_32 iphlpapi bcrypt crypt32)
endif()
target_link_libraries(bbgpu PUBLIC bbnet)
# In-game settings menu: Dear ImGui (third_party/imgui, MIT) with its Vulkan backend, and an
# embedded Cyrillic font (third_party/fonts).
target_sources(bbgpu PRIVATE third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp)
target_include_directories(bbgpu PRIVATE third_party/imgui third_party/imgui/backends)
target_compile_definitions(bbgpu PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES
    BB_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf"
    BB_EXIT_BODY_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../launcher/assets/fonts/001futura-webfont.ttf"
    BB_EXIT_HEADING_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../launcher/assets/fonts/004russian-webfont.ttf")
set_source_files_properties(shim/bbport_overlay.cpp PROPERTIES
    OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf)
target_include_directories(bbgpu PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include third_party/half/include)
target_compile_definitions(bbgpu PRIVATE BOOST_ASIO_STANDALONE NDEBUG)
target_compile_definitions(bbgpu PRIVATE BB_DIAGNOSTICS=$<BOOL:${BB_DIAGNOSTICS}>)
target_compile_options(bbgpu PRIVATE -Wno-deprecated-declarations -Wno-missing-field-initializers)
# Runtime symbols (runtime_memory_*, runtime_process_*) resolve against DriveClubGame.
if (NOT WIN32)
    target_link_options(bbgpu PRIVATE -Wl,--allow-shlib-undefined)
endif()
target_link_libraries(bbgpu PRIVATE ffx-vulkan::portable ffx-vulkan::fsr4-v07-vulkan
    ffx-vulkan::fsr3-vk-up315-fg316)
target_link_libraries(bbgpu PUBLIC Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum
    tsl::robin_map GPUOpen::VulkanMemoryAllocator PkgConfig::XXHASH PkgConfig::SDL3 PkgConfig::FFMPEG miniz::miniz Zydis::Zydis sirit)
if (WIN32)
    # Guest code is entered with the System V ABI; NOMINMAX/WIN32_LEAN_AND_MEAN keep
    # <windows.h> out of the way of std::min/max and the Vulkan/SDL headers.
    target_compile_definitions(bbgpu PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(bbgpu PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/shim/bb_win_compat.h)
    target_link_libraries(bbgpu PUBLIC Vulkan::Vulkan ws2_32 bcrypt ntdll synchronization winmm)
else()
    # The Vulkan loader is the host's; recording it as a dependency keeps libbbgpu loadable
    # when the executable is linked with --as-needed.
    target_link_libraries(bbgpu PUBLIC PkgConfig::X11 Vulkan::Vulkan)
endif()

# The SRT walker's x86-64 code generator (Xbyak, vendored). Other hosts (aarch64) interpret the walker
# instead (shader_recompiler/ir/passes/flatten_extended_userdata_pass.cpp).
if (WIN32 OR CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    target_include_directories(bbgpu PRIVATE third_party/xbyak)
endif()

# bbport (Windows): bbgpu is a static library there, so the loader and the HLE runtime are
# linked here, where CMake knows its dependencies (build.sh builds them with ninja).
if (WIN32)
    enable_language(RC)
    file(GLOB BB_RUNTIME CONFIGURE_DEPENDS ../src/runtime*.c)
    file(GLOB BB_ATRAC9 CONFIGURE_DEPENDS ../third_party/LibAtrac9/C/src/*.c)
    add_library(atrac9 STATIC ${BB_ATRAC9})
    target_compile_options(atrac9 PRIVATE -w)
    set_target_properties(atrac9 PROPERTIES C_STANDARD 99 INTERPROCEDURAL_OPTIMIZATION OFF)
    add_executable(DriveClubGame ../src/probe.c ${BB_RUNTIME} ../src/compat_win.c ../src/vulkan_smoke.c
        ../packaging/windows/DriveClubGame.rc)
    set_target_properties(DriveClubGame PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
    target_compile_definitions(DriveClubGame PRIVATE BB_DIAGNOSTICS=$<BOOL:${BB_DIAGNOSTICS}>)
    target_compile_options(DriveClubGame PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror>)
    target_include_directories(DriveClubGame PRIVATE .. ../src)
    target_link_libraries(DriveClubGame PRIVATE bbgpu atrac9 onecore bcrypt ws2_32 -pthread)
    # The PS4 range (64 GiB - 1 TiB) must stay free: no high-entropy ASLR for heaps and stacks.
    # The guest main thread runs on the process stack: 16 MiB, committed (no stack probes).
    target_link_options(DriveClubGame PRIVATE -Wl,--disable-high-entropy-va "SHELL:-Xlinker --stack -Xlinker 16777216,16777216")
endif()

option(BB_BUILD_DIAGNOSTIC_TESTS "Build the diagnostics gate regression test" OFF)
if (BB_BUILD_DIAGNOSTIC_TESTS)
    add_executable(bb_diagnostics_gate_test ../tools/diagnostics_gate_test.cpp
        shadps4/video_core/renderer_vulkan/vk_frame_capture.cpp)
    target_include_directories(bb_diagnostics_gate_test PRIVATE shim shadps4)
    target_compile_options(bb_diagnostics_gate_test PRIVATE -UNDEBUG)
    target_compile_definitions(bb_diagnostics_gate_test PRIVATE BB_DIAGNOSTICS=$<BOOL:${BB_DIAGNOSTICS}>)
    target_link_libraries(bb_diagnostics_gate_test PRIVATE Vulkan::Headers fmt::fmt)
    add_executable(bb_quarantine_test ../tools/quarantine_test.cpp)
    target_compile_options(bb_quarantine_test PRIVATE -UNDEBUG)
    target_compile_definitions(bb_quarantine_test PRIVATE BB_DIAGNOSTICS=$<BOOL:${BB_DIAGNOSTICS}>)
endif()
