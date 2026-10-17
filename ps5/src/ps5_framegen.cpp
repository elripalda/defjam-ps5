/* [ps5 port] Frame generation (the menu's Video > Frame generation): mihawk-99's
 * MihawkGen (ps5/third_party/mihawkgen, GPL-3.0-or-later) behind the renderer's
 * frame generator hook. Its trained network makes the picture halfway between
 * the game's last two frames from their colour alone; the renderer shows that
 * and then the frame, on a 120 Hz output. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>
#include "framegen.h"

extern "C" {
#include "d3d8_vk.h"
const char *ps5_root(void);
void ps5_framegen_register(void);
}

static fg_context *s_ctx;

static std::vector<unsigned char> read_file(const std::string &path)
{
    std::vector<unsigned char> data;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return data;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0 && n < 64 * 1024 * 1024) {
        data.resize((size_t)n);
        if (fread(data.data(), 1, data.size(), f) != data.size())
            data.clear();
    }
    fclose(f);
    return data;
}

static int create(void *, void *device, void *physical_device, uint32_t width, uint32_t height)
{
    char error[256] = "";
    fg_create_info info;
    memset(&info, 0, sizeof info);
    info.device = (VkDevice)device;
    info.physical_device = (VkPhysicalDevice)physical_device;
    info.width = width;
    info.height = height;
    info.slots = 1;                     /* the renderer waits for every frame it submits */
    info.output_format = VK_FORMAT_R8G8B8A8_UNORM;
    if (const char *why = fg_check_create_info(&info)) {
        fprintf(stderr, "[FRAMEGEN] %s\n", why);
        return 0;
    }
    if (fg_context_create(&info, &s_ctx, error, sizeof error) != VK_SUCCESS) {
        fprintf(stderr, "[FRAMEGEN] could not start: %s\n", error);
        s_ctx = nullptr;
        return 0;
    }
    std::string path = std::string(ps5_root()) + "/framegen/network.fgw";
    std::vector<unsigned char> blob = read_file(path);
    if (blob.empty() || fg_context_load_network(s_ctx, blob.data(), blob.size(), error, sizeof error) != VK_SUCCESS) {
        fprintf(stderr, "[FRAMEGEN] no network from %s: %s\n", path.c_str(), blob.empty() ? "missing" : error);
        fg_context_destroy(s_ctx);
        s_ctx = nullptr;
        return 0;
    }
    fprintf(stderr, "[FRAMEGEN] MihawkGen ready, %ux%u\n", width, height);
    return 1;
}

static void generate(void *, void *command_buffer, void *prev_view, void *next_view, void *out_view, float t)
{
    fg_frame_pair pair;
    memset(&pair, 0, sizeof pair);
    pair.prev_color = (VkImageView)prev_view;
    pair.next_color = (VkImageView)next_view;
    pair.t = t;
    fg_record_generate(s_ctx, (VkCommandBuffer)command_buffer, 0, FG_METHOD_NEURAL, &pair, (VkImageView)out_view);
}

static void destroy(void *)
{
    if (s_ctx)
        fg_context_destroy(s_ctx);
    s_ctx = nullptr;
}

static const D3D8VkFrameGen s_generator = { create, generate, destroy, nullptr };

void ps5_framegen_register(void)
{
    d3d8_vk_set_frame_generator(&s_generator);
}
