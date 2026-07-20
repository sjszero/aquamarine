// src/backend/anland/AnlandAllocator.cpp
#include "AnlandAllocator.hpp"
#include "AnlandOutput.hpp"
#include "AnlandBuffer.hpp"
#include <aquamarine/backend/Backend.hpp>

#define ANLAND_TRACE(fmt, ...) do { fprintf(stderr, "[ANLAND][TRACE] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while(0)
#define ANLAND_ERR(fmt, ...) do { fprintf(stderr, "[ANLAND][ERR] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while(0)

namespace Aquamarine {

using Hyprutils::Memory::CSharedPointer;

CAnlandAllocator::CAnlandAllocator(CAnlandOutput* output)
    : m_output(output) {
}

CSharedPointer<IAllocator> CAnlandAllocator::create(CAnlandOutput* output) {
    if (!output) return nullptr;
    auto alloc = new CAnlandAllocator(output);
    return CSharedPointer<IAllocator>(static_cast<IAllocator*>(alloc));
}

CSharedPointer<IBuffer> CAnlandAllocator::acquire(const SAllocatorBufferParams& params, CSharedPointer<CSwapchain> swapchain) {
    (void)params;
    if (!m_output) return nullptr;

    int count = m_output->getBufferCount();
    if (count <= 0) {
        ANLAND_ERR("acquire: no buffers available");
        return nullptr;
    }

    // 找下一个可用的缓冲区（not in use）
    int start = (m_lastAcquired + 1) % count;
    int idx = start;
    
    for (int i = 0; i < count; i++) {
        auto buf = m_output->getBuffer(idx);
        if (buf && buf->good() && !buf->inUse) {
            buf->inUse = true;
            m_lastAcquired = idx;
            ANLAND_TRACE("acquire: using buffer %d (fd=%d)", idx, buf->dmabuf().fds[0]);
            // 修复：使用隐式转换，与 m_output 中的 shared_ptr 共享所有权
            return buf;
        }
        idx = (idx + 1) % count;
    }

    // 所有缓冲区都在使用中，尝试返回第一个可用的（即使 inUse=true）
    auto buf = m_output->getBuffer(start);
    if (buf && buf->good()) {
        ANLAND_TRACE("acquire: reusing buffer %d (all busy, fd=%d)", start, buf->dmabuf().fds[0]);
        buf->inUse = true;
        m_lastAcquired = start;
        // 修复：使用隐式转换，与 m_output 中的 shared_ptr 共享所有权
        return buf;
    }

    ANLAND_ERR("acquire: no usable buffers");
    return nullptr;
}

CSharedPointer<CBackend> CAnlandAllocator::getBackend() {
    if (!m_output) return nullptr;
    return m_output->getCBackend();
}

} // namespace Aquamarine