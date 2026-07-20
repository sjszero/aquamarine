// src/backend/anland/AnlandAllocator.cpp
#include "AnlandAllocator.hpp"
#include "AnlandOutput.hpp"
#include "AnlandBuffer.hpp"
#include <aquamarine/backend/Backend.hpp>

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
    if (count <= 0) return nullptr;

    // 找下一个可用的缓冲区
    int start = (m_lastAcquired + 1) % count;
    int idx = start;
    
    for (int i = 0; i < count; i++) {
        auto buf = m_output->getBuffer(idx);
        if (buf && !buf->inUse) {
            buf->inUse = true;
            m_lastAcquired = idx;
            ANLAND_TRACE("acquire: using buffer %d", idx);
            return CSharedPointer<IBuffer>(static_cast<IBuffer*>(buf.get()));
        }
        idx = (idx + 1) % count;
    }

    // 如果所有缓冲区都在使用中，返回第一个（覆盖）
    auto buf = m_output->getBuffer(start);
    if (buf) {
        buf->inUse = true;
        m_lastAcquired = start;
        ANLAND_TRACE("acquire: reusing buffer %d (all busy)", start);
        return CSharedPointer<IBuffer>(static_cast<IBuffer*>(buf.get()));
    }

    return nullptr;
}

CSharedPointer<CBackend> CAnlandAllocator::getBackend() {
    if (!m_output) return nullptr;
    return m_output->getCBackend();
}

} // namespace Aquamarine