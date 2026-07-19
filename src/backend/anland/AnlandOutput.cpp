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
#define ANLAND_TRACE(fmt, ...) fprintf(stderr, "[ANLAND][TRACE] " fmt "\n", ##__VA_ARGS__)

namespace Aquamarine {

using Hyprutils::Memory::CSharedPointer;
using Hyprutils::Memory::makeShared;
using Hyprutils::Math::CRegion;

CAnlandOutput::CAnlandOutput(CAnlandBackend* backend)
    : m_backend(backend) {
    ANLAND_TRACE("CAnlandOutput constructor START");
    this->name = "anland-1";
    this->description = "Anland virtual output";
    this->make = "Anland";
    this->model = "Anland Display";
    this->serial = "anland-1";
    this->subpixel = AQ_SUBPIXEL_UNKNOWN;
    this->enabled = false;
    this->state = makeShared<COutputState>();
    m_firstCommit = true;
    ANLAND_TRACE("CAnlandOutput constructor END");
}

CAnlandOutput::~CAnlandOutput() {
    ANLAND_TRACE("CAnlandOutput destructor START");
    if (m_shutdownDone.exchange(true)) return;
    m_destroying = true;
    releaseBuffers();
    m_swapchain.reset();
    ANLAND_TRACE("CAnlandOutput destructor END");
}

display_ctx* CAnlandOutput::display() {
    if (m_destroying || !m_backend) return nullptr;
    return m_backend->display();
}

bool CAnlandOutput::initialize(uint32_t width, uint32_t height, uint32_t refresh) {
    ANLAND_TRACE("CAnlandOutput::initialize START: %dx%d @ %d mHz", width, height, refresh);
    if (m_destroying) {
        ANLAND_LOG("initialize: destroying, returning false");
        return false;
    }

    m_width = width;
    m_height = height;
    m_refresh = refresh > 0 ? refresh : 60000;

    this->physicalSize = Hyprutils::Math::Vector2D(
        static_cast<float>(width) / 96.0f,
        static_cast<float>(height) / 96.0f
    );

    ANLAND_TRACE("initialize: creating mode");
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

    ANLAND_TRACE("initialize: setting state");
    this->enabled = true;
    this->state->setEnabled(true);
    this->state->setMode(mode);
    this->state->setFormat(DRM_FORMAT_XRGB8888);

    m_outputReady = true;
    m_firstCommit = true;

    ANLAND_TRACE("initialize: emitting frame event");
    events.frame.emit();
    ANLAND_TRACE("initialize: frame event emitted");

    ANLAND_LOG("initialize: %dx%d @ %d mHz, output ready", width, height, refresh);
    return true;
}

void CAnlandOutput::updateRefreshRate(uint32_t refresh) {
    ANLAND_TRACE("updateRefreshRate: %d -> %d", m_refresh, refresh);
    if (refresh == m_refresh || refresh == 0) return;
    m_refresh = refresh;
    if (!this->modes.empty()) {
        this->modes[0]->refreshRate = refresh;
    }
    ANLAND_LOG("updateRefreshRate: %d mHz", refresh);
}

void CAnlandOutput::releaseBuffers() {
    ANLAND_TRACE("releaseBuffers START");
    m_swapchain.reset();
    ANLAND_TRACE("releaseBuffers END");
}

void CAnlandOutput::reconfigureSwapchain() {
    ANLAND_TRACE("reconfigureSwapchain START");
    if (m_destroying) {
        ANLAND_TRACE("reconfigureSwapchain: destroying, returning");
        return;
    }
    if (m_inFallback) {
        ANLAND_TRACE("reconfigureSwapchain: in fallback, returning");
        return;
    }
    if (!m_outputReady) {
        ANLAND_TRACE("reconfigureSwapchain: output not ready, returning");
        return;
    }

    ANLAND_TRACE("reconfigureSwapchain: getting allocator");
    auto alloc = m_backend ? m_backend->getAllocator() : nullptr;
    if (!alloc) {
        ANLAND_ERR("reconfigureSwapchain: no allocator");
        return;
    }

    ANLAND_TRACE("reconfigureSwapchain: getting mode");
    auto mode = state->state().mode.lock();
    if (!mode) mode = state->state().customMode;
    if (!mode) {
        ANLAND_ERR("reconfigureSwapchain: no mode");
        return;
    }

    ANLAND_TRACE("reconfigureSwapchain: mode: %dx%d", (int)mode->pixelSize.x, (int)mode->pixelSize.y);

    if (!m_swapchain) {
        ANLAND_TRACE("reconfigureSwapchain: creating swapchain");
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

    ANLAND_TRACE("reconfigureSwapchain: reconfiguring swapchain");
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

bool CAnlandOutput::test() {
    ANLAND_TRACE("CAnlandOutput::test() called - returning true");
    return true;
}

bool CAnlandOutput::commit() {
    ANLAND_TRACE("CAnlandOutput::commit() START");
    if (m_destroying) {
        ANLAND_TRACE("commit: destroying, returning true");
        return true;
    }
    if (m_commitInProgress.exchange(true)) {
        ANLAND_TRACE("commit: already in progress, returning false");
        return false;
    }

    struct CommitGuard {
        std::atomic<bool>& flag;
        ~CommitGuard() { 
            ANLAND_TRACE("CommitGuard destructor: releasing flag");
            flag = false; 
        }
    } guard(m_commitInProgress);

    ANLAND_TRACE("commit: firstCommit=%d, inFallback=%d, outputReady=%d", 
                 m_firstCommit, m_inFallback, m_outputReady);

    if (m_firstCommit) {
        ANLAND_LOG("commit: FIRST COMMIT - immediate completion");
        m_firstCommit = false;
        m_framePending = false;

        ANLAND_TRACE("commit: emitting present event (first commit)");
        events.present.emit(IOutput::SPresentEvent{
            .presented = true,
            .when = nullptr,
            .seq = 0,
            .refresh = (int)m_refresh,
            .flags = AQ_OUTPUT_PRESENT_VSYNC | AQ_OUTPUT_PRESENT_HW_CLOCK
        });
        ANLAND_TRACE("commit: emitting commit event (first commit)");
        events.commit.emit();
        ANLAND_TRACE("commit: first commit DONE");
        return true;
    }

    if (m_inFallback || !m_outputReady) {
        ANLAND_LOG("commit: FALLBACK mode - immediate completion");
        m_framePending = false;
        ANLAND_TRACE("commit: emitting present event (fallback)");
        events.present.emit(IOutput::SPresentEvent{
            .presented = true,
            .when = nullptr,
            .seq = 0,
            .refresh = (int)m_refresh,
            .flags = AQ_OUTPUT_PRESENT_VSYNC | AQ_OUTPUT_PRESENT_HW_CLOCK
        });
        ANLAND_TRACE("commit: emitting commit event (fallback)");
        events.commit.emit();
        ANLAND_TRACE("commit: fallback DONE");
        return true;
    }

    ANLAND_TRACE("commit: checking display");
    auto* dpy = display();
    if (!dpy) {
        ANLAND_LOG("commit: NO DISPLAY - immediate completion");
        m_framePending = false;
        ANLAND_TRACE("commit: emitting present event (no display)");
        events.present.emit(IOutput::SPresentEvent{
            .presented = true,
            .when = nullptr,
            .seq = 0,
            .refresh = (int)m_refresh,
            .flags = AQ_OUTPUT_PRESENT_VSYNC
        });
        ANLAND_TRACE("commit: emitting commit event (no display)");
        events.commit.emit();
        ANLAND_TRACE("commit: no display DONE");
        return true;
    }

    ANLAND_TRACE("commit: checking swapchain");
    if (!m_swapchain) {
        ANLAND_TRACE("commit: no swapchain, reconfiguring");
        reconfigureSwapchain();
        if (!m_swapchain) {
            ANLAND_LOG("commit: NO SWAPCHAIN - immediate completion");
            m_framePending = false;
            ANLAND_TRACE("commit: emitting present event (no swapchain)");
            events.present.emit(IOutput::SPresentEvent{
                .presented = true,
                .when = nullptr,
                .seq = 0,
                .refresh = (int)m_refresh,
                .flags = AQ_OUTPUT_PRESENT_VSYNC
            });
            ANLAND_TRACE("commit: emitting commit event (no swapchain)");
            events.commit.emit();
            ANLAND_TRACE("commit: no swapchain DONE");
            return true;
        }
    }

    ANLAND_TRACE("commit: acquiring buffer from swapchain");
    int age = 0;
    auto buffer = m_swapchain->next(&age);
    if (!buffer) {
        ANLAND_LOG("commit: NO BUFFER - immediate completion");
        m_framePending = false;
        ANLAND_TRACE("commit: emitting present event (no buffer)");
        events.present.emit(IOutput::SPresentEvent{
            .presented = true,
            .when = nullptr,
            .seq = 0,
            .refresh = (int)m_refresh,
            .flags = AQ_OUTPUT_PRESENT_VSYNC
        });
        ANLAND_TRACE("commit: emitting commit event (no buffer)");
        events.commit.emit();
        ANLAND_TRACE("commit: no buffer DONE");
        return true;
    }

    ANLAND_TRACE("commit: setting buffer to state");
    state->setBuffer(buffer);
    state->addDamage(CRegion(0, 0, (int)m_width, (int)m_height));

    ANLAND_TRACE("commit: calling trigger_refresh");
    int ret = trigger_refresh(dpy);
    ANLAND_TRACE("commit: trigger_refresh returned %d", ret);
    if (ret < 0) {
        ANLAND_ERR("commit: trigger_refresh failed");
        m_swapchain->rollback();
        m_framePending = false;
        ANLAND_TRACE("commit: emitting present event (trigger_refresh failed)");
        events.present.emit(IOutput::SPresentEvent{
            .presented = true,
            .when = nullptr,
            .seq = 0,
            .refresh = (int)m_refresh,
            .flags = AQ_OUTPUT_PRESENT_VSYNC
        });
        ANLAND_TRACE("commit: emitting commit event (trigger_refresh failed)");
        events.commit.emit();
        ANLAND_TRACE("commit: trigger_refresh failed DONE");
        return true;
    }

    m_framePending = true;
    ANLAND_LOG("commit: frame pending - waiting for buffer_ready");
    ANLAND_TRACE("commit: emitting commit event");
    events.commit.emit();
    ANLAND_TRACE("commit: END - returning true");
    return true;
}

void CAnlandOutput::scheduleFrame(const scheduleFrameReason reason) {
    ANLAND_TRACE("CAnlandOutput::scheduleFrame START: reason=%d", (int)reason);
    if (m_destroying) {
        ANLAND_TRACE("scheduleFrame: destroying, returning");
        return;
    }

    ANLAND_TRACE("scheduleFrame: inFallback=%d, outputReady=%d, frameScheduled=%d", 
                 m_inFallback, m_outputReady, m_frameScheduled);

    if (m_inFallback || !m_outputReady) {
        ANLAND_LOG("scheduleFrame: fallback mode - immediate frame");
        ANLAND_TRACE("scheduleFrame: emitting frame event (fallback)");
        events.frame.emit();
        ANLAND_TRACE("scheduleFrame: fallback DONE");
        return;
    }

    if (m_frameScheduled) {
        ANLAND_TRACE("scheduleFrame: already scheduled, returning");
        return;
    }

    m_frameScheduled = true;
    m_needsFrame = true;

    ANLAND_TRACE("scheduleFrame: emitting frame event immediately");
    events.frame.emit();

    if (!m_frameIdle) {
        ANLAND_TRACE("scheduleFrame: creating idle event");
        m_frameIdle = Hyprutils::Memory::makeShared<std::function<void(void)>>([this]() {
            ANLAND_TRACE("scheduleFrame idle callback START");
            m_frameScheduled = false;
            if (m_destroying || m_inFallback || !m_outputReady) {
                ANLAND_TRACE("scheduleFrame idle callback: skipping (destroying/fallback/notready)");
                return;
            }
            ANLAND_LOG("scheduleFrame idle: emitting frame");
            events.frame.emit();
            ANLAND_TRACE("scheduleFrame idle callback END");
        });
    }

    auto backend = m_backend ? m_backend->getBackend() : nullptr;
    if (backend) {
        ANLAND_TRACE("scheduleFrame: adding idle event to backend");
        backend->addIdleEvent(m_frameIdle);
    } else {
        ANLAND_TRACE("scheduleFrame: no backend, clearing scheduled flag");
        m_frameScheduled = false;
    }
    ANLAND_TRACE("scheduleFrame END");
}

void CAnlandOutput::onBufferReady() {
    ANLAND_TRACE("CAnlandOutput::onBufferReady START");
    if (m_inFallback) {
        ANLAND_TRACE("onBufferReady: in fallback, returning");
        return;
    }
    if (m_destroying) {
        ANLAND_TRACE("onBufferReady: destroying, returning");
        return;
    }

    ANLAND_LOG("onBufferReady: buffer consumed by Android");
    m_frameScheduled = false;

    auto* dpy = display();
    if (!dpy || is_fallback(dpy)) {
        ANLAND_LOG("onBufferReady: no display or fallback - immediate completion");
        m_framePending = false;
        ANLAND_TRACE("onBufferReady: emitting present event (no display)");
        events.present.emit(IOutput::SPresentEvent{
            .presented = true,
            .when = nullptr,
            .seq = 0,
            .refresh = (int)m_refresh,
            .flags = AQ_OUTPUT_PRESENT_VSYNC | AQ_OUTPUT_PRESENT_HW_CLOCK
        });
        ANLAND_TRACE("onBufferReady: END");
        return;
    }

    int fd = get_buffer_ready_fd(dpy);
    if (fd >= 0) {
        ANLAND_TRACE("onBufferReady: reading eventfd %d", fd);
        uint64_t val;
        if (read(fd, &val, sizeof(val)) < 0 && errno != EAGAIN) {
            ANLAND_ERR("onBufferReady: read eventfd failed: %s", strerror(errno));
        } else {
            ANLAND_TRACE("onBufferReady: read %llu from eventfd", (unsigned long long)val);
        }
    }

    m_framePending = false;

    timespec mono{};
    clock_gettime(CLOCK_MONOTONIC, &mono);
    ANLAND_TRACE("onBufferReady: emitting present event");
    events.present.emit(IOutput::SPresentEvent{
        .presented = true,
        .when = &mono,
        .seq = 0,
        .refresh = (int)m_refresh,
        .flags = AQ_OUTPUT_PRESENT_VSYNC | AQ_OUTPUT_PRESENT_HW_CLOCK
    });

    m_needsFrame = false;
    ANLAND_LOG("onBufferReady: done");
    ANLAND_TRACE("onBufferReady END");
}

void CAnlandOutput::enterFallback() {
    ANLAND_TRACE("CAnlandOutput::enterFallback START");
    if (m_inFallback) {
        ANLAND_TRACE("enterFallback: already in fallback, returning");
        return;
    }
    if (m_destroying) {
        ANLAND_TRACE("enterFallback: destroying, returning");
        return;
    }

    std::lock_guard<std::mutex> lock(m_bufferMutex);

    ANLAND_LOG("enterFallback: consumer disconnected");

    m_inFallback = true;
    m_framePending = false;
    m_outputReady = false;
    m_frameScheduled = false;
    this->enabled = false;
    this->state->setEnabled(false);

    m_firstCommit = true;
    ANLAND_TRACE("enterFallback: firstCommit reset to true");

    if (m_frameIdle) {
        ANLAND_TRACE("enterFallback: removing idle event");
        auto backend = m_backend ? m_backend->getBackend() : nullptr;
        if (backend) {
            backend->removeIdleEvent(m_frameIdle);
        }
        m_frameIdle = nullptr;
    }

    ANLAND_TRACE("enterFallback: emitting present event");
    events.present.emit(IOutput::SPresentEvent{
        .presented = true,
        .when = nullptr,
        .seq = 0,
        .refresh = (int)m_refresh,
        .flags = AQ_OUTPUT_PRESENT_VSYNC
    });
    ANLAND_TRACE("enterFallback END");
}

void CAnlandOutput::exitFallback() {
    ANLAND_TRACE("CAnlandOutput::exitFallback START");
    if (!m_inFallback) {
        ANLAND_TRACE("exitFallback: not in fallback, returning");
        return;
    }
    if (m_destroying) {
        ANLAND_TRACE("exitFallback: destroying, returning");
        return;
    }

    ANLAND_LOG("exitFallback: consumer reconnected");

    m_inFallback = false;
    this->enabled = true;
    this->state->setEnabled(true);

    m_outputReady = true;
    m_needsFrame = true;
    m_frameScheduled = false;

    m_firstCommit = true;
    ANLAND_TRACE("exitFallback: firstCommit reset to true");

    ANLAND_TRACE("exitFallback: reconfiguring swapchain");
    reconfigureSwapchain();

    // 关键修复：强制发送 frame 和 present 事件
    ANLAND_TRACE("exitFallback: emitting frame event");
    events.frame.emit();
    
    ANLAND_TRACE("exitFallback: emitting present event");
    events.present.emit(IOutput::SPresentEvent{
        .presented = true,
        .when = nullptr,
        .seq = 0,
        .refresh = (int)m_refresh,
        .flags = AQ_OUTPUT_PRESENT_VSYNC | AQ_OUTPUT_PRESENT_HW_CLOCK
    });

    ANLAND_TRACE("exitFallback: scheduling frame");
    scheduleFrame(AQ_SCHEDULE_NEW_CONNECTOR);
    
    ANLAND_LOG("exitFallback: done");
    ANLAND_TRACE("exitFallback END");
}

} // namespace Aquamarine