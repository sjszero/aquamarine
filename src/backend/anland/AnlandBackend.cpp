// src/backend/anland/AnlandBackend.cpp
#include "AnlandBackend.hpp"
#include "AnlandOutput.hpp"
#include "AnlandAllocator.hpp"
#include "AnlandPointer.hpp"
#include "AnlandKeyboard.hpp"
#include "AnlandTouch.hpp"
#include <sys/timerfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdio>
#include <chrono>
#include <xf86drm.h>

#define ANLAND_LOG(fmt, ...) fprintf(stderr, "[ANLAND] " fmt "\n", ##__VA_ARGS__)
#define ANLAND_ERR(fmt, ...) fprintf(stderr, "[ANLAND][ERR] " fmt "\n", ##__VA_ARGS__)

namespace Aquamarine {

using Hyprutils::Memory::CSharedPointer;
using Hyprutils::Memory::makeShared;

static uint32_t getCurrentTimeMs() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

static void anland_fallback_callback(void* data) {
    auto* backend = static_cast<CAnlandBackend*>(data);
    if (backend) backend->onFallback();
}

int CAnlandBackend::openDummyDRM() {
    // 策略：尝试多个可能的 DRM 节点，找到第一个可用的
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
            // 验证是否真的是 DRM 设备
            drmVersion* ver = drmGetVersion(fd);
            if (ver) {
                drmFreeVersion(ver);
                ANLAND_LOG("Opened dummy DRM device: %s (fd=%d)", paths[i], fd);
                return fd;
            }
            close(fd);
        }
    }

    ANLAND_ERR("Failed to open any DRM device for EGL initialization");
    return -1;
}

CAnlandBackend::CAnlandBackend(CSharedPointer<CBackend> backend, const std::string& socketPath)
    : m_backend(backend), m_socketPath(socketPath) {
    ANLAND_LOG("CAnlandBackend constructed, socket: %s", socketPath.c_str());
    m_dummyDRMFD = openDummyDRM();
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
    m_allocator.reset();
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

    anland_audio_start();
    anland_camera_start();

    if (!tryConnect()) {
        ANLAND_LOG("no consumer yet, starting in fallback mode");
        setupReconnectTimer();
    } else {
        ANLAND_LOG("connected successfully");
    }

    createOutputIfNeeded();
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

    for (int attempt = 0; attempt < 100; attempt++) {
        if (m_destroying) return false;
        if (try_exit_fallback(m_display) == 0) {
            m_inFallback = false;
            ANLAND_LOG("consumer connected, exiting fallback (attempt %d)", attempt);

            // 创建分配器 - 从 Android 获取 dmabuf
            m_allocator = CAnlandAllocator::create(m_display, this);
            if (!m_allocator) {
                ANLAND_ERR("Failed to create allocator");
                enterFallback();
                return false;
            }

            updateAudioFd();
            updateCameraResources();

            if (m_output) {
                m_output->exitFallback();
                m_output->reconfigureSwapchain();
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
        m_output->reconfigureSwapchain();
        m_output->scheduleFrame(IOutput::AQ_SCHEDULE_NEW_MONITOR);
    }
    ANLAND_LOG("onReady done: fallback=%d", m_inFallback);
}

void CAnlandBackend::enterFallback() {
    if (m_inFallback || m_destroying) return;
    ANLAND_LOG("enterFallback: manually entering fallback");

    m_inFallback = true;
    m_outputEmitted = false;

    anland_audio_set_fd(-1);
    anland_camera_clear();

    m_allocator.reset();

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
        auto pfd = Hyprutils::Memory::makeShared<SPollFD>();
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
            auto pfd = Hyprutils::Memory::makeShared<SPollFD>();
            pfd->fd = dataFd;
            pfd->onSignal = [weakSelf]() {
                auto self = weakSelf.lock();
                if (!self || self->m_inFallback || !self->m_output || self->m_destroying) return;
                self->dispatchEvents();
            };
            result.push_back(pfd);
        }

        if (bufFd >= 0) {
            auto pfd = Hyprutils::Memory::makeShared<SPollFD>();
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

        case INPUT_TYPE_POINTER_MOTION: {
            if (!m_pointer) {
                m_pointer = CSharedPointer<CAnlandPointer>(new CAnlandPointer(this));
                m_backend->events.newPointer.emit(m_pointer);
            }
            m_pointer->emitMotion(getCurrentTimeMs(),
                Hyprutils::Math::Vector2D(ev.pointer_motion.dx, ev.pointer_motion.dy));
            m_pointer->emitFrame();
            break;
        }

        case INPUT_TYPE_POINTER_BUTTON: {
            if (!m_pointer) {
                m_pointer = CSharedPointer<CAnlandPointer>(new CAnlandPointer(this));
                m_backend->events.newPointer.emit(m_pointer);
            }
            m_pointer->emitButton(getCurrentTimeMs(), ev.pointer_button.button, ev.pointer_button.pressed != 0);
            m_pointer->emitFrame();
            break;
        }

        case INPUT_TYPE_POINTER_AXIS: {
            if (!m_pointer) {
                m_pointer = CSharedPointer<CAnlandPointer>(new CAnlandPointer(this));
                m_backend->events.newPointer.emit(m_pointer);
            }
            m_pointer->emitAxis(getCurrentTimeMs(), ev.pointer_axis.axis, ev.pointer_axis.value);
            m_pointer->emitFrame();
            break;
        }

        case INPUT_TYPE_KEY: {
            if (!m_keyboard) {
                m_keyboard = CSharedPointer<CAnlandKeyboard>(new CAnlandKeyboard(this));
                m_backend->events.newKeyboard.emit(m_keyboard);
            }
            m_keyboard->emitKey(getCurrentTimeMs(), ev.key.keycode, ev.key.action == INPUT_ACTION_DOWN);
            break;
        }

        case INPUT_TYPE_TOUCH: {
            if (!m_touch) {
                m_touch = CSharedPointer<CAnlandTouch>(new CAnlandTouch(this));
                m_backend->events.newTouch.emit(m_touch);
            }
            switch (ev.touch.action) {
                case INPUT_ACTION_DOWN:
                    m_touch->emitDown(getCurrentTimeMs(), ev.touch.pointer_id,
                        Hyprutils::Math::Vector2D(ev.touch.x, ev.touch.y));
                    break;
                case INPUT_ACTION_UP:
                    m_touch->emitUp(getCurrentTimeMs(), ev.touch.pointer_id);
                    break;
                case INPUT_ACTION_MOVE:
                    m_touch->emitMotion(getCurrentTimeMs(), ev.touch.pointer_id,
                        Hyprutils::Math::Vector2D(ev.touch.x, ev.touch.y));
                    break;
                default: break;
            }
            m_touch->emitFrame();
            break;
        }

        case INPUT_TYPE_TOUCH_FRAME: {
            if (m_touch) m_touch->emitFrame();
            break;
        }

        case INPUT_TYPE_DISPLAY_REFRESH: {
            if (m_output) {
                m_output->updateRefreshRate(ev.display.refresh_mhz);
            }
            break;
        }

        default:
            break;
    }
}

void CAnlandBackend::handleResourceEvent(const InputEvent& ev) {
    if (ev.resource.type == SERVICE_TYPE_CAMERA) {
        int fds[8];
        int fd_count = 0;
        if (poll_input_event_extend_fds(m_display, fds, 8, &fd_count, 5000) == 1 && fd_count > 0) {
            anland_camera_set_resources(fds[0], fd_count > 1 ? &fds[1] : nullptr, fd_count - 1);
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

std::vector<SDRMFormat> CAnlandBackend::getRenderFormats() {
    // 返回标准格式，让 Hyprland 以为我们支持这些格式
    // 实际上这些格式由 Android 端决定
    std::vector<SDRMFormat> formats;
    formats.push_back({.drmFormat = DRM_FORMAT_XRGB8888, .modifiers = {DRM_FORMAT_MOD_INVALID}});
    formats.push_back({.drmFormat = DRM_FORMAT_ARGB8888, .modifiers = {DRM_FORMAT_MOD_INVALID}});
    formats.push_back({.drmFormat = DRM_FORMAT_XBGR8888, .modifiers = {DRM_FORMAT_MOD_INVALID}});
    formats.push_back({.drmFormat = DRM_FORMAT_ABGR8888, .modifiers = {DRM_FORMAT_MOD_INVALID}});
    formats.push_back({.drmFormat = DRM_FORMAT_XRGB2101010, .modifiers = {DRM_FORMAT_MOD_INVALID}});
    formats.push_back({.drmFormat = DRM_FORMAT_ARGB2101010, .modifiers = {DRM_FORMAT_MOD_INVALID}});
    formats.push_back({.drmFormat = DRM_FORMAT_XBGR2101010, .modifiers = {DRM_FORMAT_MOD_INVALID}});
    formats.push_back({.drmFormat = DRM_FORMAT_ABGR2101010, .modifiers = {DRM_FORMAT_MOD_INVALID}});
    return formats;
}

void CAnlandBackend::shutdown() {
    if (m_shutdownDone.exchange(true)) return;
    m_destroying = true;
    teardownReconnectTimer();
    if (m_output) {
        m_output->releaseBuffers();
        m_output.reset();
    }
    m_allocator.reset();
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