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

bool CAnlandOutput::test() {
    auto* dpy = display();
    if (!dpy || is_fallback(dpy)) {
        return false;
    }
    if (!m_swapchain) {
        reconfigureSwapchain();
        if (!m_swapchain) return false;
    }
    return true;
}

bool CAnlandOutput::commit() {
    if (m_destroying) return true;
    if (m_commitInProgress.exchange(true)) {
        return false;
    }

    struct CommitGuard {
        std::atomic<bool>& flag;
        ~CommitGuard() { flag = false; }
    } guard(m_commitInProgress);

    if (m_inFallback || !m_outputReady) {
        return true;
    }

    auto* dpy = display();
    if (!dpy || is_fallback(dpy)) {
        return true;
    }

    if (!m_swapchain) {
        reconfigureSwapchain();
        if (!m_swapchain) return true;
    }

    int age = 0;
    auto buffer = m_swapchain->next(&age);
    if (!buffer) {
        ANLAND_ERR("commit: failed to acquire buffer");
        return true;
    }

    state->setBuffer(buffer);
    state->addDamage(CRegion(0, 0, (int)m_width, (int)m_height));

    int ret = trigger_refresh(dpy);
    if (ret < 0) {
        ANLAND_ERR("commit: trigger_refresh failed");
        m_swapchain->rollback();
        m_inFallback = true;
        return true;
    }

    m_framePending = true;
    events.commit.emit();
    return true;
}

void CAnlandOutput::scheduleFrame(const scheduleFrameReason reason) {
    if (m_destroying || m_inFallback || !m_outputReady) return;
    if (m_frameScheduled) return;

    m_frameScheduled = true;
    m_needsFrame = true;

    if (!m_frameIdle) {
        m_frameIdle = Hyprutils::Memory::makeShared<std::function<void(void)>>([this]() {
            m_frameScheduled = false;
            if (m_destroying || m_inFallback || !m_outputReady) return;
            events.frame.emit();
        });
    }

    auto backend = m_backend ? m_backend->getBackend() : nullptr;
    if (backend) {
        backend->addIdleEvent(m_frameIdle);
    } else {
        events.frame.emit();
        m_frameScheduled = false;
    }
}

void CAnlandOutput::onBufferReady() {
    if (m_inFallback || m_destroying) return;

    m_frameScheduled = false;

    auto* dpy = display();
    if (!dpy || is_fallback(dpy)) return;

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
}

void CAnlandOutput::enterFallback() {
    if (m_inFallback || m_destroying) return;
    std::lock_guard<std::mutex> lock(m_bufferMutex);

    m_inFallback = true;
    m_framePending = false;
    m_outputReady = false;
    m_frameScheduled = false;
    this->enabled = false;
    this->state->setEnabled(false);

    if (m_frameIdle) {
        auto backend = m_backend ? m_backend->getBackend() : nullptr;
        if (backend) {
            backend->removeIdleEvent(m_frameIdle);
        }
        m_frameIdle = nullptr;
    }
}

void CAnlandOutput::exitFallback() {
    if (!m_inFallback || m_destroying) return;

    m_inFallback = false;
    this->enabled = true;
    this->state->setEnabled(true);

    m_outputReady = true;
    m_needsFrame = true;
    m_frameScheduled = false;

    reconfigureSwapchain();
    scheduleFrame(AQ_SCHEDULE_NEW_CONNECTOR);
    events.frame.emit();
}

} // namespace Aquamarine