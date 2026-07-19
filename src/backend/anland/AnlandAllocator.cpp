// src/backend/anland/AnlandAllocator.cpp
#include "AnlandAllocator.hpp"
#include "AnlandBuffer.hpp"
#include "AnlandBackend.hpp"
#include "display_producer.h"
#include <algorithm>

namespace Aquamarine {

using namespace Hyprutils::Memory;

CAnlandAllocator::CAnlandAllocator(display_ctx* display, CAnlandBackend* backend)
    : m_display(display), m_backend(backend) {
}

CAnlandAllocator::~CAnlandAllocator() {
    destroyBuffers();
}

SP<CAnlandAllocator> CAnlandAllocator::create(display_ctx* display, CAnlandBackend* backend) {
    auto alloc = SP<CAnlandAllocator>(new CAnlandAllocator(display, backend));
    if (!alloc->importBuffers()) {
        return nullptr;
    }
    return alloc;
}

bool CAnlandAllocator::importBuffers() {
    if (!m_display || is_fallback(m_display))
        return false;

    int count = get_buf_count(m_display);
    if (count <= 0 || count > MAX_BUFS)
        return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    destroyBuffers();

    for (int i = 0; i < count; ++i) {
        int fd = get_dmabuf_fd_at(m_display, i);
        if (fd < 0) continue;
        buf_info info;
        if (get_dmabuf_info_at(m_display, i, &info) < 0) {
            close(fd);
            continue;
        }
        auto buf = SP<CAnlandBuffer>(new CAnlandBuffer(fd, info, this));
        if (buf->good()) {
            m_buffers.emplace_back(buf);
        }
        close(fd);
    }
    m_bufferCount = m_buffers.size();
    return m_bufferCount > 0;
}

void CAnlandAllocator::destroyBuffers() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_buffers.clear();
    m_bufferCount = 0;
    m_lastAcquired = -1;
}

SP<IBuffer> CAnlandAllocator::acquire(const SAllocatorBufferParams& params, SP<CSwapchain> swapchain_) {
    (void)params;
    (void)swapchain_;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_buffers.empty())
        return nullptr;

    int next = (m_lastAcquired + 1) % m_buffers.size();
    int attempts = 0;
    while (attempts < (int)m_buffers.size()) {
        auto b = m_buffers[next].lock();
        if (b && !b->inUse) {
            b->inUse = true;
            m_lastAcquired = next;
            return b;
        }
        next = (next + 1) % m_buffers.size();
        attempts++;
    }
    // Fallback: return first buffer even if in use
    auto b = m_buffers[next].lock();
    if (b) {
        b->inUse = true;
        m_lastAcquired = next;
        return b;
    }
    return nullptr;
}

SP<CBackend> CAnlandAllocator::getBackend() {
    return m_backend ? m_backend->getBackend() : nullptr;
}

} // namespace Aquamarine