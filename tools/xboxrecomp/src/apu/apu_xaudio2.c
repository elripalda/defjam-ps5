/**
 * XAudio2 Audio Output Backend
 *
 * Provides low-latency audio output via XAudio2 (Win7+).
 * Called from the APU monitor frame to submit mixed samples.
 * Falls back gracefully if XAudio2 is unavailable.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "apu_xaudio2.h"

/* The XAudio2 backend is Windows-only. On Linux all xa2_* functions are
 * stubbed to report inactive; real audio output via SDL2 comes later. */
#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

#define XA2_SAMPLE_RATE   48000
#define XA2_CHANNELS      2
#define XA2_BUF_SAMPLES   1024   /* ~21ms per submission */
#define XA2_NUM_BUFS      3

static IXAudio2               *g_xa2 = NULL;
static IXAudio2MasteringVoice *g_xa2_master = NULL;
static IXAudio2SourceVoice    *g_xa2_source = NULL;
static int16_t                 g_xa2_bufs[XA2_NUM_BUFS][XA2_BUF_SAMPLES][2];
static int                     g_xa2_next_buf = 0;
static int                     g_xa2_initialized = 0;
static int                     g_xa2_frames_written = 0;
/* Submissions refused because the queue was full, and submissions that
 * found it empty (the device had run dry: a gap in what is heard). */
int                            g_xa2_dropped, g_xa2_starved;

int xa2_init(void)
{
    HRESULT hr;
    int com_initialized;
    WAVEFORMATEX wfx = { 0 };

    if (g_xa2_initialized) return 1;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "[XA2] CoInitializeEx failed: 0x%08lX\n", hr);
        return 0;
    }
    com_initialized = SUCCEEDED(hr);

    hr = XAudio2Create(&g_xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !g_xa2) {
        fprintf(stderr, "[XA2] XAudio2Create failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master,
        XA2_CHANNELS, XA2_SAMPLE_RATE, 0, NULL, NULL, 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateMasteringVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = XA2_CHANNELS;
    wfx.nSamplesPerSec  = XA2_SAMPLE_RATE;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = XA2_CHANNELS * 2;
    wfx.nAvgBytesPerSec = XA2_SAMPLE_RATE * wfx.nBlockAlign;

    hr = IXAudio2_CreateSourceVoice(g_xa2, &g_xa2_source,
        &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateSourceVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2SourceVoice_Start(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] Start failed: 0x%08lX\n", hr);
        goto fail;
    }

    g_xa2_next_buf = 0;
    g_xa2_initialized = 1;
    g_xa2_frames_written = 0;

    fprintf(stderr, "[XA2] XAudio2 initialized (%d Hz stereo 16-bit, %d x %d-sample buffers)\n",
            XA2_SAMPLE_RATE, XA2_NUM_BUFS, XA2_BUF_SAMPLES);
    return 1;

fail:
    xa2_shutdown();
    /* Failed initialization still runs on the COM-initializing thread. */
    if (com_initialized) CoUninitialize();
    return 0;
}

void xa2_shutdown(void)
{
    if (g_xa2_source) {
        IXAudio2SourceVoice_Stop(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
        IXAudio2SourceVoice_FlushSourceBuffers(g_xa2_source);
        g_xa2_source->lpVtbl->DestroyVoice(g_xa2_source);
        g_xa2_source = NULL;
    }
    if (g_xa2_master) {
        g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
        g_xa2_master = NULL;
    }
    if (g_xa2) {
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
    }

    if (g_xa2_initialized)
        fprintf(stderr, "[XA2] Shut down (%d frames written)\n", g_xa2_frames_written);
    g_xa2_initialized = 0;
}

int xa2_is_active(void)
{
    return g_xa2_initialized;
}

/* Submit a buffer of mixed samples to XAudio2.
 * Called from APU frame thread. Returns 1 if buffer was submitted. */
int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    XAUDIO2_VOICE_STATE state;
    XAUDIO2_BUFFER xbuf;
    int idx;
    int copy_samples;
    HRESULT hr;

    if (!g_xa2_initialized || !g_xa2_source) return 0;

    IXAudio2SourceVoice_GetState(g_xa2_source, &state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if (state.BuffersQueued == 0 && g_xa2_frames_written > 0)
        g_xa2_starved++;
    if ((int)state.BuffersQueued >= XA2_NUM_BUFS) {
        g_xa2_dropped++;
        return 0;
    }

    idx = g_xa2_next_buf;
    copy_samples = (num_samples > XA2_BUF_SAMPLES) ? XA2_BUF_SAMPLES : num_samples;
    memcpy(g_xa2_bufs[idx], samples, copy_samples * XA2_CHANNELS * sizeof(int16_t));

    memset(&xbuf, 0, sizeof(xbuf));
    xbuf.AudioBytes = copy_samples * XA2_CHANNELS * sizeof(int16_t);
    xbuf.pAudioData = (const BYTE *)g_xa2_bufs[idx];

    hr = IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &xbuf, NULL);
    if (FAILED(hr)) return 0;

    g_xa2_next_buf = (idx + 1) % XA2_NUM_BUFS;
    g_xa2_frames_written++;
    return 1;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

#else /* !_WIN32 -- the same interface on an SDL audio device */

#if defined(__PROSPERO__)
/* [ps5 port] The console's own audio port; SDL has no PS5 audio driver. */
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
typedef uint8_t Uint8;
int32_t sceAudioOutInit(void);
int32_t sceAudioOutOpen(int32_t user_id, int32_t type, int32_t index, uint32_t len,
                        uint32_t freq, uint32_t param);
int32_t sceAudioOutOutput(int32_t handle, const void *ptr);
int32_t sceAudioOutClose(int32_t handle);
#elif defined(XBOXRECOMP_HAVE_SDL3)
#include <SDL3/SDL.h>
#else
#include <SDL.h>
#endif
#include <stdlib.h>
#include <string.h>

#define XA2_SAMPLE_RATE   48000
#define XA2_CHANNELS      2
#define XA2_BUF_SAMPLES   1024   /* ~21ms per submission */
#define XA2_NUM_BUFS      3
#define RING_FRAMES       8192   /* power of two, well over what is ever queued */
/* The device starts, and starts again after running out, once this much is
 * waiting: two submissions, 43 ms. Without it the ring sits near empty and
 * any lateness in the mixer is a gap. Above XA2_RING_MAX a submission is
 * refused, which bounds the delay when the mixer's clock runs fast. */
#if defined(__PROSPERO__)
/* [ps5 port] One more submission of slack each way (21 ms more delay): the
 * console has no device queue of its own between this ring and the output
 * (SDL keeps one on the desktop), and the gaps logged so far followed late
 * timers of 5 to 15 ms. */
#define XA2_PREROLL       (3 * XA2_BUF_SAMPLES)
#define XA2_RING_MAX      (5 * XA2_BUF_SAMPLES)
#else
#define XA2_PREROLL       (2 * XA2_BUF_SAMPLES)
#define XA2_RING_MAX      (4 * XA2_BUF_SAMPLES)
#endif

/* A ring the device's callback drains. SDL's own queue cannot tell "empty
 * because the device took it all into its buffer" from "empty because nothing
 * was sent", and the second is the one that is heard. */
#if defined(__PROSPERO__)
static int g_sdl_dev;                       /* the audio port's handle + 1 */
static pthread_mutex_t g_ring_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_out_thread;
static volatile int g_out_running;
#else
static SDL_AudioDeviceID g_sdl_dev;
#endif
static int16_t  g_ring[RING_FRAMES][XA2_CHANNELS];
static unsigned g_ring_r, g_ring_w;         /* frame counters, free-running */
static int g_xa2_initialized;
static int g_xa2_frames_written;
static int g_dry;                           /* the callback is in a gap */
static int g_primed;                        /* enough is queued to play from */
/* As on Windows: submissions refused because three buffers were waiting, and
 * times the device ran out (a gap in what is heard). */
int  g_xa2_dropped, g_xa2_starved;

#if defined(__PROSPERO__)
/* [ps5 port] A gap used to start and end with a step: the wave cut to zero
 * where the ring ran out and came back at full level, two clicks around the
 * hole. 64 frames (1.3 ms) of ramp at each end make them inaudible; the gap
 * itself is unchanged. */
#define XA2_FADE 64
static int16_t g_last_out[XA2_CHANNELS];   /* the last frame played */
static int     g_fade_in;                   /* frames of ramp still to apply */

static void fade_out_into(int16_t (*out)[XA2_CHANNELS], unsigned n)
{
    unsigned i, c, k = n < XA2_FADE ? n : XA2_FADE;
    for (i = 0; i < n; i++)
        for (c = 0; c < XA2_CHANNELS; c++)
            out[i][c] = i < k ? (int16_t)((int)g_last_out[c] * (int)(k - 1 - i) / (int)k) : 0;
    for (c = 0; c < XA2_CHANNELS; c++)
        g_last_out[c] = 0;
}
#endif

static void sdl_callback(void *user, Uint8 *stream, int len)
{
    int16_t (*out)[XA2_CHANNELS] = (void *)stream;
    unsigned want = (unsigned)len / (XA2_CHANNELS * sizeof(int16_t)), i;
    unsigned have = g_ring_w - g_ring_r;

    (void)user;
    if (!g_primed) {
        if (have < XA2_PREROLL) {
#if defined(__PROSPERO__)
            fade_out_into(out, want);
#else
            memset(stream, 0, (size_t)len);
#endif
            return;
        }
        g_primed = 1;
#if defined(__PROSPERO__)
        g_fade_in = XA2_FADE;
#endif
    }
    if (have > want)
        have = want;
    for (i = 0; i < have; i++)
        memcpy(out[i], g_ring[(g_ring_r + i) % RING_FRAMES], sizeof out[i]);
    g_ring_r += have;
#if defined(__PROSPERO__)
    for (i = 0; i < have && g_fade_in > 0; i++, g_fade_in--) {
        unsigned c;
        int gain = XA2_FADE - g_fade_in + 1;         /* 1..64 of 64 */
        for (c = 0; c < XA2_CHANNELS; c++)
            out[i][c] = (int16_t)((int)out[i][c] * gain / XA2_FADE);
    }
    if (have > 0)
        memcpy(g_last_out, out[have - 1], sizeof g_last_out);
#endif
    if (have < want) {
#if defined(__PROSPERO__)
        fade_out_into(out + have, want - have);
#else
        memset(out[have], 0, (size_t)(want - have) * sizeof out[0]);
#endif
        /* One gap however many callbacks it lasts, and none before the title
         * has sent anything. */
        if (!g_dry && g_xa2_frames_written > 0) {
            g_dry = 1;
            g_xa2_starved++;
        }
        g_primed = 0;
    } else {
        g_dry = 0;
    }
}

#if defined(__PROSPERO__)
/* [ps5 port] sceAudioOutOutput blocks until the previous buffer has started
 * playing, so this loop runs at exactly the output rate: 256 frames, 5.3 ms. */
#define PS5_GRAIN 256
static void *ps5_out_thread(void *unused)
{
    static int16_t buf[PS5_GRAIN * XA2_CHANNELS] __attribute__((aligned(256)));
    extern void ps5_prof_register(const char *name) __attribute__((weak));
    extern void ps5_off_guest_cpu(void) __attribute__((weak));
    unsigned failures = 0;
    (void)unused;
    if (ps5_prof_register)
        ps5_prof_register("audio out");
    {
        /* Off the title's CPU (it can be started from a title thread, and
         * would inherit that CPU), and ahead of the other host threads: it
         * blocks in the output call nearly all the time, and a late return
         * is a click the ring cannot see. */
        struct sched_param sp;
        int e;
        if (ps5_off_guest_cpu)
            ps5_off_guest_cpu();
        memset(&sp, 0, sizeof sp);
        sp.sched_priority = 650;
        e = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        fprintf(stderr, "[XA2] output thread priority 650 (%s)\n", e ? "refused" : "ok");
    }
    while (g_out_running) {
        int r;
        pthread_mutex_lock(&g_ring_lock);
        sdl_callback(NULL, (Uint8 *)buf, (int)sizeof buf);
        pthread_mutex_unlock(&g_ring_lock);
        r = sceAudioOutOutput(g_sdl_dev - 1, buf);
        if (r < 0) {
            /* It paces this loop; failing at once, it would drain the ring
             * in a moment and keep a CPU busy for good. */
            if (failures++ < 4)
                fprintf(stderr, "[XA2] sceAudioOutOutput failed (0x%x)\n", (unsigned)r);
            usleep(5000);
        }
    }
    return NULL;
}
#elif defined(XBOXRECOMP_HAVE_SDL3)
/* SDL3 asks a stream for more instead of handing over a buffer. */
static void SDLCALL sdl3_feed(void *user, SDL_AudioStream *stream, int additional, int total)
{
    static Uint8 chunk[4096];
    (void)total;
    while (additional > 0) {
        int n = additional < (int)sizeof chunk ? additional : (int)sizeof chunk;
        n -= n % (XA2_CHANNELS * (int)sizeof(int16_t));
        if (n <= 0)
            break;
        sdl_callback(user, chunk, n);
        SDL_PutAudioStreamData(stream, chunk, n);
        additional -= n;
    }
}
static SDL_AudioStream *g_sdl_stream;
#endif

int xa2_init(void)
{
#if defined(__PROSPERO__)
    int port;
    if (g_xa2_initialized)
        return 1;
    sceAudioOutInit();   /* already initialised by someone else is fine */
    port = sceAudioOutOpen(0xFF /* system user */, 0 /* main */, 0, PS5_GRAIN,
                           XA2_SAMPLE_RATE, 1 /* s16 stereo */);
    if (port < 0) {
        fprintf(stderr, "[XA2] sceAudioOutOpen failed (0x%x)\n", (unsigned)port);
        return 0;
    }
    g_sdl_dev = port + 1;
    g_out_running = 1;
    if (pthread_create(&g_out_thread, NULL, ps5_out_thread, NULL) != 0) {
        sceAudioOutClose(port);
        g_sdl_dev = 0;
        return 0;
    }
    g_xa2_initialized = 1;
    g_xa2_frames_written = 0;
    fprintf(stderr, "[XA2] PS5 audio output initialized (%d Hz, %d channels, port %d)\n",
            XA2_SAMPLE_RATE, XA2_CHANNELS, port);
    return 1;
}
#else
#ifdef XBOXRECOMP_HAVE_SDL3
    SDL_AudioSpec have;
#else
    SDL_AudioSpec want, have;
#endif

    if (g_xa2_initialized)
        return 1;
    /* A run without a display is an unattended one: keep its timing (the
     * dummy driver consumes at the real rate) and keep it quiet. */
    if (getenv("RECOMP_HEADLESS") && !getenv("SDL_AUDIODRIVER") && !getenv("SDL_AUDIO_DRIVER")) {
#ifdef XBOXRECOMP_HAVE_SDL3
        /* SDL3 reads its own copy of the environment, taken before this. */
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
#else
        setenv("SDL_AUDIODRIVER", "dummy", 1);
#endif
    }
#ifdef XBOXRECOMP_HAVE_SDL3
    {
        SDL_AudioSpec spec;
        if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            fprintf(stderr, "[XA2] SDL audio unavailable: %s\n", SDL_GetError());
            return 0;
        }
        spec.freq = XA2_SAMPLE_RATE;
        spec.format = SDL_AUDIO_S16;
        spec.channels = XA2_CHANNELS;
        g_sdl_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                                                 sdl3_feed, NULL);
        if (!g_sdl_stream) {
            fprintf(stderr, "[XA2] no audio device: %s\n", SDL_GetError());
            return 0;
        }
        g_sdl_dev = SDL_GetAudioStreamDevice(g_sdl_stream);
        SDL_ResumeAudioStreamDevice(g_sdl_stream);
        have.freq = spec.freq;
        have.channels = spec.channels;
    }
#else
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[XA2] SDL audio unavailable: %s\n", SDL_GetError());
        return 0;
    }
    SDL_zero(want);
    want.freq = XA2_SAMPLE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = XA2_CHANNELS;
    want.samples = 512;
    want.callback = sdl_callback;
    g_sdl_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_sdl_dev) {
        fprintf(stderr, "[XA2] no audio device: %s\n", SDL_GetError());
        return 0;
    }
    SDL_PauseAudioDevice(g_sdl_dev, 0);
#endif
    g_xa2_initialized = 1;
    g_xa2_frames_written = 0;
    fprintf(stderr, "[XA2] SDL audio output initialized (%d Hz, %d channels, driver %s)\n",
            have.freq, have.channels, SDL_GetCurrentAudioDriver());
    return 1;
}
#endif /* __PROSPERO__ */

void xa2_shutdown(void)
{
#if defined(__PROSPERO__)
    if (g_out_running) {
        g_out_running = 0;
        pthread_join(g_out_thread, NULL);
    }
    if (g_sdl_dev) {
        sceAudioOutClose(g_sdl_dev - 1);
        g_sdl_dev = 0;
    }
#elif defined(XBOXRECOMP_HAVE_SDL3)
    if (g_sdl_stream) {
        SDL_DestroyAudioStream(g_sdl_stream);
        g_sdl_stream = NULL;
    }
    g_sdl_dev = 0;
#else
    if (g_sdl_dev) {
        SDL_CloseAudioDevice(g_sdl_dev);
        g_sdl_dev = 0;
    }
#endif
    if (g_xa2_initialized)
        fprintf(stderr, "[XA2] Shut down (%d frames written)\n", g_xa2_frames_written);
    g_xa2_initialized = 0;
}

int xa2_is_active(void) { return g_xa2_initialized; }

int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    unsigned n = (unsigned)(num_samples > XA2_BUF_SAMPLES ? XA2_BUF_SAMPLES : num_samples), i;
    int ok = 1;

    if (!g_xa2_initialized || !g_sdl_dev)
        return 0;
    /* A gap in the sound goes in the log, from here rather than from the device's own
     * thread: when it happened and how many so far, so a stutter can be lined up with
     * what the game was doing. */
    {
        static int told, seen;
        if (g_xa2_starved != seen && told < 60) {
            fprintf(stderr, "[XA2] the device ran dry (gap %d, %d frames written, %d submissions refused)\n",
                    g_xa2_starved, g_xa2_frames_written, g_xa2_dropped);
            told++;
        }
        seen = g_xa2_starved;
    }
#if defined(__PROSPERO__)
    pthread_mutex_lock(&g_ring_lock);
#elif defined(XBOXRECOMP_HAVE_SDL3)
    SDL_LockAudioStream(g_sdl_stream);
#else
    SDL_LockAudioDevice(g_sdl_dev);
#endif
    if (g_ring_w - g_ring_r + n > XA2_RING_MAX) {
        g_xa2_dropped++;
        ok = 0;
    } else {
        for (i = 0; i < n; i++)
            memcpy(g_ring[(g_ring_w + i) % RING_FRAMES], samples + i * XA2_CHANNELS,
                   sizeof g_ring[0]);
        g_ring_w += n;
        g_xa2_frames_written++;
    }
#if defined(__PROSPERO__)
    pthread_mutex_unlock(&g_ring_lock);
#elif defined(XBOXRECOMP_HAVE_SDL3)
    SDL_UnlockAudioStream(g_sdl_stream);
#else
    SDL_UnlockAudioDevice(g_sdl_dev);
#endif
    return ok;
}

int xa2_get_buffer_size(void) { return XA2_BUF_SAMPLES; }

#endif /* _WIN32 */
