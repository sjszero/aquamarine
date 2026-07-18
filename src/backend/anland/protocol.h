// src/backend/anland/protocol.h
#ifndef ANLAND_PROTOCOL_H
#define ANLAND_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 控制通道协议 (CTRL_MSG)
 * 用于初始握手、屏幕信息获取、资源拾取
 */
#define CTRL_MSG_CONSUMER_HELLO  1
#define CTRL_MSG_PRODUCER_HELLO  2
#define CTRL_MSG_SCREEN_INFO     7
#define CTRL_MSG_REJECT          8
#define CTRL_MSG_PICKUP_FDS      9
#define CTRL_MSG_FDS_READY      10

/*
 * 数据通道协议 (DATA_MSG)
 * 用于输入事件、输出事件、dmabuf 传输
 */
#define DATA_MSG_BUF_READY       100
#define DATA_MSG_REFRESH_DONE    101
#define DATA_MSG_INPUT_EVENT     102
#define DATA_MSG_OUTPUT_EVENT    103
#define DATA_MSG_INPUT_EXTEND_FDS 104
#define DATA_MSG_BUFS_READY      200

/* 最大 dmabuf 缓冲区数量（双缓冲/三缓冲） */
#define MAX_BUFS 8

/* 服务类型（用于资源请求） */
#define SERVICE_TYPE_CAMERA 1

/*
 * 控制消息头
 */
struct ctrl_msg {
    uint32_t type;
    uint32_t size;
    uint8_t  payload[];
} __attribute__((packed));

/*
 * 数据消息头
 */
struct data_msg {
    uint32_t type;
    uint32_t size;
    uint8_t  payload[];
} __attribute__((packed));

/*
 * 屏幕信息（由消费者在 PRODUCER_HELLO 后回复）
 */
struct screen_info {
    uint32_t width;      /* 屏幕宽度（像素） */
    uint32_t height;     /* 屏幕高度（像素） */
    uint32_t format;     /* 像素格式（1=RGBA_8888, 2=RGBX_8888） */
    uint32_t refresh;    /* 刷新率（mHz） */
} __attribute__((packed));

/*
 * dmabuf 缓冲区信息
 */
struct buf_info {
    uint32_t stride;     /* 行跨度（字节） */
    uint32_t width;      /* 缓冲区宽度 */
    uint32_t height;     /* 缓冲区高度 */
    uint32_t format;     /* 消费者端格式 */
    uint64_t modifier;   /* DRM modifier（0 = 无） */
    uint32_t offset;     /* 平面偏移 */
} __attribute__((packed));

/*
 * 输入事件类型
 */
#define INPUT_TYPE_TOUCH          1
#define INPUT_TYPE_KEY            2
#define INPUT_TYPE_POINTER_MOTION 3
#define INPUT_TYPE_POINTER_BUTTON 4
#define INPUT_TYPE_POINTER_AXIS   5
#define INPUT_TYPE_TOUCH_FRAME    6
#define INPUT_TYPE_DISPLAY_REFRESH 7
#define INPUT_TYPE_CLIPBOARD      8
#define INPUT_TYPE_TEXT_INPUT     9
#define INPUT_TYPE_ACTION         10
#define INPUT_TYPE_RESOURCE       11

/*
 * 输入动作
 */
#define INPUT_ACTION_DOWN    0
#define INPUT_ACTION_UP      1
#define INPUT_ACTION_MOVE    2

/*
 * 输入事件结构体
 */
struct InputEvent {
    uint32_t type;
    union {
        /* 触摸事件 */
        struct {
            int32_t  action;      /* INPUT_ACTION_* */
            float    x;           /* 归一化 X (0.0 - 1.0) */
            float    y;           /* 归一化 Y (0.0 - 1.0) */
            int32_t  pointer_id;  /* 触摸点 ID */
        } touch;

        /* 键盘事件 */
        struct {
            int32_t  action;      /* INPUT_ACTION_* */
            int32_t  keycode;     /* Linux keycode */
        } key;

        /* 指针移动 */
        struct {
            float    x;           /* 归一化 X (0.0 - 1.0) */
            float    y;           /* 归一化 Y (0.0 - 1.0) */
            float    dx;          /* 相对位移 X */
            float    dy;          /* 相对位移 Y */
        } pointer_motion;

        /* 指针按钮 */
        struct {
            uint32_t button;      /* 按钮编号 (1=左, 2=中, 3=右) */
            int32_t  pressed;     /* 0=释放, 1=按下 */
        } pointer_button;

        /* 指针滚轮 */
        struct {
            uint32_t axis;        /* 0=垂直, 1=水平 */
            float    value;       /* 滚动值 */
            int32_t  discrete;    /* 离散步进 (120 为一格) */
        } pointer_axis;

        /* 显示刷新率更新 */
        struct {
            uint32_t refresh_mhz; /* 刷新率 (mHz) */
        } display;

        /* 剪贴板数据（后跟实际数据） */
        struct {
            uint32_t size;        /* 数据大小 */
        } clipboard;

        /* 文本输入（后跟实际数据） */
        struct {
            uint32_t size;        /* 数据大小 */
        } text_input;

        /* 输入动作 */
        struct {
            uint32_t action;
            int32_t  value;
        } input_action;

        /* 资源（摄像头等） */
        struct {
            uint32_t type;        /* SERVICE_TYPE_* */
            uint32_t fdnum;       /* 跟随的 fd 数量 */
        } resource;

        /* 填充 */
        struct {
            uint32_t padding[4];
        } _pad;
    };
} __attribute__((packed));

/*
 * 输出事件类型
 */
#define OUTPUT_TYPE_CLIPBOARD         1
#define OUTPUT_TYPE_RESOURCES_REQUEST 2

/*
 * 输出事件结构体
 */
struct OutputEvent {
    uint32_t type;
    union {
        /* 剪贴板数据（后跟实际数据） */
        struct {
            uint32_t size;
        } clipboard;

        /* 资源请求（摄像头等） */
        struct {
            uint32_t type;         /* SERVICE_TYPE_* */
            uint32_t args[3];      /* 参数 */
        } resources_request;

        struct {
            uint32_t padding[4];
        } _pad;
    };
} __attribute__((packed));

/*
 * 音频协议
 */
#define AUDIO_MSG_FORMAT 1
#define AUDIO_MSG_PCM    2
#define AUDIO_MSG_SHM    3
#define AUDIO_MSG_SHM_FD 4

#define AUDIO_FORMAT_S16LE 0
#define AUDIO_ROLE_PLAYBACK 0
#define AUDIO_ROLE_CAPTURE  1

struct audio_format {
    uint32_t rate;       /* 采样率 (Hz) */
    uint32_t channels;   /* 通道数 (1=单声道, 2=立体声) */
    uint32_t format;     /* AUDIO_FORMAT_* */
    uint32_t role;       /* AUDIO_ROLE_* */
    uint32_t quantum;    /* 缓冲区大小 (帧) */
} __attribute__((packed));

struct audio_msg {
    uint32_t type;       /* AUDIO_MSG_* */
    uint32_t size;       /* 后续数据大小 */
} __attribute__((packed));

#ifdef __cplusplus
}
#endif

#endif /* ANLAND_PROTOCOL_H */