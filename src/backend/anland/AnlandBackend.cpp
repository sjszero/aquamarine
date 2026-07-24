// src/backend/anland/AnlandBackend.cpp
#include "AnlandBackend.hpp"
#include "AnlandOutput.hpp"
#include "AnlandPointer.hpp"
#include "AnlandKeyboard.hpp"
#include "AnlandTouch.hpp"
#include <sys/timerfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <xf86drm.h>

#define ANLAND_LOG(fmt, ...) do { fprintf(stderr, "[ANLAND] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while(0)
#define ANLAND_ERR(fmt, ...) do { fprintf(stderr, "[ANLAND][ERR] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while(0)
#define ANLAND_TRACE(fmt, ...) do { fprintf(stderr, "[ANLAND][TRACE] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while(0)

namespace Aquamarine {

using Hyprutils::Memory::CSharedPointer;
using Hyprutils::Memory::makeShared;
using Hyprutils::OS::CFileDescriptor;

static uint32_t getCurrentTimeMs() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

static void anland_fallback_callback(void* data) {
    auto* backend = static_cast<CAnlandBackend*>(data);
    if (backend) backend->onFallback();
}

int CAnlandBackend::openDummyDRM() {
    const char* paths[] = {
        "/dev/dri/renderD128",
        "/dev/dri/renderD129",
        "/dev/dri/card0",
        "/dev/dri/card1",
        nullptr
    };

    for (int i = 0; paths[i] != nullptr; i++) {
        int fd = open(paths[i], O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            drmVersion* ver = drmGetVersion(fd);
            if (ver) {
                drmFreeVersion(ver);
                return fd;
            }
            close(fd);
        }
    }

    return -1;
}

CAnlandBackend::CAnlandBackend(CSharedPointer<CBackend> backend, const std::string& socketPath)
    : m_backend(backend), m_socketPath(socketPath) {
    ANLAND_LOG("CAnlandBackend constructed, socket: %s", socketPath.c_str());
    m_dummyDRMFD = openDummyDRM();
    
    // 初始化触摸点
    for (auto& tp : m_touchPoints) {
        tp.active = false;
        tp.id = -1;
    }
}

CAnlandBackend::~CAnlandBackend() {
    if (m_shutdownDone.exchange(true)) return;
    m_destroying = true;
    teardownReconnectTimer();
    anland_audio_stop();
    anland_camera_stop();
    if (m_output) {
        m_output->releaseBuffers();
        m_output.reset();
    }
    if (m_display) {
        disconnect(m_display);
        m_display = nullptr;
    }
    if (m_dummyDRMFD >= 0) {
        close(m_dummyDRMFD);
        m_dummyDRMFD = -1;
    }
    ANLAND_LOG("CAnlandBackend destroyed");
}

bool CAnlandBackend::start() {
    if (m_running) return true;
    ANLAND_LOG("Anland backend starting, socket: %s", m_socketPath.c_str());

    m_running = true;
    m_inFallback = true;

    // Initialize audio and camera engines (nodes persist across connections)
    anland_audio_start();
    anland_camera_start();

    createOutputIfNeeded();
    emitOutputIfReady();

    if (!tryConnect()) {
        ANLAND_LOG("no consumer yet, starting in fallback mode");
        setupReconnectTimer();
    } else {
        ANLAND_LOG("connected successfully");
    }

    return true;
}

bool CAnlandBackend::tryConnect() {
    if (m_destroying) return false;
    std::lock_guard<std::mutex> lock(m_connectMutex);

    if (!m_display) {
        display_ctx* display = nullptr;
        if (connect_to_deamon(&display, m_socketPath.c_str()) < 0 || !display) {
            ANLAND_LOG("connect_to_daemon failed");
            return false;
        }
        m_display = display;
        set_fallback_callback(m_display, anland_fallback_callback, this);

        uint32_t width, height, format, refresh;
        get_screen_info(m_display, &width, &height, &format, &refresh);
        m_screenWidth = width;
        m_screenHeight = height;
        m_screenFormat = format;
        m_screenRefresh = refresh;
        m_hasDisplayInfo = true;
        ANLAND_LOG("connected to daemon: %dx%d @ %d mHz", width, height, refresh);
    }

    createOutputIfNeeded();

    for (int attempt = 0; attempt < 100; attempt++) {
        if (m_destroying) return false;
        if (try_exit_fallback(m_display) == 0) {
            m_inFallback = false;
            ANLAND_LOG("consumer connected, exiting fallback (attempt %d)", attempt);

            updateAudioFd();
            updateCameraResources();

            if (m_output) {
                m_output->exitFallback();
                m_output->events.frame.emit();
                m_output->scheduleFrame(IOutput::AQ_SCHEDULE_NEW_CONNECTOR);
            }

            m_backend->events.pollFDsChanged.emit();
            return true;
        }
        struct timespec ts = {0, 100000000};
        nanosleep(&ts, nullptr);
    }
    return false;
}

void CAnlandBackend::onReady() {
    ANLAND_LOG("onReady() called");
    createOutputIfNeeded();
    emitOutputIfReady();

    if (m_output && !m_inFallback) {
        m_output->exitFallback();
        m_output->scheduleFrame(IOutput::AQ_SCHEDULE_NEW_MONITOR);
        m_output->events.frame.emit();
    }
}

void CAnlandBackend::enterFallback() {
    if (m_inFallback || m_destroying) return;
    ANLAND_LOG("enterFallback: manually entering fallback");

    m_inFallback = true;
    m_outputEmitted = false;

    anland_audio_set_fd(-1);
    anland_camera_clear();

    if (m_output) {
        m_output->enterFallback();
        m_output->releaseBuffers();
    }

    setupReconnectTimer();
    m_backend->events.pollFDsChanged.emit();
}

void CAnlandBackend::onFallback() {
    if (m_destroying || m_inFallback) return;
    ANLAND_LOG("entered fallback (consumer disconnected)");
    enterFallback();
}

std::vector<CSharedPointer<SPollFD>> CAnlandBackend::pollFDs() {
    std::vector<CSharedPointer<SPollFD>> result;
    if (!m_running || m_destroying) return result;

    auto weakSelf = CWeakPointer<CAnlandBackend>(this->self.lock());

    if (m_reconnectTimerFd < 0 && m_inFallback) setupReconnectTimer();
    if (m_reconnectTimerFd >= 0) {
        auto pfd = makeShared<SPollFD>();
        pfd->fd = m_reconnectTimerFd;
        pfd->onSignal = [weakSelf]() {
            auto self = weakSelf.lock();
            if (self && !self->m_destroying) self->onReconnectTimerFd();
        };
        result.push_back(pfd);
    }

    if (m_display && !m_inFallback) {
        int dataFd = get_data_fd(m_display);
        int bufFd = get_buffer_ready_fd(m_display);

        if (dataFd >= 0) {
            auto pfd = makeShared<SPollFD>();
            pfd->fd = dataFd;
            pfd->onSignal = [weakSelf]() {
                auto self = weakSelf.lock();
                if (!self || self->m_inFallback || !self->m_output || self->m_destroying) return;
                self->dispatchEvents();
            };
            result.push_back(pfd);
        }

        if (bufFd >= 0) {
            auto pfd = makeShared<SPollFD>();
            pfd->fd = bufFd;
            pfd->onSignal = [weakSelf]() {
                auto self = weakSelf.lock();
                if (!self || self->m_inFallback || !self->m_output || self->m_destroying) return;
                self->m_output->onBufferReady();
            };
            result.push_back(pfd);
        }
    }

    return result;
}

void CAnlandBackend::onReconnectTimerFd() {
    if (m_reconnectTimerFd < 0 || m_destroying) return;
    uint64_t expirations;
    ssize_t ret = read(m_reconnectTimerFd, &expirations, sizeof(expirations));
    (void)ret;

    if (m_inFallback && tryConnect()) {
        m_backend->events.pollFDsChanged.emit();
        teardownReconnectTimer();
    }
}

void CAnlandBackend::setupReconnectTimer() {
    if (m_reconnectPending || m_destroying) return;
    m_reconnectPending = true;
    if (m_reconnectTimerFd >= 0) {
        close(m_reconnectTimerFd);
        m_reconnectTimerFd = -1;
    }

    m_reconnectTimerFd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (m_reconnectTimerFd < 0) {
        m_reconnectPending = false;
        return;
    }

    struct itimerspec ts = {
        .it_interval = { .tv_sec = 0, .tv_nsec = 100000000 },
        .it_value = { .tv_sec = 0, .tv_nsec = 100000000 }
    };
    timerfd_settime(m_reconnectTimerFd, 0, &ts, nullptr);
}

void CAnlandBackend::teardownReconnectTimer() {
    m_reconnectPending = false;
    if (m_reconnectTimerFd >= 0) {
        close(m_reconnectTimerFd);
        m_reconnectTimerFd = -1;
    }
}

void CAnlandBackend::createOutputIfNeeded() {
    if (m_outputCreated || m_destroying) return;

    uint32_t width = m_hasDisplayInfo ? m_screenWidth : 1920;
    uint32_t height = m_hasDisplayInfo ? m_screenHeight : 1080;
    uint32_t refresh = m_hasDisplayInfo ? m_screenRefresh : 60000;

    ANLAND_LOG("creating output with %dx%d @ %d mHz", width, height, refresh);

    m_output = CSharedPointer<CAnlandOutput>(new CAnlandOutput(this));
    if (m_output->initialize(width, height, refresh)) {
        m_outputCreated = true;
        ANLAND_LOG("output created successfully");
    } else {
        m_output.reset();
        ANLAND_ERR("output creation failed");
    }
}

void CAnlandBackend::emitOutputIfReady() {
    if (m_output && !m_outputEmitted && !m_inFallback) {
        m_backend->events.newOutput.emit(m_output);
        m_outputEmitted = true;
        ANLAND_LOG("output emitted to Aquamarine");
    }
}

bool CAnlandBackend::createOutput(const std::string& name) {
    (void)name;
    createOutputIfNeeded();
    return m_output != nullptr;
}

bool CAnlandBackend::dispatchEvents() {
    if (!m_running || !m_display || m_inFallback || m_destroying) return true;

    InputEvent ev;
    while (poll_input_event(m_display, &ev, 0) > 0) {
        handleInputEvent(ev);
    }
    return true;
}

void CAnlandBackend::handleInputEvent(const InputEvent& ev) {
    switch (ev.type) {
        case INPUT_TYPE_RESOURCE:
            handleResourceEvent(ev);
            break;

        case INPUT_TYPE_CLIPBOARD:
            updateClipboard(ev);
            break;

        case INPUT_TYPE_TEXT_INPUT:
            updateTextInput(ev);
            break;

        case INPUT_TYPE_POINTER_MOTION:
            processPointerMotion(ev);
            break;

        case INPUT_TYPE_POINTER_BUTTON:
            processPointerButton(ev);
            break;

        case INPUT_TYPE_POINTER_AXIS:
            processPointerAxis(ev);
            break;

        case INPUT_TYPE_KEY:
            processKey(ev);
            break;

        case INPUT_TYPE_TOUCH:
            processTouch(ev);
            break;

        case INPUT_TYPE_TOUCH_FRAME:
            if (m_touch) m_touch->emitFrame();
            break;

        case INPUT_TYPE_DISPLAY_REFRESH:
            processDisplayRefresh(ev);
            break;

        default:
            ANLAND_TRACE("unhandled input event type %d", ev.type);
            break;
    }
}

void CAnlandBackend::processPointerMotion(const InputEvent& ev) {
    if (!m_pointer) {
        m_pointer = CSharedPointer<CAnlandPointer>(new CAnlandPointer(this));
        m_backend->events.newPointer.emit(m_pointer);
    }
    
    uint32_t timeMs = getCurrentTimeMs();
    
    // Android 发送绝对坐标 (x, y) 和相对增量 (dx, dy)
    // 对绝对坐标，映射到屏幕空间
    if (ev.pointer_motion.x >= 0 && ev.pointer_motion.y >= 0) {
        // 绝对坐标: 转换为逻辑坐标 (0-1 范围)
        float nx = ev.pointer_motion.x / (float)m_screenWidth;
        float ny = ev.pointer_motion.y / (float)m_screenHeight;
        m_pointer->emitWarp(timeMs, Vector2D(nx, ny));
    }
    
    // 相对增量
    if (ev.pointer_motion.dx != 0 || ev.pointer_motion.dy != 0) {
        m_pointer->emitMotion(timeMs, Vector2D(ev.pointer_motion.dx, ev.pointer_motion.dy));
    }
    
    m_pointer->emitFrame();
}

void CAnlandBackend::processPointerButton(const InputEvent& ev) {
    if (!m_pointer) {
        m_pointer = CSharedPointer<CAnlandPointer>(new CAnlandPointer(this));
        m_backend->events.newPointer.emit(m_pointer);
    }
    m_pointer->emitButton(getCurrentTimeMs(), ev.pointer_button.button, ev.pointer_button.pressed != 0);
    m_pointer->emitFrame();
}

void CAnlandBackend::processPointerAxis(const InputEvent& ev) {
    if (!m_pointer) {
        m_pointer = CSharedPointer<CAnlandPointer>(new CAnlandPointer(this));
        m_backend->events.newPointer.emit(m_pointer);
    }
    m_pointer->emitAxis(getCurrentTimeMs(), ev.pointer_axis.axis, ev.pointer_axis.value);
    m_pointer->emitFrame();
}

void CAnlandBackend::processKey(const InputEvent& ev) {
    if (!m_keyboard) {
        m_keyboard = CSharedPointer<CAnlandKeyboard>(new CAnlandKeyboard(this));
        m_backend->events.newKeyboard.emit(m_keyboard);
    }
    // Android keycode 是 evdev 码，需要 +8 转为 XKB
    uint32_t xkbCode = ev.key.keycode + 8;
    m_keyboard->emitKey(getCurrentTimeMs(), xkbCode, ev.key.action == INPUT_ACTION_DOWN);
}

void CAnlandBackend::processTouch(const InputEvent& ev) {
    if (!m_touch) {
        m_touch = CSharedPointer<CAnlandTouch>(new CAnlandTouch(this));
        m_backend->events.newTouch.emit(m_touch);
    }
    
    uint32_t timeMs = getCurrentTimeMs();
    float nx = ev.touch.x / (float)m_screenWidth;
    float ny = ev.touch.y / (float)m_screenHeight;
    
    // 限制范围
    nx = std::clamp(nx, 0.0f, 1.0f);
    ny = std::clamp(ny, 0.0f, 1.0f);
    
    // 更新触摸点状态
    {
        std::lock_guard<std::mutex> lock(m_touchMutex);
        for (auto& tp : m_touchPoints) {
            if (tp.id == ev.touch.pointer_id) {
                tp.pos = Vector2D(nx, ny);
                tp.active = (ev.touch.action != INPUT_ACTION_UP);
                break;
            }
        }
        // 如果是 DOWN，找到空槽位
        if (ev.touch.action == INPUT_ACTION_DOWN) {
            for (auto& tp : m_touchPoints) {
                if (!tp.active) {
                    tp.id = ev.touch.pointer_id;
                    tp.pos = Vector2D(nx, ny);
                    tp.active = true;
                    break;
                }
            }
        }
    }
    
    switch (ev.touch.action) {
        case INPUT_ACTION_DOWN:
            m_touch->emitDown(timeMs, ev.touch.pointer_id, Vector2D(nx, ny));
            break;
        case INPUT_ACTION_UP:
            m_touch->emitUp(timeMs, ev.touch.pointer_id);
            break;
        case INPUT_ACTION_MOVE:
            m_touch->emitMotion(timeMs, ev.touch.pointer_id, Vector2D(nx, ny));
            break;
        default:
            break;
    }
}

void CAnlandBackend::processDisplayRefresh(const InputEvent& ev) {
    if (m_output) {
        m_output->updateRefreshRate(ev.display.refresh_mhz);
        ANLAND_LOG("display refresh updated to %d mHz", ev.display.refresh_mhz);
    }
}

void CAnlandBackend::handleResourceEvent(const InputEvent& ev) {
    if (ev.resource.type == SERVICE_TYPE_CAMERA) {
        int fds[8];
        int fd_count = 0;
        if (poll_input_event_extend_fds(m_display, fds, 8, &fd_count, 5000) == 1 && fd_count > 0) {
            anland_camera_set_resources(fds[0], fd_count > 1 ? &fds[1] : nullptr, fd_count - 1);
            // 注意：anland_camera_set_resources 会 dup fds，所以这里可以关闭
            for (int i = 0; i < fd_count; i++) close(fds[i]);
            ANLAND_LOG("Camera resources updated: %d streams", fd_count - 1);
        }
    }
}

void CAnlandBackend::updateAudioFd() {
    if (!m_display || m_inFallback) return;
    int audioFd = get_audio_fd(m_display);
    if (audioFd >= 0) {
        anland_audio_set_fd(audioFd);
        ANLAND_LOG("Audio fd set: %d", audioFd);
    }
}

void CAnlandBackend::updateCameraResources() {
    if (!m_display || m_inFallback) return;
    push_resources_request(m_display, SERVICE_TYPE_CAMERA, nullptr);
    ANLAND_LOG("Camera resources requested");
}

void CAnlandBackend::updateClipboard(const InputEvent& ev) {
    if (m_inFallback || !m_display) return;

    const uint32_t size = ev.clipboard.size;
    if (size == 0) return;

    std::vector<uint8_t> data(size);
    if (poll_input_event_extend_data(m_display, data.data(), size, 5000) != 1) {
        ANLAND_ERR("updateClipboard: failed to read clipboard data");
        return;
    }

    std::string text(reinterpret_cast<char*>(data.data()), size);

    // 去重
    if (text == m_lastClipboardText) {
        ANLAND_TRACE("updateClipboard: text unchanged, skipping");
        return;
    }
    m_lastClipboardText = text;

    ANLAND_LOG("updateClipboard: received %zu bytes", text.size());

    std::lock_guard<std::mutex> lock(m_callbackMutex);
    if (m_clipboardCallback) {
        m_clipboardCallback(text);
        ANLAND_LOG("updateClipboard: callback invoked");
    } else {
        ANLAND_LOG("updateClipboard: no callback registered, text dropped");
    }
}

void CAnlandBackend::updateTextInput(const InputEvent& ev) {
    if (m_inFallback || !m_display) return;

    const uint32_t size = ev.text_input.size;
    if (size == 0) return;

    std::vector<uint8_t> data(size);
    if (poll_input_event_extend_data(m_display, data.data(), size, 5000) != 1) {
        ANLAND_ERR("updateTextInput: failed to read text input data");
        return;
    }

    std::string text(reinterpret_cast<char*>(data.data()), size);
    ANLAND_LOG("updateTextInput: received text: %s", text.c_str());

    // 延迟重绘：让客户端有机会更新 surface
    deferFrameForIME();

    std::lock_guard<std::mutex> lock(m_callbackMutex);
    if (m_textInputCallback) {
        m_textInputCallback(text);
        ANLAND_LOG("updateTextInput: callback invoked");
    } else {
        ANLAND_LOG("updateTextInput: no callback registered, text dropped");
    }
}

void CAnlandBackend::deferFrameForIME() {
    m_imeDeferred = true;
    m_imeDeferDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
    
    if (m_imeDeferCallback) return;
    
    m_imeDeferCallback = makeShared<std::function<void()>>([this]() {
        m_imeDeferred = false;
        m_imeDeferCallback = nullptr;
        if (m_output && !m_inFallback) {
            m_output->scheduleFrame(IOutput::AQ_SCHEDULE_NEEDS_FRAME);
        }
    });
    
    // 使用 backend 的 idle 队列延迟执行
    auto backend = m_backend.lock();
    if (backend) {
        backend->addIdleEvent(m_imeDeferCallback);
    } else {
        m_imeDeferred = false;
        m_imeDeferCallback = nullptr;
    }
}

std::vector<SDRMFormat> CAnlandBackend::getRenderFormats() {
    std::vector<SDRMFormat> formats;

    // 优先 ABGR8888，带 LINEAR 修饰符
    formats.push_back({.drmFormat = DRM_FORMAT_ABGR8888, .modifiers = {DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID}});
    formats.push_back({.drmFormat = DRM_FORMAT_XBGR8888, .modifiers = {DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID}});
    formats.push_back({.drmFormat = DRM_FORMAT_ARGB8888, .modifiers = {DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID}});
    formats.push_back({.drmFormat = DRM_FORMAT_XRGB8888, .modifiers = {DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID}});
    
    // 如果消费者实际导入了其他格式，添加它们
    if (m_output && m_output->getBufferCount() > 0) {
        auto buf = m_output->getBuffer(0);
        if (buf && buf->good()) {
            auto attrs = buf->dmabuf();
            if (attrs.success) {
                bool exists = false;
                for (const auto& f : formats) {
                    if (f.drmFormat == attrs.format) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    formats.push_back({.drmFormat = attrs.format, .modifiers = {attrs.modifier, DRM_FORMAT_MOD_INVALID}});
                }
            }
        }
    }

    return formats;
}

uint32_t CAnlandBackend::getCurrentTimeMs() {
    return ::getCurrentTimeMs();
}

void CAnlandBackend::shutdown() {
    if (m_shutdownDone.exchange(true)) return;
    m_destroying = true;
    teardownReconnectTimer();
    if (m_output) {
        m_output->releaseBuffers();
        m_output.reset();
    }
    if (m_display) {
        disconnect(m_display);
        m_display = nullptr;
    }
    if (m_dummyDRMFD >= 0) {
        close(m_dummyDRMFD);
        m_dummyDRMFD = -1;
    }
    anland_audio_stop();
    anland_camera_stop();
    ANLAND_LOG("shutdown completed");
}

} // namespace Aquamarine