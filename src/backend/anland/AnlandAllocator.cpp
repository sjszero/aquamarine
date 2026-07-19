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
    (void)swapchain;
    if (!m_output) return nullptr;

    int count = m_output->getBufferCount();
    if (count <= 0) return nullptr;

    m_lastAcquired = (m_lastAcquired + 1) % count;
    auto buf = m_output->getBuffer(m_lastAcquired);
    if (buf) {
        buf->inUse = true;
    }
    return CSharedPointer<IBuffer>(static_cast<IBuffer*>(buf.get()));
}

CSharedPointer<CBackend> CAnlandAllocator::getBackend() {
    if (!m_output) return nullptr;
    return m_output->getCBackend();
}

} // namespace Aquamarine