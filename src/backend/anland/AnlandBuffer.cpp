// src/backend/anland/AnlandBuffer.cpp
#include "AnlandBuffer.hpp"
#include "AnlandBackend.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <cstring>

#define ANLAND_LOG(fmt, ...) fprintf(stderr, "[ANLAND] " fmt "\n", ##__VA_ARGS__)

namespace Aquamarine {

CAnlandBuffer::CAnlandBuffer(display_ctx* display, int index, CAnlandBackend* backend)
    : m_display(display), m_backend(backend), m_index(index) {
    if (!m_display) return;

    int fd = get_dmabuf_fd_at(m_display, index);
    if (fd < 0) {
        ANLAND_LOG("CAnlandBuffer: get_dmabuf_fd_at failed for %d", index);
        return;
    }

    struct buf_info info;
    if (get_dmabuf_info_at(m_display, index, &info) < 0) {
        close(fd);
        ANLAND_LOG("CAnlandBuffer: get_dmabuf_info_at failed for %d", index);
        return;
    }

    m_fd = dup(fd);
    close(fd);
    if (m_fd < 0) {
        ANLAND_LOG("CAnlandBuffer: dup failed for %d", index);
        return;
    }

    // 修复：显式转换 uint32_t 到 int 避免构造函数歧义
    this->size = Hyprutils::Math::Vector2D(
        static_cast<int>(info.width),
        static_cast<int>(info.height)
    );
    this->opaque = true;
    m_format = info.format;
    m_modifier = info.modifier;
    m_stride = info.stride;
    m_offset = info.offset;
    m_good = true;

    ANLAND_LOG("CAnlandBuffer: created for slot %d, %dx%d", index, info.width, info.height);
}

CAnlandBuffer::~CAnlandBuffer() {
    if (m_mappedData) {
        munmap(m_mappedData, m_mappedSize);
        m_mappedData = nullptr;
    }
    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
    }
    events.destroy.emit();
    ANLAND_LOG("CAnlandBuffer: destroyed for slot %d", m_index);
}

void CAnlandBuffer::update(const Hyprutils::Math::CRegion& damage) {
    (void)damage;
}

bool CAnlandBuffer::good() {
    return m_fd >= 0 && m_good;
}

SDMABUFAttrs CAnlandBuffer::dmabuf() {
    SDMABUFAttrs attrs;
    attrs.success = m_good && m_fd >= 0;
    if (!attrs.success) return attrs;

    attrs.planes = 1;
    attrs.fds[0] = m_fd;
    attrs.offsets[0] = m_offset;
    attrs.strides[0] = m_stride;
    attrs.format = m_format;
    attrs.modifier = m_modifier;
    attrs.size = this->size;

    return attrs;
}

SSHMAttrs CAnlandBuffer::shm() {
    SSHMAttrs attrs;
    attrs.success = false;
    return attrs;
}

std::tuple<uint8_t*, uint32_t, size_t> CAnlandBuffer::beginDataPtr(uint32_t flags) {
    (void)flags;

    if (m_fd < 0 || !m_good) {
        return {nullptr, 0, 0};
    }

    if (!m_mappedData) {
        off_t size = lseek(m_fd, 0, SEEK_END);
        if (size < 0) {
            ANLAND_LOG("CAnlandBuffer: lseek failed for slot %d", m_index);
            return {nullptr, 0, 0};
        }

        m_mappedSize = (size_t)size;
        m_mappedData = mmap(nullptr, m_mappedSize, PROT_READ | PROT_WRITE,
                            MAP_SHARED, m_fd, 0);
        if (m_mappedData == MAP_FAILED) {
            m_mappedData = nullptr;
            ANLAND_LOG("CAnlandBuffer: mmap failed for slot %d", m_index);
            return {nullptr, 0, 0};
        }
        m_mapped = true;
        ANLAND_LOG("CAnlandBuffer: mmap success for slot %d, size=%zu", m_index, m_mappedSize);
    }

    // 注意：这里缺少 m_mappedStride 的初始化，应该是 m_stride
    // 但 beginDataPtr 返回的第二个参数是 stride，需要正确设置
    return {static_cast<uint8_t*>(m_mappedData), m_stride, m_mappedSize};
}

void CAnlandBuffer::endDataPtr() {
    // 无需 msync，避免阻塞渲染循环
}

} // namespace Aquamarine