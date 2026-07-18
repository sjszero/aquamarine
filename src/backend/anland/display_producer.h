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

/*
 * display_ctx - 显示上下文（不透明类型）
 */
typedef struct display_ctx display_ctx;

/*
 * 连接到显示守护进程
 *
 * 建立与 display_daemon 的初始连接，获取屏幕信息。
 * 连接后上下文处于 fallback 状态（无消费者资源）。
 *
 * @param ctx           输出上下文指针
 * @param socket_path   Unix socket 路径
 * @return 0 成功, -1 失败
 */
int connect_to_deamon(display_ctx **ctx, const char *socket_path);

/*
 * 断开连接，释放所有资源
 */
void disconnect(display_ctx *ctx);

/*
 * 获取屏幕信息
 */
int get_screen_info(display_ctx *ctx, uint32_t *width, uint32_t *height,
                    uint32_t *format, uint32_t *refresh);

/*
 * 设置渲染栅栏
 *
 * 保存当前帧的渲染完成栅栏，在 trigger_refresh 时发送给消费者。
 * 由生产者（Hyprland）在渲染完成后调用。
 *
 * @param fence_fd  栅栏文件描述符（-1 表示无）
 */
void set_render_fence(display_ctx *ctx, int fence_fd);

/*
 * 触发刷新
 *
 * 发送帧完成信号给消费者，附带渲染栅栏（如果有）。
 * 消费者收到后会调度显示并回复 buffer-ready。
 *
 * @return 0 成功, -1 失败
 */
int trigger_refresh(display_ctx *ctx);

/*
 * 轮询输入事件
 *
 * 从数据通道读取一个输入事件。
 *
 * @param event        输出事件结构体
 * @param timeout_ms   超时（毫秒），0 立即返回
 * @return 1 有事件, 0 无事件, -1 错误/断开
 */
int poll_input_event(display_ctx *ctx, struct InputEvent *event, int timeout_ms);

/*
 * 接收扩展数据（用于剪贴板/文本输入等变长数据）
 *
 * @param payload      输出缓冲区
 * @param size         数据大小
 * @param timeout_ms   超时
 * @return 1 成功, 0 无数据, -1 错误
 */
int poll_input_event_extend_data(display_ctx *ctx, void* payload,
                                 size_t size, int timeout_ms);

/*
 * 接收扩展文件描述符（用于摄像头等资源）
 *
 * 在收到 INPUT_TYPE_RESOURCE 事件后调用，接收跟随的 fd。
 *
 * @param fds          输出 fd 数组
 * @param max_fds      数组容量
 * @param fd_count     实际接收的 fd 数量
 * @param timeout_ms   超时
 * @return 1 成功, 0 无数据, -1 错误
 */
int poll_input_event_extend_fds(display_ctx *ctx, int *fds, int max_fds,
                                int *fd_count, int timeout_ms);

/*
 * 请求资源（摄像头等）
 *
 * 发送 OUTPUT_TYPE_RESOURCES_REQUEST 事件给消费者。
 * 消费者异步回复 INPUT_TYPE_RESOURCE + fds。
 *
 * @param service_type  服务类型 (SERVICE_TYPE_*)
 * @param args          参数（可为 NULL）
 * @return 0 成功, -1 失败
 */
int push_resources_request(display_ctx *ctx, uint32_t service_type,
                           const uint32_t *args);

/*
 * 发送输出事件
 */
int push_output_event(display_ctx *ctx, const struct OutputEvent *event);

/*
 * 发送带扩展数据的输出事件
 */
int push_output_event_with_length(display_ctx *ctx, const struct OutputEvent *event,
                                  void* payload, size_t size);

/*
 * 设置 fallback 回调
 *
 * 当消费者断开连接时调用此回调。
 */
int set_fallback_callback(display_ctx *ctx, void (*on_fallback)(void *),
                          void *userdata);

/*
 * 检查是否处于 fallback 状态
 */
bool is_fallback(display_ctx *ctx);

/*
 * 尝试退出 fallback
 *
 * 拾取消费者资源（fds + dmabufs），成功则退出 fallback。
 *
 * @return 0 成功, -1 仍处于 fallback
 */
int try_exit_fallback(display_ctx *ctx);

/*
 * 获取数据通道 fd（用于 poll）
 */
int get_data_fd(display_ctx *ctx);

/*
 * 获取音频通道 fd
 */
int get_audio_fd(display_ctx *ctx);

/*
 * 获取 buffer-ready eventfd（用于 poll）
 */
int get_buffer_ready_fd(display_ctx *ctx);

/*
 * 获取 dmabuf 缓冲区数量
 */
int get_buf_count(display_ctx *ctx);

/*
 * 获取当前选中的缓冲区索引
 */
int get_selected_idx(display_ctx *ctx);

/*
 * 获取指定索引的 dmabuf fd
 */
int get_dmabuf_fd_at(display_ctx *ctx, int idx);

/*
 * 获取指定索引的 dmabuf 信息
 */
int get_dmabuf_info_at(display_ctx *ctx, int idx, struct buf_info *info);

#ifdef __cplusplus
}
#endif

#endif /* ANLAND_DISPLAY_PRODUCER_H */