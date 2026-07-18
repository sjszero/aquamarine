// src/backend/anland/display_producer.c
#define _GNU_SOURCE
#include "display_producer.h"
#include "socket_utils.h"

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>

/*
 * 握手超时（毫秒）
 */
#define HANDSHAKE_TIMEOUT_MS 5000

/*
 * 显示上下文结构
 */
struct display_ctx {
    /* 控制通道 */
    int ctrl_fd;

    /* 数据通道 */
    int data_fd;

    /* Buffer ready eventfd（消费者 -> 生产者） */
    int buf_ready_efd;

    /* 渲染栅栏通道（生产者 -> 消费者） */
    int fence_fd;

    /* 共享内存（缓冲区索引） */
    int shm_fd;
    volatile uint32_t *shm_ptr;

    /* 音频通道 */
    int audio_fd;

    /* 待发送的渲染栅栏 */
    int pending_render_fence;

    /* 屏幕信息 */
    uint32_t screen_w, screen_h;
    uint32_t pixel_format;
    uint32_t refresh;

    /* 状态 */
    bool fallback;
    bool in_fallback_callback;

    /* dmabuf 缓冲区 */
    int dmabuf_fds[MAX_BUFS];
    struct buf_info dmabuf_infos[MAX_BUFS];
    int buf_count;

    /* Fallback 回调 */
    void (*fallback_cb)(void *);
    void *fallback_userdata;
};

/*
 * 释放所有消费者资源
 */
static void release_consumer_resources(display_ctx *ctx)
{
    for (int i = 0; i < ctx->buf_count; i++) {
        if (ctx->dmabuf_fds[i] >= 0) {
            close(ctx->dmabuf_fds[i]);
            ctx->dmabuf_fds[i] = -1;
        }
    }
    ctx->buf_count = 0;

    if (ctx->data_fd >= 0) {
        close(ctx->data_fd);
        ctx->data_fd = -1;
    }
    if (ctx->buf_ready_efd >= 0) {
        close(ctx->buf_ready_efd);
        ctx->buf_ready_efd = -1;
    }
    if (ctx->fence_fd >= 0) {
        close(ctx->fence_fd);
        ctx->fence_fd = -1;
    }
    if (ctx->audio_fd >= 0) {
        close(ctx->audio_fd);
        ctx->audio_fd = -1;
    }
    if (ctx->pending_render_fence >= 0) {
        close(ctx->pending_render_fence);
        ctx->pending_render_fence = -1;
    }
    if (ctx->shm_ptr) {
        munmap((void *)ctx->shm_ptr, sizeof(uint32_t));
        ctx->shm_ptr = NULL;
    }
    if (ctx->shm_fd >= 0) {
        close(ctx->shm_fd);
        ctx->shm_fd = -1;
    }
}

/*
 * 进入 fallback 状态
 */
static void enter_fallback(display_ctx *ctx)
{
    if (ctx->fallback || ctx->in_fallback_callback)
        return;

    fprintf(stderr, "ANLAND: enter_fallback()\n");
    ctx->fallback = true;
    release_consumer_resources(ctx);

    if (ctx->fallback_cb) {
        ctx->in_fallback_callback = true;
        void (*cb)(void *) = ctx->fallback_cb;
        void *userdata = ctx->fallback_userdata;
        ctx->fallback_cb = NULL;
        ctx->fallback_userdata = NULL;

        cb(userdata);

        /* 恢复回调（以防被覆盖） */
        if (ctx->fallback_cb == NULL) {
            ctx->fallback_cb = cb;
            ctx->fallback_userdata = userdata;
        }
        ctx->in_fallback_callback = false;
    }
}

/*
 * 拾取消费者资源
 */
static int pickup_fds(display_ctx *ctx)
{
    fprintf(stderr, "ANLAND: pickup_fds() start\n");

    struct ctrl_msg hdr = { .type = CTRL_MSG_PICKUP_FDS, .size = 0 };
    if (send_all(ctx->ctrl_fd, &hdr, sizeof(hdr)) < 0) {
        fprintf(stderr, "ANLAND: pickup_fds() send_all failed\n");
        return -1;
    }
    fprintf(stderr, "ANLAND: pickup_fds() sent PICKUP_FDS, waiting...\n");

    struct pollfd pfd = { .fd = ctx->ctrl_fd, .events = POLLIN };
    int ret = poll(&pfd, 1, HANDSHAKE_TIMEOUT_MS);
    if (ret <= 0) {
        fprintf(stderr, "ANLAND: pickup_fds() poll timeout/error: %d\n", ret);
        return -1;
    }

    int fds[5];
    int fd_count = 0;
    struct ctrl_msg resp;
    int n = recv_fds(ctx->ctrl_fd, &resp, sizeof(resp), fds, 5, &fd_count);
    if (n <= 0) {
        fprintf(stderr, "ANLAND: pickup_fds() recv_fds failed: %d\n", n);
        return -1;
    }

    fprintf(stderr, "ANLAND: pickup_fds() type=%d, fd_count=%d\n", resp.type, fd_count);

    if (resp.type != CTRL_MSG_FDS_READY || fd_count < 5) {
        fprintf(stderr, "ANLAND: pickup_fds() invalid response\n");
        for (int i = 0; i < fd_count; i++)
            close(fds[i]);
        return -1;
    }

    /* 顺序：buf_ready, fence, data, shm, audio */
    ctx->buf_ready_efd = fds[0];
    ctx->fence_fd      = fds[1];
    ctx->data_fd       = fds[2];
    ctx->shm_fd        = fds[3];
    ctx->audio_fd      = fds[4];

    fprintf(stderr, "ANLAND: fds: buf_ready=%d, fence=%d, data=%d, shm=%d, audio=%d\n",
            ctx->buf_ready_efd, ctx->fence_fd, ctx->data_fd,
            ctx->shm_fd, ctx->audio_fd);

    ctx->shm_ptr = mmap(NULL, sizeof(uint32_t), PROT_READ, MAP_SHARED,
                        ctx->shm_fd, 0);
    if (ctx->shm_ptr == MAP_FAILED) {
        fprintf(stderr, "ANLAND: pickup_fds() mmap failed\n");
        ctx->shm_ptr = NULL;
        return -1;
    }

    fprintf(stderr, "ANLAND: pickup_fds() success\n");
    return 0;
}

/*
 * 接收 dmabuf 缓冲区
 */
static int receive_dmabufs(display_ctx *ctx)
{
    if (ctx->buf_count > 0) {
        fprintf(stderr, "ANLAND: receive_dmabufs() already have %d buffers\n",
                ctx->buf_count);
        return 0;
    }

    fprintf(stderr, "ANLAND: receive_dmabufs() waiting for dmabufs...\n");

    struct pollfd pfd = {
        .fd = ctx->data_fd,
        .events = POLLIN | POLLHUP | POLLERR
    };
    int ret = poll(&pfd, 1, HANDSHAKE_TIMEOUT_MS);
    if (ret <= 0) {
        fprintf(stderr, "ANLAND: receive_dmabufs() poll timeout/error: %d\n", ret);
        return -1;
    }

    if (pfd.revents & (POLLHUP | POLLERR)) {
        fprintf(stderr, "ANLAND: receive_dmabufs() data_fd error: revents=0x%x\n",
                pfd.revents);
        return -1;
    }

    struct data_msg dhdr;
    int fds[MAX_BUFS];
    int fd_count = 0;

    int n = recv_fds(ctx->data_fd, &dhdr, sizeof(dhdr), fds, MAX_BUFS, &fd_count);
    if (n < (int)sizeof(struct data_msg) || fd_count < 1) {
        fprintf(stderr, "ANLAND: receive_dmabufs() recv_fds failed: n=%d, fd_count=%d\n",
                n, fd_count);
        return -1;
    }

    fprintf(stderr, "ANLAND: receive_dmabufs() type=%d, size=%d, fd_count=%d\n",
            dhdr.type, dhdr.size, fd_count);

    if (dhdr.type != DATA_MSG_BUFS_READY) {
        fprintf(stderr, "ANLAND: receive_dmabufs() invalid type: %d\n", dhdr.type);
        for (int i = 0; i < fd_count; i++)
            close(fds[i]);
        return -1;
    }

    int count = dhdr.size / sizeof(struct buf_info);
    if (count != fd_count || count > MAX_BUFS) {
        fprintf(stderr, "ANLAND: receive_dmabufs() count mismatch\n");
        for (int i = 0; i < fd_count; i++)
            close(fds[i]);
        return -1;
    }

    struct buf_info infos[MAX_BUFS];
    if (recv_all(ctx->data_fd, infos, dhdr.size) < 0) {
        fprintf(stderr, "ANLAND: receive_dmabufs() recv_all failed\n");
        for (int i = 0; i < fd_count; i++)
            close(fds[i]);
        return -1;
    }

    for (int i = 0; i < count; i++) {
        ctx->dmabuf_fds[i] = fds[i];
        ctx->dmabuf_infos[i] = infos[i];
        fprintf(stderr, "ANLAND: buffer %d: %dx%d fd=%d\n",
                i, infos[i].width, infos[i].height, fds[i]);
    }
    ctx->buf_count = count;

    fprintf(stderr, "ANLAND: receive_dmabufs() success, got %d buffers\n", count);
    return 0;
}

/*
 * 连接到显示守护进程
 */
int connect_to_deamon(display_ctx **out, const char *socket_path)
{
    fprintf(stderr, "ANLAND: connect_to_deamon() socket=%s\n", socket_path);

    display_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return -1;

    /* 初始化 */
    ctx->ctrl_fd = -1;
    ctx->data_fd = -1;
    ctx->buf_ready_efd = -1;
    ctx->fence_fd = -1;
    ctx->shm_fd = -1;
    ctx->audio_fd = -1;
    ctx->pending_render_fence = -1;
    ctx->shm_ptr = NULL;
    ctx->fallback = true;
    ctx->in_fallback_callback = false;
    for (int i = 0; i < MAX_BUFS; i++)
        ctx->dmabuf_fds[i] = -1;

    /* 连接控制通道 */
    ctx->ctrl_fd = connect_unix(socket_path);
    if (ctx->ctrl_fd < 0) {
        fprintf(stderr, "ANLAND: connect_unix() failed\n");
        goto fail;
    }
    fprintf(stderr, "ANLAND: connect_unix() success, fd=%d\n", ctx->ctrl_fd);

    /* 发送 PRODUCER_HELLO */
    struct ctrl_msg hdr = { .type = CTRL_MSG_PRODUCER_HELLO, .size = 0 };
    if (send_all(ctx->ctrl_fd, &hdr, sizeof(hdr)) < 0) {
        fprintf(stderr, "ANLAND: send_all(PRODUCER_HELLO) failed\n");
        goto fail;
    }

    /* 接收 SCREEN_INFO */
    uint8_t buf[sizeof(struct ctrl_msg) + sizeof(struct screen_info)];
    if (recv_all(ctx->ctrl_fd, buf, sizeof(buf)) < 0) {
        fprintf(stderr, "ANLAND: recv_all(SCREEN_INFO) failed\n");
        goto fail;
    }

    struct ctrl_msg resp;
    memcpy(&resp, buf, sizeof(resp));
    if (resp.type != CTRL_MSG_SCREEN_INFO ||
        resp.size != sizeof(struct screen_info)) {
        fprintf(stderr, "ANLAND: invalid response type=%d, size=%d\n",
                resp.type, resp.size);
        goto fail;
    }

    struct screen_info si;
    memcpy(&si, buf + sizeof(struct ctrl_msg), sizeof(si));
    ctx->screen_w = si.width;
    ctx->screen_h = si.height;
    ctx->pixel_format = si.format;
    ctx->refresh = si.refresh;

    fprintf(stderr, "ANLAND: screen info: %dx%d refresh=%d mHz\n",
            si.width, si.height, si.refresh);

    *out = ctx;
    return 0;

fail:
    if (ctx->ctrl_fd >= 0)
        close(ctx->ctrl_fd);
    free(ctx);
    return -1;
}

/*
 * 断开连接
 */
void disconnect(display_ctx *ctx)
{
    if (!ctx)
        return;
    release_consumer_resources(ctx);
    if (ctx->ctrl_fd >= 0)
        close(ctx->ctrl_fd);
    free(ctx);
}

/*
 * 获取屏幕信息
 */
int get_screen_info(display_ctx *ctx, uint32_t *width, uint32_t *height,
                    uint32_t *format, uint32_t *refresh)
{
    if (!ctx) return -1;
    *width  = ctx->screen_w;
    *height = ctx->screen_h;
    *format = ctx->pixel_format;
    *refresh = ctx->refresh;
    return 0;
}

/*
 * 设置渲染栅栏
 */
void set_render_fence(display_ctx *ctx, int fence_fd)
{
    if (ctx->pending_render_fence >= 0)
        close(ctx->pending_render_fence);
    ctx->pending_render_fence = fence_fd;
}

/*
 * 触发刷新
 */
int trigger_refresh(display_ctx *ctx)
{
    if (ctx->fallback) {
        if (ctx->pending_render_fence >= 0) {
            close(ctx->pending_render_fence);
            ctx->pending_render_fence = -1;
        }
        return 0;
    }

    char b = 0;
    struct iovec iov = { .iov_base = &b, .iov_len = 1 };

    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } cmsg;

    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
    };

    if (ctx->pending_render_fence >= 0) {
        msg.msg_control = cmsg.buf;
        msg.msg_controllen = sizeof(cmsg.buf);
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &ctx->pending_render_fence, sizeof(int));
    }

    sendmsg(ctx->fence_fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);

    if (ctx->pending_render_fence >= 0) {
        close(ctx->pending_render_fence);
        ctx->pending_render_fence = -1;
    }

    return 0;
}

/*
 * 轮询输入事件
 */
int poll_input_event(display_ctx *ctx, struct InputEvent *event, int timeout_ms)
{
    if (ctx->fallback)
        return 0;

    struct pollfd pfd = { .fd = ctx->data_fd, .events = POLLIN };
    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0)
        return 0;

    if (pfd.revents & (POLLHUP | POLLERR)) {
        enter_fallback(ctx);
        return -1;
    }

    uint8_t msg_buf[sizeof(struct data_msg) + sizeof(struct InputEvent)];
    ssize_t n = recv(ctx->data_fd, msg_buf, sizeof(msg_buf), MSG_PEEK);
    if (n < (ssize_t)sizeof(struct data_msg))
        return 0;

    struct data_msg hdr;
    memcpy(&hdr, msg_buf, sizeof(hdr));
    if (hdr.type != DATA_MSG_INPUT_EVENT)
        return 0;

    if (recv_all(ctx->data_fd, msg_buf,
                 sizeof(struct data_msg) + sizeof(struct InputEvent)) < 0)
        return -1;

    memcpy(event, msg_buf + sizeof(struct data_msg), sizeof(*event));
    return 1;
}

/*
 * 接收扩展数据
 */
int poll_input_event_extend_data(display_ctx *ctx, void* payload,
                                 size_t size, int timeout_ms)
{
    if (ctx->fallback)
        return 0;

    struct pollfd pfd = { .fd = ctx->data_fd, .events = POLLIN };
    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0)
        return 0;

    if (pfd.revents & (POLLHUP | POLLERR)) {
        enter_fallback(ctx);
        return -1;
    }

    if (recv_all(ctx->data_fd, payload, size) < 0)
        return -1;

    return 1;
}

/*
 * 接收扩展文件描述符
 */
int poll_input_event_extend_fds(display_ctx *ctx, int *fds, int max_fds,
                                int *fd_count, int timeout_ms)
{
    *fd_count = 0;
    if (ctx->fallback)
        return 0;

    struct pollfd pfd = { .fd = ctx->data_fd, .events = POLLIN };
    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0)
        return 0;

    if (pfd.revents & (POLLHUP | POLLERR)) {
        enter_fallback(ctx);
        return -1;
    }

    struct data_msg hdr;
    int got = 0;
    int n = recv_fds(ctx->data_fd, &hdr, sizeof(hdr), fds, max_fds, &got);
    if (n < (int)sizeof(struct data_msg)) {
        for (int i = 0; i < got; i++)
            close(fds[i]);
        return -1;
    }

    if (hdr.type != DATA_MSG_INPUT_EXTEND_FDS) {
        for (int i = 0; i < got; i++)
            close(fds[i]);
        return -1;
    }

    *fd_count = got;
    return 1;
}

/*
 * 请求资源
 */
int push_resources_request(display_ctx *ctx, uint32_t service_type,
                           const uint32_t *args)
{
    struct OutputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = OUTPUT_TYPE_RESOURCES_REQUEST;
    ev.resources_request.type = service_type;
    if (args) {
        ev.resources_request.args[0] = args[0];
        ev.resources_request.args[1] = args[1];
        ev.resources_request.args[2] = args[2];
    }
    return push_output_event(ctx, &ev);
}

/*
 * 发送输出事件
 */
int push_output_event(display_ctx *ctx, const struct OutputEvent *event)
{
    if (ctx->fallback)
        return 0;

    struct data_msg hdr = {
        .type = DATA_MSG_OUTPUT_EVENT,
        .size = sizeof(struct OutputEvent)
    };

    uint8_t msg[sizeof(struct data_msg) + sizeof(struct OutputEvent)];
    memcpy(msg, &hdr, sizeof(hdr));
    memcpy(msg + sizeof(hdr), event, sizeof(*event));

    if (send_all(ctx->data_fd, msg, sizeof(msg)) < 0) {
        enter_fallback(ctx);
        return -1;
    }

    return 0;
}

/*
 * 发送带扩展数据的输出事件
 */
int push_output_event_with_length(display_ctx *ctx, const struct OutputEvent *event,
                                  void* payload, size_t size)
{
    if (ctx->fallback)
        return 0;

    struct data_msg hdr = {
        .type = DATA_MSG_OUTPUT_EVENT,
        .size = sizeof(struct OutputEvent)
    };

    size_t total = sizeof(struct data_msg) + sizeof(struct OutputEvent) + size;
    uint8_t *msg = (uint8_t *)malloc(total);
    if (!msg)
        return -1;

    memcpy(msg, &hdr, sizeof(hdr));
    memcpy(msg + sizeof(hdr), event, sizeof(*event));
    if (payload && size > 0) {
        memcpy(msg + sizeof(hdr) + sizeof(struct OutputEvent), payload, size);
    }

    if (send_all(ctx->data_fd, msg, total) < 0) {
        free(msg);
        enter_fallback(ctx);
        return -1;
    }

    free(msg);
    return 0;
}

/*
 * 设置 fallback 回调
 */
int set_fallback_callback(display_ctx *ctx, void (*on_fallback)(void *),
                          void *userdata)
{
    ctx->fallback_cb = on_fallback;
    ctx->fallback_userdata = userdata;
    return 0;
}

/*
 * 检查是否处于 fallback 状态
 */
bool is_fallback(display_ctx *ctx)
{
    return ctx->fallback;
}

/*
 * 尝试退出 fallback
 */
int try_exit_fallback(display_ctx *ctx)
{
    if (!ctx->fallback) {
        fprintf(stderr, "ANLAND: try_exit_fallback() already connected\n");
        return 0;
    }

    fprintf(stderr, "ANLAND: try_exit_fallback() start\n");

    if (pickup_fds(ctx) < 0) {
        fprintf(stderr, "ANLAND: try_exit_fallback() pickup_fds failed\n");
        release_consumer_resources(ctx);
        return -1;
    }

    if (receive_dmabufs(ctx) < 0) {
        fprintf(stderr, "ANLAND: try_exit_fallback() receive_dmabufs failed\n");
        release_consumer_resources(ctx);
        return -1;
    }

    if (ctx->buf_count <= 0) {
        fprintf(stderr, "ANLAND: try_exit_fallback() no dmabufs\n");
        release_consumer_resources(ctx);
        return -1;
    }

    ctx->fallback = false;
    fprintf(stderr, "ANLAND: try_exit_fallback() success\n");
    return 0;
}

/*
 * 获取数据通道 fd
 */
int get_data_fd(display_ctx *ctx)
{
    return ctx->data_fd;
}

/*
 * 获取音频通道 fd
 */
int get_audio_fd(display_ctx *ctx)
{
    return ctx->fallback ? -1 : ctx->audio_fd;
}

/*
 * 获取 buffer-ready fd
 */
int get_buffer_ready_fd(display_ctx *ctx)
{
    return ctx->buf_ready_efd;
}

/*
 * 获取缓冲区数量
 */
int get_buf_count(display_ctx *ctx)
{
    return ctx->buf_count;
}

/*
 * 获取当前选中的缓冲区索引
 */
int get_selected_idx(display_ctx *ctx)
{
    if (!ctx->shm_ptr)
        return 0;
    uint32_t idx = *ctx->shm_ptr;
    return (idx < (uint32_t)ctx->buf_count) ? (int)idx : 0;
}

/*
 * 获取指定索引的 dmabuf fd
 */
int get_dmabuf_fd_at(display_ctx *ctx, int idx)
{
    if (idx < 0 || idx >= ctx->buf_count)
        return -1;
    return ctx->dmabuf_fds[idx];
}

/*
 * 获取指定索引的 dmabuf 信息
 */
int get_dmabuf_info_at(display_ctx *ctx, int idx, struct buf_info *info)
{
    if (idx < 0 || idx >= ctx->buf_count)
        return -1;
    *info = ctx->dmabuf_infos[idx];
    return 0;
}