# Desktop build of the tico frontend + mgba, for developing and testing the
# Vulkan renderer and slang shaders off-device. The Switch NRO is built by
# build_mgba_nro.sh. Run ./build_desktop.sh from the repo root.
cmake_minimum_required(VERSION 3.20)
project(tico_mgba_desktop C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(DEPS ${CMAKE_CURRENT_SOURCE_DIR}/deps)
set(MGBA_CORE_LIB "" CACHE FILEPATH "Static mgba libretro core")

set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(${DEPS}/glslang glslang EXCLUDE_FROM_ALL)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2 SDL2_mixer)
find_package(CURL REQUIRED)
find_package(LibArchive REQUIRED)

file(GLOB RCHEEVOS_SOURCES ${ROOT}/rcheevos/src/*.c ${ROOT}/rcheevos/src/*/*.c)
list(FILTER RCHEEVOS_SOURCES EXCLUDE REGEX "rc_client_external\\.c$")

set(LRC ${ROOT}/libretro/libretro-common)
add_library(lrc STATIC
    ${LRC}/compat/compat_posix_string.c ${LRC}/compat/compat_strcasestr.c
    ${LRC}/compat/compat_snprintf.c ${LRC}/compat/compat_strl.c ${LRC}/compat/fopen_utf8.c
    ${LRC}/encodings/encoding_utf.c ${LRC}/encodings/encoding_deflate.c
    ${LRC}/file/file_path.c ${LRC}/file/file_path_io.c ${LRC}/file/retro_dirent.c
    ${LRC}/streams/file_stream.c ${LRC}/streams/file_stream_transforms.c
    ${LRC}/string/stdstring.c ${LRC}/time/rtime.c ${LRC}/vfs/vfs_implementation.c)
target_include_directories(lrc PRIVATE ${LRC}/include ${ROOT}/libretro)

add_library(rcheevos STATIC ${RCHEEVOS_SOURCES})
target_include_directories(rcheevos PUBLIC ${ROOT}/rcheevos/include PRIVATE ${ROOT}/libretro)
target_compile_definitions(rcheevos PUBLIC RC_CLIENT_SUPPORTS_HASH)

add_executable(tico-mgba
    TicoMain.cpp TicoCore.cpp TicoVulkan.cpp TicoShaderChain.cpp TicoSlang.cpp
    overlay/imgui_overlay.cpp overlay/overlay_ui.cpp overlay/ra_alerts.cpp
    overlay/tico_config.cpp overlay/translation_manager.cpp
    ${DEPS}/volk/volk.c
    ${DEPS}/SPIRV-Reflect/spirv_reflect.c
    ${DEPS}/imgui/imgui.cpp ${DEPS}/imgui/imgui_draw.cpp ${DEPS}/imgui/imgui_tables.cpp
    ${DEPS}/imgui/imgui_widgets.cpp ${DEPS}/imgui/imgui_demo.cpp
    ${DEPS}/imgui/backends/imgui_impl_sdl2.cpp ${DEPS}/imgui/backends/imgui_impl_vulkan.cpp)
target_include_directories(tico-mgba PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${DEPS} ${DEPS}/imgui ${DEPS}/imgui/backends
    ${DEPS}/volk ${DEPS}/vulkan-headers ${DEPS}/glslang ${DEPS}/SPIRV-Reflect
    ${ROOT}/libretro)
target_compile_definitions(tico-mgba PRIVATE
    IMGUI_IMPL_VULKAN_NO_PROTOTYPES VK_NO_PROTOTYPES TICO_APP_VERSION="desktop")
target_link_libraries(tico-mgba PRIVATE
    ${MGBA_CORE_LIB} lrc rcheevos glslang glslang-default-resource-limits
    PkgConfig::SDL2 CURL::libcurl LibArchive::LibArchive ${CMAKE_DL_LIBS} pthread)

# Headless preset runner: tools/shader_test.cpp
add_executable(shader_test
    tools/shader_test.cpp TicoVulkan.cpp TicoShaderChain.cpp TicoSlang.cpp
    ${DEPS}/volk/volk.c ${DEPS}/SPIRV-Reflect/spirv_reflect.c
    ${DEPS}/imgui/imgui.cpp ${DEPS}/imgui/imgui_draw.cpp ${DEPS}/imgui/imgui_tables.cpp
    ${DEPS}/imgui/imgui_widgets.cpp ${DEPS}/imgui/backends/imgui_impl_vulkan.cpp)
target_include_directories(shader_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${DEPS} ${DEPS}/imgui ${DEPS}/imgui/backends
    ${DEPS}/volk ${DEPS}/vulkan-headers ${DEPS}/glslang ${DEPS}/SPIRV-Reflect ${ROOT}/libretro)
target_compile_definitions(shader_test PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES VK_NO_PROTOTYPES)
target_link_libraries(shader_test PRIVATE glslang glslang-default-resource-limits
    PkgConfig::SDL2 ${CMAKE_DL_LIBS} pthread)
