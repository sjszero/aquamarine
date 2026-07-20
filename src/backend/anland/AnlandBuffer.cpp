// src/backend/anland/AnlandBuffer.cpp
#include "AnlandBuffer.hpp"
#include <unistd.h>
#include <cstring>
#include <drm_fourcc.h>

#define ANLAND_TRACE(fmt, ...) do { fprintf(stderr, "[ANLAND][TRACE] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while(0)
#define ANLAND_DEBUG(fmt, ...) do { fprintf(stderr, "[ANLAND][DEBUG] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while(0)
#define ANLAND_ERROR(fmt, ...) do { fprintf(stderr, "[ANLAND][ERROR] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while(0)

namespace Aquamarine {

CAnlandDmaBuffer::CAnlandDmaBuffer(int fd, const buf_info& info, uint64_t modifier)
    : m_fd(dup(fd)), m_info(info), m_modifier(modifier) {
    size = { (float)info.width, (float)info.height };
    opaque = true;
    ANLAND_DEBUG("CAnlandDmaBuffer: fd=%d, size=%dx%d, format=0x%x, modifier=0x%lx",
                 m_fd, info.width, info.height, info.format, modifier);
}

CAnlandDmaBuffer::~CAnlandDmaBuffer() {
    ANLAND_DEBUG("CAnlandDmaBuffer destructor: fd=%d", m_fd);
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
        ANLAND_ERROR("dmabuf: fd is invalid");
        return attrs;
    }

    attrs.success = true;
    attrs.size = size;
    attrs.format = m_info.format;
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

    return attrs;
}

void CAnlandDmaBuffer::sendRelease() {
    ANLAND_DEBUG("sendRelease: fd=%d", m_fd);
    inUse = false;
    events.backendRelease.emit();
}

} // namespace Aquamarine