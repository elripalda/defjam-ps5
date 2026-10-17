/*
 * [ps5 port] shaderc's C API (the calls in ps5/shaderc/include/shaderc/shaderc.h)
 * on glslang, compiled for the PS5. The Vulkan device writes its shaders in
 * HLSL and compiles them while it runs, so the compiler has to be on the
 * console; glslang is the compiler inside shaderc, and these are the same
 * settings shaderc gives it.
 */
#include <shaderc/shaderc.h>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <mutex>
#include <string>
#include <vector>

struct shaderc_compiler {
    int unused;
};

struct shaderc_compile_options {
    shaderc_source_language lang = shaderc_source_language_glsl;
    bool auto_map_locations = false;
    bool hlsl_io_mapping = false;
    bool hlsl_offsets = false;
    uint32_t base[6] = {0, 0, 0, 0, 0, 0};
};

struct shaderc_compilation_result {
    std::vector<uint32_t> spirv;
    std::string error;
    shaderc_compilation_status status = shaderc_compilation_status_internal_error;
};

static std::once_flag s_init;

extern "C" {

shaderc_compiler_t shaderc_compiler_initialize(void)
{
    std::call_once(s_init, [] { glslang::InitializeProcess(); });
    return new shaderc_compiler();
}

void shaderc_compiler_release(shaderc_compiler_t compiler) { delete compiler; }

shaderc_compile_options_t shaderc_compile_options_initialize(void) { return new shaderc_compile_options(); }
void shaderc_compile_options_release(shaderc_compile_options_t options) { delete options; }

void shaderc_compile_options_set_source_language(shaderc_compile_options_t o, shaderc_source_language lang)
{ o->lang = lang; }
void shaderc_compile_options_set_optimization_level(shaderc_compile_options_t o, shaderc_optimization_level level)
{ (void)o; (void)level; /* RADV's own compiler optimizes; no SPIRV-Tools here */ }
void shaderc_compile_options_set_auto_map_locations(shaderc_compile_options_t o, bool v) { o->auto_map_locations = v; }
void shaderc_compile_options_set_hlsl_io_mapping(shaderc_compile_options_t o, bool v) { o->hlsl_io_mapping = v; }
void shaderc_compile_options_set_hlsl_offsets(shaderc_compile_options_t o, bool v) { o->hlsl_offsets = v; }
void shaderc_compile_options_set_binding_base(shaderc_compile_options_t o, shaderc_uniform_kind kind, uint32_t base)
{
    if ((unsigned)kind < 6)
        o->base[kind] = base;
}

shaderc_compilation_result_t shaderc_compile_into_spv(
    const shaderc_compiler_t compiler, const char *source_text, size_t source_text_size,
    shaderc_shader_kind shader_kind, const char *input_file_name, const char *entry_point_name,
    const shaderc_compile_options_t o)
{
    auto *res = new shaderc_compilation_result();
    (void)compiler;
    const EShLanguage stage = shader_kind == shaderc_vertex_shader ? EShLangVertex : EShLangFragment;
    const bool hlsl = o && o->lang == shaderc_source_language_hlsl;

    glslang::TShader shader(stage);
    const char *text = source_text;
    const int len = (int)source_text_size;
    const char *name = input_file_name ? input_file_name : "shader";
    shader.setStringsWithLengthsAndNames(&text, &len, &name, 1);
    shader.setEntryPoint(entry_point_name ? entry_point_name : "main");
    shader.setSourceEntryPoint(entry_point_name ? entry_point_name : "main");
    shader.setEnvInput(hlsl ? glslang::EShSourceHlsl : glslang::EShSourceGlsl, stage,
                       glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    if (o) {
        shader.setAutoMapLocations(o->auto_map_locations);
        shader.setHlslIoMapping(o->hlsl_io_mapping);
        shader.setShiftBinding(glslang::EResUbo, o->base[shaderc_uniform_kind_buffer]);
        shader.setShiftBinding(glslang::EResTexture, o->base[shaderc_uniform_kind_texture]);
        shader.setShiftBinding(glslang::EResSampler, o->base[shaderc_uniform_kind_sampler]);
        shader.setShiftBinding(glslang::EResImage, o->base[shaderc_uniform_kind_image]);
        shader.setShiftBinding(glslang::EResSsbo, o->base[shaderc_uniform_kind_storage_buffer]);
        shader.setShiftBinding(glslang::EResUav, o->base[shaderc_uniform_kind_unordered_access_view]);
    }

    int messages = EShMsgSpvRules | EShMsgVulkanRules;
    if (hlsl)
        messages |= EShMsgReadHlsl;
    if (o && o->hlsl_offsets)
        messages |= EShMsgHlslOffsets;

    if (!shader.parse(GetDefaultResources(), 110, false, (EShMessages)messages)) {
        res->status = shaderc_compilation_status_compilation_error;
        res->error = shader.getInfoLog();
        return res;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link((EShMessages)messages) || !program.mapIO()) {
        res->status = shaderc_compilation_status_compilation_error;
        res->error = program.getInfoLog();
        return res;
    }
    glslang::SpvOptions spv;
    spv.generateDebugInfo = false;
    spv.disableOptimizer = true;
    spv.validate = false;
    glslang::GlslangToSpv(*program.getIntermediate(stage), res->spirv, &spv);
    res->status = res->spirv.empty() ? shaderc_compilation_status_internal_error
                                     : shaderc_compilation_status_success;
    return res;
}

void shaderc_result_release(shaderc_compilation_result_t result) { delete result; }
size_t shaderc_result_get_length(const shaderc_compilation_result_t r) { return r ? r->spirv.size() * 4 : 0; }
const char *shaderc_result_get_bytes(const shaderc_compilation_result_t r)
{ return r ? (const char *)r->spirv.data() : nullptr; }
shaderc_compilation_status shaderc_result_get_compilation_status(const shaderc_compilation_result_t r)
{ return r ? r->status : shaderc_compilation_status_null_result_object; }
const char *shaderc_result_get_error_message(const shaderc_compilation_result_t r)
{ return r ? r->error.c_str() : "no result"; }

}  // extern "C"
