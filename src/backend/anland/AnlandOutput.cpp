// src/backend/anland/AnlandOutput.cpp
#include "AnlandOutput.hpp"
#include "AnlandBackend.hpp"
#include "AnlandBuffer.hpp"
#include "AnlandAllocator.hpp"
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
#define ANLAND_WARN(fmt, ...) do { fprintf(stderr, "[ANLAND][WARN] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while(0)
#define ANLAND_TRACE(fmt, ...) do { fprintf(stderr, "[ANLAND][TRACE] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while(0)
#define ANLAND_DEBUG(fmt, ...) do { fprintf(stderr, "[ANLAND][DEBUG] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while(0)

namespace Aquamarine {

using Hyprutils::Memory::CSharedPointer;
using Hyprutils::Memory::makeShared;
using Hyprutils::Math::CRegion;

static uint32_t protocol_format_to_drm(uint32_t fmt) {
    switch (fmt) {
        case 1:  // Android RGBA_8888 -> DRM_ABGR8888
            return DRM_FORMAT_ABGR8888;
        case 2:  // Android RGBX_8888 -> DRM_XBGR8888
            return DRM_FORMAT_XBGR8888;
        case 3:  // Android RGBA_1010102 -> DRM_ABGR2101010
            return DRM_FORMAT_ABGR2101010;
        case 4:  // Android RGBX_1010102 -> DRM_XBGR2101010
            return DRM_FORMAT_XBGR2101010;
        case 5:  // Android RGBA_FP16 -> DRM_ABGR16161616F
            return DRM_FORMAT_ABGR16161616F;
        default:
            return DRM_FORMAT_XRGB8888;
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

    m_imageDescription = nullptr;

    for (int i = 0; i < MAX_BUFS; i++) {
        m_slots[i].imported = false;
        m_slots[i].fd = -1;
        m_slots[i].buffer = nullptr;
        m_slots[i].accumDamage = CRegion();
    }
    m_shouldTriggerRefresh = false;
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

bool CAnlandOutput::initialize(uint32_t width, uint32_t height, uint32_t refresh) {
    ANLAND_TRACE("initialize START: %dx%d @ %d mHz", width, height, refresh);
    if (m_destroying) return false;

    m_width = width;
    m_height = height;
    m_refresh = refresh > 0 ? refresh : 60000;
    m_drmFormat = DRM_FORMAT_XRGB8888;

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
    this->state->setFormat(m_drmFormat);

    // 默认 Gamma
    std::vector<uint16_t> defaultGamma;
    defaultGamma.resize(256 * 3);
    for (int i = 0; i < 256; i++) {
        uint16_t val = (i * 65535) / 255;
        defaultGamma[i * 3 + 0] = val;
        defaultGamma[i * 3 + 1] = val;
        defaultGamma[i * 3 + 2] = val;
    }
    this->state->setGammaLut(defaultGamma);

    m_outputReady = true;
    m_inFallback = true;
    m_shouldTriggerRefresh = false;

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

void CAnlandOutput::ensureEGLContext() {
    EGLDisplay dpy;
    EGLContext ctx;
    if (getEGL(dpy, ctx) && dpy != EGL_NO_DISPLAY && ctx != EGL_NO_CONTEXT) {
        EGLContext current = eglGetCurrentContext();
        if (current != ctx) {
            eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
        }
    }
}

bool CAnlandOutput::createRenderFence(int& fenceFd) {
    fenceFd = -1;
    
    EGLDisplay dpy;
    EGLContext ctx;
    if (!getEGL(dpy, ctx) || dpy == EGL_NO_DISPLAY) {
        return false;
    }
    
    // 获取函数指针
    PFNEGLCREATESYNCKHRPROC eglCreateSyncKHR =
        (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC eglDupNativeFenceFDANDROID =
        (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress("eglDupNativeFenceFDANDROID");
    PFNEGLDESTROYSYNCKHRPROC eglDestroySyncKHR =
        (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
    
    if (!eglCreateSyncKHR || !eglDupNativeFenceFDANDROID || !eglDestroySyncKHR) {
        // 回退: glFinish
        glFinish();
        return false;
    }
    
    // Flush 确保渲染命令已提交
    glFlush();
    
    EGLSyncKHR sync = eglCreateSyncKHR(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
    if (sync == EGL_NO_SYNC_KHR) {
        glFinish();
        return false;
    }
    
    int fd = eglDupNativeFenceFDANDROID(dpy, sync);
    eglDestroySyncKHR(dpy, sync);
    
    if (fd >= 0) {
        fenceFd = fd;
        return true;
    }
    
    glFinish();
    return false;
}

void CAnlandOutput::releaseBuffers() {
    ANLAND_TRACE("releaseBuffers START");
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    for (int i = 0; i < MAX_BUFS; i++) {
        std::lock_guard<std::mutex> slotLock(m_slots[i].mutex);
        m_slots[i].inUse = false;
        m_slots[i].accumDamage = CRegion();
        if (m_slots[i].buffer) {
            m_slots[i].buffer->sendRelease();
            m_slots[i].buffer = nullptr;
        }
        m_slots[i].imported = false;
    }
    m_bufferCount = 0;
    m_buffersImported = false;
    this->swapchain.reset();
    ANLAND_TRACE("releaseBuffers END");
}

void CAnlandOutput::updateMode(uint32_t width, uint32_t height, uint32_t format) {
    if (width == m_width && height == m_height && format == m_drmFormat)
        return;

    ANLAND_LOG("updateMode: %dx%d fmt 0x%x -> %dx%d fmt 0x%x",
               m_width, m_height, m_drmFormat, width, height, format);

    uint32_t chosenFormat = format;
    if (chosenFormat == 0) chosenFormat = DRM_FORMAT_XRGB8888;

    m_width = width;
    m_height = height;
    m_drmFormat = chosenFormat;

    auto mode = CSharedPointer<SOutputMode>(
        new SOutputMode{
            .pixelSize = Hyprutils::Math::Vector2D(static_cast<float>(width), static_cast<float>(height)),
            .refreshRate = m_refresh,
            .preferred = true,
        });
    this->modes.clear();
    this->modes.push_back(mode);
    this->state->setMode(mode);
    this->state->setFormat(m_drmFormat);

    this->physicalSize = Hyprutils::Math::Vector2D(
        static_cast<float>(width) / 96.0f,
        static_cast<float>(height) / 96.0f
    );

    Hyprutils::Math::Vector2D size(static_cast<float>(width), static_cast<float>(height));
    events.state.emit(IOutput::SStateEvent{.size = size});
}

bool CAnlandOutput::importBuffer(int index) {
    ANLAND_TRACE("importBuffer: index=%d", index);
    if (index < 0 || index >= MAX_BUFS || m_destroying) return false;

    auto* slot = &m_slots[index];
    std::lock_guard<std::mutex> slotLock(slot->mutex);
    
    if (slot->imported) return true;

    auto* dpy = display();
    if (!dpy || is_fallback(dpy)) {
        ANLAND_ERR("importBuffer: display not ready");
        return false;
    }

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

    uint32_t drmFormat = protocol_format_to_drm(info.format);
    
    // 尝试使用原始修饰符，如果无效则回退到 LINEAR
    uint64_t modifier = info.modifier;
    if (modifier == DRM_FORMAT_MOD_INVALID || modifier == 0) {
        modifier = DRM_FORMAT_MOD_LINEAR;
        ANLAND_DEBUG("importBuffer: forcing LINEAR modifier for fd=%d", fd);
    }

    ANLAND_DEBUG("importBuffer: fd=%d, size=%dx%d, protocol_fmt=0x%x -> drm_fmt=0x%x, modifier=0x%lx",
                 fd, info.width, info.height, info.format, drmFormat, modifier);

    slot->buffer = CSharedPointer<CAnlandDmaBuffer>(
        new CAnlandDmaBuffer(fd, info, drmFormat, modifier));
    close(fd);

    if (!slot->buffer->good()) {
        ANLAND_ERR("importBuffer: buffer not good");
        slot->buffer = nullptr;
        return false;
    }

    auto releaseListener = slot->buffer->events.backendRelease.listen([this, index]() {
        ANLAND_TRACE("Buffer %d released", index);
        if (index < m_bufferCount) {
            std::lock_guard<std::mutex> slotLock(m_slots[index].mutex);
            m_slots[index].inUse = false;
        }
    });

    auto destroyListener = slot->buffer->events.destroy.listen([this, index]() {
        ANLAND_TRACE("Buffer %d destroyed", index);
        if (index < m_bufferCount) {
            std::lock_guard<std::mutex> slotLock(m_slots[index].mutex);
            m_slots[index].inUse = false;
            m_slots[index].accumDamage = CRegion();
            m_slots[index].imported = false;
        }
    });

    slot->width = info.width;
    slot->height = info.height;
    slot->format = drmFormat;
    slot->modifier = modifier;
    slot->offset = info.offset;
    slot->stride = info.stride;
    slot->imported = true;
    slot->inUse = false;
    slot->accumDamage = CRegion(0, 0, info.width, info.height);
    slot->hasDamage = true;

    ANLAND_LOG("importBuffer: slot %d registered (size %dx%d, fmt 0x%x, mod 0x%lx)",
               index, info.width, info.height, drmFormat, modifier);
    return true;
}

void CAnlandOutput::destroyBuffer(int index) {
    if (index < 0 || index >= MAX_BUFS || m_destroying) return;
    auto* slot = &m_slots[index];
    std::lock_guard<std::mutex> slotLock(slot->mutex);
    if (slot->buffer) {
        slot->buffer->sendRelease();
        slot->buffer = nullptr;
    }
    slot->imported = false;
    slot->inUse = false;
    slot->hasDamage = true;
    slot->accumDamage = CRegion();
}

void CAnlandOutput::importBuffers() {
    ANLAND_TRACE("importBuffers START");
    auto* dpy = display();
    if (!dpy || is_fallback(dpy)) {
        ANLAND_ERR("importBuffers: display not ready");
        return;
    }

    int count = get_buf_count(dpy);
    ANLAND_LOG("importBuffers: got %d buffers", count);
    if (count <= 0 || count > MAX_BUFS) {
        ANLAND_ERR("importBuffers: invalid count %d", count);
        return;
    }

    std::lock_guard<std::mutex> lock(m_bufferMutex);

    for (int i = 0; i < MAX_BUFS; i++) {
        destroyBuffer(i);
    }

    for (int i = 0; i < count; i++) {
        if (!importBuffer(i)) {
            ANLAND_ERR("importBuffers: failed to import buffer %d", i);
        }
    }

    m_bufferCount = count;
    m_buffersImported = true;

    if (count > 0 && m_slots[0].imported) {
        std::lock_guard<std::mutex> slotLock(m_slots[0].mutex);
        uint32_t w = m_slots[0].width;
        uint32_t h = m_slots[0].height;
        uint32_t fmt = m_slots[0].format;
        if (w > 0 && h > 0) {
            if (w != m_width || h != m_height) {
                ANLAND_LOG("importBuffers: buffer size changed to %dx%d, updating output", w, h);
                // 先释放锁再调用 updateMode
                slotLock.unlock();
                updateMode(w, h, fmt);
            }
        }
    }

    ANLAND_LOG("importBuffers: registered %d buffers", m_bufferCount);
}

bool CAnlandOutput::test() {
    return true;
}

std::vector<SDRMFormat> CAnlandOutput::getRenderFormats() {
    std::vector<SDRMFormat> formats;

    // 优先返回 LINEAR 修饰符
    formats.push_back({.drmFormat = DRM_FORMAT_ABGR8888, .modifiers = {DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID}});
    formats.push_back({.drmFormat = DRM_FORMAT_XBGR8888, .modifiers = {DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID}});
    formats.push_back({.drmFormat = DRM_FORMAT_ARGB8888, .modifiers = {DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID}});
    formats.push_back({.drmFormat = DRM_FORMAT_XRGB8888, .modifiers = {DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID}});

    // 添加实际导入的格式
    if (m_buffersImported && m_bufferCount > 0 && m_slots[0].imported) {
        std::lock_guard<std::mutex> slotLock(m_slots[0].mutex);
        uint32_t actualFmt = m_slots[0].format;
        uint64_t actualMod = m_slots[0].modifier;
        formats.clear();
        formats.push_back({.drmFormat = actualFmt, .modifiers = {actualMod, DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID}});
        ANLAND_DEBUG("getRenderFormats: using actual format 0x%x mod 0x%lx", actualFmt, actualMod);
    }

    // 合并后端格式
    if (m_backend) {
        auto backendFormats = m_backend->getRenderFormats();
        for (const auto& fmt : backendFormats) {
            bool exists = false;
            for (const auto& existing : formats) {
                if (existing.drmFormat == fmt.drmFormat) {
                    exists = true;
                    break;
                }
            }
            if (!exists && fmt.drmFormat != DRM_FORMAT_INVALID) {
                formats.push_back(fmt);
            }
        }
    }

    return formats;
}

bool CAnlandOutput::commit() {
    ANLAND_TRACE("commit START: shouldTriggerRefresh=%d", m_shouldTriggerRefresh);
    if (m_destroying) return true;

    // 确保 EGL 上下文
    ensureEGLContext();

    if (m_commitInProgress.exchange(true)) {
        ANLAND_TRACE("commit: already in progress");
        return false;
    }

    struct CommitGuard {
        std::atomic<bool>& flag;
        ~CommitGuard() { flag = false; }
    } guard(m_commitInProgress);

    if (m_inFallback || !m_outputReady) {
        ANLAND_LOG("commit: fallback - immediate completion");
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

    if (!m_buffersImported) {
        ANLAND_TRACE("commit: importing buffers");
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
    ANLAND_TRACE("commit: selected index=%d", m_selectedIndex);
    if (m_selectedIndex >= m_bufferCount) {
        m_selectedIndex = 0;
    }

    auto& slot = m_slots[m_selectedIndex];
    std::lock_guard<std::mutex> slotLock(slot.mutex);

    if (!slot.imported) {
        slotLock.unlock();
        if (!importBuffer(m_selectedIndex)) {
            ANLAND_ERR("commit: importBuffer failed for %d", m_selectedIndex);
        }
        slotLock.lock();
    }

    if (slot.buffer) {
        // 直接设置缓冲区
        state->setBuffer(slot.buffer);

        // 使用累积的损伤，而不是 state->damage
        if (slot.hasDamage && !slot.accumDamage.empty()) {
            state->addDamage(slot.accumDamage);
            ANLAND_DEBUG("commit: using buffer %d with accumulated damage", m_selectedIndex);
        } else {
            ANLAND_DEBUG("commit: buffer %d has no new damage", m_selectedIndex);
        }

        // 清空累积损伤，但保留已设置到 state 的损伤
        slot.accumDamage = CRegion();
        slot.hasDamage = false;
    } else {
        ANLAND_ERR("commit: slot %d has no buffer", m_selectedIndex);
        state->addDamage(CRegion(0, 0, m_width, m_height));
    }

    // 创建渲染 fence
    int fenceFd = -1;
    if (createRenderFence(fenceFd)) {
        set_render_fence(dpy, fenceFd);
        ANLAND_DEBUG("commit: created render fence fd=%d", fenceFd);
    } else {
        ANLAND_DEBUG("commit: using glFinish fallback for sync");
    }

    if (m_shouldTriggerRefresh) {
        m_shouldTriggerRefresh = false;
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
        ANLAND_LOG("commit: no trigger_refresh");
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
        events.frame.emit();
        return;
    }

    if (m_frameScheduled) return;

    m_frameScheduled = true;
    this->needsFrame = true;

    // 累积损伤到当前选中的 buffer
    if (m_buffersImported && m_selectedIndex < m_bufferCount) {
        auto& slot = m_slots[m_selectedIndex];
        std::lock_guard<std::mutex> slotLock(slot.mutex);
        if (slot.imported && slot.buffer) {
            const auto& damage = state->state().damage;
            if (!damage.empty()) {
                slot.accumDamage.add(damage);
                slot.hasDamage = true;
                ANLAND_TRACE("scheduleFrame: accumulated damage for buffer %d", m_selectedIndex);
            }
        }
    }

    m_shouldTriggerRefresh = true;
    events.frame.emit();

    if (!m_frameIdle) {
        m_frameIdle = makeShared<std::function<void(void)>>([this]() {
            m_frameScheduled = false;
            if (m_destroying || m_inFallback || !m_outputReady) return;
            m_shouldTriggerRefresh = true;
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
            ANLAND_ERR("onBufferReady: read failed: %s", strerror(errno));
        }
    }

    // 释放当前 buffer
    if (m_selectedIndex >= 0 && m_selectedIndex < m_bufferCount) {
        auto& slot = m_slots[m_selectedIndex];
        std::lock_guard<std::mutex> slotLock(slot.mutex);
        if (slot.buffer) {
            slot.buffer->sendRelease();
            slot.inUse = false;
            // 清空损伤，因为已经展示
            slot.accumDamage = CRegion();
            slot.hasDamage = true; // 下次需要全屏重绘
            ANLAND_DEBUG("onBufferReady: released buffer %d", m_selectedIndex);
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
    m_shouldTriggerRefresh = false;
    this->swapchain.reset();

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
    m_shouldTriggerRefresh = false;
    this->swapchain.reset();

    importBuffers();

    scheduleFrame(AQ_SCHEDULE_NEW_CONNECTOR);
    events.frame.emit();
    ANLAND_TRACE("exitFallback END");
}

CSharedPointer<CAnlandDmaBuffer> CAnlandOutput::getBuffer(int index) const {
    if (index < 0 || index >= m_bufferCount) return nullptr;
    std::lock_guard<std::mutex> slotLock(m_slots[index].mutex);
    return m_slots[index].buffer;
}

CSharedPointer<CBackend> CAnlandOutput::getCBackend() const {
    if (!m_backend) return nullptr;
    return m_backend->getBackend();
}

CSharedPointer<IBackendImplementation> CAnlandOutput::getBackend() {
    if (m_destroying || !m_backend) return nullptr;
    return m_backend->self.lock();
}

} // namespace Aquamarine