// src/backend/anland/anland_audio.c
#define _GNU_SOURCE
#include "anland_audio.h"
#include "protocol.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/hook.h>

#define DEFAULT_RATE          48000
#define DEFAULT_PLAY_CHANNELS 2
#define DEFAULT_CAP_CHANNELS  1
#define MIC_RING_BYTES        (48000 * 2 * (int)sizeof(int16_t))
#define MAX_DGRAM             (64 * 1024)
#define RECONNECT_SECS        1

struct anland_audio {
    struct pw_thread_loop *loop;
    struct pw_context     *context;
    struct pw_core        *core;
    struct spa_hook        core_listener;
    struct spa_source     *reconnect_timer;
    bool                   pw_connected;

    struct pw_stream      *capture;
    struct spa_hook        capture_listener;
    struct pw_stream      *source;
    struct spa_hook        source_listener;

    uint32_t               play_rate, play_channels;
    uint32_t               cap_rate, cap_channels;
    uint32_t               play_quantum, cap_quantum;

    int                    audio_fd;
    struct spa_source     *io;

    uint8_t               *ring;
    size_t                 ring_size, ring_head, ring_tail, ring_fill;

    uint8_t                rx[MAX_DGRAM];
};

static struct anland_audio *g_audio = NULL;

static int connect_stream(struct pw_stream *stream, enum spa_direction direction,
                          uint32_t rate, uint32_t channels, uint32_t quantum);
static const struct spa_pod *build_format(struct spa_pod_builder *bld,
                                          uint32_t rate, uint32_t channels);
static void set_latency(struct pw_stream *stream, uint32_t quantum, uint32_t rate);

static void ring_reset(struct anland_audio *a)
{
    a->ring_head = a->ring_tail = a->ring_fill = 0;
}

static void ring_write(struct anland_audio *a, const uint8_t *p, size_t n)
{
    if (n > a->ring_size) {
        p += n - a->ring_size;
        n = a->ring_size;
    }
    if (a->ring_fill + n > a->ring_size) {
        size_t drop = a->ring_fill + n - a->ring_size;
        a->ring_tail = (a->ring_tail + drop) % a->ring_size;
        a->ring_fill -= drop;
    }
    size_t first = a->ring_size - a->ring_head;
    if (first > n)
        first = n;
    memcpy(a->ring + a->ring_head, p, first);
    memcpy(a->ring, p + first, n - first);
    a->ring_head = (a->ring_head + n) % a->ring_size;
    a->ring_fill += n;
}

static size_t ring_read(struct anland_audio *a, uint8_t *p, size_t n)
{
    size_t got = n < a->ring_fill ? n : a->ring_fill;
    size_t first = a->ring_size - a->ring_tail;
    if (first > got)
        first = got;
    memcpy(p, a->ring + a->ring_tail, first);
    memcpy(p + first, a->ring, got - first);
    a->ring_tail = (a->ring_tail + got) % a->ring_size;
    a->ring_fill -= got;
    return got;
}

static void on_capture_process(void *data)
{
    struct anland_audio *a = data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(a->capture);
    if (!b)
        return;

    struct spa_data *d = &b->buffer->datas[0];
    if (d->data && d->chunk->size > 0 && a->audio_fd >= 0) {
        struct audio_msg h = { .type = AUDIO_MSG_PCM, .size = d->chunk->size };
        struct iovec iov[2] = {
            { .iov_base = &h, .iov_len = sizeof(h) },
            { .iov_base = (uint8_t *)d->data + d->chunk->offset, .iov_len = d->chunk->size },
        };
        struct msghdr m = { .msg_iov = iov, .msg_iovlen = 2 };
        sendmsg(a->audio_fd, &m, MSG_DONTWAIT | MSG_NOSIGNAL);
    }
    pw_stream_queue_buffer(a->capture, b);
}

static void on_source_process(void *data)
{
    struct anland_audio *a = data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(a->source);
    if (!b)
        return;

    struct spa_data *d = &b->buffer->datas[0];
    const uint32_t stride = sizeof(int16_t) * a->cap_channels;
    uint32_t frames = d->maxsize / stride;
    if (b->requested && b->requested < frames)
        frames = b->requested;
    uint32_t bytes = frames * stride;

    size_t got = ring_read(a, d->data, bytes);
    if (got < bytes)
        memset((uint8_t *)d->data + got, 0, bytes - got);

    d->chunk->offset = 0;
    d->chunk->stride = stride;
    d->chunk->size = bytes;
    pw_stream_queue_buffer(a->source, b);
}

static const struct pw_stream_events capture_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_capture_process,
};

static const struct pw_stream_events source_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_source_process,
};

static void apply_format(struct anland_audio *a, const struct audio_format *f)
{
    const bool playback = (f->role == AUDIO_ROLE_PLAYBACK);
    const uint32_t rate = f->rate ? f->rate : DEFAULT_RATE;
    const uint32_t channels = f->channels ? f->channels
                                          : (playback ? DEFAULT_PLAY_CHANNELS : DEFAULT_CAP_CHANNELS);

    uint32_t *cur_rate     = playback ? &a->play_rate : &a->cap_rate;
    uint32_t *cur_channels = playback ? &a->play_channels : &a->cap_channels;
    uint32_t *cur_quantum  = playback ? &a->play_quantum : &a->cap_quantum;
    struct pw_stream *stream = playback ? a->capture : a->source;

    const bool format_changed = (rate != *cur_rate || channels != *cur_channels);
    const bool quantum_changed = (f->quantum != *cur_quantum);
    if (!format_changed && !quantum_changed)
        return;

    *cur_rate = rate;
    *cur_channels = channels;
    *cur_quantum = f->quantum;

    if (!a->pw_connected || !stream)
        return;

    if (format_changed) {
        set_latency(stream, f->quantum, rate);
        uint8_t buffer[1024];
        struct spa_pod_builder bld = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const struct spa_pod *params[1] = { build_format(&bld, rate, channels) };
        pw_stream_update_params(stream, params, 1);
    } else if (f->quantum > 0) {
        set_latency(stream, f->quantum, rate);
    }
}

static void on_audio_readable(void *data, int fd, uint32_t mask)
{
    struct anland_audio *a = data;
    if (mask & (SPA_IO_ERR | SPA_IO_HUP))
        return;
    if (!(mask & SPA_IO_IN))
        return;

    for (;;) {
        ssize_t n = recv(fd, a->rx, sizeof(a->rx), MSG_DONTWAIT);
        if (n <= 0)
            break;
        if ((size_t)n < sizeof(struct audio_msg))
            continue;
        struct audio_msg h;
        memcpy(&h, a->rx, sizeof(h));
        size_t avail = (size_t)n - sizeof(struct audio_msg);

        if (h.type == AUDIO_MSG_FORMAT) {
            if (avail >= sizeof(struct audio_format)) {
                struct audio_format f;
                memcpy(&f, a->rx + sizeof(struct audio_msg), sizeof(f));
                apply_format(a, &f);
            }
            continue;
        }
        if (h.type != AUDIO_MSG_PCM)
            continue;
        size_t size = h.size < avail ? h.size : avail;
        ring_write(a, a->rx + sizeof(struct audio_msg), size);
    }
}

static void arm_reconnect(struct anland_audio *a)
{
    struct timespec val = { .tv_sec = RECONNECT_SECS, .tv_nsec = 0 };
    pw_loop_update_timer(pw_thread_loop_get_loop(a->loop), a->reconnect_timer,
                         &val, NULL, false);
}

static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
    struct anland_audio *a = data;
    (void)seq;
    (void)message;
    if (id == PW_ID_CORE && res == -EPIPE) {
        a->pw_connected = false;
        arm_reconnect(a);
    }
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .error = on_core_error,
};

static const struct spa_pod *build_format(struct spa_pod_builder *bld,
                                          uint32_t rate, uint32_t channels)
{
    struct spa_audio_info_raw info = {
        .format = SPA_AUDIO_FORMAT_S16_LE,
        .rate = rate,
        .channels = channels,
    };
    if (channels >= 2) {
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
    } else {
        info.position[0] = SPA_AUDIO_CHANNEL_MONO;
    }
    return spa_format_audio_raw_build(bld, SPA_PARAM_EnumFormat, &info);
}

static void set_latency(struct pw_stream *stream, uint32_t quantum, uint32_t rate)
{
    if (quantum == 0)
        return;
    char latency[32];
    snprintf(latency, sizeof(latency), "%u/%u", quantum, rate);
    struct spa_dict_item items[] = {
        SPA_DICT_ITEM_INIT(PW_KEY_NODE_LATENCY, latency),
    };
    struct spa_dict dict = SPA_DICT_INIT(items, 1);
    pw_stream_update_properties(stream, &dict);
}

static int connect_stream(struct pw_stream *stream, enum spa_direction direction,
                          uint32_t rate, uint32_t channels, uint32_t quantum)
{
    set_latency(stream, quantum, rate);

    uint8_t buffer[1024];
    struct spa_pod_builder bld = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[1] = { build_format(&bld, rate, channels) };

    return pw_stream_connect(stream, direction, PW_ID_ANY,
                             PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                 PW_STREAM_FLAG_RT_PROCESS,
                             params, 1);
}

static void teardown_pw(struct anland_audio *a)
{
    if (a->capture) {
        spa_hook_remove(&a->capture_listener);
        pw_stream_destroy(a->capture);
        a->capture = NULL;
    }
    if (a->source) {
        spa_hook_remove(&a->source_listener);
        pw_stream_destroy(a->source);
        a->source = NULL;
    }
    if (a->core) {
        spa_hook_remove(&a->core_listener);
        pw_core_disconnect(a->core);
        a->core = NULL;
    }
}

static int build_pw(struct anland_audio *a)
{
    a->core = pw_context_connect(a->context, NULL, 0);
    if (!a->core)
        return -1;
    pw_core_add_listener(a->core, &a->core_listener, &core_events, a);

    a->capture = pw_stream_new(a->core, "anland-speaker",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CLASS, "Audio/Sink",
            PW_KEY_NODE_NAME, "anland-speaker",
            PW_KEY_NODE_DESCRIPTION, "Anland remote speaker",
            PW_KEY_PRIORITY_SESSION, "1010",
            PW_KEY_PRIORITY_DRIVER, "1010",
            NULL));
    if (!a->capture)
        return -1;
    pw_stream_add_listener(a->capture, &a->capture_listener, &capture_events, a);

    a->source = pw_stream_new(a->core, "anland-mic",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CLASS, "Audio/Source",
            PW_KEY_NODE_NAME, "anland-mic",
            PW_KEY_NODE_DESCRIPTION, "Anland remote microphone",
            PW_KEY_PRIORITY_SESSION, "1010",
            PW_KEY_PRIORITY_DRIVER, "1010",
            NULL));
    if (!a->source)
        return -1;
    pw_stream_add_listener(a->source, &a->source_listener, &source_events, a);

    if (connect_stream(a->capture, PW_DIRECTION_INPUT, a->play_rate, a->play_channels,
                       a->play_quantum) < 0)
        return -1;
    if (connect_stream(a->source, PW_DIRECTION_OUTPUT, a->cap_rate, a->cap_channels,
                       a->cap_quantum) < 0)
        return -1;

    return 0;
}

static void on_reconnect_timer(void *data, uint64_t expirations)
{
    struct anland_audio *a = data;
    (void)expirations;
    if (a->pw_connected)
        return;

    teardown_pw(a);
    if (build_pw(a) == 0) {
        a->pw_connected = true;
    } else {
        teardown_pw(a);
        arm_reconnect(a);
    }
}

int anland_audio_start(void)
{
    if (g_audio)
        return 0;

    pw_init(NULL, NULL);

    struct anland_audio *a = calloc(1, sizeof(*a));
    if (!a)
        return -1;
    a->audio_fd = -1;
    a->play_rate = DEFAULT_RATE;
    a->play_channels = DEFAULT_PLAY_CHANNELS;
    a->cap_rate = DEFAULT_RATE;
    a->cap_channels = DEFAULT_CAP_CHANNELS;
    a->ring_size = MIC_RING_BYTES;
    a->ring = malloc(a->ring_size);
    if (!a->ring)
        goto fail;

    a->loop = pw_thread_loop_new("anland-audio", NULL);
    if (!a->loop)
        goto fail;

    a->context = pw_context_new(pw_thread_loop_get_loop(a->loop), NULL, 0);
    if (!a->context)
        goto fail;

    a->reconnect_timer = pw_loop_add_timer(pw_thread_loop_get_loop(a->loop),
                                           on_reconnect_timer, a);
    if (!a->reconnect_timer)
        goto fail;

    if (pw_thread_loop_start(a->loop) < 0)
        goto fail;

    pw_thread_loop_lock(a->loop);
    if (build_pw(a) == 0) {
        a->pw_connected = true;
    } else {
        teardown_pw(a);
        arm_reconnect(a);
    }
    pw_thread_loop_unlock(a->loop);

    g_audio = a;
    return 0;

fail:
    if (a->loop)
        pw_thread_loop_destroy(a->loop);
    if (a->context)
        pw_context_destroy(a->context);
    free(a->ring);
    free(a);
    pw_deinit();
    return -1;
}

void anland_audio_stop(void)
{
    struct anland_audio *a = g_audio;
    if (!a)
        return;
    g_audio = NULL;

    if (a->loop)
        pw_thread_loop_stop(a->loop);
    teardown_pw(a);
    if (a->io)
        pw_loop_destroy_source(pw_thread_loop_get_loop(a->loop), a->io);
    if (a->reconnect_timer)
        pw_loop_destroy_source(pw_thread_loop_get_loop(a->loop), a->reconnect_timer);
    if (a->context)
        pw_context_destroy(a->context);
    if (a->loop)
        pw_thread_loop_destroy(a->loop);
    free(a->ring);
    free(a);
    pw_deinit();
}

void anland_audio_set_fd(int audio_fd)
{
    struct anland_audio *a = g_audio;
    if (!a)
        return;

    pw_thread_loop_lock(a->loop);

    struct pw_loop *loop = pw_thread_loop_get_loop(a->loop);
    if (a->io) {
        pw_loop_destroy_source(loop, a->io);
        a->io = NULL;
    }
    a->audio_fd = audio_fd;
    ring_reset(a);

    if (audio_fd >= 0) {
        a->io = pw_loop_add_io(loop, audio_fd, SPA_IO_IN, false, on_audio_readable, a);
    }

    pw_thread_loop_unlock(a->loop);
}