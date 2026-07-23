// src/backend/anland/AnlandBuffer.cpp
#include "AnlandBuffer.hpp"
#include <unistd.h>
#include <cstring>
#include <drm_fourcc.h>

// 日志宏
#define ANLAND_TRACE(fmt, ...) do { fprintf(stderr, "[ANLAND][TRACE] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while(0)
#define ANLAND_DEBUG(fmt, ...) do { fprintf(stderr, "[ANLAND][DEBUG] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while(0)
#define ANLAND_WARN(fmt, ...) do { fprintf(stderr, "[ANLAND][WARN] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while(0)
#define ANLAND_ERROR(fmt, ...) do { fprintf(stderr, "[ANLAND][ERROR] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while(0)

namespace Aquamarine {

CAnlandDmaBuffer::CAnlandDmaBuffer(int fd, const buf_info& info,
                                   uint32_t drmFormat, uint64_t modifier,
                                   bool forceLinear)
    : m_info(info), m_drmFormat(drmFormat), m_modifier(modifier), m_forceLinear(forceLinear) {
    
    // Force LINEAR modifier if requested
    if (m_forceLinear) {
        m_modifier = DRM_FORMAT_MOD_LINEAR;
        ANLAND_DEBUG("Forcing LINEAR modifier for buffer");
    }
    
    // If modifier is INVALID and we're not forcing linear, use LINEAR as fallback
    if (m_modifier == DRM_FORMAT_MOD_INVALID) {
        m_modifier = DRM_FORMAT_MOD_LINEAR;
        ANLAND_DEBUG("Converting INVALID modifier to LINEAR for compatibility");
    }
    
    m_ownedFd = dup(fd);
    if (m_ownedFd < 0) {
        ANLAND_ERROR("CAnlandDmaBuffer: dup failed for fd %d", fd);
        m_fd = -1;
        return;
    }
    m_fd = m_ownedFd;
    size = { (float)info.width, (float)info.height };
    opaque = true;
    ANLAND_DEBUG("CAnlandDmaBuffer: fd=%d, size=%dx%d, drm_fmt=0x%x, modifier=0x%lx",
                 m_fd, info.width, info.height, drmFormat, m_modifier);
}

CAnlandDmaBuffer::~CAnlandDmaBuffer() {
    ANLAND_DEBUG("CAnlandDmaBuffer destructor: fd=%d", m_fd);
    inUse = false;
    
    if (m_ownedFd >= 0) {
        close(m_ownedFd);
        m_ownedFd = -1;
        m_fd = -1;
    }
    events.destroy.emit();
}

SDMABUFAttrs CAnlandDmaBuffer::dmabuf() {
    SDMABUFAttrs attrs;
    attrs.success = false;
    if (m_fd < 0) {
        ANLAND_DEBUG("dmabuf: fd is invalid");
        return attrs;
    }

    attrs.success = true;
    attrs.size = size;
    attrs.format = m_drmFormat;
    attrs.modifier = m_modifier;
    attrs.planes = 1;
    attrs.fds[0] = m_fd;
    attrs.offsets[0] = m_info.offset;
    attrs.strides[0] = m_info.stride;
    for (int i = 1; i < 4; i++) {
        attrs.fds[i] = -1;
        attrs.offsets[i] = 0;
        attrs.strides[i] = 0;
    }

    ANLAND_DEBUG("dmabuf: fd=%d, size=%.0fx%.0f, format=0x%x, modifier=0x%lx",
                 m_fd, size.x, size.y, attrs.format, attrs.modifier);
    return attrs;
}

void CAnlandDmaBuffer::sendRelease() {
    ANLAND_DEBUG("sendRelease: fd=%d", m_fd);
    inUse = false;
    events.backendRelease.emit();
}

} // namespace Aquamarine