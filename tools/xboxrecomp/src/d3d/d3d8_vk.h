/*
 * d3d8_vk.h -- what the Vulkan backend needs from the program that hosts it.
 *
 * The backend creates the Vulkan instance and device and draws; it does not
 * create a window. A host that has one says which instance extensions its
 * window system needs and how to make a surface for it, before the device is
 * created. With no host set (or a host that returns no surface) the backend
 * renders off screen: every frame is drawn and can be captured, and nothing
 * is presented. That is what an unattended test run on a machine without a
 * display uses.
 */
#ifndef XBOXRECOMP_D3D8_VK_H
#define XBOXRECOMP_D3D8_VK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct D3D8VkHost {
    /* Instance extensions the window system needs (VK_KHR_surface and its
     * platform one), as SDL_Vulkan_GetInstanceExtensions returns them. */
    const char *const *instance_extensions;
    uint32_t           instance_extension_count;
    /* Make a VkSurfaceKHR for the window: instance is a VkInstance, surface
     * points at a VkSurfaceKHR. Returns non-zero on success. Called once,
     * from the thread that creates the device. */
    int  (*create_surface)(void *instance, void *surface, void *user);
    /* The window's size in pixels, asked whenever the swap chain is built. */
    void (*drawable_size)(int *width, int *height, void *user);
    void *user;
    /* The refresh rate of the display the window is on, in whole Hz (0 when
     * unknown), asked whenever the swap chain is built. Optional. */
    unsigned (*refresh_hz)(void *user);
} D3D8VkHost;

void d3d8_vk_set_host(const D3D8VkHost *host);

/* A host program's drawing on top of the picture, in window pixels, after the
 * title's target has been scaled into the window: a menu, an on-screen display.
 * The Vulkan counterpart of d3d8_present_set_overlay.
 *
 * Each frame the backend asks `active`; when it says yes, the backend begins a
 * render pass on the window's image (colour attachment, the picture already in
 * it, kept), calls `draw` on the thread that presents with the frame's command
 * buffer recording inside that pass, and ends it. `draw` records draw commands
 * and nothing else it must not submit that buffer. Handles are Vulkan's, as
 * plain pointers so this header does not need vulkan.h.
 *
 * `generation` changes whenever `render_pass` or the image count was replaced
 * (a swap chain with another format); a host that built pipelines against the
 * pass makes them again. The picture a harness captures is the title's target,
 * so nothing drawn here is ever in a capture. */
typedef struct D3D8VkOverlayFrame {
    void    *instance;          /* VkInstance */
    void    *physical_device;   /* VkPhysicalDevice */
    void    *device;            /* VkDevice */
    void    *queue;             /* VkQueue, the one that submits the frame */
    uint32_t queue_family;
    void    *command_buffer;    /* VkCommandBuffer, inside the pass */
    void    *render_pass;       /* VkRenderPass the draw is compatible with */
    uint32_t image_count;       /* swap chain images */
    uint32_t width, height;     /* window pixels */
    int      format;            /* VkFormat of the window's image */
    uint32_t generation;
    int32_t  picture_x0, picture_y0, picture_x1, picture_y1;  /* where the title's picture is, in window pixels */
    void    *side_blur;         /* VkImageView: the picture's 4:3 middle blurred (64x48), when asked for; else NULL */
    uint32_t side_blur_generation;   /* changes when that image is made again */
} D3D8VkOverlayFrame;

typedef struct D3D8VkOverlay {
    int  (*active)(void *user);
    void (*draw)(const D3D8VkOverlayFrame *frame, void *user);
    void *user;
} D3D8VkOverlay;

/* NULL removes it. Safe to call before the device exists. */
void d3d8_vk_set_overlay(const D3D8VkOverlay *overlay);

/* From the overlay's active(): blur the picture's 4:3 middle this frame and pass
 * it to draw() as side_blur, for filling the sides beside a 4:3 menu. */
void d3d8_vk_request_side_blur(void);

/* A frame generator, the host's: with one set (on a display refreshing at twice
 * the title's rate), each title frame is shown twice, first as a picture the
 * generator makes halfway between the previous frame and this one, then as
 * itself. Handles are Vulkan's as plain pointers. `create` is called when the
 * picture's size is known (and again after `destroy` if it changes) and says
 * whether the generator is ready. `generate` records into `command_buffer`:
 * `prev` and `next` are R8G8B8A8 views in SHADER_READ_ONLY_OPTIMAL, `out` an
 * R8G8B8A8 storage view in GENERAL, and the backend makes every barrier around
 * the call. NULL removes it; call before the first frame. */
typedef struct D3D8VkFrameGen {
    int  (*create)(void *user, void *device, void *physical_device, uint32_t width, uint32_t height);
    void (*generate)(void *user, void *command_buffer, void *prev_view, void *next_view, void *out_view, float t);
    void (*destroy)(void *user);
    void *user;
} D3D8VkFrameGen;
void d3d8_vk_set_frame_generator(const D3D8VkFrameGen *generator);
/* With a generator set: on (the default) shows the generated pictures; off shows
 * each title frame once, as without a generator, and lets the generator go. Any
 * time, any thread; it takes effect at the next present. */
void d3d8_vk_set_frame_generation(int on);

/* DXT textures kept compressed on the GPU (on, the default, where the device can)
 * or decoded to R8G8B8A8 (off). Applies to textures made from then on. */
void d3d8_vk_set_native_bc(int on);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_D3D8_VK_H */
