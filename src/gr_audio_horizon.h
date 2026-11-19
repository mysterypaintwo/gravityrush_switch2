/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Audio output on the Switch 2 (Horizon, no SDL): the sink runtime_audio.c writes to.
 *
 * SDL mixes one stream per PS4 audio port; audout has a single output, so this mixes: every
 * sink (port) keeps a ring of stereo float frames, and one mixer thread sums them into 48 kHz
 * stereo s16 buffers for audout. Four 512-frame buffers are in flight (about 43 ms). A sink that
 * has nothing queued adds silence; the runtime's pacing (runtime_audio.c) keeps every port's
 * queue near two buffers, as with SDL.
 *
 * sink_queued() reports the queue in the bytes of the port's own format (frames x channels x
 * sample size), the unit SDL_GetAudioStreamQueued uses, so the pacing code works unchanged.
 * On HOME / sleep (switch2/host/gr_suspend_horizon.c) the output stops and starts again with
 * runtime_audio_host_suspend. */
#pragma once
#include <switch.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define SINK_RING_FRAMES 16384u /* per port: 341 ms */
#define MIX_FRAMES 512u
#define MIX_BUFFERS 4

typedef struct AudioSink {
    int is_float, channels; /* the port's format as it is written (1 or 2 channels) */
    float ring[SINK_RING_FRAMES][2];
    unsigned head, count;   /* frames */
    struct AudioSink* next;
} AudioSink;

static pthread_mutex_t sink_lock = PTHREAD_MUTEX_INITIALIZER;
static AudioSink* sinks;
static int sink_state = -1; /* -1 not started, 0 failed, 1 running */
static AudioOutBuffer mix_buffers[MIX_BUFFERS];
static int16_t* mix_data[MIX_BUFFERS];

static void mix_into(int16_t* out)
{
    float acc[MIX_FRAMES][2];
    memset(acc, 0, sizeof acc);
    pthread_mutex_lock(&sink_lock);
    for (AudioSink* s = sinks; s; s = s->next) {
        const unsigned n = s->count < MIX_FRAMES ? s->count : MIX_FRAMES;
        for (unsigned f = 0; f < n; ++f) {
            const unsigned at = (s->head + f) % SINK_RING_FRAMES;
            acc[f][0] += s->ring[at][0];
            acc[f][1] += s->ring[at][1];
        }
        s->head = (s->head + n) % SINK_RING_FRAMES;
        s->count -= n;
    }
    pthread_mutex_unlock(&sink_lock);
    for (unsigned f = 0; f < MIX_FRAMES; ++f)
        for (int c = 0; c < 2; ++c) {
            float v = acc[f][c];
            v = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
            out[f * 2 + c] = (int16_t)(v * 32767.0f);
        }
}

void gr_quit_point(void); /* switch2/host/gr_suspend_horizon.c: ends the thread when the game exits */
static void* mixer_thread(void* arg)
{
    (void)arg;
    for (;;) {
        AudioOutBuffer* released = NULL;
        u32 count = 0;
        gr_quit_point();
        if (R_FAILED(audoutWaitPlayFinish(&released, &count, 50000000ull)) || !released) continue;
        for (u32 i = 0; i < count; ++i) {
            AudioOutBuffer* b = &released[i];
            mix_into((int16_t*)b->buffer);
            b->next = NULL;
            audoutAppendAudioOutBuffer(b);
        }
    }
    return NULL;
}

/* Starts audout and the mixer once; 0 when there is no audio output. */
static int sink_start(void)
{
    if (sink_state >= 0) return sink_state;
    sink_state = 0;
    if (R_FAILED(audoutInitialize()) || R_FAILED(audoutStartAudioOut())) {
        fprintf(stderr, "Runtime: audout is not available; the game runs silent\n");
        return 0;
    }
    const size_t bytes = MIX_FRAMES * 2 * sizeof(int16_t), aligned = (bytes + 0xfff) & ~(size_t)0xfff;
    for (int i = 0; i < MIX_BUFFERS; ++i) {
        mix_data[i] = aligned_alloc(0x1000, aligned);
        if (!mix_data[i]) return 0;
        memset(mix_data[i], 0, aligned);
        mix_buffers[i] = (AudioOutBuffer){.next = NULL, .buffer = mix_data[i], .buffer_size = aligned,
                                          .data_size = bytes, .data_offset = 0};
        audoutAppendAudioOutBuffer(&mix_buffers[i]);
    }
    pthread_t thread;
    if (pthread_create(&thread, NULL, mixer_thread, NULL)) return 0;
    pthread_detach(thread);
    printf("Runtime: audio backend audout (%u Hz, %u channels, %u-frame mix)\n", audoutGetSampleRate(),
           audoutGetChannelCount(), MIX_FRAMES);
    sink_state = 1;
    return 1;
}

static AudioSink* sink_open(int is_float, int channels)
{
    if (channels < 1 || channels > 2) return NULL; /* 8-channel ports are mixed down first */
    AudioSink* s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->is_float = is_float;
    s->channels = channels;
    pthread_mutex_lock(&sink_lock);
    s->next = sinks;
    sinks = s;
    pthread_mutex_unlock(&sink_lock);
    return s;
}

static void sink_close(AudioSink* s)
{
    pthread_mutex_lock(&sink_lock);
    for (AudioSink** p = &sinks; *p; p = &(*p)->next)
        if (*p == s) { *p = s->next; break; }
    pthread_mutex_unlock(&sink_lock);
    free(s);
}

static int sink_queued(AudioSink* s)
{
    pthread_mutex_lock(&sink_lock);
    const unsigned frames = s->count;
    pthread_mutex_unlock(&sink_lock);
    return (int)(frames * (unsigned)s->channels * (s->is_float ? 4u : 2u));
}

static void sink_put(AudioSink* s, const void* data, int bytes)
{
    const unsigned frame_bytes = (unsigned)s->channels * (s->is_float ? 4u : 2u);
    const unsigned frames = (unsigned)bytes / frame_bytes;
    pthread_mutex_lock(&sink_lock);
    for (unsigned f = 0; f < frames; ++f) {
        float v[2];
        for (int c = 0; c < s->channels; ++c) {
            const size_t i = (size_t)f * (size_t)s->channels + (size_t)c;
            if (s->is_float) memcpy(&v[c], (const char*)data + i * 4, 4);
            else { int16_t x; memcpy(&x, (const char*)data + i * 2, 2); v[c] = (float)x / 32768.0f; }
        }
        if (s->channels == 1) v[1] = v[0];
        if (s->count == SINK_RING_FRAMES) { s->head = (s->head + 1) % SINK_RING_FRAMES; --s->count; } /* full: drop the oldest */
        const unsigned at = (s->head + s->count) % SINK_RING_FRAMES;
        s->ring[at][0] = v[0];
        s->ring[at][1] = v[1];
        ++s->count;
    }
    pthread_mutex_unlock(&sink_lock);
}

/* HOME menu / sleep: stop the output (nothing keeps playing from queued buffers), then start it
 * again. Called by the host's applet thread with the game's threads frozen. */
void runtime_audio_host_suspend(int suspend)
{
    if (sink_state != 1) return;
    if (suspend) audoutStopAudioOut();
    else audoutStartAudioOut();
}
