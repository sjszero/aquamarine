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

    // 找下一个可用的缓冲区（未在使用中）
    int start = (m_lastAcquired + 1) % count;
    int idx = start;
    bool found = false;
    
    for (int i = 0; i < count; i++) {
        auto buf = m_output->getBuffer(idx);
        if (buf && !buf->inUse) {
            buf->inUse = true;
            m_lastAcquired = idx;
            found = true;
            // 确保缓冲区对象被正确引用
            return CSharedPointer<IBuffer>(static_cast<IBuffer*>(buf.get()));
        }
        idx = (idx + 1) % count;
    }

    // 如果没有找到空闲缓冲区，返回第一个（老缓冲区会被覆盖）
    auto buf = m_output->getBuffer(start);
    if (buf) {
        buf->inUse = true;
        m_lastAcquired = start;
        return CSharedPointer<IBuffer>(static_cast<IBuffer*>(buf.get()));
    }

    return nullptr;
}

CSharedPointer<CBackend> CAnlandAllocator::getBackend() {
    if (!m_output) return nullptr;
    return m_output->getCBackend();
}

} // namespace Aquamarine