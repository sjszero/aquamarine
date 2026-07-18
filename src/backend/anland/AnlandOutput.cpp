// src/backend/anland/AnlandOutput.cpp
#include "AnlandOutput.hpp"
#include "AnlandBackend.hpp"
#include "AnlandBuffer.hpp"
#include <drm_fourcc.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <cstdio>
#include <cerrno>

#define ANLAND_LOG(fmt, ...) fprintf(stderr, "[ANLAND] " fmt "\n", ##__VA_ARGS__)
#define ANLAND_ERR(fmt, ...) fprintf(stderr, "[ANLAND][ERR] " fmt "\n", ##__VA_ARGS__)

namespace Aquamarine {

// 全局 EGL 函数指针（由 Hyprland 的 OpenGL 上下文提供）
static PFNEGLCREATEIMAGEKHRPROC g_eglCreateImageKHR = nullptr;
static PFNEGLDESTROYIMAGEKHRPROC g_eglDestroyImageKHR = nullptr;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC g_glEGLImageTargetTexture2DOES = nullptr;
static EGLDisplay g_eglDisplay = EGL_NO_DISPLAY;
static bool g_eglFunctionsInitialized = false;

static bool initEGLFunctions() {
    if (g_eglFunctionsInitialized) return (g_eglCreateImageKHR != nullptr);
    g_eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    g_eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    g_glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    g_eglFunctionsInitialized = true;
    return g_eglCreateImageKHR && g_eglDestroyImageKHR && g_glEGLImageTargetTexture2DOES;
}

static EGLDisplay getEGLDisplay() {
    if (g_eglDisplay != EGL_NO_DISPLAY) return g_eglDisplay;

    // 优先使用当前上下文中的 Display
    EGLDisplay dpy = eglGetCurrentDisplay();
    if (dpy != EGL_NO_DISPLAY) {
        g_eglDisplay = dpy;
        ANLAND_LOG("getEGLDisplay: from current context");
        return dpy;
    }

    // 其次尝试默认 Display
    dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy != EGL_NO_DISPLAY) {
        EGLint major, minor;
        if (eglInitialize(dpy, &major, &minor)) {
            g_eglDisplay = dpy;
            ANLAND_LOG("getEGLDisplay: initialized default display");
            return dpy;
        }
    }

    ANLAND_ERR("getEGLDisplay: failed to get any display");
    return EGL_NO_DISPLAY;
}

static uint32_t protocolFormatToDrmImpl(uint32_t fmt) {
    switch (fmt) {
        case 1: return DRM_FORMAT_ABGR8888;
        case 2: return DRM_FORMAT_XBGR8888;
        case 3: return DRM_FORMAT_RGB565;
        default: return DRM_FORMAT_XRGB8888;
    }
}

// ============================================================
// CAnlandOutput 实现
// ============================================================

CAnlandOutput::CAnlandOutput(CAnlandBackend* backend) : m_backend(backend) {
    for (int i = 0; i < MAX_BUFS; i++) {
        m_slots[i].imported = false;
        m_slots[i].fd = -1;
        m_slots[i].framebuffer = 0;
        m_slots[i].texture = 0;
        m_slots[i].eglImage = EGL_NO_IMAGE_KHR;
    }

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
    for (int i = 0; i < MAX_BUFS; i++) destroyBuffer(i);
    ANLAND_LOG("CAnlandOutput destroyed");
}

display_ctx* CAnlandOutput::display() {
    if (m_destroying || !m_backend) return nullptr;
    return m_backend->display();
}

bool CAnlandOutput::ensureEGLInitialized() {
    if (m_eglInitialized && g_eglDisplay != EGL_NO_DISPLAY) return true;

    g_eglDisplay = getEGLDisplay();
    if (g_eglDisplay == EGL_NO_DISPLAY) {
        ANLAND_ERR("ensureEGLInitialized: no EGLDisplay");
        return false;
    }

    if (!initEGLFunctions()) {
        ANLAND_ERR("ensureEGLInitialized: EGL functions not available");
        return false;
    }

    m_eglInitialized = true;
    ANLAND_LOG("ensureEGLInitialized: success");
    return true;
}

CSharedPointer<IBackendImplementation> CAnlandOutput::getBackend() {
    if (m_destroying || !m_backend) return nullptr;
    return m_backend->self.lock();
}

std::vector<SDRMFormat> CAnlandOutput::getRenderFormats() {
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

uint32_t CAnlandOutput::protocolFormatToDrm(uint32_t fmt) const {
    return protocolFormatToDrmImpl(fmt);
}

bool CAnlandOutput::initialize(uint32_t width, uint32_t height, uint32_t refresh) {
    if (m_destroying) return false;

    m_width = width;
    m_height = height;
    m_refresh = refresh > 0 ? refresh : 60000;

    this->physicalSize = Hyprutils::Math::Vector2D(width / 96.0f, height / 96.0f);

    auto mode = CSharedPointer<SOutputMode>(
        new SOutputMode{
            .pixelSize = Hyprutils::Math::Vector2D(static_cast<float>(width), static_cast<float>(height)),
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

bool CAnlandOutput::importBuffer(int index) {
    if (index < 0 || index >= MAX_BUFS || m_destroying) return false;

    auto* slot = &m_slots[index];
    if (slot->imported && !slot->failed) return true;
    if (slot->failed) {
        ANLAND_LOG("importBuffer: slot %d previously failed", index);
        return false;
    }

    // 1. 确保 EGL 已初始化
    if (!ensureEGLInitialized()) {
        slot->failed = true;
        return false;
    }

    // 2. 确保有 GL 上下文
    EGLContext currentCtx = eglGetCurrentContext();
    EGLContext tempCtx = EGL_NO_CONTEXT;
    bool needRestore = false;

    if (currentCtx == EGL_NO_CONTEXT) {
        ANLAND_LOG("importBuffer: no GL context, creating temporary");
        EGLint ctxAttribs[] = {
            EGL_CONTEXT_MAJOR_VERSION, 3,
            EGL_CONTEXT_MINOR_VERSION, 0,
            EGL_NONE
        };
        tempCtx = eglCreateContext(g_eglDisplay, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctxAttribs);
        if (tempCtx == EGL_NO_CONTEXT) {
            ANLAND_ERR("importBuffer: eglCreateContext failed");
            slot->failed = true;
            return false;
        }
        if (!eglMakeCurrent(g_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, tempCtx)) {
            ANLAND_ERR("importBuffer: eglMakeCurrent failed");
            eglDestroyContext(g_eglDisplay, tempCtx);
            slot->failed = true;
            return false;
        }
        needRestore = true;
    }

    auto* dpy = display();
    if (!dpy || is_fallback(dpy)) {
        ANLAND_LOG("importBuffer: display not ready");
        if (needRestore) {
            eglMakeCurrent(g_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroyContext(g_eglDisplay, tempCtx);
        }
        slot->failed = true;
        return false;
    }

    // 3. 获取 dmabuf fd 和信息
    int fd = get_dmabuf_fd_at(dpy, index);
    if (fd < 0) {
        ANLAND_ERR("importBuffer: get_dmabuf_fd_at failed for %d", index);
        if (needRestore) { /* ... */ }
        slot->failed = true;
        return false;
    }

    buf_info info;
    if (get_dmabuf_info_at(dpy, index, &info) < 0) {
        ANLAND_ERR("importBuffer: get_dmabuf_info_at failed for %d", index);
        close(fd);
        if (needRestore) { /* ... */ }
        slot->failed = true;
        return false;
    }

    // 4. 保存信息
    if (slot->fd >= 0) close(slot->fd);
    slot->fd = dup(fd);
    close(fd);
    if (slot->fd < 0) {
        ANLAND_ERR("importBuffer: dup failed");
        slot->failed = true;
        return false;
    }

    slot->width = info.width;
    slot->height = info.height;
    slot->format = info.format;
    slot->modifier = info.modifier;
    slot->offset = info.offset;
    slot->stride = info.stride;

    uint32_t drmFmt = protocolFormatToDrm(info.format);

    ANLAND_LOG("importBuffer: slot %d: %dx%d, drm_fmt=0x%x, mod=0x%llx",
               index, info.width, info.height, drmFmt,
               (unsigned long long)info.modifier);

    // 5. 构建 EGLImage
    EGLint attribs[50];
    int idx = 0;

    attribs[idx++] = EGL_WIDTH;
    attribs[idx++] = (EGLint)info.width;
    attribs[idx++] = EGL_HEIGHT;
    attribs[idx++] = (EGLint)info.height;
    attribs[idx++] = EGL_LINUX_DRM_FOURCC_EXT;
    attribs[idx++] = (EGLint)drmFmt;

    attribs[idx++] = EGL_DMA_BUF_PLANE0_FD_EXT;
    attribs[idx++] = slot->fd;
    attribs[idx++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
    attribs[idx++] = (EGLint)info.offset;
    attribs[idx++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
    attribs[idx++] = (EGLint)info.stride;

    if (info.modifier != 0 && info.modifier != DRM_FORMAT_MOD_INVALID) {
        attribs[idx++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
        attribs[idx++] = (EGLint)(info.modifier & 0xFFFFFFFF);
        attribs[idx++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
        attribs[idx++] = (EGLint)(info.modifier >> 32);
    }

    attribs[idx++] = EGL_IMAGE_PRESERVED_KHR;
    attribs[idx++] = EGL_TRUE;
    attribs[idx++] = EGL_NONE;

    slot->eglImage = g_eglCreateImageKHR(g_eglDisplay, EGL_NO_CONTEXT,
                                          EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
    if (slot->eglImage == EGL_NO_IMAGE_KHR) {
        ANLAND_ERR("importBuffer: eglCreateImageKHR failed, error=0x%x", eglGetError());
        if (needRestore) { /* ... */ }
        slot->failed = true;
        return false;
    }

    // 6. 创建 GL 纹理
    glGenTextures(1, &slot->texture);
    glBindTexture(GL_TEXTURE_2D, slot->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    g_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, slot->eglImage);
    glBindTexture(GL_TEXTURE_2D, 0);

    // 7. 创建 FBO
    glGenFramebuffers(1, &slot->framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, slot->framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, slot->texture, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        ANLAND_ERR("importBuffer: FBO incomplete for %d", index);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        destroyBuffer(index);
        slot->failed = true;
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // 8. 创建 AnlandBuffer
    slot->buffer = CSharedPointer<CAnlandBuffer>(new CAnlandBuffer(dpy, index, m_backend));
    slot->imported = true;
    slot->hasDamage = true;
    slot->inUse = false;
    slot->displayed = false;
    slot->rendered = false;
    slot->failed = false;

    // 9. 恢复上下文
    if (needRestore) {
        eglMakeCurrent(g_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(g_eglDisplay, tempCtx);
        ANLAND_LOG("importBuffer: restored GL context");
    }

    ANLAND_LOG("importBuffer: slot %d imported successfully", index);
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
    if (slot->eglImage != EGL_NO_IMAGE_KHR && g_eglDestroyImageKHR) {
        g_eglDestroyImageKHR(g_eglDisplay, slot->eglImage);
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
    slot->displayed = false;
    slot->rendered = false;
    slot->failed = false;
    slot->accumDamage.clear();
}

void CAnlandOutput::importBuffers(int count) {
    if (count <= 0 || count > MAX_BUFS || m_destroying) return;
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    m_bufferCount = count;
    m_buffersImported = false;
    ANLAND_LOG("importBuffers: count=%d", count);
}

void CAnlandOutput::releaseBuffers() {
    if (m_destroying) return;
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    for (int i = 0; i < MAX_BUFS; i++) {
        destroyBuffer(i);
    }
    m_bufferCount = 0;
    m_buffersImported = false;
    ANLAND_LOG("releaseBuffers: all buffers released");
}

void CAnlandOutput::selectNextBuffer() {
    if (m_bufferCount <= 0) {
        m_backIndex = 0;
        return;
    }

    int next = (m_backIndex + 1) % m_bufferCount;
    int attempts = 0;
    while ((next == m_frontIndex || !m_slots[next].imported || m_slots[next].inUse || m_slots[next].failed)
           && attempts < m_bufferCount) {
        next = (next + 1) % m_bufferCount;
        attempts++;
    }
    if (attempts >= m_bufferCount) {
        next = (m_backIndex + 1) % m_bufferCount;
    }

    m_backIndex = next;
    ANLAND_LOG("selectNextBuffer: back=%d, front=%d", m_backIndex, m_frontIndex);
}

bool CAnlandOutput::beginRender() {
    if (m_destroying || m_inFallback || !m_outputReady) {
        return false;
    }

    auto* dpy = display();
    if (!dpy || is_fallback(dpy)) {
        return false;
    }

    if (!ensureEGLInitialized()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_bufferMutex);

    // 获取缓冲区数量
    if (m_bufferCount == 0) {
        int count = get_buf_count(dpy);
        if (count > 0 && count <= MAX_BUFS) {
            m_bufferCount = count;
            ANLAND_LOG("beginRender: got %d buffers from display", count);
        }
        if (m_bufferCount == 0) {
            ANLAND_ERR("beginRender: no buffers");
            return false;
        }
    }

    // 选择可用槽位
    if (m_selectedIndex < 0 || m_selectedIndex >= m_bufferCount) {
        bool found = false;
        for (int i = 0; i < m_bufferCount; i++) {
            if (m_slots[i].imported && !m_slots[i].failed) {
                m_selectedIndex = i;
                found = true;
                break;
            }
        }
        if (!found) {
            for (int i = 0; i < m_bufferCount; i++) {
                if (!m_slots[i].failed) {
                    m_selectedIndex = i;
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            ANLAND_ERR("beginRender: no available slot");
            return false;
        }
        ANLAND_LOG("beginRender: selected slot %d", m_selectedIndex);
    }

    auto* slot = &m_slots[m_selectedIndex];

    // 确保已导入
    if (!slot->imported && !slot->failed) {
        if (!importBuffer(m_selectedIndex)) {
            ANLAND_ERR("beginRender: importBuffer failed for %d", m_selectedIndex);
            return false;
        }
    }

    if (!slot->imported) {
        ANLAND_ERR("beginRender: slot %d not imported", m_selectedIndex);
        return false;
    }

    // 标记状态
    slot->rendered = false;
    slot->inUse = true;

    // 绑定 FBO
    glBindFramebuffer(GL_FRAMEBUFFER, slot->framebuffer);
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        ANLAND_ERR("beginRender: glBindFramebuffer error=0x%x", err);
    }
    glViewport(0, 0, slot->width, slot->height);

    ANLAND_LOG("beginRender: slot %d FBO=%u", m_selectedIndex, slot->framebuffer);
    return true;
}

void CAnlandOutput::endRender() {
    if (m_inFallback || !m_outputReady || m_destroying) return;

    auto* slot = &m_slots[m_selectedIndex];
    if (!slot->imported) {
        ANLAND_LOG("endRender: slot not imported");
        return;
    }

    slot->rendered = true;
    slot->hasDamage = true;
    ANLAND_LOG("endRender: slot %d rendered", m_selectedIndex);

    glFlush();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool CAnlandOutput::commit() {
    if (m_destroying) return true;
    if (m_commitInProgress.exchange(true)) {
        ANLAND_LOG("commit: already in progress");
        return false;
    }

    struct CommitGuard {
        std::atomic<bool>& flag;
        ~CommitGuard() { flag = false; }
    } guard(m_commitInProgress);

    if (m_inFallback || !m_outputReady) {
        ANLAND_LOG("commit: fallback or not ready");
        return true;
    }

    auto* dpy = display();
    if (!dpy || is_fallback(dpy)) {
        ANLAND_LOG("commit: no display or fallback");
        return true;
    }

    std::lock_guard<std::mutex> lock(m_bufferMutex);

    if (m_selectedIndex < 0 || m_selectedIndex >= m_bufferCount) {
        ANLAND_LOG("commit: invalid selectedIndex %d", m_selectedIndex);
        return true;
    }

    auto* slot = &m_slots[m_selectedIndex];

    if (!slot->imported && !slot->failed) {
        if (!importBuffer(m_selectedIndex)) {
            return true;
        }
    }

    if (!slot->imported || !slot->rendered) {
        ANLAND_LOG("commit: slot %d not imported/rendered (imported=%d rendered=%d)",
                   m_selectedIndex, slot->imported, slot->rendered);
        return true;
    }

    // 触发刷新
    int ret = trigger_refresh(dpy);
    if (ret < 0) {
        ANLAND_ERR("commit: trigger_refresh failed");
        m_inFallback = true;
        return true;
    }

    m_framePending = true;
    slot->inUse = false;
    slot->displayed = true;
    ANLAND_LOG("commit: frame pending, slot %d displayed", m_selectedIndex);

    events.commit.emit();
    return true;
}

bool CAnlandOutput::test() {
    return !m_destroying && this->modes.size() > 0;
}

CSharedPointer<IBuffer> CAnlandOutput::getCurrentBuffer() {
    std::lock_guard<std::mutex> lock(m_bufferMutex);

    auto* dpy = display();
    if (!dpy || is_fallback(dpy)) {
        ANLAND_LOG("getCurrentBuffer: display not ready");
        return nullptr;
    }

    if (!ensureEGLInitialized()) {
        return nullptr;
    }

    if (m_bufferCount == 0) {
        int count = get_buf_count(dpy);
        if (count > 0 && count <= MAX_BUFS) {
            m_bufferCount = count;
            ANLAND_LOG("getCurrentBuffer: got %d buffers", count);
        }
        if (m_bufferCount == 0) {
            ANLAND_ERR("getCurrentBuffer: no buffers");
            return nullptr;
        }
    }

    if (m_selectedIndex < 0 || m_selectedIndex >= m_bufferCount) {
        bool found = false;
        for (int i = 0; i < m_bufferCount; i++) {
            if (m_slots[i].imported && !m_slots[i].failed) {
                m_selectedIndex = i;
                found = true;
                break;
            }
        }
        if (!found) {
            for (int i = 0; i < m_bufferCount; i++) {
                if (!m_slots[i].failed) {
                    m_selectedIndex = i;
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            ANLAND_ERR("getCurrentBuffer: no available slot");
            return nullptr;
        }
        ANLAND_LOG("getCurrentBuffer: selected slot %d", m_selectedIndex);
    }

    auto* slot = &m_slots[m_selectedIndex];

    if (!slot->imported && !slot->failed) {
        if (!importBuffer(m_selectedIndex)) {
            return nullptr;
        }
    }

    if (!slot->imported) {
        ANLAND_ERR("getCurrentBuffer: slot %d not imported", m_selectedIndex);
        return nullptr;
    }

    if (!slot->buffer) {
        slot->buffer = CSharedPointer<CAnlandBuffer>(new CAnlandBuffer(dpy, m_selectedIndex, m_backend));
        ANLAND_LOG("getCurrentBuffer: created CAnlandBuffer for slot %d", m_selectedIndex);
    }

    return slot->buffer;
}

void CAnlandOutput::scheduleFrame(scheduleFrameReason reason) {
    bool shouldLog = (reason == AQ_SCHEDULE_NEW_MONITOR || reason == AQ_SCHEDULE_NEW_CONNECTOR);
    if (shouldLog) {
        ANLAND_LOG("scheduleFrame: reason=%d, fallback=%d", (int)reason, m_inFallback);
    }

    if (m_destroying || m_inFallback || !m_outputReady) {
        if (shouldLog) ANLAND_LOG("scheduleFrame: ignored");
        return;
    }

    if (m_frameScheduled) {
        if (shouldLog) ANLAND_LOG("scheduleFrame: already scheduled");
        return;
    }

    m_frameScheduled = true;
    m_needsFrame = true;

    events.needsFrame.emit();

    if (!m_frameIdle) {
        m_frameIdle = makeShared<std::function<void(void)>>([this]() {
            m_frameScheduled = false;
            if (m_destroying || m_inFallback || !m_outputReady) {
                ANLAND_LOG("scheduleFrame idle: skipped");
                return;
            }
            ANLAND_LOG("scheduleFrame idle: emitting frame");
            events.frame.emit();
        });
    }

    auto backend = m_backend ? m_backend->backend() : nullptr;
    if (backend) {
        backend->addIdleEvent(m_frameIdle);
    } else {
        ANLAND_LOG("scheduleFrame: no backend, emitting directly");
        events.frame.emit();
        m_frameScheduled = false;
    }
}

void CAnlandOutput::onBufferReady() {
    if (m_inFallback || m_destroying) {
        ANLAND_LOG("onBufferReady: ignored (fallback/destroying)");
        return;
    }

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

    if (m_selectedIndex >= 0 && m_selectedIndex < m_bufferCount) {
        m_frontIndex = m_selectedIndex;
        m_slots[m_selectedIndex].displayed = true;
        ANLAND_LOG("onBufferReady: frontIndex=%d", m_frontIndex);
    }

    m_framePending = false;
    completeFrame();

    events.frame.emit();
    m_needsFrame = false;
    ANLAND_LOG("onBufferReady: done");
}

void CAnlandOutput::completeFrame() {
    if (m_framePending) {
        m_framePending = false;
        events.present.emit(IOutput::SPresentEvent{
            .presented = true,
            .when = nullptr,
            .seq = 0,
            .refresh = (int)m_refresh,
            .flags = 0
        });
        ANLAND_LOG("completeFrame: present emitted");
    }
}

void CAnlandOutput::onInputReady() {
    // 由后端处理
}

void CAnlandOutput::enterFallback() {
    if (m_inFallback || m_destroying) return;
    std::lock_guard<std::mutex> lock(m_bufferMutex);

    m_inFallback = true;
    m_framePending = false;
    m_outputReady = false;
    m_buffersImported = false;
    m_frameScheduled = false;
    this->enabled = false;
    this->state->setEnabled(false);

    if (m_frameIdle) {
        auto backend = m_backend ? m_backend->backend() : nullptr;
        if (backend) {
            backend->removeIdleEvent(m_frameIdle);
        }
        m_frameIdle = nullptr;
    }

    m_frontIndex = 0;
    m_backIndex = 0;
    m_selectedIndex = 0;
    ANLAND_LOG("enterFallback: done");
}

void CAnlandOutput::exitFallback() {
    if (!m_inFallback || m_destroying) return;

    m_inFallback = false;
    this->enabled = true;
    this->state->setEnabled(true);

    m_outputReady = true;
    m_buffersImported = false;
    m_needsFrame = true;
    m_frameScheduled = false;

    auto* dpy = display();
    if (dpy && !is_fallback(dpy)) {
        int count = get_buf_count(dpy);
        if (count > 0 && count <= MAX_BUFS) {
            m_bufferCount = count;
            ANLAND_LOG("exitFallback: %d buffers available", count);
        }
    }

    scheduleFrame(AQ_SCHEDULE_NEW_CONNECTOR);
    events.frame.emit();
    ANLAND_LOG("exitFallback: done");
}

GLuint CAnlandOutput::getCurrentFramebuffer() const {
    if (m_selectedIndex < 0 || m_selectedIndex >= m_bufferCount) return 0;
    if (!m_slots[m_selectedIndex].imported) return 0;
    return m_slots[m_selectedIndex].framebuffer;
}

GLuint CAnlandOutput::getCurrentTexture() const {
    if (m_selectedIndex < 0 || m_selectedIndex >= m_bufferCount) return 0;
    if (!m_slots[m_selectedIndex].imported) return 0;
    return m_slots[m_selectedIndex].texture;
}

void CAnlandOutput::setRenderFence(int fenceFd) {
    if (m_renderFenceFd >= 0) close(m_renderFenceFd);
    m_renderFenceFd = fenceFd;
    ANLAND_LOG("setRenderFence: %d", fenceFd);
}

} // namespace Aquamarine