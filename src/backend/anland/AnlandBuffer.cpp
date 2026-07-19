// src/backend/anland/AnlandBuffer.cpp
#include "AnlandBuffer.hpp"
#include "AnlandAllocator.hpp"
#include <unistd.h>
#include <cstring>

namespace Aquamarine {

CAnlandBuffer::CAnlandBuffer(int fd, const buf_info& info, CAnlandAllocator* allocator)
    : m_fd(dup(fd)), m_info(info), m_allocator(allocator) {
    if (m_fd < 0) {
        m_good = false;
        return;
    }
    size = { (float)info.width, (float)info.height };
    opaque = true;
    m_good = true;
}

CAnlandBuffer::~CAnlandBuffer() {
    if (m_fd >= 0)
        close(m_fd);
    events.destroy.emit();
}

bool CAnlandBuffer::good() {
    return m_good && m_fd >= 0;
}

SDMABUFAttrs CAnlandBuffer::dmabuf() {
    SDMABUFAttrs attrs;
    attrs.success = false;
    if (!good()) return attrs;

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