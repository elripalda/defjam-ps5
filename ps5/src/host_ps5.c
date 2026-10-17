/*
 * host_ps5.c -- [ps5 port] what src/host_posix.c does on a desktop, for the
 * PS5: there is no window, so the picture goes straight to the TV through
 * VK_KHR_display (PS5_Mesa's RADV), and the DualSense is read with scePad and
 * handed to SDL as a gamepad, where the input layer (input_host.c) picks it up
 * like any other pad.
 *
 * The app lives in /data/homebrew/PPSA92004; the player's own extracted disc
 * goes in its GameData folder. Saves, settings.ini and the log are kept beside
 * it.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#include "d3d8_vk.h"
#include "mmio_trap.h"
#include "hooks/pc_settings.h"

/* The app's own folder as the app sees it (the same folder as
 * /data/homebrew/PPSA92004 when installed there): the process cannot rely on
 * reaching /data, and starts somewhere read-only. */
#ifndef DEFJAM_PS5_ROOT
#define DEFJAM_PS5_ROOT "/app0"
#endif
#define DEFJAM_PS5_ROOT_ALT "/data/homebrew/PPSA92004"

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
} Ps5Pad;

static Ps5Pad s_pads[MAX_PADS];
static Uint64 s_last_scan;

static Sint16 axis(uint8_t v)
{
    int a = ((int)v - 128) * 256;
    return (Sint16)(a > 32767 ? 32767 : a);
}

static void SDLCALL pad_update(void *userdata)
{
    Ps5Pad *p = userdata;
    PadData d;
    uint32_t b;

    if (!p->joy || p->handle < 0)
        return;
    memset(&d, 0, sizeof d);
    if (scePadReadState(p->handle, &d) != 0)
        return;
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
    SDL_SetJoystickVirtualAxis(p->joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, (Sint16)(d.l2 * 32767 / 255));
    SDL_SetJoystickVirtualAxis(p->joy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, (Sint16)(d.r2 * 32767 / 255));
}

static bool SDLCALL pad_rumble(void *userdata, Uint16 low, Uint16 high)
{
    Ps5Pad *p = userdata;
    ScePadVibrationParam v;
    if (p->handle < 0)
        return false;
    v.large_motor = (uint8_t)(low >> 8);
    v.small_motor = (uint8_t)(high >> 8);
    return scePadSetVibration(p->handle, &v) == 0;
}

/* Give every signed-in user's DualSense a gamepad. */
static void scan_pads(void)
{
    SceUserServiceLoginUserIdList users;
    int i, u;

    for (i = 0; i < 4; i++)
        users.user_id[i] = -1;
    if (sceUserServiceGetLoginUserIdList(&users) < 0)
        sceUserServiceGetForegroundUser(&users.user_id[0]);
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
    int want_4k = pc_display("output_4k", 0);
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
        /* 1080p by default (the game draws at a few times its 640x480, and
         * the TV scales); 4K when the setting asks and the TV offers it. */
        for (m = 0; m < modes_n; m++) {
            VkExtent2D e = modes[m].parameters.visibleRegion;
            if (want_4k && e.width == 3840 && e.height == 2160) { best = (int)m; break; }
            if (e.width == 1920 && e.height == 1080 && best < 0) best = (int)m;
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
    fprintf(stderr, "[PS5] TV output %ux%u at %u Hz\n", s_width, s_height, s_refresh);
    fflush(stderr);
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

/* The earliest point the app's own code runs: if this message never shows,
 * the console did not load the program at all. */
__attribute__((constructor(101))) static void ps5_loaded(void)
{
    ps5_notify("Def Jam (PS5 test build) loaded");
}

static void ps5_fatal(const char *message)
{
    fprintf(stderr, "[FATAL] %s\n", message);
    fflush(stderr);
    ps5_notify(message);
    sleep(3);
    sceSystemServiceLoadExec("exit", NULL);
    _exit(1);
}

/* ── Paths ─────────────────────────────────────────────────────────────── */

/* The app's folder, found by looking for its eboot.bin: /app0 is how an app
 * sees its own folder, and /data/homebrew/PPSA92004 is where it is installed. */
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
    snprintf(msg, sizeof msg, "Def Jam crashed early (signal %d). Send logs/defjam.log.err.", sig);
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

static int (*s_game_main)(void);
static int s_game_result;

static void *game_thread(void *unused)
{
    (void)unused;
    fprintf(stderr, "[PS5] game thread started\n");
    s_game_result = s_game_main();
    return NULL;
}

int host_posix_run(int (*game_main)(void))
{
    pthread_t th;
    pthread_attr_t attr;
    char log[512];

    const char *root = ps5_root();
    struct stat st;

    {
        char msg[300];
        snprintf(msg, sizeof msg, "Def Jam is starting (app folder: %s)", root);
        ps5_notify(msg);
    }
    /* chdir does not take in an app on every firmware (FIFA Street ignores
     * its result too): every path below is absolute instead. */
    (void)chdir(root);
    if (!getenv("DEFJAM_DATA"))
        setenv("DEFJAM_DATA", root, 1);
    snprintf(log, sizeof log, "%s/logs", root);
    mkdir(log, 0777);
    snprintf(log, sizeof log, "%s/logs/defjam.log", root);
    if (!freopen(log, "w", stdout))
        ps5_notify("Def Jam can't write its log (logs folder in the app folder)");
    snprintf(log, sizeof log, "%s/logs/defjam.log.err", root);
    freopen(log, "w", stderr);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    fprintf(stderr, "[PS5] Def Jam: Fight for NY (PS5) starting in %s\n", root);
    fflush(stderr);
    snprintf(log, sizeof log, "%s/logs/defjam.log.err", root);
    s_crash_fd = open(log, O_WRONLY | O_APPEND);
    early_fault_handler();
    fprintf(stderr, "[PS5] host_posix_run is at %p\n", (void *)&host_posix_run);
    {   /* The decoder for trapped device registers reads instructions from
         * this copy: code itself is execute-only here (mmio_trap.h). */
        char code[300];
        snprintf(code, sizeof code, "%s/code.bin", root);
        if (mmio_trap_load_code(code) != 0)
            ps5_fatal("Def Jam: code.bin is missing from the app folder. Copy the whole PPSA92004 folder again.");
        fprintf(stderr, "[PS5] code copy loaded from %s\n", code);
    }
    if (stat(ps5_game_xbe(), &st) != 0) {
        char msg[400];
        snprintf(msg, sizeof msg, "Def Jam: game files not found. Copy your extracted USA Xbox disc "
                 "into %s (default.xbe goes in there).", ps5_game_dir());
        ps5_fatal(msg);
    }

    /* No signal handlers from SDL: the runtime's own fault handler traps the
     * console's device registers and must see every SIGSEGV/SIGBUS first. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    pads_init();

    s_game_main = game_main;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    if (pthread_create(&th, &attr, game_thread, NULL) != 0) {
        ps5_fatal("could not start the game thread");
        return 1;
    }
    pthread_attr_destroy(&attr);
    pthread_join(th, NULL);
    fflush(stdout);
    fflush(stderr);
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
