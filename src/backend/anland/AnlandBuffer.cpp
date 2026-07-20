// src/backend/anland/AnlandBuffer.cpp
#include "AnlandBuffer.hpp"
#include <unistd.h>
#include <cstring>
#include <drm_fourcc.h>

#define ANLAND_TRACE(fmt, ...) do { fprintf(stderr, "[ANLAND][TRACE] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while(0)

namespace Aquamarine {

CAnlandDmaBuffer::CAnlandDmaBuffer(int fd, const buf_info& info)
    : m_fd(dup(fd)), m_info(info) {
    ANLAND_TRACE("CAnlandDmaBuffer: fd=%d, size=%dx%d", fd, info.width, info.height);
    size = { (float)info.width, (float)info.height };
    opaque = true;
}

CAnlandDmaBuffer::~CAnlandDmaBuffer() {
    ANLAND_TRACE("CAnlandDmaBuffer destructor: fd=%d", m_fd);
    if (m_fd >= 0) close(m_fd);
    events.destroy.emit();
}

SDMABUFAttrs CAnlandDmaBuffer::dmabuf() {
    SDMABUFAttrs attrs;
    attrs.success = false;
    if (m_fd < 0) return attrs;

    attrs.success = true;
    attrs.size = size;
    attrs.format = m_info.format;
    // 强制使用 INVALID modifier，避免 EGL_BAD_MATCH
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
    return attrs;
}

} // namespace Aquamarine