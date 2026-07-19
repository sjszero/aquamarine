// src/backend/anland/AnlandAllocator.cpp
#include "AnlandAllocator.hpp"
#include "AnlandBuffer.hpp"
#include "AnlandBackend.hpp"
#include "display_producer.h"
#include <algorithm>

#define ANLAND_LOG(fmt, ...) fprintf(stderr, "[ANLAND] " fmt "\n", ##__VA_ARGS__)
#define ANLAND_ERR(fmt, ...) fprintf(stderr, "[ANLAND][ERR] " fmt "\n", ##__VA_ARGS__)
#define ANLAND_TRACE(fmt, ...) fprintf(stderr, "[ANLAND][TRACE] " fmt "\n", ##__VA_ARGS__)

namespace Aquamarine {

using namespace Hyprutils::Memory;

CAnlandAllocator::CAnlandAllocator(display_ctx* display, CAnlandBackend* backend)
    : m_display(display), m_backend(backend) {
    ANLAND_TRACE("CAnlandAllocator constructor");
}

CAnlandAllocator::~CAnlandAllocator() {
    ANLAND_TRACE("CAnlandAllocator destructor");
    destroyBuffers();
}

CSharedPointer<IAllocator> CAnlandAllocator::create(display_ctx* display, CAnlandBackend* backend) {
    ANLAND_TRACE("CAnlandAllocator::create START");
    
    auto alloc = new CAnlandAllocator(display, backend);
    ANLAND_TRACE("CAnlandAllocator::create: allocated, importing buffers");
    
    if (!alloc->importBuffers()) {
        ANLAND_ERR("CAnlandAllocator::create: importBuffers failed");
        delete alloc;
        return nullptr;
    }
    
    ANLAND_TRACE("CAnlandAllocator::create: importBuffers succeeded");
    auto result = CSharedPointer<IAllocator>(static_cast<IAllocator*>(alloc));
    ANLAND_TRACE("CAnlandAllocator::create END - success");
    return result;
}

bool CAnlandAllocator::importBuffers() {
    ANLAND_TRACE("CAnlandAllocator::importBuffers START");
    
    if (!m_display) {
        ANLAND_ERR("importBuffers: no display");
        return false;
    }
    
    if (is_fallback(m_display)) {
        ANLAND_LOG("importBuffers: display in fallback");
        return false;
    }

    int count = get_buf_count(m_display);
    ANLAND_LOG("importBuffers: get_buf_count returned %d", count);
    
    if (count <= 0 || count > MAX_BUFS) {
        ANLAND_ERR("importBuffers: invalid count %d", count);
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    destroyBuffers();

    for (int i = 0; i < count; ++i) {
        ANLAND_TRACE("importBuffers: getting fd for buffer %d", i);
        int fd = get_dmabuf_fd_at(m_display, i);
        if (fd < 0) {
            ANLAND_ERR("importBuffers: get_dmabuf_fd_at failed for %d", i);
            continue;
        }
        
        buf_info info;
        if (get_dmabuf_info_at(m_display, i, &info) < 0) {
            ANLAND_ERR("importBuffers: get_dmabuf_info_at failed for %d", i);
            close(fd);
            continue;
        }
        
        ANLAND_TRACE("importBuffers: buffer %d: %dx%d fd=%d", i, info.width, info.height, fd);
        
        auto buf = CSharedPointer<CAnlandBuffer>(new CAnlandBuffer(fd, info, this));
        if (buf->good()) {
            m_buffers.emplace_back(buf);
            ANLAND_TRACE("importBuffers: buffer %d imported successfully", i);
        } else {
            ANLAND_ERR("importBuffers: buffer %d not good", i);
        }
    }
    m_bufferCount = m_buffers.size();
    ANLAND_LOG("importBuffers: imported %d/%d buffers", m_bufferCount, count);
    return m_bufferCount > 0;
}

void CAnlandAllocator::destroyBuffers() {
    ANLAND_TRACE("destroyBuffers: clearing %zu buffers", m_buffers.size());
    std::lock_guard<std::mutex> lock(m_mutex);
    m_buffers.clear();
    m_bufferCount = 0;
    m_lastAcquired = -1;
}

CSharedPointer<IBuffer> CAnlandAllocator::acquire(const SAllocatorBufferParams& params, CSharedPointer<CSwapchain> swapchain_) {
    (void)params;
    (void)swapchain_;
    ANLAND_TRACE("acquire START");
    
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_buffers.empty()) {
        ANLAND_ERR("acquire: no buffers");
        return nullptr;
    }

    int next = (m_lastAcquired + 1) % m_buffers.size();
    int attempts = 0;
    while (attempts < (int)m_buffers.size()) {
        auto b = m_buffers[next].lock();
        if (b && !b->inUse) {
            b->inUse = true;
            m_lastAcquired = next;
            ANLAND_TRACE("acquire: returning buffer %d", next);
            return CSharedPointer<IBuffer>(static_cast<IBuffer*>(b.get()));
        }
        next = (next + 1) % m_buffers.size();
        attempts++;
    }
    
    auto b = m_buffers[next].lock();
    if (b) {
        b->inUse = true;
        m_lastAcquired = next;
        ANLAND_TRACE("acquire: returning buffer %d (forced)", next);
        return CSharedPointer<IBuffer>(static_cast<IBuffer*>(b.get()));
    }
    
    ANLAND_ERR("acquire: no buffer available");
    return nullptr;
}

CSharedPointer<CBackend> CAnlandAllocator::getBackend() {
    return m_backend ? m_backend->getBackend() : nullptr;
}

} // namespace Aquamarine