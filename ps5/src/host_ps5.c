/*
 * host_ps5.c -- [ps5 port] what src/host_posix.c does on a desktop, for the
 * PS5: there is no window, so the picture goes straight to the TV through
 * VK_KHR_display (PS5_Mesa's RADV), and the DualSense is read with scePad and
 * handed to SDL as a gamepad, where the input layer (input_host.c) picks it up
 * like any other pad.
 *
 * The app lives in /data/homebrew/PPSA92046; the player's own extracted disc
 * goes in its GameData folder. Saves, settings.ini and the log are kept beside
 * it.
 */
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#include "d3d8_vk.h"
#include "mmio_trap.h"
#include "hooks/pc_settings.h"

/* The app's own folder as the app sees it (the same folder as
 * /data/homebrew/PPSA92046 when installed there): the process cannot rely on
 * reaching /data, and starts somewhere read-only. */
#ifndef DEFJAM_PS5_ROOT
#define DEFJAM_PS5_ROOT "/app0"
#endif
#define DEFJAM_PS5_ROOT_ALT "/data/homebrew/PPSA92046"

/* ── PS5 system calls ──────────────────────────────────────────────────── */

int sceSystemServiceHideSplashScreen(void);
int sceSystemServiceLoadExec(const char *path, void *args);
int sceUserServiceInitialize(void *params);
int sceUserServiceGetForegroundUser(int32_t *user_id);
typedef struct { int32_t user_id[4]; } SceUserServiceLoginUserIdList;
int sceUserServiceGetLoginUserIdList(SceUserServiceLoginUserIdList *list);
int scePadInit(void);
int scePadOpen(int32_t user_id, int32_t type, int32_t index, const void *param);
int scePadGetHandle(int32_t user_id, int32_t type, int32_t index);
int scePadReadState(int32_t handle, void *data);
int scePadClose(int32_t handle);
typedef struct { uint8_t large_motor, small_motor; } ScePadVibrationParam;
int scePadSetVibration(int32_t handle, const ScePadVibrationParam *param);
int scePadSetVibrationMode(int32_t handle, int32_t mode);

/* The start of ScePadData, with room for the rest (verified on hardware by
 * ps5-plume-triangle and the FIFA Street port). */
typedef struct {
    uint32_t buttons;
    uint8_t  lx, ly, rx, ry;
    uint8_t  l2, r2;
    uint16_t pad0;
    uint8_t  rest[240];
} PadData;

enum {
    PAD_L3 = 0x2, PAD_R3 = 0x4, PAD_OPTIONS = 0x8,
    PAD_UP = 0x10, PAD_RIGHT = 0x20, PAD_DOWN = 0x40, PAD_LEFT = 0x80,
    PAD_L1 = 0x400, PAD_R1 = 0x800,
    PAD_TRIANGLE = 0x1000, PAD_CIRCLE = 0x2000, PAD_CROSS = 0x4000, PAD_SQUARE = 0x8000,
    PAD_TOUCHPAD = 0x100000,
};

/* ── DualSense -> SDL gamepad ──────────────────────────────────────────── */

#define MAX_PADS 4
#define SYSTEM_USER 0xFF

typedef struct {
    int32_t user, handle;
    SDL_JoystickID id;
    SDL_Joystick *joy;
    int dead;               /* the last read failed and the pad was released */
} Ps5Pad;

static Ps5Pad s_pads[MAX_PADS];
static Uint64 s_last_scan;

/* 0..255 with 128 at rest onto the full axis, the same reach both ways
 * (255 used to stop at 32512, 99% of a push right or down). */
static Sint16 axis(uint8_t v)
{
    int a = (int)v - 128;
    return (Sint16)(a < 0 ? a * 256 : a * 32767 / 127);
}

/* A virtual joystick's trigger axes use the whole range, released at the
 * minimum (SDL fills them with SDL_JOYSTICK_AXIS_MIN when it attaches one),
 * and SDL halves that range into the gamepad's 0..32767. Written as 0..32767,
 * a trigger at rest read as half pulled, so the title saw both triggers held
 * all the time: harmless in a fight (its press threshold is half way), but
 * the first-time help popups close only when nothing at all is held, and the
 * pause menu's START close is refused while a held trigger auto-repeats. */
static Sint16 trigger(uint8_t v)
{
    return (Sint16)((int)v * 65535 / 255 - 32768);
}

static void SDLCALL pad_update(void *userdata)
{
    Ps5Pad *p = userdata;
    PadData d;
    uint32_t b;

    if (!p->joy || p->handle < 0)
        return;
    memset(&d, 0, sizeof d);
    if (scePadReadState(p->handle, &d) != 0) {
        /* A read that fails (the pad switched off, its user signed out) must
         * not leave the last buttons held and the sticks pushed: the game
         * would keep seeing them. Released once, until reads work again. */
        int i;
        if (p->dead)
            return;
        p->dead = 1;
        for (i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
            SDL_SetJoystickVirtualButton(p->joy, i, false);
        SDL_SetJoystickVirtualAxis(p->joy, SDL_GAMEPAD_AXIS_LEFTX, 0);
        SDL_SetJoystickVirtualAxis(p->joy, SDL_GAMEPAD_AXIS_LEFTY, 0);
        SDL_SetJoystickVirtualAxis(p->joy, SDL_GAMEPAD_AXIS_RIGHTX, 0);
        SDL_SetJoystickVirtualAxis(p->joy, SDL_GAMEPAD_AXIS_RIGHTY, 0);
        SDL_SetJoystickVirtualAxis(p->joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, trigger(0));
        SDL_SetJoystickVirtualAxis(p->joy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, trigger(0));
        return;
    }
    p->dead = 0;
    b = d.buttons;
    SDL_SetJoystickVirtualButton(p->joy, SDL_GAMEPAD_BUTTON_SOUTH, (b & PAD_CROSS) != 0);
    SDL_SetJoystickVirtualButton(p->joy, SDL_GAMEPAD_BUTTON_EAST, (b & PAD_CIRCLE) != 0);
    SDL_SetJoystickVirtualButton(p->joy, SDL_GAMEPAD_BUTTON_WEST, (b & PAD_SQUARE) != 0);
    SDL_SetJoystickVirtualButton(p->joy, SDL_GAMEPAD_BUTTON_NORTH, (b & PAD_TRIANGLE) != 0);
    SDL_SetJoystickVirtualButton(p->joy, SDL_GAMEPAD_BUTTON_BACK, (b & PAD_TOUCHPAD) != 0);
    SDL_SetJoystickVirtualButton(p->joy, SDL_GAMEPAD_BUTTON_START, (b & PAD_OPTIONS) != 0);
    SDL_SetJoystickVirtualButton(p->joy, SDL_GAMEPAD_BUTTON_LEFT_STICK, (b & PAD_L3) != 0);
    SDL_SetJoystickVirtualButton(p->joy, SDL_GAMEPAD_BUTTON_RIGHT_STICK, (b & PAD_R3) != 0);
    SDL_SetJoystickVirtualButton(p->joy, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, (b & PAD_L1) != 0);
    SDL_SetJoystickVirtualButton(p->joy, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, (b & PAD_R1) != 0);
    SDL_SetJoystickVirtualButton(p->joy, SDL_GAMEPAD_BUTTON_DPAD_UP, (b & PAD_UP) != 0);
    SDL_SetJoystickVirtualButton(p->joy, SDL_GAMEPAD_BUTTON_DPAD_DOWN, (b & PAD_DOWN) != 0);
    SDL_SetJoystickVirtualButton(p->joy, SDL_GAMEPAD_BUTTON_DPAD_LEFT, (b & PAD_LEFT) != 0);
    SDL_SetJoystickVirtualButton(p->joy, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, (b & PAD_RIGHT) != 0);
    SDL_SetJoystickVirtualAxis(p->joy, SDL_GAMEPAD_AXIS_LEFTX, axis(d.lx));
    SDL_SetJoystickVirtualAxis(p->joy, SDL_GAMEPAD_AXIS_LEFTY, axis(d.ly));
    SDL_SetJoystickVirtualAxis(p->joy, SDL_GAMEPAD_AXIS_RIGHTX, axis(d.rx));
    SDL_SetJoystickVirtualAxis(p->joy, SDL_GAMEPAD_AXIS_RIGHTY, axis(d.ry));
    SDL_SetJoystickVirtualAxis(p->joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, trigger(d.l2));
    SDL_SetJoystickVirtualAxis(p->joy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, trigger(d.r2));
}

static bool SDLCALL pad_rumble(void *userdata, Uint16 low, Uint16 high)
{
    Ps5Pad *p = userdata;
    ScePadVibrationParam v;
    if (p->handle < 0)
        return false;
    v.large_motor = (uint8_t)(low >> 8);
    v.small_motor = (uint8_t)(high >> 8);
    {
        static unsigned logged;
        int r = scePadSetVibration(p->handle, &v);
        if ((v.large_motor || v.small_motor || r != 0) && logged < 6) {
            logged++;
            fprintf(stderr, "[PS5] rumble %u/%u -> 0x%x\n", v.large_motor, v.small_motor, (unsigned)r);
        }
        return r == 0;
    }
}

/* Give every signed-in user's DualSense a gamepad. */
static void scan_pads(void)
{
    SceUserServiceLoginUserIdList users;
    int i, u;

    int32_t fg = -1;

    for (i = 0; i < 4; i++)
        users.user_id[i] = -1;
    if (sceUserServiceGetLoginUserIdList(&users) < 0)
        users.user_id[0] = -1;
    /* The user who started the game is player 1, whatever order the other
     * signed-in users come in. */
    if (sceUserServiceGetForegroundUser(&fg) == 0 && fg != -1 && fg != SYSTEM_USER) {
        int32_t sorted[4] = { fg, -1, -1, -1 };
        int n = 1;
        for (i = 0; i < 4 && n < 4; i++)
            if (users.user_id[i] != fg)
                sorted[n++] = users.user_id[i];
        for (i = 0; i < 4; i++)
            users.user_id[i] = sorted[i];
    }
    for (u = 0; u < 4; u++) {
        int32_t user = users.user_id[u], handle;
        Ps5Pad *slot = NULL;
        SDL_VirtualJoystickDesc desc;

        if (user == -1 || user == SYSTEM_USER)
            continue;
        for (i = 0; i < MAX_PADS; i++)
            if (s_pads[i].handle >= 0 && s_pads[i].user == user)
                break;
        if (i < MAX_PADS)
            continue;
        for (i = 0; i < MAX_PADS && !slot; i++)
            if (s_pads[i].handle < 0)
                slot = &s_pads[i];
        if (!slot)
            break;
        handle = scePadOpen(user, 0, 0, NULL);
        if (handle < 0)
            handle = scePadGetHandle(user, 0, 0);
        if (handle < 0)
            continue;
        slot->user = user;
        slot->handle = handle;
        {
            /* A DualSense opens in haptics mode, where scePadSetVibration's
             * two motor levels do nothing (the call still succeeds): mode 2
             * is the compatible two-motor rumble. */
            int vr = scePadSetVibrationMode(handle, 2);
            fprintf(stderr, "[PS5] DualSense rumble mode: %s (0x%x)\n", vr == 0 ? "two motors" : "refused",
                    (unsigned)vr);
        }

        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.vendor_id = 0x054C;    /* Sony */
        desc.product_id = 0x0CE6;   /* DualSense */
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name = "DualSense";
        desc.userdata = slot;
        desc.Update = pad_update;
        desc.Rumble = pad_rumble;
        slot->id = SDL_AttachVirtualJoystick(&desc);
        slot->joy = slot->id ? SDL_OpenJoystick(slot->id) : NULL;
        fprintf(stderr, "[PS5] DualSense for user 0x%x (pad handle %d)\n", (unsigned)user, handle);
    }
    s_last_scan = SDL_GetTicks();
}

static void *pad_scan_thread(void *unused)
{
    (void)unused;
    for (;;) {
        SDL_Delay(1000);
        scan_pads();
    }
    return NULL;
}

static void pads_init(void)
{
    int i;
    pthread_t th;

    for (i = 0; i < MAX_PADS; i++)
        s_pads[i].handle = -1;
    sceUserServiceInitialize(NULL);
    if (scePadInit() < 0) {
        fprintf(stderr, "[PS5] scePadInit failed\n");
        return;
    }
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "[PS5] SDL gamepads unavailable: %s\n", SDL_GetError());
        return;
    }
    scan_pads();
    /* A second player signing in later gets a pad too. */
    pthread_create(&th, NULL, pad_scan_thread, NULL);
    pthread_detach(th);
}

/* ── The TV ────────────────────────────────────────────────────────────── */

static uint32_t s_width = 1920, s_height = 1080;
static unsigned s_refresh = 60;
static int s_surface_made;

static int create_surface(void *instance_p, void *surface_p, void *user)
{
    VkInstance instance = (VkInstance)instance_p;
    VkSurfaceKHR *out = surface_p;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    uint32_t n = 1, displays_n = 0, planes_n = 0, i, m;
    VkDisplayPropertiesKHR displays[4];
    VkDisplayPlanePropertiesKHR planes[8];
    VkDisplayKHR display = VK_NULL_HANDLE;
    VkDisplayModeKHR mode = VK_NULL_HANDLE;
    VkExtent2D extent = { 1920, 1080 };
    uint32_t plane = 0;
    VkDisplaySurfaceCreateInfoKHR ci;
    int want_4k = pc_display("output_4k", 1);
    (void)user;

    if (vkEnumeratePhysicalDevices(instance, &n, &phys) < 0 || !phys) {
        fprintf(stderr, "[PS5] no GPU\n");
        return 0;
    }
    displays_n = 4;
    if (vkGetPhysicalDeviceDisplayPropertiesKHR(phys, &displays_n, displays) < 0 || !displays_n) {
        fprintf(stderr, "[PS5] no display\n");
        return 0;
    }
    planes_n = 8;
    if (vkGetPhysicalDeviceDisplayPlanePropertiesKHR(phys, &planes_n, planes) < 0 || !planes_n) {
        fprintf(stderr, "[PS5] no display plane\n");
        return 0;
    }
    for (i = 0; i < displays_n && !mode; i++) {
        VkDisplayModePropertiesKHR modes[32];
        uint32_t modes_n = 32;
        int best = -1;
        if (vkGetDisplayModePropertiesKHR(phys, displays[i].display, &modes_n, modes) < 0 || !modes_n)
            continue;
        {
            char list[1024];
            size_t at = 0;
            list[0] = 0;
            for (m = 0; m < modes_n && at < sizeof list - 32; m++)
                at += (size_t)snprintf(list + at, sizeof list - at, "%s%ux%u@%u", m ? " " : "",
                                       modes[m].parameters.visibleRegion.width,
                                       modes[m].parameters.visibleRegion.height,
                                       (modes[m].parameters.refreshRate + 500) / 1000);
            fprintf(stderr, "[PS5] display modes: %s\n", list);
        }
        /* 120 Hz whenever the TV takes it. The video output is switched to 120 as it opens
         * (param.json asks for it), whichever mode is picked here, so the 120 mode is the
         * truth; frame generation then fills every other refresh, or each frame is shown
         * twice (d3d8_vk_set_frame_generation), and the switch works while playing.
         * Otherwise 60: 4K when the setting asks and the TV offers it, else 1080p. */
        {
            static const uint32_t sizes[3][2] = { { 3840, 2160 }, { 2560, 1440 }, { 1920, 1080 } };
            uint32_t k;
            for (k = want_4k ? 0 : 1; k < 3 && best < 0; k++)
                for (m = 0; m < modes_n; m++) {
                    VkExtent2D e = modes[m].parameters.visibleRegion;
                    if (e.width == sizes[k][0] && e.height == sizes[k][1] &&
                        modes[m].parameters.refreshRate >= 119000) { best = (int)m; break; }
                }
            for (m = 0; m < modes_n && best < 0; m++)
                if (modes[m].parameters.refreshRate >= 119000) { best = (int)m; break; }
        }
        for (m = 0; m < modes_n && best < 0; m++) {
            VkExtent2D e = modes[m].parameters.visibleRegion;
            uint32_t hz = (modes[m].parameters.refreshRate + 500) / 1000;
            if (hz && (hz < 59 || hz > 61))
                continue;
            if (want_4k && e.width == 3840 && e.height == 2160) { best = (int)m; break; }
        }
        for (m = 0; m < modes_n && best < 0; m++) {
            VkExtent2D e = modes[m].parameters.visibleRegion;
            uint32_t hz = (modes[m].parameters.refreshRate + 500) / 1000;
            if (e.width == 1920 && e.height == 1080 && (!hz || (hz >= 59 && hz <= 61))) best = (int)m;
        }
        if (best < 0)
            best = 0;
        display = displays[i].display;
        mode = modes[best].displayMode;
        extent = modes[best].parameters.visibleRegion;
        if (modes[best].parameters.refreshRate)
            s_refresh = (modes[best].parameters.refreshRate + 500) / 1000;
    }
    if (!mode) {
        fprintf(stderr, "[PS5] no display mode\n");
        return 0;
    }
    for (i = 0; i < planes_n; i++)
        if (planes[i].currentDisplay == VK_NULL_HANDLE || planes[i].currentDisplay == display) {
            plane = i;
            break;
        }
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR;
    ci.displayMode = mode;
    ci.planeIndex = plane;
    ci.planeStackIndex = planes[plane].currentStackIndex;
    ci.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    ci.globalAlpha = 1.0f;
    ci.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
    ci.imageExtent = extent;
    if (vkCreateDisplayPlaneSurfaceKHR(instance, &ci, NULL, out) != VK_SUCCESS) {
        fprintf(stderr, "[PS5] vkCreateDisplayPlaneSurfaceKHR failed\n");
        return 0;
    }
    s_width = extent.width;
    s_height = extent.height;
    s_surface_made = 1;
    fprintf(stderr, "[PS5] TV output %ux%u at %u Hz\n", s_width, s_height, s_refresh);
    {
        /* The generator is there on a 120 Hz TV; the menu switches it (pc_settings_apply_display). */
        extern void ps5_framegen_register(void);
        if (s_refresh >= 100) {
            ps5_framegen_register();
            fprintf(stderr, "[PS5] frame generation (beta): %s, switchable while playing\n",
                    pc_display("frame_gen", 0) ? "on" : "off");
        } else {
            fprintf(stderr, "[PS5] frame generation (beta): the TV offers no 120 Hz mode, so it is not available\n");
        }
    }
    (fflush)(stderr);
    sceSystemServiceHideSplashScreen();
    return 1;
}

static void drawable_size(int *w, int *h, void *user)
{
    (void)user;
    *w = (int)s_width;
    *h = (int)s_height;
}

static unsigned refresh_hz(void *user)
{
    (void)user;
    return s_refresh;
}

/* For the menu: the TV runs at 120, so frame generation can be switched on. Before the
 * display is set up it does not know yet and says yes. */
int ps5_frame_gen_available(void) { return !s_surface_made || s_refresh >= 100; }

/* ── What the rest of the program asks the host ────────────────────────── */

void host_posix_call_on_main(void (*fn)(void *), void *arg) { fn(arg); }
int  host_posix_have_video(void) { return 0; }   /* no desktop launcher here */
int  host_posix_shift_held(void) { return 0; }
void host_posix_overlay_changed(int open) { (void)open; }
int  host_posix_vk_from_scancode(int scancode) { (void)scancode; return 0; }

int host_posix_display_size(int *w, int *h)
{
    *w = (int)s_width;
    *h = (int)s_height;
    return 1;
}

int host_posix_open_window(int width, int height, const char *title)
{
    static const char *const extensions[] = { "VK_KHR_surface", "VK_KHR_display" };
    static D3D8VkHost host;
    (void)width; (void)height; (void)title;

    host.instance_extensions = extensions;
    host.instance_extension_count = 2;
    host.create_surface = create_surface;
    host.drawable_size = drawable_size;
    host.refresh_hz = refresh_hz;
    d3d8_vk_set_host(&host);
    return 1;
}

/* A message on the PS5's screen: the only thing a player sees when the app
 * stops before it shows a picture. */
int sceKernelSendNotificationRequest(int device, void *req, size_t size, int blocking);
void ps5_notify(const char *text)
{
    struct { char unused[45]; char message[3075]; } req;
    memset(&req, 0, sizeof req);
    strncpy(req.message, text, sizeof req.message - 1);
    sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

/* No message when the app starts: only when something stops it (ps5_fatal,
 * the crash handlers, main.c's fatal path). */

static void ps5_fatal(const char *message)
{
    fprintf(stderr, "[FATAL] %s\n", message);
    (fflush)(stderr);
    ps5_notify(message);
    sleep(3);
    sceSystemServiceLoadExec("exit", NULL);
    _exit(1);
}

/* ── Paths ─────────────────────────────────────────────────────────────── */

/* The app's folder, found by looking for its eboot.bin: /app0 is how an app
 * sees its own folder, and /data/homebrew/PPSA92046 is where it is installed. */
static char s_root[256], s_game_dir[300], s_game_xbe[320];

const char *ps5_root(void)
{
    static const char *const candidates[] = { DEFJAM_PS5_ROOT, DEFJAM_PS5_ROOT_ALT };
    struct stat st;
    char probe[300];
    size_t i;

    if (s_root[0])
        return s_root;
    snprintf(s_root, sizeof s_root, "%s", DEFJAM_PS5_ROOT_ALT);
    for (i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        snprintf(probe, sizeof probe, "%s/eboot.bin", candidates[i]);
        if (stat(probe, &st) == 0) {
            snprintf(s_root, sizeof s_root, "%s", candidates[i]);
            break;
        }
    }
    snprintf(s_game_dir, sizeof s_game_dir, "%s/GameData", s_root);
    snprintf(s_game_xbe, sizeof s_game_xbe, "%s/default.xbe", s_game_dir);
    return s_root;
}

const char *ps5_game_dir(void) { ps5_root(); return s_game_dir; }
const char *ps5_game_xbe(void) { ps5_root(); return s_game_xbe; }

/* ── Early crash report ──────────────────────────────────────────────────
 * Until the game installs its own fault handler (main.c), a crash would end
 * the app with nothing in the log. This one says where: the faulting
 * instruction as an offset into the program, which the build's symbols name. */
#include <signal.h>

int host_posix_run(int (*game_main)(void));

/* Its own descriptor on the log, so a report survives anything that closed
 * stderr's stream. */
static int s_crash_fd = -1;

void ps5_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ps5_logf(const char *fmt, ...)
{
    char buf[768];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 1)
        n = (int)sizeof buf - 1;
    if (s_crash_fd >= 0 && n > 0)
        write(s_crash_fd, buf, (size_t)n);
}

/* The runtime's crash handler, before it reports through stdio. snprintf into
 * a stack buffer and one write: no lock that the crashed thread may hold. */
void ps5_crash_note(const char *what, unsigned long long pc, unsigned long long addr)
{
    char msg[200];
    int n = snprintf(msg, sizeof msg, "[PS5] crash: %s at pc 0x%llx (image at 0x%llx), address 0x%llx;"
                     " the report is in defjam.log.err\n", what, pc,
                     (unsigned long long)((uintptr_t)&host_posix_run & ~0xFFFFFull), addr);
    if (s_crash_fd >= 0 && n > 0)
        write(s_crash_fd, msg, (size_t)n);
}

static void early_fault(int sig, siginfo_t *si, void *uctx)
{
    uintptr_t pc = mmio_trap_pc(uctx);
    uintptr_t self = (uintptr_t)&host_posix_run;
    char msg[300];
    int n = snprintf(msg, sizeof msg, "[PS5] early crash: signal %d at pc 0x%llx (host_posix_run is at 0x%llx), "
                     "address 0x%llx, thread %p\n", sig, (unsigned long long)pc,
                     (unsigned long long)self, (unsigned long long)(uintptr_t)si->si_addr,
                     (void *)pthread_self());
    if (s_crash_fd >= 0 && n > 0)
        write(s_crash_fd, msg, (size_t)n);
    snprintf(msg, sizeof msg, "Def Jam crashed early (signal %d). Send the logs folder.", sig);
    ps5_notify(msg);
    sleep(2);
    _exit(1);
}

static void early_fault_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = early_fault;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

/* ── Start-up ──────────────────────────────────────────────────────────── */

/* The console writes files in the app folder slowly, and every few seconds
 * one write takes a third of a second. A thread that flushed a log then
 * stalled, and when it was the one running the GPU's work, the game stuttered.
 * stdout and stderr write into pipes instead; one thread per log moves the
 * text to its file, so only that thread ever waits for the disk. */
struct log_pump { int in, out; char chunk[65536]; };

static void *log_pump_thread(void *arg)
{
    struct log_pump *lp = arg;
    for (;;) {
        ssize_t n = read(lp->in, lp->chunk, sizeof lp->chunk);
        if (n > 0) {
            ssize_t off = 0;
            while (off < n) {
                ssize_t w = write(lp->out, lp->chunk + off, (size_t)(n - off));
                if (w <= 0)
                    break;
                off += w;
            }
        } else if (n == 0) {
            break;
        } else {
            usleep(1000);
        }
    }
    return NULL;
}

static int s_log_pipes;     /* how many of the two logs go through a pipe */

static int log_to_file(const char *path, FILE *f)
{
    static struct log_pump pumps[2];
    int fd, out, p[2];
    pthread_t th;

    if (!freopen(path, "w", f))
        return -1;
    fd = fileno(f);
    if (fd < 0 || s_log_pipes >= 2)
        return 0;
    out = open(path, O_WRONLY | O_APPEND);
    if (out < 0)
        return 0;
    if (pipe(p) != 0) {
        close(out);
        return 0;           /* writes go to the file directly */
    }
    pumps[s_log_pipes].in = p[0];
    pumps[s_log_pipes].out = out;
    if (pthread_create(&th, NULL, log_pump_thread, &pumps[s_log_pipes]) != 0) {
        close(p[0]); close(p[1]); close(out);
        return 0;
    }
    pthread_detach(th);
    if (dup2(p[1], fd) < 0) {
        close(p[1]);
        return 0;
    }
    close(p[1]);
    s_log_pipes++;
    return 0;
}

/* Files the app makes belong to it, and a PS5 FTP server running as another
 * user could not delete them (the save folder): open them up to everyone. */
#include <dirent.h>
static void open_up_tree(const char *dir, int depth)
{
    DIR *d;
    struct dirent *e;
    char path[768];

    {
        struct stat st;
        if (stat(dir, &st) == 0 && (st.st_mode & 0777) != 0777)
            chmod(dir, 0777);
    }
    if (depth > 6 || !(d = opendir(dir)))
        return;
    while ((e = readdir(d)) != NULL) {
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (lstat(path, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
            open_up_tree(path, depth + 1);
        else if ((st.st_mode & 0666) != 0666)   /* only what still needs it */
            chmod(path, 0666);
    }
    closedir(d);
}

/* ── Saves and settings ──────────────────────────────────────────────────
 * Saves and settings.ini live outside the app, so deleting or replacing the
 * app folder never takes them: the first of these folders the app can write
 * files in (DEFJAM_SAVE overrides the saves' place). The first start of a build
 * that does this copies the old ones across and leaves the originals. When
 * none can be written, both stay in the app folder as before. */
static const char *const k_homes[] = { "/data/DefJamFFNY", "/data/homebrew/DefJamFFNY" };
static const char *s_home;
static int s_home_checked;

static void say_fail(const char *what, const char *path)
{
    static int said;
    if (said++ < 8)
        fprintf(stderr, "[PS5] %s %s: %s\n", what, path, strerror(errno));
}

static int copy_file(const char *from, const char *to)
{
    char buf[65536];
    ssize_t n = 0;
    int in = open(from, O_RDONLY), out, bad = 0;
    if (in < 0) { say_fail("cannot read", from); return -1; }
    out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0) { say_fail("cannot create", to); close(in); return -1; }
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t)n) != n) { say_fail("cannot write", to); bad = 1; break; }
    if (n < 0) { say_fail("cannot read", from); bad = 1; }
    close(in);
    if (close(out) != 0) bad = 1;
    return bad ? -1 : 0;
}

static int copy_tree(const char *from, const char *to, int depth)
{
    DIR *d;
    struct dirent *e;
    char a[768], b[768];
    int bad = 0;
    if (depth > 8 || !(d = opendir(from))) { say_fail("cannot open", from); return -1; }
    if (mkdir(to, 0777) != 0 && errno != EEXIST) { say_fail("cannot make", to); closedir(d); return -1; }
    while ((e = readdir(d)) != NULL) {
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(a, sizeof a, "%s/%s", from, e->d_name);
        snprintf(b, sizeof b, "%s/%s", to, e->d_name);
        if (lstat(a, &st) != 0) { say_fail("cannot stat", a); bad = 1; continue; }
        if (S_ISDIR(st.st_mode)) { if (copy_tree(a, b, depth + 1)) bad = 1; }
        else if (S_ISREG(st.st_mode) && copy_file(a, b)) bad = 1;
    }
    closedir(d);
    return bad ? -1 : 0;
}

/* Whether files can be made in `dir` (made if missing): a small file is written and removed. */
static int writable_dir(const char *dir)
{
    char probe[600];
    int fd;
    if (mkdir(dir, 0777) != 0 && errno != EEXIST) { say_fail("cannot make", dir); return 0; }
    snprintf(probe, sizeof probe, "%s/.probe", dir);
    fd = open(probe, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) { say_fail("cannot create", probe); return 0; }
    if (write(fd, "ok", 2) != 2) { say_fail("cannot write", probe); close(fd); unlink(probe); return 0; }
    close(fd);
    unlink(probe);
    return 1;
}

/* The folder outside the app for saves and settings, or NULL for the app folder. */
const char *ps5_data_home(void)
{
    size_t i;
    if (s_home_checked)
        return s_home;
    s_home_checked = 1;
    for (i = 0; i < sizeof k_homes / sizeof k_homes[0] && !s_home; i++)
        if (writable_dir(k_homes[i]))
            s_home = k_homes[i];
    if (s_home)
        fprintf(stderr, "[PS5] saves and settings live in %s\n", s_home);
    else
        fprintf(stderr, "[PS5] saves and settings stay in the app folder (no folder outside it can be written)\n");
    return s_home;
}

/* settings.ini's place: in the home when there is one (copied from the app folder the first time). */
const char *ps5_settings_path(char *out, size_t size)
{
    const char *home = ps5_data_home();
    char old[512];
    struct stat st;
    snprintf(old, sizeof old, "%s/settings.ini", ps5_root());
    if (!home) {
        snprintf(out, size, "%s", old);
        return out;
    }
    snprintf(out, size, "%s/settings.ini", home);
    if (stat(out, &st) != 0 && stat(old, &st) == 0 && copy_file(old, out) == 0)
        fprintf(stderr, "[PS5] settings.ini copied to %s\n", home);
    return out;
}

const char *ps5_save_dir(void)
{
    static char dir[512];
    char old[512], part[560];
    struct stat st;
    const char *env = getenv("DEFJAM_SAVE"), *home;
    if (dir[0])
        return dir;
    if (env && *env) {
        snprintf(dir, sizeof dir, "%s", env);
        return dir;
    }
    snprintf(old, sizeof old, "%s/save", ps5_root());
    home = ps5_data_home();
    if (!home) {
        snprintf(dir, sizeof dir, "%s", old);
        return dir;
    }
    snprintf(dir, sizeof dir, "%s/save", home);
    if (stat(dir, &st) == 0 || stat(old, &st) != 0 || !S_ISDIR(st.st_mode))
        return dir;                              /* already moved, or nothing to move */
    snprintf(part, sizeof part, "%s.copying", dir);
    if (copy_tree(old, part, 0) == 0 && rename(part, dir) == 0) {
        fprintf(stderr, "[PS5] saves copied to %s (the old ones stay in %s)\n", dir, old);
    } else {
        /* Keep playing on the old folder rather than start from nothing. */
        say_fail("could not copy the saves to", dir);
        snprintf(dir, sizeof dir, "%s", old);
    }
    return dir;
}

static void open_up_app_files(void)
{
    const char *subs[] = { "save", "logs", "radv-shader-cache", "texture-cache" };
    char path[512];
    unsigned i;
    for (i = 0; i < sizeof subs / sizeof subs[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", ps5_root(), subs[i]);
        open_up_tree(path, 0);
    }
    if (s_home)
        open_up_tree(s_home, 0);
}

/* How long the basic things the runtime leans on take here, once at start. */
static __thread volatile int t_bench;

static double bench_us(struct timespec a, struct timespec b, int n)
{
    return ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / 1000.0 / n;
}

static void ps5_time_basics(void)
{
    struct timespec a, b, c;
    pthread_key_t key;
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    double w_err, flush, gettime, tsc, yield, spec, tls, mtx, slp;
    uint64_t tsc_hz;
    int i;

    clock_gettime(CLOCK_MONOTONIC, &a);
    for (i = 0; i < 20; i++)
        fprintf(stderr, "[PS5] timing line %d\n", i);
    clock_gettime(CLOCK_MONOTONIC, &b);
    w_err = bench_us(a, b, 20);
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (i = 0; i < 5; i++) {
        fprintf(stderr, "[PS5] timing flush %d\n", i);
        (fflush)(stderr);
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    flush = bench_us(a, b, 5);

    clock_gettime(CLOCK_MONOTONIC, &a);
    for (i = 0; i < 1000; i++)
        clock_gettime(CLOCK_MONOTONIC, &c);
    clock_gettime(CLOCK_MONOTONIC, &b);
    gettime = bench_us(a, b, 1000);

    {
        extern uint64_t sceKernelReadTsc(void);
        volatile uint64_t sink = 0;
        clock_gettime(CLOCK_MONOTONIC, &a);
        for (i = 0; i < 1000; i++)
            sink += sceKernelReadTsc();
        clock_gettime(CLOCK_MONOTONIC, &b);
        tsc = bench_us(a, b, 1000);
        (void)sink;
    }
    {
        extern void ps5_tsc_info(uint64_t *reported, uint64_t *measured, uint64_t *used);
        uint64_t reported, measured, used;
        ps5_tsc_info(&reported, &measured, &used);
        tsc_hz = used;
        fprintf(stderr, "[PS5] time stamp counter: libkernel says %llu Hz, measured %llu Hz;"
                        " clocks use %s\n", (unsigned long long)reported,
                (unsigned long long)measured, used > 1 ? "the TSC" : "clock_gettime");
    }

    clock_gettime(CLOCK_MONOTONIC, &a);
    for (i = 0; i < 100; i++)
        sched_yield();
    clock_gettime(CLOCK_MONOTONIC, &b);
    yield = bench_us(a, b, 100);

    pthread_key_create(&key, NULL);
    pthread_setspecific(key, &a);
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (i = 0; i < 1000; i++)
        (void)*(volatile void **)&(void *){pthread_getspecific(key)};
    clock_gettime(CLOCK_MONOTONIC, &b);
    spec = bench_us(a, b, 1000);

    clock_gettime(CLOCK_MONOTONIC, &a);
    for (i = 0; i < 100000; i++)
        t_bench++;
    clock_gettime(CLOCK_MONOTONIC, &b);
    tls = bench_us(a, b, 100000);

    clock_gettime(CLOCK_MONOTONIC, &a);
    for (i = 0; i < 1000; i++) {
        pthread_mutex_lock(&m);
        pthread_mutex_unlock(&m);
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    mtx = bench_us(a, b, 1000);

    clock_gettime(CLOCK_MONOTONIC, &a);
    for (i = 0; i < 10; i++)
        usleep(1);
    clock_gettime(CLOCK_MONOTONIC, &b);
    slp = bench_us(a, b, 10);

    fprintf(stderr, "[PS5] timing (us each): log line %.1f, flushed line %.1f, clock %.3f,"
            " TSC %.3f (%llu Hz), yield %.1f, getspecific %.3f, thread-local %.4f, mutex %.3f,"
            " usleep(1) %.1f\n",
            w_err, flush, gettime, tsc, (unsigned long long)tsc_hz, yield, spec, tls, mtx, slp);
}

static int (*s_game_main)(void);
static int s_game_result;

/* A line in the log now and then, so a black screen still says whether the
 * game is moving (the guest's indirect-call count) or stuck, and where each
 * guest thread is: a few samples of its host pc, taken with a signal.
 * test-N-llvm-pie.elf names them (the image starts at 0x400000). */
extern volatile uint64_t g_icall_count;
uint64_t mmio_trap_fault_count(void);
void mmio_trap_last_faults(uintptr_t addr[8], uintptr_t pc[8]);
uint64_t mmio_trap_neighbour_count(void);

static volatile int s_guest_thread_count;

void ps5_prof_register(const char *name);
void ps5_prof_start(void);
void ps5_prof_report(unsigned secs);
void ps5_gamewatch_start(void);

void ps5_guest_thread_register(void)
{
    char name[32];
    int n = __atomic_fetch_add(&s_guest_thread_count, 1, __ATOMIC_SEQ_CST);
    snprintf(name, sizeof name, "guest %d", n);
    ps5_prof_register(name);
}

static void report_traps(void)
{
    uintptr_t a[8], p[8];
    mmio_trap_last_faults(a, p);
    ps5_logf("[PS5]   traps %llu (neighbour pages %llu)\n",
             (unsigned long long)mmio_trap_fault_count(),
             (unsigned long long)mmio_trap_neighbour_count());
    (void)a; (void)p;
}

extern long long xbox_kernel_call_count(void);
extern volatile unsigned g_vk_frames_presented;

/* Pushes the buffered logs out once a second. On its own thread, so a stream
 * stuck behind a lock cannot also silence the heartbeat. */
/* The port's own helper threads (heartbeat, log flush, profiler, game watch)
 * start before any guest thread pins itself, so they inherit every CPU,
 * including the one the title runs on. Once the guest CPU is chosen, each of
 * them moves off it the next time it calls this. */
void ps5_off_guest_cpu(void)
{
    extern volatile uint64_t g_ps5_host_cpu_mask;
    extern void *scePthreadSelf(void);
    extern int scePthreadSetaffinity(void *thread, uint64_t mask);
    static __thread int moved;
    uint64_t mask = g_ps5_host_cpu_mask;
    if (!moved && mask) {
        moved = 1;
        scePthreadSetaffinity(scePthreadSelf(), mask);
    }
}

static void *flush_thread(void *unused)
{
    (void)unused;
    for (;;) {
        struct timespec a, b;
        long ms;
        static unsigned slow;
        sleep(1);
        ps5_off_guest_cpu();
        clock_gettime(CLOCK_MONOTONIC, &a);
        (fflush)(stdout);
        (fflush)(stderr);
        clock_gettime(CLOCK_MONOTONIC, &b);
        /* Every log line waits while this holds the stream: say when it is
         * slow, so a hitch can be matched to it. */
        ms = (long)((b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000);
        if (ms >= 20 && slow++ < 200)
            ps5_logf("[PS5] log flush took %ld ms\n", ms);
    }
    return NULL;
}

static void *heartbeat_thread(void *unused)
{
    unsigned secs = 0;
    int profiling;
    pthread_t fl;
    (void)unused;
    /* The sampling profiler signals every thread about a thousand times a
     * second, the audio output thread among them, whose blocking output call
     * a signal can cut short: only when asked for (RECOMP_PS5_PROFILE=1). */
    {
        const char *e = getenv("RECOMP_PS5_PROFILE");
        profiling = e && *e == '1';
    }
    if (profiling)
        ps5_prof_start();
    {
        /* The input watch did its job (the help popups and pause menu work
         * since the trigger fix): only when asked for (RECOMP_PS5_WATCH=1). */
        const char *e = getenv("RECOMP_PS5_WATCH");
        if (e && *e == '1')
            ps5_gamewatch_start();
    }
    if (pthread_create(&fl, NULL, flush_thread, NULL) == 0)
        pthread_detach(fl);
    for (;;) {
        unsigned step = secs < 300 ? 10 : 60;
        sleep(step);
        ps5_off_guest_cpu();
        secs += step;
        if (secs == 10 || (secs <= 300 ? secs % 60 == 0 : secs % 300 == 0))
            open_up_app_files();
        if (secs == 10)
            ps5_logf("[PS5] logs through %d pipe(s)\n", s_log_pipes);
        ps5_logf("[PS5] %u s: kernel calls %lld, guest calls %llu, frames %u, %d guest thread(s)\n",
                 secs, xbox_kernel_call_count(), (unsigned long long)g_icall_count,
                 g_vk_frames_presented, (int)s_guest_thread_count);
        if (profiling)
            ps5_prof_report(secs);
        if (secs % 60 == 0)
            report_traps();
    }
    return NULL;
}

/* One CPU for the title's threads, the rest for everything else, decided
 * before any thread starts. The guest CPU used to be chosen when the first
 * title thread pinned itself, after the GPU executor, the audio threads, the
 * tick thread and the USB thread had started with every CPU in their mask:
 * they could land on the title's CPU and the title waited behind them. This
 * thread moves to the host CPUs, so what it starts inherits them; threads made
 * through CreateThread are placed by the runtime, title threads pin
 * themselves. */
static void split_cpus(void)
{
    extern volatile uint64_t g_ps5_host_cpu_mask;
    extern volatile int g_ps5_guest_cpu;
    extern void *scePthreadSelf(void);
    extern int scePthreadGetaffinity(void *thread, uint64_t *mask);
    extern int scePthreadSetaffinity(void *thread, uint64_t mask);
    const char *s = getenv("RECOMP_GUEST_CORES");
    uint64_t mask = 0;
    int cpu = -1, i, e;

    if (s && !strcmp(s, "all"))
        return;
    e = scePthreadGetaffinity(scePthreadSelf(), &mask);
    if (e != 0 || !mask)
        return;
    if (mask & (1ull << 2))
        cpu = 2;
    else
        for (i = 0; i < 64 && cpu < 0; i++)
            if (mask & (1ull << i))
                cpu = i;
    if (cpu < 0 || !(mask & ~(1ull << cpu)))
        return;
    g_ps5_host_cpu_mask = mask & ~(1ull << cpu);
    g_ps5_guest_cpu = cpu;
    e = scePthreadSetaffinity(scePthreadSelf(), g_ps5_host_cpu_mask);
    fprintf(stderr, "[PS5] CPUs 0x%llX: the title gets CPU %d, the rest 0x%llX (%s)\n",
            (unsigned long long)mask, cpu, (unsigned long long)g_ps5_host_cpu_mask,
            e ? "could not move the main thread" : "ok");
}

static void *game_thread(void *unused)
{
    (void)unused;
    ps5_prof_register("game main");
    fprintf(stderr, "[PS5] game thread started\n");
    s_game_result = s_game_main();
    return NULL;
}

/* Every way the game ends on purpose (the title asking to leave, the
 * runtime's ExitProcess) comes here: logs written, files opened up, back to
 * the home screen. */
void ps5_exit_app(int code) __attribute__((noreturn));
void ps5_exit_app(int code)
{
    fprintf(stderr, "[PS5] the game is leaving (code %d)\n", code);
    (fflush)(stdout);
    (fflush)(stderr);
    open_up_app_files();
    sceSystemServiceLoadExec("exit", NULL);
    _exit(code);
}

/* The menu's restart (settings that only take effect at the start): the app's
 * own eboot.bin again, in place of this one. Returns only when the system
 * refuses, with its code; the player then closes the game and starts it. */
int ps5_restart_app(void)
{
    int r;
    fprintf(stderr, "[PS5] starting the game again for the new settings\n");
    (fflush)(stdout);
    (fflush)(stderr);
    open_up_app_files();
    r = sceSystemServiceLoadExec("/app0/eboot.bin", NULL);
    fprintf(stderr, "[PS5] the system did not start the game again (0x%08x)\n", (unsigned)r);
    return r ? r : -1;
}

/* The last two runs' logs are kept as *.prev and *.prev2, so a crash
 * followed by a relaunch or two still has its log. */
static void keep_previous_logs(const char *root)
{
    static const char *const names[] = { "defjam.log", "defjam.log.err", "ps5.log" };
    char cur[512], prev[520], prev2[524];
    size_t i;
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        snprintf(cur, sizeof cur, "%s/logs/%s", root, names[i]);
        snprintf(prev, sizeof prev, "%s.prev", cur);
        snprintf(prev2, sizeof prev2, "%s.prev2", cur);
        (void)rename(prev, prev2);
        (void)rename(cur, prev);
    }
}

int host_posix_run(int (*game_main)(void))
{
    pthread_t th;
    pthread_attr_t attr;
    char log[512];

    const char *root = ps5_root();
    struct stat st;

    /* chdir does not take in an app on every firmware (FIFA Street ignores
     * its result too): every path below is absolute instead. */
    (void)chdir(root);
    if (!getenv("DEFJAM_DATA"))
        setenv("DEFJAM_DATA", root, 1);
    snprintf(log, sizeof log, "%s/logs", root);
    mkdir(log, 0777);
    keep_previous_logs(root);
    snprintf(log, sizeof log, "%s/logs/defjam.log", root);
    if (log_to_file(log, stdout) != 0)
        ps5_notify("Def Jam can't write its log (logs folder in the app folder)");
    snprintf(log, sizeof log, "%s/logs/defjam.log.err", root);
    log_to_file(log, stderr);
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    setvbuf(stderr, NULL, _IOFBF, 1 << 16);
    fprintf(stderr, "[PS5] Def Jam: Fight for NY (PS5) starting in %s\n", root);
    (fflush)(stderr);
    /* The heartbeat and early crash reports go to their own file, written
     * straight to it: they still arrive if a thread is stuck holding the
     * stderr stream. */
    snprintf(log, sizeof log, "%s/logs/ps5.log", root);
    s_crash_fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    early_fault_handler();
    fprintf(stderr, "[PS5] host_posix_run is at %p\n", (void *)&host_posix_run);
    {   /* The decoder for trapped device registers reads instructions from
         * this copy: code itself is execute-only here (mmio_trap.h). */
        char code[300];
        snprintf(code, sizeof code, "%s/code.bin", root);
        if (mmio_trap_load_code(code) != 0)
            ps5_fatal("Def Jam: code.bin is missing from the app folder. Copy the whole PPSA92046 folder again.");
        fprintf(stderr, "[PS5] code copy loaded from %s\n", code);
    }
    if (stat(ps5_game_xbe(), &st) != 0) {
        char msg[400];
        snprintf(msg, sizeof msg, "Def Jam: game files not found. Copy your extracted USA Xbox disc "
                 "into %s (default.xbe goes in there).", ps5_game_dir());
        ps5_fatal(msg);
    }

    split_cpus();

    /* No signal handlers from SDL: the runtime's own fault handler traps the
     * console's device registers and must see every SIGSEGV/SIGBUS first. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    pads_init();

    ps5_time_basics();

    /* One [PACING] line every two seconds while frames are being shown. */
    setenv("RECOMP_PRESENT_PACING", "1", 0);
    /* No keyboard here: without this the hub always carried an idle second
     * controller for one, and a second player's pad came in as player 3. */
    setenv("RECOMP_KEYBOARD_PLAYER", "off", 0);
    /* The title's 60 Hz frame clock is the GPU timer's alarm: a [PTIMER] line
     * every five seconds with how many alarms fired, and one if an alarm
     * fires and is never re-armed (the clock, and with it the game's input
     * thread, stopping). */
    setenv("RECOMP_PTIMER_TRACE", "1", 0);
    {
        pthread_t hb;
        if (pthread_create(&hb, NULL, heartbeat_thread, NULL) == 0)
            pthread_detach(hb);
    }

    s_game_main = game_main;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    if (pthread_create(&th, &attr, game_thread, NULL) != 0) {
        ps5_fatal("could not start the game thread");
        return 1;
    }
    pthread_attr_destroy(&attr);
    pthread_join(th, NULL);
    (fflush)(stdout);
    (fflush)(stderr);
    open_up_app_files();
    sleep(1);               /* the log pumps write out what is left */
    sceSystemServiceLoadExec("exit", NULL);
    return s_game_result;
}

/* No desktop launcher on the console (host_posix_have_video says so, so this
 * is not reached); 1 means "start the game". */
int pc_launcher_run(void) { return 1; }

/* The PS5's libc has no explicit_bzero; SecureZeroMemory uses it. */
void explicit_bzero(void *p, size_t n)
{
    volatile unsigned char *b = p;
    while (n--)
        *b++ = 0;
}

/* Debug-register watchpoints: Windows only. */
int watchpoint_init(void) { return 0; }
int watchpoint_handle_trap(void *ctx) { (void)ctx; return 0; }
