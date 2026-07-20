// src/backend/anland/AnlandBuffer.cpp
#include "AnlandBuffer.hpp"
#include <unistd.h>
#include <cstring>
#include <drm_fourcc.h>

#define ANLAND_TRACE(fmt, ...) do { fprintf(stderr, "[ANLAND][TRACE] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while(0)
#define ANLAND_DEBUG(fmt, ...) do { fprintf(stderr, "[ANLAND][DEBUG] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while(0)
#define ANLAND_ERROR(fmt, ...) do { fprintf(stderr, "[ANLAND][ERROR] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while(0)

namespace Aquamarine {

CAnlandDmaBuffer::CAnlandDmaBuffer(int fd, const buf_info& info)
    : m_fd(dup(fd)), m_info(info) {
    ANLAND_DEBUG("CAnlandDmaBuffer: fd=%d (dup from %d), size=%dx%d, format=0x%x (%s), modifier=0x%lx, stride=%d, offset=%d",
                 m_fd, fd, info.width, info.height, info.format, 
                 info.format == 1 ? "XR24" : "unknown", 
                 info.modifier, info.stride, info.offset);
    size = { (float)info.width, (float)info.height };
    opaque = true;
}

CAnlandDmaBuffer::~CAnlandDmaBuffer() {
    ANLAND_DEBUG("CAnlandDmaBuffer destructor: fd=%d, inUse=%d", m_fd, inUse);
    inUse = false;
    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
    }
    events.destroy.emit();
}

SDMABUFAttrs CAnlandDmaBuffer::dmabuf() {
    SDMABUFAttrs attrs;
    attrs.success = false;
    if (m_fd < 0) {
        ANLAND_ERROR("dmabuf: fd is invalid (%d)", m_fd);
        return attrs;
    }

    attrs.success = true;
    attrs.size = size;
    attrs.format = m_info.format;
    attrs.modifier = DRM_FORMAT_MOD_INVALID;
    attrs.planes = 1;
    attrs.fds[0] = m_fd;
    attrs.offsets[0] = m_info.offset;
    attrs.strides[0] = m_info.stride;
    for (int i = 1; i < 4; i++) {
        attrs.fds[i] = -1;
        attrs.offsets[i] = 0;
        attrs.strides[i] = 0;
    }
    
    ANLAND_DEBUG("dmabuf: fd=%d, size=%.0fx%.0f, format=0x%x, modifier=0x%lx (INVALID), stride=%d, offset=%d, planes=%d",
                 m_fd, size.x, size.y, attrs.format, attrs.modifier, attrs.strides[0], attrs.offsets[0], attrs.planes);
    return attrs;
}

void CAnlandDmaBuffer::sendRelease() {
    ANLAND_DEBUG("sendRelease: fd=%d, inUse was %d", m_fd, inUse);
    inUse = false;
    events.backendRelease.emit();
}

} // namespace Aquamarine