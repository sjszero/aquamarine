// src/backend/anland/AnlandOutput.cpp
#include "AnlandOutput.hpp"
#include "AnlandBackend.hpp"
#include "AnlandAllocator.hpp"
#include <drm_fourcc.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <chrono>

#define ANLAND_LOG(fmt, ...) fprintf(stderr, "[ANLAND] " fmt "\n", ##__VA_ARGS__)
#define ANLAND_ERR(fmt, ...) fprintf(stderr, "[ANLAND][ERR] " fmt "\n", ##__VA_ARGS__)

namespace Aquamarine {

using Hyprutils::Memory::CSharedPointer;
using Hyprutils::Memory::makeShared;
using Hyprutils::Math::CRegion;

CAnlandOutput::CAnlandOutput(CAnlandBackend* backend)
    : m_backend(backend) {
    this->name = "anland-1";
    this->description = "Anland virtual output";
    this->make = "Anland";
    this->model = "Anland Display";
    this->serial = "anland-1";
    this->subpixel = AQ_SUBPIXEL_UNKNOWN;
    this->enabled = false;
    this->state = makeShared<COutputState>();
    m_firstCommit = true;
    ANLAND_LOG("CAnlandOutput constructed");
}

CAnlandOutput::~CAnlandOutput() {
    if (m_shutdownDone.exchange(true)) return;
    m_destroying = true;
    releaseBuffers();
    m_swapchain.reset();
    ANLAND_LOG("CAnlandOutput destroyed");
}

display_ctx* CAnlandOutput::display() {
    if (m_destroying || !m_backend) return nullptr;
    return m_backend->display();
}

bool CAnlandOutput::initialize(uint32_t width, uint32_t height, uint32_t refresh) {
    if (m_destroying) return false;

    m_width = width;
    m_height = height;
    m_refresh = refresh > 0 ? refresh : 60000;

    this->physicalSize = Hyprutils::Math::Vector2D(
        static_cast<float>(width) / 96.0f,
        static_cast<float>(height) / 96.0f
    );

    auto mode = CSharedPointer<SOutputMode>(
        new SOutputMode{
            .pixelSize = Hyprutils::Math::Vector2D(
                static_cast<float>(width),
                static_cast<float>(height)
            ),
            .refreshRate = m_refresh,
            .preferred = true,
        });

    this->modes.clear();
    this->modes.push_back(mode);

    this->enabled = true;
    this->state->setEnabled(true);
    this->state->setMode(mode);
    this->state->setFormat(DRM_FORMAT_XRGB8888);

    m_outputReady = true;
    m_firstCommit = true;

    // KWin 风格：立即发送 frame 事件，让上层知道输出已准备好
    events.frame.emit();

    ANLAND_LOG("initialize: %dx%d @ %d mHz", width, height, refresh);
    return true;
}

void CAnlandOutput::updateRefreshRate(uint32_t refresh) {
    if (refresh == m_refresh || refresh == 0) return;
    m_refresh = refresh;
    if (!this->modes.empty()) {
        this->modes[0]->refreshRate = refresh;
    }
    ANLAND_LOG("updateRefreshRate: %d mHz", refresh);
}

void CAnlandOutput::releaseBuffers() {
    m_swapchain.reset();
    ANLAND_LOG("releaseBuffers: all buffers released");
}

void CAnlandOutput::reconfigureSwapchain() {
    if (m_destroying || m_inFallback || !m_outputReady) return;

    auto alloc = m_backend ? m_backend->getAllocator() : nullptr;
    if (!alloc) {
        ANLAND_ERR("reconfigureSwapchain: no allocator");
        return;
    }

    auto mode = state->state().mode.lock();
    if (!mode) mode = state->state().customMode;
    if (!mode) {
        ANLAND_ERR("reconfigureSwapchain: no mode");
        return;
    }

    if (!m_swapchain) {
        m_swapchain = CSwapchain::create(alloc, m_backend->self.lock());
        if (!m_swapchain) {
            ANLAND_ERR("reconfigureSwapchain: failed to create swapchain");
            return;
        }
    }

    SSwapchainOptions opts;
    opts.length = 3;
    opts.size = mode->pixelSize;
    opts.format = DRM_FORMAT_XRGB8888;
    opts.scanout = true;

    if (!m_swapchain->reconfigure(opts)) {
        ANLAND_ERR("reconfigureSwapchain: failed to reconfigure");
        return;
    }

    ANLAND_LOG("reconfigureSwapchain: %dx%d", (int)opts.size.x, (int)opts.size.y);
}

CSharedPointer<IBackendImplementation> CAnlandOutput::getBackend() {
    if (m_destroying || !m_backend) return nullptr;
    return m_backend->self.lock();
}

std::vector<SDRMFormat> CAnlandOutput::getRenderFormats() {
    return m_backend ? m_backend->getRenderFormats() : std::vector<SDRMFormat>{};
}

// KWin 风格: test() 永远返回 true
bool CAnlandOutput::test() {
    // 永远假装测试通过，让 Hyprland 继续初始化
    return true;
}

// KWin 风格: commit() 永远成功
bool CAnlandOutput::commit() {
    if (m_destroying) return true;
    if (m_commitInProgress.exchange(true)) {
        return false;
    }

    struct CommitGuard {
        std::atomic<bool>& flag;
        ~CommitGuard() { flag = false; }
    } guard(m_commitInProgress);

    // 第一次 commit: 立即完成，让 Hyprland 初始化
    if (m_firstCommit) {
        m_firstCommit = false;
        m_framePending = false;
        ANLAND_LOG("commit: first commit - immediate completion");

        events.present.emit(IOutput::SPresentEvent{
            .presented = true,
            .when = nullptr,
            .seq = 0,
            .refresh = (int)m_refresh,
            .flags = AQ_OUTPUT_PRESENT_VSYNC | AQ_OUTPUT_PRESENT_HW_CLOCK
        });
        events.commit.emit();
        return true;
    }

    // fallback 状态: 也返回成功，让 Hyprland 继续运行
    if (m_inFallback || !m_outputReady) {
        ANLAND_LOG("commit: fallback mode - immediate completion");
        m_framePending = false;
        events.present.emit(IOutput::SPresentEvent{
            .presented = true,
            .when = nullptr,
            .seq = 0,
            .refresh = (int)m_refresh,
            .flags = AQ_OUTPUT_PRESENT_VSYNC | AQ_OUTPUT_PRESENT_HW_CLOCK
        });
        events.commit.emit();
        return true;
    }

    auto* dpy = display();
    if (!dpy) {
        // 没有 display 也返回成功
        m_framePending = false;
        events.present.emit(IOutput::SPresentEvent{
            .presented = true,
            .when = nullptr,
            .seq = 0,
            .refresh = (int)m_refresh,
            .flags = AQ_OUTPUT_PRESENT_VSYNC
        });
        events.commit.emit();
        return true;
    }

    if (!m_swapchain) {
        reconfigureSwapchain();
        if (!m_swapchain) {
            // 没有 swapchain 也返回成功
            m_framePending = false;
            events.present.emit(IOutput::SPresentEvent{
                .presented = true,
                .when = nullptr,
                .seq = 0,
                .refresh = (int)m_refresh,
                .flags = AQ_OUTPUT_PRESENT_VSYNC
            });
            events.commit.emit();
            return true;
        }
    }

    // 尝试获取缓冲区
    int age = 0;
    auto buffer = m_swapchain->next(&age);
    if (!buffer) {
        // 没有 buffer 也返回成功
        ANLAND_LOG("commit: no buffer available - immediate completion");
        m_framePending = false;
        events.present.emit(IOutput::SPresentEvent{
            .presented = true,
            .when = nullptr,
            .seq = 0,
            .refresh = (int)m_refresh,
            .flags = AQ_OUTPUT_PRESENT_VSYNC
        });
        events.commit.emit();
        return true;
    }

    state->setBuffer(buffer);
    state->addDamage(CRegion(0, 0, (int)m_width, (int)m_height));

    // 发送刷新信号给 Android
    int ret = trigger_refresh(dpy);
    if (ret < 0) {
        ANLAND_ERR("commit: trigger_refresh failed");
        m_swapchain->rollback();
        // 即使失败也返回成功
        m_framePending = false;
        events.present.emit(IOutput::SPresentEvent{
            .presented = true,
            .when = nullptr,
            .seq = 0,
            .refresh = (int)m_refresh,
            .flags = AQ_OUTPUT_PRESENT_VSYNC
        });
        events.commit.emit();
        return true;
    }

    m_framePending = true;
    ANLAND_LOG("commit: frame pending - waiting for buffer_ready");
    events.commit.emit();
    return true;
}

void CAnlandOutput::scheduleFrame(const scheduleFrameReason reason) {
    // KWin 风格: scheduleFrame 只是设置标志，立即触发 frame 事件
    if (m_destroying) return;

    ANLAND_LOG("scheduleFrame: reason=%d", (int)reason);

    if (m_inFallback || !m_outputReady) {
        // 即使 fallback 也触发 frame，让 Hyprland 继续
        ANLAND_LOG("scheduleFrame: fallback mode - immediate frame");
        events.frame.emit();
        return;
    }

    if (m_frameScheduled) {
        return;
    }

    m_frameScheduled = true;
    m_needsFrame = true;

    // 立即触发 frame 事件
    events.frame.emit();

    // 同时使用空闲事件确保帧被处理
    if (!m_frameIdle) {
        m_frameIdle = Hyprutils::Memory::makeShared<std::function<void(void)>>([this]() {
            m_frameScheduled = false;
            if (m_destroying || m_inFallback || !m_outputReady) {
                return;
            }
            ANLAND_LOG("scheduleFrame idle: emitting frame");
            events.frame.emit();
        });
    }

    auto backend = m_backend ? m_backend->getBackend() : nullptr;
    if (backend) {
        backend->addIdleEvent(m_frameIdle);
    } else {
        m_frameScheduled = false;
    }
}

void CAnlandOutput::onBufferReady() {
    if (m_inFallback || m_destroying) return;

    ANLAND_LOG("onBufferReady: buffer consumed by Android");

    m_frameScheduled = false;

    auto* dpy = display();
    if (!dpy || is_fallback(dpy)) {
        // 即使 display 不可用，也发送 present 事件
        m_framePending = false;
        events.present.emit(IOutput::SPresentEvent{
            .presented = true,
            .when = nullptr,
            .seq = 0,
            .refresh = (int)m_refresh,
            .flags = AQ_OUTPUT_PRESENT_VSYNC | AQ_OUTPUT_PRESENT_HW_CLOCK
        });
        return;
    }

    int fd = get_buffer_ready_fd(dpy);
    if (fd >= 0) {
        uint64_t val;
        if (read(fd, &val, sizeof(val)) < 0 && errno != EAGAIN) {
            ANLAND_ERR("onBufferReady: read eventfd failed: %s", strerror(errno));
        }
    }

    m_framePending = false;

    timespec mono{};
    clock_gettime(CLOCK_MONOTONIC, &mono);
    events.present.emit(IOutput::SPresentEvent{
        .presented = true,
        .when = &mono,
        .seq = 0,
        .refresh = (int)m_refresh,
        .flags = AQ_OUTPUT_PRESENT_VSYNC | AQ_OUTPUT_PRESENT_HW_CLOCK
    });

    m_needsFrame = false;
    ANLAND_LOG("onBufferReady: done");
}

void CAnlandOutput::enterFallback() {
    if (m_inFallback || m_destroying) return;
    std::lock_guard<std::mutex> lock(m_bufferMutex);

    ANLAND_LOG("enterFallback: consumer disconnected");

    m_inFallback = true;
    m_framePending = false;
    m_outputReady = false;
    m_frameScheduled = false;
    this->enabled = false;
    this->state->setEnabled(false);

    // 重置 firstCommit，以便重新连接时重新初始化
    m_firstCommit = true;

    if (m_frameIdle) {
        auto backend = m_backend ? m_backend->getBackend() : nullptr;
        if (backend) {
            backend->removeIdleEvent(m_frameIdle);
        }
        m_frameIdle = nullptr;
    }

    // 发送 present 事件让 Hyprland 继续
    events.present.emit(IOutput::SPresentEvent{
        .presented = true,
        .when = nullptr,
        .seq = 0,
        .refresh = (int)m_refresh,
        .flags = AQ_OUTPUT_PRESENT_VSYNC
    });
}

void CAnlandOutput::exitFallback() {
    if (!m_inFallback || m_destroying) return;

    ANLAND_LOG("exitFallback: consumer reconnected");

    m_inFallback = false;
    this->enabled = true;
    this->state->setEnabled(true);

    m_outputReady = true;
    m_needsFrame = true;
    m_frameScheduled = false;

    // 重置 firstCommit，重新初始化
    m_firstCommit = true;

    reconfigureSwapchain();

    // 立即触发 frame 事件让 Hyprland 开始渲染
    scheduleFrame(AQ_SCHEDULE_NEW_CONNECTOR);
    events.frame.emit();
    ANLAND_LOG("exitFallback: done");
}

} // namespace Aquamarine