/*
 * [ps5 port] The part of shaderc's C API the Vulkan device uses, backed by
 * glslang directly (ps5/shaderc/shaderc_glslang.cpp). shaderc itself pulls in
 * SPIRV-Tools and a build that has no PS5 support; this is the same calls on
 * the compiler underneath it.
 */
#ifndef DEFJAM_PS5_SHADERC_H
#define DEFJAM_PS5_SHADERC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    shaderc_vertex_shader = 0,
    shaderc_fragment_shader = 1,
} shaderc_shader_kind;

typedef enum {
    shaderc_source_language_glsl = 0,
    shaderc_source_language_hlsl = 1,
} shaderc_source_language;

typedef enum {
    shaderc_optimization_level_zero = 0,
    shaderc_optimization_level_size = 1,
    shaderc_optimization_level_performance = 2,
} shaderc_optimization_level;

typedef enum {
    shaderc_uniform_kind_image = 0,
    shaderc_uniform_kind_sampler = 1,
    shaderc_uniform_kind_texture = 2,
    shaderc_uniform_kind_buffer = 3,
    shaderc_uniform_kind_storage_buffer = 4,
    shaderc_uniform_kind_unordered_access_view = 5,
} shaderc_uniform_kind;

typedef enum {
    shaderc_compilation_status_success = 0,
    shaderc_compilation_status_invalid_stage = 1,
    shaderc_compilation_status_compilation_error = 2,
    shaderc_compilation_status_internal_error = 3,
    shaderc_compilation_status_null_result_object = 4,
    shaderc_compilation_status_invalid_assembly = 5,
} shaderc_compilation_status;

typedef struct shaderc_compiler *shaderc_compiler_t;
typedef struct shaderc_compile_options *shaderc_compile_options_t;
typedef struct shaderc_compilation_result *shaderc_compilation_result_t;

shaderc_compiler_t shaderc_compiler_initialize(void);
void shaderc_compiler_release(shaderc_compiler_t compiler);

shaderc_compile_options_t shaderc_compile_options_initialize(void);
void shaderc_compile_options_release(shaderc_compile_options_t options);
void shaderc_compile_options_set_source_language(shaderc_compile_options_t options,
                                                 shaderc_source_language lang);
void shaderc_compile_options_set_optimization_level(shaderc_compile_options_t options,
                                                    shaderc_optimization_level level);
void shaderc_compile_options_set_auto_map_locations(shaderc_compile_options_t options, bool auto_map);
void shaderc_compile_options_set_hlsl_io_mapping(shaderc_compile_options_t options, bool hlsl_iomap);
void shaderc_compile_options_set_hlsl_offsets(shaderc_compile_options_t options, bool hlsl_offsets);
void shaderc_compile_options_set_binding_base(shaderc_compile_options_t options,
                                              shaderc_uniform_kind kind, uint32_t base);

shaderc_compilation_result_t shaderc_compile_into_spv(
    const shaderc_compiler_t compiler, const char *source_text, size_t source_text_size,
    shaderc_shader_kind shader_kind, const char *input_file_name, const char *entry_point_name,
    const shaderc_compile_options_t additional_options);

void shaderc_result_release(shaderc_compilation_result_t result);
size_t shaderc_result_get_length(const shaderc_compilation_result_t result);
const char *shaderc_result_get_bytes(const shaderc_compilation_result_t result);
shaderc_compilation_status shaderc_result_get_compilation_status(const shaderc_compilation_result_t result);
const char *shaderc_result_get_error_message(const shaderc_compilation_result_t result);

#ifdef __cplusplus
}
#endif

#endif
