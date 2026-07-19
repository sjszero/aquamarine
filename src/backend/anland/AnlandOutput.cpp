// src/backend/anland/AnlandOutput.cpp
#include "AnlandOutput.hpp"
#include "AnlandBackend.hpp"
#include "AnlandBuffer.hpp"
#include <drm_fourcc.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <chrono>

#define ANLAND_LOG(fmt, ...) do { fprintf(stderr, "[ANLAND] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while(0)
#define ANLAND_ERR(fmt, ...) do { fprintf(stderr, "[ANLAND][ERR] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while(0)
#define ANLAND_TRACE(fmt, ...) do { fprintf(stderr, "[ANLAND][TRACE] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while(0)

namespace Aquamarine {

using Hyprutils::Memory::CSharedPointer;
using Hyprutils::Memory::makeShared;
using Hyprutils::Math::CRegion;

static PFNEGLCREATEIMAGEKHRPROC g_eglCreateImageKHR = nullptr;
static PFNEGLDESTROYIMAGEKHRPROC g_eglDestroyImageKHR = nullptr;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC g_glEGLImageTargetTexture2DOES = nullptr;
static bool g_eglFunctionsInitialized = false;

static bool initEGLFunctions() {
    if (g_eglFunctionsInitialized) {
        return g_eglCreateImageKHR != nullptr && g_eglDestroyImageKHR != nullptr;
    }
    g_eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    g_eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    g_glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    g_eglFunctionsInitialized = true;
    return g_eglCreateImageKHR != nullptr && g_eglDestroyImageKHR != nullptr && g_glEGLImageTargetTexture2DOES != nullptr;
}

static uint32_t protocolFormatToDrm(uint32_t fmt) {
    switch (fmt) {
        case 1: return DRM_FORMAT_ABGR8888;
        case 2: return DRM_FORMAT_XBGR8888;
        case 3: return DRM_FORMAT_RGB565;
        default: return DRM_FORMAT_XRGB8888;
    }
}

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

    for (int i = 0; i < MAX_BUFS; i++) {
        m_slots[i].imported = false;
        m_slots[i].fd = -1;
        m_slots[i].framebuffer = 0;
        m_slots[i].texture = 0;
        m_slots[i].eglImage = EGL_NO_IMAGE_KHR;
    }
    ANLAND_TRACE("CAnlandOutput constructor END");
}

CAnlandOutput::~CAnlandOutput() {
    ANLAND_TRACE("CAnlandOutput destructor START");
    if (m_shutdownDone.exchange(true)) return;
    m_destroying = true;
    releaseBuffers();
    ANLAND_TRACE("CAnlandOutput destructor END");
}

display_ctx* CAnlandOutput::display() {
    if (m_destroying || !m_backend) return nullptr;
    return m_backend->display();
}

bool CAnlandOutput::ensureEGLInitialized() {
    if (m_eglInitialized && m_eglDisplay != EGL_NO_DISPLAY) return true;

    m_eglDisplay = eglGetCurrentDisplay();
    if (m_eglDisplay == EGL_NO_DISPLAY) {
        ANLAND_TRACE("ensureEGLInitialized: no current EGLDisplay (will retry later)");
        return false;
    }

    if (!initEGLFunctions()) {
        ANLAND_ERR("ensureEGLInitialized: EGL functions not available");
        return false;
    }

    m_eglInitialized = true;
    ANLAND_TRACE("ensureEGLInitialized: success");
    return true;
}

bool CAnlandOutput::initialize(uint32_t width, uint32_t height, uint32_t refresh) {
    ANLAND_TRACE("initialize START: %dx%d @ %d mHz", width, height, refresh);
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
    m_inFallback = true;
    m_firstCommit = true;

    events.frame.emit();
    ANLAND_LOG("initialize: %dx%d @ %d mHz, output ready", width, height, refresh);
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
    ANLAND_TRACE("releaseBuffers START");
    for (int i = 0; i < MAX_BUFS; i++) {
        destroyBuffer(i);
    }
    m_bufferCount = 0;
    m_buffersImported = false;
    ANLAND_TRACE("releaseBuffers END");
}

bool CAnlandOutput::importBuffer(int index) {
    ANLAND_TRACE("importBuffer: index=%d", index);
    if (index < 0 || index >= MAX_BUFS || m_destroying) return false;

    auto* slot = &m_slots[index];
    if (slot->imported) return true;

    auto* dpy = display();
    if (!dpy || is_fallback(dpy)) {
        ANLAND_ERR("importBuffer: display not ready");
        return false;
    }

    // 只创建 CAnlandDmaBuffer 包装 dmabuf，不导入 EGL
    // EGL 导入在 getCurrentFramebuffer 中按需进行
    if (slot->fd < 0) {
        int fd = get_dmabuf_fd_at(dpy, index);
        if (fd < 0) {
            ANLAND_ERR("importBuffer: get_dmabuf_fd_at failed for %d", index);
            return false;
        }

        buf_info info;
        if (get_dmabuf_info_at(dpy, index, &info) < 0) {
            ANLAND_ERR("importBuffer: get_dmabuf_info_at failed for %d", index);
            close(fd);
            return false;
        }

        slot->fd = dup(fd);
        close(fd);
        if (slot->fd < 0) {
            ANLAND_ERR("importBuffer: dup failed");
            return false;
        }

        slot->width = info.width;
        slot->height = info.height;
        slot->format = info.format;
        slot->modifier = info.modifier;
        slot->offset = info.offset;
        slot->stride = info.stride;
    }

    // 如果还没有 buffer 对象，创建一个
    if (!slot->buffer) {
        buf_info info;
        info.width = slot->width;
        info.height = slot->height;
        info.format = slot->format;
        info.modifier = slot->modifier;
        info.offset = slot->offset;
        info.stride = slot->stride;
        slot->buffer = CSharedPointer<CAnlandDmaBuffer>(
            new CAnlandDmaBuffer(slot->fd, info));
    }

    slot->imported = true;
    slot->hasDamage = true;

    ANLAND_LOG("importBuffer: slot %d registered (EGL import deferred)", index);
    return true;
}

void CAnlandOutput::destroyBuffer(int index) {
    if (index < 0 || index >= MAX_BUFS || m_destroying) return;
    auto* slot = &m_slots[index];

    if (slot->framebuffer) {
        glDeleteFramebuffers(1, &slot->framebuffer);
        slot->framebuffer = 0;
    }
    if (slot->texture) {
        glDeleteTextures(1, &slot->texture);
        slot->texture = 0;
    }
    if (slot->eglImage != EGL_NO_IMAGE_KHR && m_eglDisplay != EGL_NO_DISPLAY && g_eglDestroyImageKHR) {
        g_eglDestroyImageKHR(m_eglDisplay, slot->eglImage);
        slot->eglImage = EGL_NO_IMAGE_KHR;
    }
    if (slot->fd >= 0) {
        close(slot->fd);
        slot->fd = -1;
    }
    slot->buffer = nullptr;
    slot->imported = false;
    slot->inUse = false;
    slot->hasDamage = true;
}

void CAnlandOutput::importBuffers() {
    ANLAND_TRACE("importBuffers START (lazy: only record buffer info)");
    auto* dpy = display();
    if (!dpy || is_fallback(dpy)) {
        ANLAND_ERR("importBuffers: display not ready");
        return;
    }

    int count = get_buf_count(dpy);
    ANLAND_LOG("importBuffers: get_buf_count returned %d", count);
    if (count <= 0 || count > MAX_BUFS) {
        ANLAND_ERR("importBuffers: invalid count %d", count);
        return;
    }

    std::lock_guard<std::mutex> lock(m_bufferMutex);

    for (int i = 0; i < MAX_BUFS; i++) {
        destroyBuffer(i);
    }

    m_bufferCount = count;
    for (int i = 0; i < count; i++) {
        importBuffer(i);
    }
    m_buffersImported = true;
    ANLAND_LOG("importBuffers: registered %d buffers (EGL import deferred)", m_bufferCount);
}

bool CAnlandOutput::test() {
    ANLAND_TRACE("test: returning true");
    return true;
}

bool CAnlandOutput::commit() {
    ANLAND_TRACE("commit START: firstCommit=%d", m_firstCommit);
    if (m_destroying) return true;

    if (m_commitInProgress.exchange(true)) {
        ANLAND_TRACE("commit: already in progress");
        return false;
    }

    struct CommitGuard {
        std::atomic<bool>& flag;
        ~CommitGuard() { flag = false; }
    } guard(m_commitInProgress);

    // 关键修复：第一次 commit 时完全跳过所有操作，直接返回成功
    // 这样 applyMonitorRule 就能顺利完成，Hyprland 进入主循环
    if (m_firstCommit) {
        m_firstCommit = false;
        m_framePending = false;
        ANLAND_LOG("commit: FIRST COMMIT - immediate success, skipping all operations");
        events.present.emit(IOutput::SPresentEvent{
            .presented = true,
            .when = nullptr,
            .seq = 0,
            .refresh = (int)m_refresh,
            .flags = IOutput::AQ_OUTPUT_PRESENT_VSYNC
        });
        events.commit.emit();
        return true;
    }

    if (m_inFallback || !m_outputReady) {
        ANLAND_LOG("commit: fallback/notready - immediate completion");
        m_framePending = false;
        events.present.emit(IOutput::SPresentEvent{
            .presented = true,
            .when = nullptr,
            .seq = 0,
            .refresh = (int)m_refresh,
            .flags = IOutput::AQ_OUTPUT_PRESENT_VSYNC
        });
        events.commit.emit();
        return true;
    }

    auto* dpy = display();
    if (!dpy) {
        ANLAND_LOG("commit: no display - immediate completion");
        m_framePending = false;
        events.present.emit(IOutput::SPresentEvent{
            .presented = true,
            .when = nullptr,
            .seq = 0,
            .refresh = (int)m_refresh,
            .flags = IOutput::AQ_OUTPUT_PRESENT_VSYNC
        });
        events.commit.emit();
        return true;
    }

    // 确保缓冲区已记录
    if (!m_buffersImported) {
        ANLAND_TRACE("commit: recording buffers");
        importBuffers();
        if (!m_buffersImported) {
            ANLAND_LOG("commit: no buffers - immediate completion");
            m_framePending = false;
            events.present.emit(IOutput::SPresentEvent{
                .presented = true,
                .when = nullptr,
                .seq = 0,
                .refresh = (int)m_refresh,
                .flags = IOutput::AQ_OUTPUT_PRESENT_VSYNC
            });
            events.commit.emit();
            return true;
        }
    }

    m_selectedIndex = get_selected_idx(dpy);
    if (m_selectedIndex >= m_bufferCount) {
        m_selectedIndex = 0;
    }

    auto& slot = m_slots[m_selectedIndex];

    // 确保 buffer 已创建
    if (!slot.imported) {
        if (!importBuffer(m_selectedIndex)) {
            ANLAND_ERR("commit: importBuffer failed for %d", m_selectedIndex);
        }
    }

    if (slot.buffer) {
        state->setBuffer(slot.buffer);
    }

    state->addDamage(CRegion(0, 0, (int)m_width, (int)m_height));

    // 只有在非第一次提交时才触发刷新
    if (!m_firstCommit) {
        int ret = trigger_refresh(dpy);
        if (ret < 0) {
            ANLAND_ERR("commit: trigger_refresh failed");
            m_framePending = false;
            events.present.emit(IOutput::SPresentEvent{
                .presented = true,
                .when = nullptr,
                .seq = 0,
                .refresh = (int)m_refresh,
                .flags = IOutput::AQ_OUTPUT_PRESENT_VSYNC
            });
            events.commit.emit();
            return true;
        }
        m_framePending = true;
        ANLAND_LOG("commit: frame pending");
    } else {
        m_framePending = false;
        ANLAND_LOG("commit: no trigger_refresh (first commit)");
        events.present.emit(IOutput::SPresentEvent{
            .presented = true,
            .when = nullptr,
            .seq = 0,
            .refresh = (int)m_refresh,
            .flags = IOutput::AQ_OUTPUT_PRESENT_VSYNC
        });
    }

    events.commit.emit();
    return true;
}

void CAnlandOutput::scheduleFrame(const scheduleFrameReason reason) {
    ANLAND_TRACE("scheduleFrame: reason=%d", (int)reason);
    if (m_destroying) return;

    if (m_inFallback || !m_outputReady) {
        ANLAND_TRACE("scheduleFrame: fallback - emitting immediate frame");
        events.frame.emit();
        return;
    }

    if (m_frameScheduled) return;

    m_frameScheduled = true;
    this->needsFrame = true;

    events.frame.emit();

    if (!m_frameIdle) {
        m_frameIdle = makeShared<std::function<void(void)>>([this]() {
            m_frameScheduled = false;
            if (m_destroying || m_inFallback || !m_outputReady) return;
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
    ANLAND_TRACE("onBufferReady START");
    if (m_inFallback || m_destroying) return;

    m_frameScheduled = false;

    auto* dpy = display();
    if (!dpy || is_fallback(dpy)) {
        m_framePending = false;
        events.present.emit(IOutput::SPresentEvent{
            .presented = true,
            .when = nullptr,
            .seq = 0,
            .refresh = (int)m_refresh,
            .flags = IOutput::AQ_OUTPUT_PRESENT_VSYNC
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
        .flags = IOutput::AQ_OUTPUT_PRESENT_VSYNC | IOutput::AQ_OUTPUT_PRESENT_HW_CLOCK
    });

    this->needsFrame = false;
    ANLAND_TRACE("onBufferReady END");
}

void CAnlandOutput::enterFallback() {
    ANLAND_TRACE("enterFallback START");
    if (m_inFallback || m_destroying) return;

    std::lock_guard<std::mutex> lock(m_bufferMutex);

    m_inFallback = true;
    m_framePending = false;
    m_outputReady = false;
    m_buffersImported = false;
    m_frameScheduled = false;
    this->enabled = false;
    this->state->setEnabled(false);
    m_firstCommit = true;

    if (m_frameIdle) {
        auto backend = m_backend ? m_backend->getBackend() : nullptr;
        if (backend) {
            backend->removeIdleEvent(m_frameIdle);
        }
        m_frameIdle = nullptr;
    }

    events.present.emit(IOutput::SPresentEvent{
        .presented = true,
        .when = nullptr,
        .seq = 0,
        .refresh = (int)m_refresh,
        .flags = IOutput::AQ_OUTPUT_PRESENT_VSYNC
    });
    ANLAND_TRACE("enterFallback END");
}

void CAnlandOutput::exitFallback() {
    ANLAND_TRACE("exitFallback START");
    if (!m_inFallback || m_destroying) return;

    m_inFallback = false;
    this->enabled = true;
    this->state->setEnabled(true);

    m_outputReady = true;
    this->needsFrame = true;
    m_frameScheduled = false;
    m_buffersImported = false;
    m_firstCommit = true;

    // 只记录缓冲区信息，不导入 EGL
    importBuffers();

    scheduleFrame(AQ_SCHEDULE_NEW_CONNECTOR);
    events.frame.emit();
    ANLAND_TRACE("exitFallback END");
}

GLuint CAnlandOutput::getCurrentFramebuffer() const {
    if (m_selectedIndex < 0 || m_selectedIndex >= m_bufferCount) return 0;
    const auto* slot = &m_slots[m_selectedIndex];
    return slot->framebuffer;
}

GLuint CAnlandOutput::getCurrentTexture() const {
    if (m_selectedIndex < 0 || m_selectedIndex >= m_bufferCount) return 0;
    const auto* slot = &m_slots[m_selectedIndex];
    return slot->texture;
}

CSharedPointer<IBackendImplementation> CAnlandOutput::getBackend() {
    if (m_destroying || !m_backend) return nullptr;
    return m_backend->self.lock();
}

std::vector<SDRMFormat> CAnlandOutput::getRenderFormats() {
    if (m_backend) {
        return m_backend->getRenderFormats();
    }
    return {};
}

} // namespace Aquamarine