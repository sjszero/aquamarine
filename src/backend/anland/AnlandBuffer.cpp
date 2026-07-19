// src/backend/anland/AnlandBuffer.cpp
#include "AnlandBuffer.hpp"
#include "AnlandAllocator.hpp"
#include <unistd.h>
#include <cstring>

#define ANLAND_LOG(fmt, ...) fprintf(stderr, "[ANLAND] " fmt "\n", ##__VA_ARGS__)
#define ANLAND_ERR(fmt, ...) fprintf(stderr, "[ANLAND][ERR] " fmt "\n", ##__VA_ARGS__)
#define ANLAND_TRACE(fmt, ...) fprintf(stderr, "[ANLAND][TRACE] " fmt "\n", ##__VA_ARGS__)

namespace Aquamarine {

CAnlandBuffer::CAnlandBuffer(int fd, const buf_info& info, CAnlandAllocator* allocator)
    : m_fd(dup(fd)), m_info(info), m_allocator(allocator) {
    ANLAND_TRACE("CAnlandBuffer: fd=%d, size=%dx%d", fd, info.width, info.height);
    if (m_fd < 0) {
        ANLAND_ERR("CAnlandBuffer: dup failed for fd=%d", fd);
        m_good = false;
        return;
    }
    size = { (float)info.width, (float)info.height };
    opaque = true;
    m_good = true;
    ANLAND_TRACE("CAnlandBuffer: success, dup fd=%d", m_fd);
}

CAnlandBuffer::~CAnlandBuffer() {
    ANLAND_TRACE("CAnlandBuffer destructor: fd=%d", m_fd);
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