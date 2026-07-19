// src/backend/anland/AnlandBuffer.cpp
#include "AnlandBuffer.hpp"
#include <unistd.h>
#include <cstring>

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
    attrs.modifier = m_info.modifier;
    attrs.planes = 1;
    attrs.fds[0] = m_fd;
    attrs.offsets[0] = m_info.offset;
    attrs.strides[0] = m_info.stride;
    return attrs;
}

} // namespace Aquamarine