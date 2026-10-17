# [ps5 port] What the PS5 build needs before the toolkit is configured:
# SDL3, glslang and the Vulkan headers built for the PS5 in DEFJAM_PS5_PREFIX
# (see ps5/README.md), the Vulkan target (headers + the generated entry
# points that call RADV) and shaderc on glslang.

if(NOT DEFJAM_PS5_PREFIX)
  set(DEFJAM_PS5_PREFIX "$ENV{DEFJAM_PS5_PREFIX}")
endif()
if(NOT DEFJAM_PS5_PREFIX OR NOT EXISTS "${DEFJAM_PS5_PREFIX}/lib/libSDL3.a")
  message(FATAL_ERROR "Set DEFJAM_PS5_PREFIX to the folder with the PS5 builds of SDL3, glslang and the Vulkan headers")
endif()
list(PREPEND CMAKE_PREFIX_PATH "${DEFJAM_PS5_PREFIX}")
list(PREPEND CMAKE_FIND_ROOT_PATH "${DEFJAM_PS5_PREFIX}")
set(DEFJAM_PS5_DIR "${CMAKE_CURRENT_LIST_DIR}/..")

# No separate libm on the PS5 (the maths is in libc): a target named m makes
# the toolkit's "m" link to nothing.
add_library(m INTERFACE)

# Vulkan: headers, and the entry points (generated from the sources that call
# Vulkan, so every command used has one).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_vk_sources
    "${XBOXRECOMP_DIR}/src/d3d/d3d8_vk.c"
    "${DEFJAM_PS5_DIR}/src/host_ps5.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/hooks/pc_ui.cpp"
    "${DEFJAM_PS5_DIR}/third_party/mihawkgen/src/fg_vulkan.cpp"
    "${DEFJAM_PS5_DIR}/third_party/mihawkgen/src/fg_neural.cpp"
    "${DEFJAM_PS5_DIR}/third_party/mihawkgen/src/fg_splat.cpp"
    "${DEFJAM_PS5_DIR}/third_party/mihawkgen/src/fg_common.cpp")
if(DEFJAM_PS5_IMGUI_DIR)
  list(APPEND _vk_sources "${DEFJAM_PS5_IMGUI_DIR}/backends/imgui_impl_vulkan.cpp")
endif()
set(_vk_loader "${CMAKE_BINARY_DIR}/vk_ps5_loader.c")
add_custom_command(
  OUTPUT "${_vk_loader}"
  COMMAND Python3::Interpreter "${DEFJAM_PS5_DIR}/tools/gen_vk_loader.py"
          "${DEFJAM_PS5_PREFIX}/include/vulkan/vulkan_core.h" "${_vk_loader}" ${_vk_sources}
  DEPENDS "${DEFJAM_PS5_DIR}/tools/gen_vk_loader.py" ${_vk_sources}
  COMMENT "[ps5 port] Vulkan entry points for RADV")
add_library(defjam_vk_ps5 STATIC "${_vk_loader}")
target_include_directories(defjam_vk_ps5 PUBLIC "${DEFJAM_PS5_PREFIX}/include")
add_library(Vulkan::Vulkan ALIAS defjam_vk_ps5)
set(Vulkan_FOUND TRUE)
set(Vulkan_INCLUDE_DIRS "${DEFJAM_PS5_PREFIX}/include")
set(Vulkan_LIBRARY "")

# shaderc's API on glslang.
find_package(glslang REQUIRED CONFIG)
enable_language(CXX)
add_library(defjam_shaderc STATIC "${DEFJAM_PS5_DIR}/shaderc/shaderc_glslang.cpp")
target_include_directories(defjam_shaderc PUBLIC "${DEFJAM_PS5_DIR}/shaderc/include")
target_link_libraries(defjam_shaderc PRIVATE glslang::glslang glslang::SPIRV glslang::glslang-default-resource-limits)
set_target_properties(defjam_shaderc PROPERTIES CXX_STANDARD 17)
target_compile_definitions(defjam_shaderc PRIVATE ENABLE_HLSL=1)

# Every C file: a flush of stdout or stderr does nothing (the port's flush
# thread writes them once a second), so no game thread waits on the file
# system for a log line. See ps5/include/ps5_quiet_fflush.h.
add_compile_options("$<$<COMPILE_LANGUAGE:C>:-include${DEFJAM_PS5_DIR}/include/ps5_quiet_fflush.h>")
