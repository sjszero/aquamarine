// src/backend/anland/AnlandAllocator.cpp
#include "AnlandAllocator.hpp"
#include "AnlandOutput.hpp"
#include "AnlandBuffer.hpp"
#include <aquamarine/backend/Backend.hpp>

#define ANLAND_TRACE(fmt, ...) do { fprintf(stderr, "[ANLAND][TRACE] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while(0)
#define ANLAND_DEBUG(fmt, ...) do { fprintf(stderr, "[ANLAND][DEBUG] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while(0)
#define ANLAND_ERROR(fmt, ...) do { fprintf(stderr, "[ANLAND][ERROR] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while(0)

namespace Aquamarine {

using Hyprutils::Memory::CSharedPointer;

CAnlandAllocator::CAnlandAllocator(CAnlandOutput* output)
    : m_output(output) {
    ANLAND_DEBUG("CAnlandAllocator constructed");
}

CSharedPointer<IAllocator> CAnlandAllocator::create(CAnlandOutput* output) {
    if (!output) {
        ANLAND_ERROR("create: output is null");
        return nullptr;
    }
    auto alloc = new CAnlandAllocator(output);
    return CSharedPointer<IAllocator>(static_cast<IAllocator*>(alloc));
}

CSharedPointer<IBuffer> CAnlandAllocator::acquire(const SAllocatorBufferParams& params, CSharedPointer<CSwapchain> swapchain) {
    (void)params;
    if (!m_output) {
        ANLAND_ERROR("acquire: m_output is null");
        return nullptr;
    }

    int count = m_output->getBufferCount();
    if (count <= 0) {
        ANLAND_ERROR("acquire: no buffers available");
        return nullptr;
    }

    int start = (m_lastAcquired + 1) % count;
    int idx = start;
    
    for (int i = 0; i < count; i++) {
        auto buf = m_output->getBuffer(idx);
        if (buf && buf->good() && !buf->inUse) {
            buf->inUse = true;
            m_lastAcquired = idx;
            ANLAND_DEBUG("acquire: using buffer %d", idx);
            return buf;
        }
        idx = (idx + 1) % count;
    }

    auto buf = m_output->getBuffer(start);
    if (buf && buf->good()) {
        ANLAND_DEBUG("acquire: reusing buffer %d (all busy)", start);
        buf->inUse = true;
        m_lastAcquired = start;
        return buf;
    }

    ANLAND_ERROR("acquire: no usable buffers");
    return nullptr;
}

CSharedPointer<CBackend> CAnlandAllocator::getBackend() {
    if (!m_output) return nullptr;
    return m_output->getCBackend();
}

} // namespace Aquamarine