// src/backend/anland/AnlandAllocator.hpp
#ifndef AQUAMARINE_ANLAND_ALLOCATOR_HPP
#define AQUAMARINE_ANLAND_ALLOCATOR_HPP

#include <aquamarine/allocator/Allocator.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/memory/WeakPtr.hpp>
#include <vector>
#include <mutex>

namespace Aquamarine {

using Hyprutils::Memory::CSharedPointer;
using Hyprutils::Memory::CWeakPointer;

class CAnlandOutput;
class CAnlandDmaBuffer;
class CBackend;

// 最小化分配器：不分配新内存，只返回 CAnlandOutput 已注册的缓冲区
class CAnlandAllocator : public IAllocator {
public:
    static CSharedPointer<IAllocator> create(CAnlandOutput* output);
    virtual ~CAnlandAllocator() = default;

    virtual CSharedPointer<IBuffer> acquire(const SAllocatorBufferParams& params, CSharedPointer<CSwapchain> swapchain) override;
    virtual CSharedPointer<CBackend> getBackend() override;
    virtual int drmFD() override { return -1; }
    virtual eAllocatorType type() override { return AQ_ALLOCATOR_TYPE_GBM; }
    virtual void destroyBuffers() override {}

private:
    CAnlandAllocator(CAnlandOutput* output);
    CAnlandOutput* m_output = nullptr;
    int m_lastAcquired = -1;
};

} // namespace Aquamarine

#endif