// src/backend/anland/display_producer.h
#ifndef ANLAND_DISPLAY_PRODUCER_H
#define ANLAND_DISPLAY_PRODUCER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct display_ctx display_ctx;

int connect_to_deamon(display_ctx **ctx, const char *socket_path);
void disconnect(display_ctx *ctx);

int get_screen_info(display_ctx *ctx, uint32_t *width, uint32_t *height,
                    uint32_t *format, uint32_t *refresh);

void set_render_fence(display_ctx *ctx, int fence_fd);
int trigger_refresh(display_ctx *ctx);

int poll_input_event(display_ctx *ctx, struct InputEvent *event, int timeout_ms);
int poll_input_event_extend_data(display_ctx *ctx, void* payload,
                                 size_t size, int timeout_ms);
int poll_input_event_extend_fds(display_ctx *ctx, int *fds, int max_fds,
                                int *fd_count, int timeout_ms);

int push_resources_request(display_ctx *ctx, uint32_t service_type,
                           const uint32_t *args);
int push_output_event(display_ctx *ctx, const struct OutputEvent *event);
int push_output_event_with_length(display_ctx *ctx, const struct OutputEvent *event,
                                  void* payload, size_t size);

int set_fallback_callback(display_ctx *ctx, void (*on_fallback)(void *),
                          void *userdata);

bool is_fallback(display_ctx *ctx);
int try_exit_fallback(display_ctx *ctx);

int get_data_fd(display_ctx *ctx);
int get_audio_fd(display_ctx *ctx);
int get_buffer_ready_fd(display_ctx *ctx);

int get_buf_count(display_ctx *ctx);
int get_selected_idx(display_ctx *ctx);
int get_dmabuf_fd_at(display_ctx *ctx, int idx);
int get_dmabuf_info_at(display_ctx *ctx, int idx, struct buf_info *info);

#ifdef __cplusplus
}
#endif

#endif /* ANLAND_DISPLAY_PRODUCER_H */